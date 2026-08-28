#include "didi/offline/deep_domain_support.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
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
    static const std::regex pattern(
        R"(^(.+)\(([0-9]+),([0-9]+)\):\s*(error|warning)\s+([A-Za-z]+[0-9]+):\s*(.*?)(?:\s+\[[^\]]+\])?\s*$)",
        std::regex::icase);
    std::vector<DomainDiagnostic> diagnostics;
    for (const auto& raw : strings::split(output, '\n')) {
        if (diagnostics.size() >= kMaxDiagnostics) break;
        std::smatch match;
        const std::string line = strings::trim(raw);
        if (!std::regex_match(line, match, pattern)) continue;
        std::string severity = match[4].str();
        std::transform(severity.begin(), severity.end(), severity.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        diagnostics.push_back({severity, match[5].str(), strings::trim(match[6].str()),
                               match[1].str(), std::stoi(match[2].str()), std::stoi(match[3].str())});
    }
    return diagnostics;
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
    static const std::regex shader_location_pattern(
        R"(^\s*at:\s*.*\(:([0-9]+)\)\s*$)", std::regex::icase);
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
            diagnostics.back().line = std::stoi(match[1].str());
        }
    }
    return diagnostics;
}

std::vector<json> parseExportPresets(const std::string& contents) {
    static const std::regex preset_section(R"(^\[preset\.([0-9]+)\]$)");
    static const std::regex options_section(R"(^\[preset\.([0-9]+)\.options\]$)");
    std::vector<json> presets;
    std::optional<size_t> current;
    bool in_options = false;
    bool malformed = false;

    for (const auto& raw : strings::split(contents, '\n')) {
        const std::string line = strings::trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        std::smatch match;
        if (std::regex_match(line, match, preset_section)) {
            const int index = std::stoi(match[1].str());
            presets.push_back({{"index", index}, {"name", ""}, {"platform", ""},
                               {"runnable", false}, {"export_filter", ""}, {"export_path", ""}});
            current = presets.size() - 1;
            in_options = false;
            continue;
        }
        if (std::regex_match(line, match, options_section)) {
            current.reset();
            in_options = true;
            continue;
        }
        if (!line.empty() && line.front() == '[') {
            current.reset();
            in_options = false;
            continue;
        }
        if (in_options) continue;
        const size_t equals = line.find('=');
        if (!current.has_value() || equals == std::string::npos) {
            malformed = true;
            continue;
        }
        const std::string key = strings::trim(line.substr(0, equals));
        const std::string value = unquote(line.substr(equals + 1));
        if (key == "name" || key == "platform" || key == "export_filter" || key == "export_path") {
            presets[*current][key] = value;
        } else if (key == "runnable") {
            if (value != "true" && value != "false") malformed = true;
            else presets[*current][key] = value == "true";
        }
    }

    std::set<std::string> names;
    for (const auto& preset : presets) {
        const std::string name = preset.value("name", "");
        const std::string platform = preset.value("platform", "");
        if (name.empty() || platform.empty() || !names.insert(name).second) malformed = true;
    }
    return malformed ? std::vector<json>{} : presets;
}

} // namespace didi::offline
