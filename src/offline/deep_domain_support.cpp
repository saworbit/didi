#include "didi/offline/deep_domain_support.hpp"

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"
#include <map>
#include <algorithm>
#include <cctype>
#include <regex>
#include <fstream>
#include <sstream>
#include <optional>
#include <filesystem>
#include <set>

namespace didi::offline {

std::vector<std::string> isolatedGodotArguments(std::vector<std::string> arguments) {
    std::vector<std::string> isolated = {"--headless"};
    isolated.reserve(arguments.size() + 1);
    for (auto& argument : arguments) isolated.push_back(std::move(argument));
    return isolated;
}
namespace {

constexpr size_t kMaxDiagnostics = 1000;

std::string unquote(std::string value) {
    value = strings::trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        value = strings::replaceAll(value, "\\\"", "\"");
        value = strings::replaceAll(value, "\\\\", "\\");
    }
    return value;
}

} // namespace

json DomainDiagnostic::toJson() const {
    json value = {{"severity", severity}, {"message", message}};
    if (!code.empty()) value["code"] = code;
    if (!path.empty()) value["path"] = path;
    if (line > 0) value["line"] = line;
    if (column > 0) value["column"] = column;
    return value;
}

std::vector<DomainDiagnostic> parseMsBuildDiagnostics(const std::string& output) {
    // Roslyn emits both (line,col) and four part (startLine,startCol,endLine,
    // endCol) spans, and diagnostic codes are not always letters then digits:
    // NETSDK1004, MSB3073 and analyzer ids all turn up here.
    //
    // The code is optional. MSBuild documents the shape as
    // "[Origin:] [Subcategory] category [Code]: [Text]", and NuGet uses the
    // codeless form for the one that matters most here:
    // "NuGet.targets(196,5): warning : Unable to find a project to restore!".
    // Requiring a code dropped it, so a build that compiled nothing reported
    // zero diagnostics (#702).
    static const std::regex pattern(
        R"(^(.+)\(([0-9]+),([0-9]+)(?:,[0-9]+,[0-9]+)?\):\s*(error|warning)\s+(?:([A-Za-z][A-Za-z0-9_.-]*[0-9][A-Za-z0-9_.-]*)\s*)?:\s*(.*?)(?:\s+\[[^\]]+\])?\s*$)",
        std::regex::icase);
    std::vector<DomainDiagnostic> diagnostics;
    // The console logger prints every diagnostic twice: once as it happens and
    // once in the Summary block it appends by default, so every count was
    // doubled (#702). The two printings are the same line, byte for byte,
    // project suffix included, so the trimmed line is the identity. Keying on
    // that rather than on the parsed fields keeps the genuinely distinct pair a
    // shared source file compiled by two projects produces, which differ only
    // in that suffix. Matching on text also means no reliance on the wording of
    // "Build FAILED.", which is localised.
    std::set<std::string> seen;
    for (const auto& raw : strings::split(output, '\n')) {
        if (diagnostics.size() >= kMaxDiagnostics) break;
        std::smatch match;
        const std::string line = strings::trim(raw);
        if (!std::regex_match(line, match, pattern)) continue;
        if (!seen.insert(line).second) continue;
        std::string severity = match[4].str();
        std::transform(severity.begin(), severity.end(), severity.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        diagnostics.push_back({severity, match[5].str(), strings::trim(match[6].str()),
                               match[1].str(), std::stoi(match[2].str()), std::stoi(match[3].str())});
    }
    return diagnostics;
}

int parseMsBuildProjectOutputCount(const std::string& output) {
    // "  Game -> D:\game\bin\Debug\net8.0\Game.dll". A diagnostic can carry an
    // arrow in its message text, so the diagnostic shape is excluded first
    // rather than trusted not to collide.
    static const std::regex diagnostic_shape(
        R"(^.+\([0-9]+,[0-9]+(?:,[0-9]+,[0-9]+)?\):\s*(?:error|warning)\b.*$)", std::regex::icase);
    static const std::regex output_line(R"(^([^>]+?)\s->\s(\S.*)$)");
    std::set<std::string> projects;
    for (const auto& raw : strings::split(output, '\n')) {
        const std::string line = strings::trim(raw);
        if (line.empty()) continue;
        if (std::regex_match(line, diagnostic_shape)) continue;
        std::smatch match;
        if (!std::regex_match(line, match, output_line)) continue;
        projects.insert(strings::trim(match[1].str()) + " -> " + strings::trim(match[2].str()));
    }
    return static_cast<int>(projects.size());
}

std::vector<DomainDiagnostic> parseGodotDiagnostics(const std::string& output) {
    static const std::regex resource_pattern(
        R"(^\s*(?:ERROR|SCRIPT ERROR):\s*(?:Parse Error:\s*)?(res://[^:]+):([0-9]+)\s*(?:-|:)\s*(.+?)\s*$)",
        std::regex::icase);
    static const std::regex trailing_resource_pattern(
        R"(^\s*(?:ERROR|SCRIPT ERROR):\s*(?:Parse Error:\s*)?(.+?)\s+in\s+(res://[^:]+):([0-9]+)\s*$)",
        std::regex::icase);
    static const std::regex shader_pattern(
        R"(^\s*SHADER ERROR:\s*(.+?)\s*$)", std::regex::icase);
    // Godot 4 prints shader locations several ways: "at: (res://x.gdshader:14)",
    // "at: (:14)" and a bare "at: :14". The engine also prints its own C++ frames
    // in the same shape, so a location that names a C++ source file is not the
    // user's shader line and must not be taken as one.
    // The leading (?:\S+\s+)* skips whatever Godot puts before the location: a
    // function name, or the literal "(null)" the dummy renderer prints.
    static const std::regex shader_location_pattern(
        R"(^\s*at:\s*(?:\S+\s+)*\(?\s*([^()\s]*?)\s*:\s*([0-9]+)\s*\)?\s*$)",
        std::regex::icase);
    const auto isEngineSource = [](const std::string& path) {
        static const std::regex engine_source(R"(\.(?:cpp|cc|cxx|h|hpp|inl)$)", std::regex::icase);
        return std::regex_search(path, engine_source);
    };
    std::vector<DomainDiagnostic> diagnostics;
    for (const auto& raw : strings::split(output, '\n')) {
        if (diagnostics.size() >= kMaxDiagnostics) break;
        std::smatch match;
        if (std::regex_match(raw, match, resource_pattern)) {
            diagnostics.push_back({"error", "", strings::trim(match[3].str()), match[1].str(),
                                   std::stoi(match[2].str()), 0});
        } else if (std::regex_match(raw, match, trailing_resource_pattern)) {
            diagnostics.push_back({"error", "", strings::trim(match[1].str()), match[2].str(),
                                   std::stoi(match[3].str()), 0});
        } else if (std::regex_match(raw, match, shader_pattern)) {
            diagnostics.push_back({"error", "GODOT_SHADER", strings::trim(match[1].str()), "", 0, 0});
        } else if (!diagnostics.empty() && diagnostics.back().code == "GODOT_SHADER" &&
                   diagnostics.back().line == 0 &&
                   std::regex_match(raw, match, shader_location_pattern)) {
            const auto path = match[1].str();
            if (isEngineSource(path)) continue;
            diagnostics.back().line = std::stoi(match[2].str());
            if (!path.empty() && diagnostics.back().path.empty()) {
                diagnostics.back().path = path;
            }
        }
    }
    return diagnostics;
}

ExportPresetsFile readExportPresets(const std::string& contents) {
    // Matched against the section name the engine reads, not against the raw
    // line. `[ preset.0 ]` is the section preset.0, so an anchored whole-line
    // pattern skipped every key under it and reported a project with a working
    // export preset as a project with none (#814).
    static const std::regex preset_section(R"(^preset\.([0-9]+)$)");
    std::vector<json> presets;
    std::map<std::string, size_t> preset_of_section;
    bool malformed = false;

    // The first cause wins and the rest are not collected. A file with two
    // faults is repaired one at a time, and the first one is the one the reader
    // reaches: everything after a value the parser will not start is behind an
    // ERR_PARSE_ERROR anyway.
    std::string reason;
    std::string detail;
    int line = 0;
    const auto note = [&](const char* cause, std::string sentence, int at) {
        malformed = true;
        if (!reason.empty()) return;
        reason = cause;
        detail = std::move(sentence);
        line = at;
    };

    // `#` is not a comment in a ConfigFile. A `#` line with an `=` is a key
    // whose name carries the hash, and a `#` line without one joins forward
    // into the next line that has one, taking any section header in between
    // with it. Skipping those lines read a file whose presets are not what it
    // appears to say as a file that parsed (#812), and the key rule underneath
    // both is #813.
    const auto scanned = config_file::scan(contents);
    // A file that ends inside a value is ERR_PARSE_ERROR for the engine and
    // none of it loads, so nothing read above it describes an export.
    if (!scanned.complete) {
        // The last key read is the one whose value never closed.
        const int at = scanned.entries.empty() ? 0 : scanned.entries.back().line;
        note("truncated_value",
             std::string("export_presets.cfg ends part-way through a value") +
                 (at > 0 ? ", starting on line " + std::to_string(at) : "") +
                 ", so Godot answers ERR_PARSE_ERROR for the whole file and the editor "
                 "detects no presets at all. That is a truncated write rather than a preset "
                 "to correct, so repair the line or delete the file and export once from the "
                 "editor to write a new one.",
             at);
    }
    // Balanced is not loadable either. `export_path=)` closes every bracket it
    // opens, so counting brackets let it through and the list published a
    // preset with `)` as the path an export would write to (#823). Godot's own
    // answer to that file is `Invalid export preset name: Windows` with an
    // empty list of detected presets, on 4.5.1, 4.6.2 and 4.7.2 -- the keys
    // ahead of the bad value parse and the editor still has no preset. So a
    // value this cannot start is the whole file, not one field.
    for (const auto& entry : scanned.entries) {
        const auto problem = config_file::valueProblem(entry.value_text);
        if (problem.empty()) continue;
        // The sentence and the line were both already in hand here and both
        // were dropped. project_audit_assets publishes the pair for the same
        // failure in project.godot and in a .import.
        note("unloadable_value",
             "Godot's parser refuses the value of \"" + entry.key + "\" on line " +
                 std::to_string(entry.line) + ", because " + problem +
                 ". It answers ERR_PARSE_ERROR for the whole file, so the editor detects no "
                 "presets even though the keys ahead of this one are fine.",
             entry.line);
        break;
    }
    // A file with content but no section the engine honours is not an ini. A
    // trailing `# note` under a preset is not that, which is the difference a
    // one-token fix could not draw (#812), and an ini whose sections are all
    // something else is a project with no export presets rather than a broken
    // file (#651).
    if (scanned.headers.empty() && (!scanned.entries.empty() || scanned.trailing_key)) {
        note("no_section_header",
             "export_presets.cfg has content and no section header the engine honours, so it "
             "is not the file Godot writes. Delete it and export once from the editor to "
             "write a new one.",
             0);
    }

    for (const auto& header : scanned.headers) {
        std::smatch match;
        if (!std::regex_match(header.name, match, preset_section)) continue;
        if (preset_of_section.count(header.name) != 0) continue;
        // A run of digits longer than any real preset index would throw out of
        // std::stoi rather than parse, and a section nobody wrote is not worth
        // a crash.
        const auto digits = match[1].str();
        if (digits.size() > 9) continue;
        const int index = std::stoi(digits);
        presets.push_back({{"index", index}, {"name", ""}, {"platform", ""},
                           {"runnable", false}, {"export_filter", ""}, {"export_path", ""}});
        preset_of_section.emplace(header.name, presets.size() - 1);
    }

    for (const auto& entry : scanned.entries) {
        // A key before any section at all is a file this cannot make sense of.
        // That is also where a swallowed `[preset.N]` header leaves the keys
        // that were meant to be under it.
        if (entry.section.empty()) {
            note("key_before_section",
                 "Line " + std::to_string(entry.line) + " carries the key \"" + entry.key +
                     "\" before any section header, so nothing owns it. That is also where "
                     "a [preset.N] header swallowed by the line above it leaves its keys.",
                 entry.line);
            continue;
        }
        const auto at = preset_of_section.find(entry.section);
        // [preset.N.options] and any other section are skipped. Their keys are
        // not about a preset, so they are not evidence that the file is broken:
        // a valid ini with no preset sections used to be reported as malformed,
        // which made "this project has no export presets" an error where the
        // same fact with no file at all was a success (#651).
        if (at == preset_of_section.end()) continue;
        const auto& key = entry.key;
        const std::string value = unquote(entry.value_text);
        if (key == "name" || key == "platform" || key == "export_filter" || key == "export_path") {
            presets[at->second][key] = value;
        } else if (key == "runnable") {
            if (value != "true" && value != "false") {
                note("invalid_runnable",
                     "[" + entry.section + "] sets runnable to \"" + value + "\" on line " +
                         std::to_string(entry.line) +
                         ", and Godot writes only true or false there. Fix it in the editor's "
                         "Export dialog, which writes the value it means.",
                     entry.line);
            } else {
                presets[at->second][key] = value == "true";
            }
        }
    }

    std::set<std::string> names;
    for (const auto& preset : presets) {
        const std::string name = preset.value("name", "");
        const std::string platform = preset.value("platform", "");
        const auto index = std::to_string(preset.value("index", 0));
        if (name.empty() || platform.empty()) {
            note("incomplete_preset",
                 "[preset." + index + "] declares no " +
                     (name.empty() ? std::string("name") : std::string("platform")) +
                     ", and Godot needs both to detect a preset. Add it in the editor's "
                     "Export dialog.",
                 0);
            continue;
        }
        if (!names.insert(name).second) {
            note("duplicate_preset_name",
                 "[preset." + index + "] is named \"" + name +
                     "\", and so is an earlier preset. Godot addresses a preset by name, so "
                     "the second one cannot be reached. Rename one in the editor's Export "
                     "dialog.",
                 0);
        }
    }
    ExportPresetsFile file;
    file.section_count = presets.size();
    file.malformed = malformed;
    file.reason = std::move(reason);
    file.detail = std::move(detail);
    file.line = line;
    if (!malformed) file.presets = std::move(presets);
    return file;
}

std::vector<json> parseExportPresets(const std::string& contents) {
    return readExportPresets(contents).presets;
}


// One code, one opening, and the cause carried with it.
//
// The remedy is not the same for all six. A file that ends inside a value or
// holds a value the parser will not start is a broken write, and "fix it in the
// Export dialog" is not a remedy for it: the dialog will not open a file that
// does not parse. A duplicate name is two presets somebody made in the editor,
// and the dialog is exactly where that one is fixed. So each cause carries its
// own sentence and its own next step (#828).
std::string malformedPresetsMessage(const ExportPresetsFile& file) {
    std::string message = "export_presets.cfg is there and could not be parsed.";
    if (!file.detail.empty()) message += " " + file.detail;
    else message += " Fix it in the editor's Export dialog, or delete it to start again.";
    return message;
}

json malformedPresetsData(const ExportPresetsFile& file) {
    json data = {{"presets_file_exists", true},
                 {"declared_preset_sections", file.section_count}};
    // `reason` is the stable token a caller branches on; the sentence is for
    // the reader. A finding with no line does not publish `line: 0`, which
    // would be a line nobody can open.
    if (!file.reason.empty()) data["reason"] = file.reason;
    if (file.line > 0) data["line"] = file.line;
    return data;
}

std::optional<Error> checkExportPreset(const std::string& preset) {
    std::error_code error;
    const auto root = std::filesystem::current_path(error);
    if (error) return Error::internal("The project root could not be resolved");
    const auto path = root / "export_presets.cfg";
    if (!std::filesystem::exists(path, error) || error) {
        return Error::notFound(
            "This project has no export_presets.cfg, so it has no export presets. Add one in the "
            "editor's Export dialog.");
    }
    // The same bound the Phase 5 readers apply, so this cannot pull in a file
    // they would have refused.
    constexpr uintmax_t kMaxPresetFileBytes = 1024u * 1024u;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return Error(403, "export_presets.cfg is there and cannot be read");
    if (size > kMaxPresetFileBytes) {
        return Error::invalidArgument("export_presets.cfg exceeds the 1 MiB Phase 5 limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Error(403, "export_presets.cfg is there and cannot be read");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto file = readExportPresets(buffer.str());
    if (file.malformed) return Error(422, malformedPresetsMessage(file), malformedPresetsData(file));
    json available = json::array();
    for (const auto& item : file.presets) available.push_back(item.value("name", ""));
    const bool found = std::any_of(file.presets.begin(), file.presets.end(),
                                   [&](const json& item) {
                                       return item.value("name", "") == preset;
                                   });
    if (!found) {
        return Error(404, "Export preset not found: " + preset,
                     {{"preset", preset}, {"available_presets", std::move(available)}});
    }
    return std::nullopt;
}

} // namespace didi::offline
