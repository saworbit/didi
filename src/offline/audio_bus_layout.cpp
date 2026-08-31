#include "didi/offline/audio_bus_layout.hpp"

#include "didi/common/project_path.hpp"

#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace didi::offline {
namespace {

constexpr size_t kMaxBuses = 512;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string unquote(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

// project.godot names the layout when the project has moved it. Godot falls
// back to res://default_bus_layout.tres, so this does the same rather than
// reporting no layout for a project that has one.
//
// The setting is audio/buses/default_bus_layout, but project.godot is an ini
// file: the first segment is the section header and only the rest is the key.
// Matching the full name against the file finds nothing, which reads as a
// project with no buses rather than as a lookup that missed.
std::string layoutPathFrom(const std::filesystem::path& root) {
    const auto settings = readFile(root / "project.godot");
    static const std::regex sectioned(R"re(^\s*buses/default_bus_layout\s*=\s*"(res://[^"]+)")re");

    std::istringstream lines(settings);
    std::string line;
    bool in_audio = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto trimmed = strings::trim(line);
        if (!trimmed.empty() && trimmed.front() == '[') {
            in_audio = trimmed == "[audio]";
            continue;
        }
        if (!in_audio) continue;
        std::smatch match;
        if (std::regex_search(trimmed, match, sectioned)) return match[1].str();
    }
    return "res://default_bus_layout.tres";
}

struct Bus {
    std::string name;
    std::string send;
    double volume_db{0.0};
    bool mute{false};
    bool solo{false};
    bool bypass{false};
    bool seen{false};
};

} // namespace

Result<json> readAudioBusLayout(const std::string& root_dir) {
    const auto root = paths::projectPathFromUtf8(root_dir);
    const auto layout_path = layoutPathFrom(root);

    auto relative = layout_path;
    if (strings::startsWith(relative, "res://")) relative.erase(0, 6);
    const auto text = readFile(root / paths::projectPathFromUtf8(relative));
    if (text.empty()) {
        // Godot writes this file only once a project has more than the default
        // Master bus, so its absence is an answer rather than a failure.
        return json{{"layout_path", layout_path},
                    {"layout_present", false},
                    {"buses", json::array()},
                    {"bus_count", 0},
                    {"note", "The project ships no bus layout file, so Godot uses a single "
                             "Master bus at 0 dB."}};
    }

    // bus/0/name = "Master", bus/0/mute = false, and so on. Indices are not
    // guaranteed contiguous in the file, so they are collected by index and
    // emitted in order.
    static const std::regex entry(R"re(^\s*bus/(\d+)/([a-z_]+)\s*=\s*(.+?)\s*$)re");
    std::map<int, Bus> buses;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (!std::regex_match(line, match, entry)) continue;
        const int index = std::atoi(match[1].str().c_str());
        if (index < 0 || static_cast<size_t>(index) >= kMaxBuses) continue;
        const auto key = match[2].str();
        const auto value = match[3].str();

        auto& bus = buses[index];
        bus.seen = true;
        if (key == "name") bus.name = unquote(value);
        else if (key == "send") bus.send = unquote(value);
        else if (key == "volume_db") bus.volume_db = std::atof(value.c_str());
        else if (key == "mute") bus.mute = value == "true";
        else if (key == "solo") bus.solo = value == "true";
        else if (key == "bypass_fx") bus.bypass = value == "true";
    }

    json array = json::array();
    for (const auto& [index, bus] : buses) {
        if (!bus.seen) continue;
        array.push_back({{"index", index},
                         {"name", bus.name},
                         {"volume_db", bus.volume_db},
                         {"mute", bus.mute},
                         {"solo", bus.solo},
                         {"bypass_effects", bus.bypass},
                         {"send", bus.send}});
    }

    return json{
        {"layout_path", layout_path},
        {"layout_present", true},
        {"buses", std::move(array)},
        {"bus_count", buses.size()},
        // Effects are stored as sub-resources rather than as bus properties, so
        // the file says how a bus is routed but not what processes it. Saying
        // that is better than reporting an empty effect list as if it were one.
        {"note", "Effect chains are not read offline. Attach the editor for the "
                 "effects on each bus and for any change a script made at runtime."}
    };
}

} // namespace didi::offline
