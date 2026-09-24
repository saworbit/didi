#include "didi/offline/project_settings_file.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/project_file_lock.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

namespace didi::offline {
namespace {

std::string escapeSettingString(const std::string& text) {
    std::string out;
    for (char character : text) {
        if (character == '\\') out += "\\\\";
        else if (character == '"') out += "\\\"";
        else if (character == '\n') out += "\\n";
        else if (character == '\r') out += "\\r";
        else if (character == '\t') out += "\\t";
        else out += character;
    }
    return out;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::string settingName(const std::string& section, const std::string& key) {
    if (section.empty()) return key;
    return section + "/" + key;
}

std::string settingName(const config_file::Entry& entry) {
    return settingName(entry.section, entry.key);
}

Result<std::string> readWholeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return Error::notFound("project.godot cannot be opened for reading");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) return Error::internal("project.godot could not be read");
    return contents.str();
}

} // namespace

// A project.godot Godot will not load is ERR_PARSE_ERROR and the project does
// not open at all: `--headless --path` falls through to the project manager and
// ConfigFile.load answers 43. The trap is that load() still hands back the
// sections it managed to read, so a partial parse looks like a parse, and a
// reader that answered out of it described a project that does not run (#817,
// #820). `config_file::loadFailure` is where that decision lives; this is the
// wording for every call that would otherwise read or write a project nobody
// can open, here and in the bus layout reader (#903).
std::optional<Error> refuseUnloadable(const config_file::Scan& scanned, const char* verb) {
    const auto failure = config_file::loadFailure(scanned);
    if (!failure) return std::nullopt;
    if (failure->unterminated) {
        return Error(409, std::string("project.godot ends part-way through a value, which Godot "
                                      "answers with ERR_PARSE_ERROR: the project does not open "
                                      "and none of the settings in the file are what it runs on. "
                                      "Repair the unterminated value before ") +
                              verb + ".");
    }
    return Error(409, "project.godot line " + std::to_string(failure->line) + " sets " +
                          settingName(failure->section, failure->key) +
                          " to a value Godot's parser refuses, because " + failure->value_reason +
                          ". The engine answers ERR_PARSE_ERROR for the whole file, "
                          "so the project does not open and none of the settings in it are "
                          "what it runs on. Repair the value before " + verb + ".");
}

Result<std::string> settingLiteral(const json& value, int depth) {
    if (depth > 16) {
        return Error::invalidArgument("JSON value is nested more than 16 levels deep");
    }
    if (value.is_null()) return std::string("null");
    if (value.is_boolean()) return std::string(value.get<bool>() ? "true" : "false");
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return Error::invalidArgument("Unsigned integer is outside Godot Variant int range");
        }
        return std::to_string(static_cast<int64_t>(number));
    }
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) return Error::invalidArgument("JSON real must be finite");
        // Godot's text writer always shows a real as a real. Dropping the
        // fractional part would write an int, and the setting would come back
        // a different Variant type than the live path stores.
        auto text = value.dump();
        if (text.find_first_of(".eE") == std::string::npos) text += ".0";
        return text;
    }
    if (value.is_string()) return "\"" + escapeSettingString(value.get<std::string>()) + "\"";
    if (value.is_array()) {
        std::ostringstream out;
        out << "[";
        bool first = true;
        for (const auto& element : value) {
            auto rendered = settingLiteral(element, depth + 1);
            if (rendered.isErr()) return rendered.error();
            if (!first) out << ", ";
            first = false;
            out << rendered.value();
        }
        out << "]";
        return out.str();
    }
    if (value.is_object()) {
        std::ostringstream out;
        out << "{";
        bool first = true;
        for (auto entry = value.begin(); entry != value.end(); ++entry) {
            auto rendered = settingLiteral(entry.value(), depth + 1);
            if (rendered.isErr()) return rendered.error();
            if (!first) out << ", ";
            first = false;
            out << "\"" << escapeSettingString(entry.key()) << "\": " << rendered.value();
        }
        out << "}";
        return out.str();
    }
    return Error::invalidArgument("JSON value cannot be converted to a supported Godot Variant");
}

Result<std::vector<ProjectAutoload>> readProjectAutoloads(
    const std::filesystem::path& project_root) {
    auto contents = readWholeFile(project_root / "project.godot");
    if (contents.isErr()) return contents.error();

    const auto scanned = config_file::scan(contents.value());
    if (auto unloadable = refuseUnloadable(scanned, "listing the autoloads in it")) {
        return *unloadable;
    }

    // The last spelling of a key wins, which is the engine's rule for a
    // duplicated setting, so this collects rather than pushes and keeps reading
    // to the end.
    std::map<std::string, ProjectAutoload> found;
    for (const auto& entry : scanned.entries) {
        if (entry.section != "autoload" || entry.key.empty()) continue;
        ProjectAutoload autoload;
        autoload.name = entry.key;
        // The string as the parser reads it, escapes undone (#934). The file
        // has loaded by this point, so the parser accepted every string in it.
        std::string value = strings::trim(entry.value_text);
        if (auto decoded = config_file::stringValue(value); decoded && decoded->problem.empty()) {
            value = std::move(decoded->text);
        }
        // A leading `*` is how the engine spells "enters the tree as a
        // singleton", and it is part of the stored value rather than part of
        // the path.
        autoload.singleton = !value.empty() && value.front() == '*';
        autoload.path = autoload.singleton ? value.substr(1) : value;
        found[autoload.name] = std::move(autoload);
    }

    std::vector<ProjectAutoload> autoloads;
    autoloads.reserve(found.size());
    for (auto& [name, autoload] : found) autoloads.push_back(std::move(autoload));
    return autoloads;
}

Result<ProjectSettingRead> readProjectSetting(const std::filesystem::path& project_root,
                                              const std::string& setting) {
    if (setting.empty() || setting.find('/') == std::string::npos) {
        return Error::invalidArgument("setting must be a slash-delimited ProjectSettings name");
    }
    const auto slash = setting.find('/');
    const auto section = setting.substr(0, slash);
    const auto key = setting.substr(slash + 1);

    auto contents = readWholeFile(project_root / "project.godot");
    if (contents.isErr()) return contents.error();

    ProjectSettingRead report;
    report.setting = setting;
    // The key the engine registers, not the text before the first `=`. Godot
    // drops the whitespace inside a key and joins a line that has no `=`
    // forward into the next line that does, so `config / name` is this setting
    // and a `# note` above it is not (#813). The last spelling in the file
    // wins, which is why this keeps reading to the end.
    const auto scanned = config_file::scan(contents.value());
    if (auto unloadable = refuseUnloadable(scanned, "reading a setting out of it")) {
        return *unloadable;
    }
    for (const auto& entry : scanned.entries) {
        if (entry.section != section || entry.key != key) continue;
        report.existed = true;
        report.literal = entry.value_text;
    }
    return report;
}

Result<ProjectSettingWrite> writeProjectSetting(const std::filesystem::path& project_root,
                                                const std::string& setting,
                                                const json& value,
                                                bool remove) {
    // The same name rules the live writer applies, so a name refused with an
    // editor attached is refused without one.
    if (setting.empty() || setting.front() == '/' || setting.back() == '/' ||
        setting.find('/') == std::string::npos || setting.find("//") != std::string::npos ||
        setting.find('\\') != std::string::npos) {
        return Error::invalidArgument("setting must be a non-empty slash-delimited ProjectSettings name");
    }
    if (setting.rfind("autoload/", 0) == 0 || setting.rfind("input/", 0) == 0) {
        return Error::invalidArgument("Use the typed autoload or InputMap tools for this setting namespace");
    }
    const auto slash = setting.find('/');
    ProjectSettingWrite report;
    report.setting = setting;
    report.section = setting.substr(0, slash);
    report.key = setting.substr(slash + 1);
    report.removed = remove;

    if (!remove) {
        auto literal = settingLiteral(value);
        if (literal.isErr()) return literal.error();
        report.literal = literal.value();
    }

    // Held from the read to the write, or a second server's write lands between
    // them and this one replaces it (#929).
    auto lock = lockProjectFile(project_root, "project.godot");
    if (lock.isErr()) return lock.error();

    const auto path = project_root / "project.godot";
    auto contents = readWholeFile(path);
    if (contents.isErr()) return contents.error();

    // Both the headers and the keys come from the engine's own rule. A header
    // swallowed by the line above it is not a header, and a key built by
    // joining lines is not this setting, so neither is somewhere to write
    // (#813).
    const auto scanned = config_file::scan(contents.value());
    // Rewriting one line of a file the engine refuses to parse leaves it
    // exactly as unloadable and reports success, so this refuses instead --
    // whether the file ends inside a value (#817) or is balanced and still
    // ERR_PARSE_ERROR (#820).
    if (auto unloadable = refuseUnloadable(scanned, "writing to it")) return *unloadable;

    // Godot writes LF. A file that arrived with CRLF keeps it, because
    // rewriting every line ending of a file to change one setting is a diff
    // nobody asked for.
    const bool crlf = contents.value().find("\r\n") != std::string::npos;
    const std::string newline = crlf ? "\r\n" : "\n";

    std::vector<std::string> lines;
    {
        std::istringstream stream(contents.value());
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    size_t assignment = lines.size();
    size_t assignment_end = lines.size();
    size_t section_end = lines.size();
    bool section_seen = false;
    for (const auto& header : scanned.headers) {
        const auto index = static_cast<size_t>(header.line - 1);
        if (section_seen && section_end == lines.size()) section_end = index;
        if (header.name == report.section) section_seen = true;
    }
    const config_file::Entry* target = nullptr;
    for (const auto& entry : scanned.entries) {
        if (entry.section != report.section || entry.key != report.key) continue;
        // The last spelling in the file wins, which is the engine's own rule.
        target = &entry;
    }
    if (target != nullptr) {
        assignment = static_cast<size_t>(target->line - 1);
        // A value can span lines. Replacing only the line holding the `=` left
        // the rest of an [input]-shaped value behind, and those leftover lines
        // join forward into whatever key comes next. Clamped for safety; a
        // value the file never closes is refused above rather than edited.
        assignment_end = std::min(static_cast<size_t>(target->value_end_line), lines.size());
        report.existed = true;
        report.previous_literal = target->value_text;

        // This works in whole lines, which is right for a value spread over
        // four of them and wrong when a line holds more than one key. `a=1
        // b=2` is two settings the engine reads and registers, and rewriting
        // the line to change one of them deleted the other while the response
        // stayed accurate about the one it was asked for (#821).
        //
        // Refusing is the answer rather than rewriting only the span the value
        // occupies, because Godot's own writer puts one key per line: no file
        // the editor produced can be in this state, and the caller who
        // hand-edited it can split the line in less time than a partial-line
        // rewrite would take to get right.
        const int first_line = target->key_line;
        const int last_line = target->value_end_line;
        for (const auto& other : scanned.entries) {
            if (&other == target) continue;
            if (other.key_line > last_line || other.value_end_line < first_line) continue;
            return Error(409, "project.godot line " + std::to_string(other.line) + " holds " +
                                  settingName(other) + " as well as " + setting +
                                  ", and this writes whole lines, so changing one would delete "
                                  "the other. A value ending does not end the line for Godot: it "
                                  "carries on reading, and both keys are settings the project "
                                  "runs on. Put them on separate lines and write again.");
        }
        // The same hazard with the key rather than the value: a key Godot
        // built by joining a line that has no `=` into this one lives on more
        // lines than the rewrite replaces, so the leftover tokens would join
        // forward again and the setting written would not be the setting
        // asked for (#813 is how a project gets here).
        if (target->joined) {
            return Error(409, "Godot registers this setting by joining line " +
                                  std::to_string(target->key_line) + " into line " +
                                  std::to_string(target->line) + ", so its name is spread over "
                                  "more lines than a rewrite of the assignment replaces and the "
                                  "text above would join forward into whatever is written here. "
                                  "Move or delete line " + std::to_string(target->key_line) +
                                  " before writing to " + setting + ".");
        }
    }

    if (remove && !report.existed) {
        return Error::notFound("Project setting not found: " + setting);
    }

    if (remove) {
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(assignment),
                    lines.begin() + static_cast<std::ptrdiff_t>(assignment_end));
    } else if (report.existed) {
        lines[assignment] = report.key + "=" + report.literal;
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(assignment + 1),
                    lines.begin() + static_cast<std::ptrdiff_t>(assignment_end));
    } else if (section_seen) {
        // Land the new key at the end of its section, not at the end of the
        // file, so the section stays one block the way Godot writes it. Any
        // blank lines the section trails are kept below the new line.
        size_t insert_at = section_end;
        while (insert_at > 0 && trimmed(lines[insert_at - 1]).empty()) --insert_at;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at),
                     report.key + "=" + report.literal);
    } else {
        report.section_created = true;
        while (!lines.empty() && trimmed(lines.back()).empty()) lines.pop_back();
        lines.push_back("");
        lines.push_back("[" + report.section + "]");
        lines.push_back("");
        lines.push_back(report.key + "=" + report.literal);
    }

    std::string rewritten;
    for (const auto& line : lines) {
        rewritten += line;
        rewritten += newline;
    }

    auto written = files::writeFileAtomically(path, rewritten);
    if (written.isErr()) return written.error();
    return report;
}

} // namespace didi::offline
