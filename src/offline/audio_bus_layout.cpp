#include "didi/offline/audio_bus_layout.hpp"

#include "didi/common/config_file_syntax.hpp"
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

// A quoted string, or the StringName literal Godot writes beside it.
//
// `default_bus_layout.tres` carries `bus/1/name = &"Music"` rather than
// `"Music"`, and an empty send is `&""`. Stripping the quotes alone left the
// `&` and the quotes attached, so every name this published was one no tool
// accepts: `audio_configure_bus` resolves a name through
// `AudioServer.get_bus_index` and answers 404 for it, and
// `AudioStreamPlayer.bus` silently keeps Master when the name is unknown
// (#844). Measured on 4.5.1 and 4.7.2, against layouts the engine saved.
std::string unquote(const std::string& value) {
    std::string text = value;
    if (!text.empty() && text.front() == '&') text.erase(0, 1);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

// Where Godot is told to find the bus layout, or the default it uses when the
// project says nothing.
//
// The setting is audio/buses/default_bus_layout. project.godot is a ConfigFile,
// so the first segment is the section and only the rest is the key.
//
// Read through the shared scan, because a header and a key are not the text on
// the line. `[ audio ]` is the audio section to the engine and
// `buses / default_bus_layout` is the same key, and a reader that compared
// whole lines saw neither: measured on 4.5.1, 4.6.2 and 4.7.2, each spelling on
// its own turned a project with a three bus layout into bus_count 0 with a note
// saying it ships no layout file (#836). That is the rule #809, #811 and #814
// taught the other readers, and this was the settings reader they never
// reached.
constexpr const char* kDefaultLayoutPath = "res://default_bus_layout.tres";

std::string layoutPathFrom(const std::filesystem::path& root) {
    const auto scanned = config_file::scan(readFile(root / "project.godot"));
    std::string path = kDefaultLayoutPath;
    for (const auto& entry : scanned.entries) {
        if (entry.section != "audio" || entry.key != "buses/default_bus_layout") continue;
        const auto value = unquote(std::string(strings::trim(entry.value_text)));
        // A value that is not a project path is left to the default, which is
        // what the engine falls back to when it cannot load what it was given.
        // No break: a key the file declares twice is the last one to the
        // engine, so it is the last one here.
        if (strings::startsWith(value, "res://")) path = value;
    }
    return path;
}

// The bus every Godot project has before anyone adds one.
//
// Measured on 4.5.1, 4.6.2 and 4.7.2, in a project with no layout file and
// again in a project naming a layout that is not there: AudioServer reports one
// bus, Master, at 0 dB, with no send, no mute, no solo, no bypass and no
// effects. Both cases run on this, which is why the absence of the file is not
// reported as the absence of audio (#837).
json defaultMasterBus() {
    return json{{"index", 0},
                {"name", "Master"},
                {"volume_db", 0.0},
                {"mute", false},
                {"solo", false},
                {"bypass_effects", false},
                {"send", ""}};
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
        // Master bus, so its absence is an answer rather than a failure. The
        // answer is the bus the engine gives that project, not no buses at all.
        // `bus_count: 0` said the project has no audio, and the note beside it
        // said in prose that it has one, so the two halves of one payload
        // disagreed and the machine readable half was the wrong one (#837).
        return json{{"layout_path", layout_path},
                    {"layout_present", false},
                    {"buses", json::array({defaultMasterBus()})},
                    {"bus_count", 1},
                    {"note", "No bus layout file is at layout_path, so this is the default "
                             "Godot runs the project with: one Master bus at 0 dB. Nothing "
                             "here was chosen by the project."}};
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

    // Bus 0 is Master, and the file is usually silent about it.
    //
    // The writer compares each `bus/0/<key>` against Master's default and skips
    // what matches, and Master's defaults are the whole of it: named Master,
    // no send, 0 dB, nothing muted, soloed or bypassed. So a layout whose
    // Master has never been touched starts at `bus/1`, and reading only what
    // the file declares reported two buses for a three bus project. Mute
    // Master and the file carries `bus/0/mute = true` and still no name, so
    // the bus arrived with an empty one. A project whose only bus is Master
    // writes an empty `[resource]` block, and loading that back gives one bus
    // named Master, so the answer there is one rather than none.
    //
    // Master cannot be renamed: `AudioServer.set_bus_name(0, "x")` is ignored
    // and the name stays Master. All of it measured on 4.5.1 and 4.7.2 (#844).
    //
    // Only index 0 needs this. Every other bus in every layout the engine wrote
    // carried all six of its keys, including one added and left untouched.
    {
        auto& master = buses[0];
        master.seen = true;
        if (master.name.empty()) master.name = "Master";
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
