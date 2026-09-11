#include "didi/offline/project_settings_file.hpp"

#include "didi/common/atomic_write.hpp"
#include "didi/common/project_path.hpp"

#include <cmath>
#include <fstream>
#include <limits>
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

// Whether this line assigns the named key, whatever spacing it uses. Godot
// writes `key=value` with no spaces, but a hand-edited file is still a file
// Didi has to update rather than duplicate a key in.
bool assignsKey(const std::string& line, const std::string& key) {
    const auto text = trimmed(line);
    if (text.size() <= key.size() || text.compare(0, key.size(), key) != 0) return false;
    const auto rest = trimmed(text.substr(key.size()));
    return !rest.empty() && rest.front() == '=';
}

std::string valueTextOf(const std::string& line) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) return {};
    return trimmed(line.substr(equals + 1));
}

bool isSectionHeader(const std::string& line) {
    const auto text = trimmed(line);
    return text.size() >= 2 && text.front() == '[' && text.back() == ']';
}

std::string sectionNameOf(const std::string& line) {
    const auto text = trimmed(line);
    return text.substr(1, text.size() - 2);
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

Result<std::string> settingLiteral(const json& value, int depth) {
    if (depth > 16) {
        return Error::invalidArgument("JSON nesting exceeds the Phase 2 limit of 16 levels");
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
    std::string current_section;
    std::istringstream stream(contents.value());
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (isSectionHeader(line)) {
            current_section = sectionNameOf(line);
            continue;
        }
        if (current_section != section) continue;
        if (!assignsKey(line, key)) continue;
        report.existed = true;
        report.literal = valueTextOf(line);
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

    const auto path = project_root / "project.godot";
    auto contents = readWholeFile(path);
    if (contents.isErr()) return contents.error();

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

    std::string current_section;
    size_t assignment = lines.size();
    size_t section_end = lines.size();
    bool section_seen = false;
    for (size_t index = 0; index < lines.size(); ++index) {
        if (isSectionHeader(lines[index])) {
            if (section_seen && section_end == lines.size()) section_end = index;
            current_section = sectionNameOf(lines[index]);
            if (current_section == report.section) section_seen = true;
            continue;
        }
        if (current_section != report.section) continue;
        if (assignsKey(lines[index], report.key)) {
            assignment = index;
            report.existed = true;
            report.previous_literal = valueTextOf(lines[index]);
        }
    }

    if (remove && !report.existed) {
        return Error::notFound("Project setting not found: " + setting);
    }

    if (remove) {
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(assignment));
    } else if (report.existed) {
        lines[assignment] = report.key + "=" + report.literal;
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
