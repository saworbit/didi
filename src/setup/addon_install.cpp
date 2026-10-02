#include "didi/setup/addon_install.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/secure_random.hpp"
#include "didi/offline/project_settings_file.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace didi::setup {
namespace {

std::string utf8(const std::filesystem::path& path) { return paths::projectPathToUtf8(path); }

std::string describeBuild(const std::optional<BuildId>& build) {
    return build ? build->text : std::string("an unknown build");
}

bool sameBytes(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    const auto left_size = std::filesystem::file_size(left, error);
    if (error) return false;
    const auto right_size = std::filesystem::file_size(right, error);
    if (error || left_size != right_size) return false;
    std::ifstream a(left, std::ios::binary);
    std::ifstream b(right, std::ios::binary);
    if (!a || !b) return false;
    return std::equal(std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(b));
}

std::string hiddenSibling(const char* purpose) {
    auto nonce = security::secureRandomHex(6);
    // Dot-prefixed, so a scan of the project never takes it for an addon.
    return "." + std::string("didi-setup-") + purpose + "-" +
           (nonce.isOk() ? nonce.value() : std::string("staging"));
}

}  // namespace

const char* extensionLibraryName() {
#if defined(_WIN32)
    return "didi_extension.dll";
#elif defined(__APPLE__)
    return "libdidi_extension.dylib";
#else
    return "libdidi_extension.so";
#endif
}

std::optional<std::string> addonDifference(const std::filesystem::path& source,
                                           const std::filesystem::path& target) {
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(source, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto relative = std::filesystem::relative(it->path(), source, error);
        if (error) return std::string("an unreadable file");
        if (!sameBytes(it->path(), target / relative)) return utf8(relative);
    }
    if (error) return std::string("a folder that could not be listed");
    return std::nullopt;
}

AddonInfo inspectAddon(const std::filesystem::path& directory) {
    AddonInfo info;
    info.directory = directory;
    std::error_code error;
    info.present = std::filesystem::is_regular_file(directory / "didi.gdextension", error) && !error;
    const auto library = directory / "bin" / extensionLibraryName();
    info.library_present = std::filesystem::is_regular_file(library, error) && !error;
    if (!info.library_present) return info;
    auto read = readBuildIdFromBinary(library);
    if (read.isErr()) {
        info.build_problem = read.error().message;
    } else if (!read.value().has_value()) {
        info.build_problem = utf8(library) + " carries no build id, so it predates the field";
    } else {
        info.build = read.value();
    }
    return info;
}

BundledAddonSearch findBundledAddon(const std::filesystem::path& executable_directory) {
    BundledAddonSearch search;
    if (executable_directory.empty()) return search;
    const auto parent = executable_directory.parent_path();
    for (const auto& candidate : {executable_directory / "addons" / "didi",
                                  parent / "addons" / "didi",
                                  parent / "lib" / "didi" / "addons" / "didi",
                                  parent / "lib64" / "didi" / "addons" / "didi"}) {
        search.searched.push_back(candidate);
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / "didi.gdextension", error) && !error) {
            search.found = candidate;
            break;
        }
    }
    return search;
}

Result<AddonInstallReport> installAddon(const std::filesystem::path& project_root,
                                        const AddonInfo& source, bool replace_newer) {
    if (!source.present || !source.library_present) {
        return Error::notFound(utf8(source.directory) + " is not a complete Didi addon");
    }
    const auto addons = project_root / "addons";
    const auto target = addons / "didi";
    const auto installed = inspectAddon(target);
    std::error_code error;
    const bool target_exists = std::filesystem::exists(target, error) && !error;

    AddonInstallReport report;
    report.previous = installed.build;
    if (target_exists && installed.present && installed.library_present) {
        const auto order = installed.build && source.build
                               ? compareBuilds(*installed.build, *source.build)
                               : BuildOrder::Unknown;
        if (order == BuildOrder::Same) {
            const auto difference = addonDifference(source.directory, target);
            if (!difference) {
                report.action = AddonAction::Unchanged;
                report.detail = "res://addons/didi already holds build " + source.build->text;
                return report;
            }
            report.detail = "res://addons/didi holds build " + source.build->text +
                            " but its " + *difference + " differs from this build's, so it was replaced";
        } else if (order == BuildOrder::Older) {
            report.detail = "replaced build " + installed.build->text + " with " +
                            source.build->text + ", which is newer";
        } else if (!replace_newer) {
            report.action = AddonAction::Refused;
            report.detail = "res://addons/didi holds " + describeBuild(installed.build) + ", which is " +
                            buildOrderWord(order) + " this server's " + describeBuild(source.build) +
                            ". Nothing was changed. Run the newer server's setup, or pass "
                            "--replace-addon to install this build over it.";
            return report;
        } else {
            report.detail = "replaced " + describeBuild(installed.build) + " with " +
                            describeBuild(source.build) + " as --replace-addon asked; the one replaced was " +
                            buildOrderWord(order) + " it";
        }
    } else if (target_exists) {
        report.detail = "replaced an incomplete res://addons/didi with build " + describeBuild(source.build);
    }

    std::filesystem::create_directories(addons, error);
    if (error) return Error::internal("Cannot create " + utf8(addons) + ": " + error.message());

    const auto staging = addons / hiddenSibling("staging");
    std::filesystem::copy(source.directory, staging,
                          std::filesystem::copy_options::recursive, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return Error::internal("Copying the addon into " + utf8(addons) + " failed: " + error.message() +
                               ". The project was not changed.");
    }

    std::filesystem::path previous;
    if (target_exists) {
        previous = addons / hiddenSibling("previous");
        error = files::renameWithRetry(target, previous, files::kReplaceRetryBudget);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove_all(staging, ignored);
            report.action = AddonAction::Refused;
            report.detail = "res://addons/didi could not be moved aside (" + error.message() +
                            "). An editor that has the project open holds the extension library; "
                            "close it and run setup again. The project was not changed.";
            return report;
        }
    }
    error = files::renameWithRetry(staging, target, files::kReplaceRetryBudget);
    if (error) {
        std::error_code ignored;
        if (!previous.empty()) std::filesystem::rename(previous, target, ignored);
        std::filesystem::remove_all(staging, ignored);
        return Error::internal("Moving the new addon into place failed: " + error.message() +
                               ". The installed addon was put back.");
    }
    if (!previous.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(previous, ignored);
        if (std::filesystem::exists(previous, ignored)) {
            report.detail += ". The old copy is still at " + utf8(previous) + "; delete it once nothing holds it";
        }
    }
    report.action = target_exists ? AddonAction::Replaced : AddonAction::Installed;
    if (!target_exists) report.detail = "installed build " + describeBuild(source.build) + " into res://addons/didi";
    return report;
}

std::optional<std::vector<std::string>> parsePluginList(const std::string& literal) {
    const auto text = strings::trim(literal);
    std::string_view inner;
    if (strings::startsWith(text, "PackedStringArray(") && strings::endsWith(text, ")")) {
        inner = std::string_view(text).substr(18, text.size() - 19);
    } else if (strings::startsWith(text, "[") && strings::endsWith(text, "]")) {
        inner = std::string_view(text).substr(1, text.size() - 2);
    } else {
        return std::nullopt;
    }
    std::vector<std::string> plugins;
    size_t at = 0;
    bool expect_item = true;
    while (at < inner.size()) {
        const char c = inner[at];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++at;
        } else if (c == ',' && !expect_item) {
            expect_item = true;
            ++at;
        } else if (c == '"' && expect_item) {
            size_t end = at + 1;
            while (end < inner.size() && inner[end] != '"') end += inner[end] == '\\' ? 2 : 1;
            if (end >= inner.size()) return std::nullopt;
            const auto value = config_file::stringValue(inner.substr(at, end - at + 1));
            if (!value || !value->problem.empty()) return std::nullopt;
            plugins.push_back(value->text);
            expect_item = false;
            at = end + 1;
        } else {
            return std::nullopt;
        }
    }
    // A trailing comma is something Godot never writes; refuse to guess.
    if (expect_item && !plugins.empty()) return std::nullopt;
    return plugins;
}

std::string pluginListLiteral(const std::vector<std::string>& plugins) {
    std::string literal = "PackedStringArray(";
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (i > 0) literal += ", ";
        literal += '"';
        for (char c : plugins[i]) {
            if (c == '"' || c == '\\') literal += '\\';
            literal += c;
        }
        literal += '"';
    }
    return literal + ")";
}

Result<PluginEnableReport> enablePlugin(const std::filesystem::path& project_root) {
    auto current = offline::readProjectSetting(project_root, "editor_plugins/enabled");
    if (current.isErr()) return current.error();
    std::vector<std::string> plugins;
    PluginEnableReport report;
    if (current.value().existed) {
        report.previous_literal = current.value().literal;
        auto parsed = parsePluginList(current.value().literal);
        if (!parsed) {
            return Error::invalidArgument(
                "editor_plugins/enabled holds " + current.value().literal +
                ", which is not a list of plugin paths this can extend without losing something. "
                "Add \"" + std::string(kPluginConfigPath) + "\" to it by hand, or enable Didi under "
                "Project > Project Settings > Plugins.");
        }
        plugins = std::move(*parsed);
    }
    if (std::find(plugins.begin(), plugins.end(), kPluginConfigPath) != plugins.end()) {
        report.literal = report.previous_literal;
        return report;
    }
    plugins.push_back(kPluginConfigPath);
    report.literal = pluginListLiteral(plugins);
    auto written = offline::writeProjectSettingLiteral(project_root, "editor_plugins/enabled", report.literal);
    if (written.isErr()) return written.error();
    report.changed = true;
    return report;
}

bool pluginEnabled(const std::filesystem::path& project_root) {
    auto current = offline::readProjectSetting(project_root, "editor_plugins/enabled");
    if (current.isErr() || !current.value().existed) return false;
    const auto parsed = parsePluginList(current.value().literal);
    return parsed && std::find(parsed->begin(), parsed->end(), kPluginConfigPath) != parsed->end();
}

}  // namespace didi::setup
