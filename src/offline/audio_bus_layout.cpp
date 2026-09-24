#include "didi/offline/audio_bus_layout.hpp"

#include "didi/common/config_file_syntax.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/project_settings_file.hpp"

#include <fstream>
#include <map>
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
//
// The quotes are not the only thing to undo. The engine keeps any name it is
// given and saves it with the string escapes, so `Say "hi"` is written
// `&"Say \"hi\""` and a name holding a tab is written with `\t`. Taking the
// text between the quotes published those escapes as part of the name, which
// is #844 again one layer in (#934). The string rule is the parser's, in
// config_file::stringValue. A value the parser refuses never gets here,
// because loadFailure has already reported the layout as one Godot does not
// load; anything that is not a string is returned as written.
std::string unquote(const std::string& value) {
    if (auto decoded = config_file::stringValue(value); decoded && decoded->problem.empty()) {
        return std::move(decoded->text);
    }
    return std::string(strings::trim(value));
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

std::string layoutPathFrom(const config_file::Scan& scanned) {
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
};

// The index a `bus/<n>/...` key names, or nothing for any other key.
std::optional<int> busIndex(const std::string& key, std::string& rest) {
    if (!strings::startsWith(key, "bus/")) return std::nullopt;
    const auto slash = key.find('/', 4);
    if (slash == std::string::npos || slash == 4) return std::nullopt;
    int index = 0;
    for (size_t at = 4; at < slash; ++at) {
        if (key[at] < '0' || key[at] > '9') return std::nullopt;
        index = index * 10 + (key[at] - '0');
        if (static_cast<size_t>(index) >= kMaxBuses) return std::nullopt;
    }
    rest = key.substr(slash + 1);
    return index;
}

} // namespace

Result<json> readAudioBusLayout(const std::string& root_dir) {
    const auto root = paths::projectPathFromUtf8(root_dir);
    // A project.godot the engine refuses does not open, so no layout is loaded
    // from it and a custom path in it names nothing the game runs on (#903).
    const auto manifest = config_file::scan(readFile(root / "project.godot"));
    if (auto unloadable = refuseUnloadable(manifest, "reading the bus layout it names")) {
        return *unloadable;
    }
    const auto layout_path = layoutPathFrom(manifest);

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
                    {"layout_loads", false},
                    {"buses", json::array({defaultMasterBus()})},
                    {"bus_count", 1},
                    {"note", "No bus layout file is at layout_path, so this is the default "
                             "Godot runs the project with: one Master bus at 0 dB. Nothing "
                             "here was chosen by the project."}};
    }

    // A layout the parser refuses loads as nothing, and AudioServer keeps the
    // one Master bus it starts with. Measured on 4.5.1, 4.6.2 and 4.7.2 with a
    // good bus above a broken value and with a value left open at the end: one
    // bus either way. Reading the lines that did parse listed buses the game
    // does not have, one of them muted (#903).
    const auto scanned = config_file::scan(text);
    if (const auto failure = config_file::loadFailure(scanned)) {
        const auto where = failure->line > 0
            ? "line " + std::to_string(failure->line) + " of the file"
            : std::string("the file");
        const auto why = failure->unterminated
            ? std::string("it ends part-way through a value")
            : "the value on " + where + " does not parse: " + failure->value_reason;
        return json{{"layout_path", layout_path},
                    {"layout_present", true},
                    {"layout_loads", false},
                    {"buses", json::array({defaultMasterBus()})},
                    {"bus_count", 1},
                    {"note", "Godot does not load the layout at layout_path, because " + why +
                             ". The project runs on the default Master bus alone until the "
                             "file is repaired."}};
    }

    // bus/1/name = &"Music", bus/1/mute = false, and so on, under [resource].
    // Read through the shared scan, which knows what the engine counts as a key,
    // a comment and the end of a value.
    std::map<int, Bus> buses;
    for (const auto& entry : scanned.entries) {
        if (entry.section != "resource") continue;
        std::string key;
        const auto index = busIndex(entry.key, key);
        if (!index) continue;
        // Any key under the index makes the bus, including an effect key.
        auto& bus = buses[*index];
        const auto value = std::string(strings::trim(entry.value_text));
        if (key == "name") bus.name = unquote(value);
        else if (key == "send") bus.send = unquote(value);
        else if (key == "volume_db") bus.volume_db = std::atof(value.c_str());
        // The engine reads these through `_set` and converts whatever the value
        // parsed to, so `mute = 1` is a muted bus. Comparing to the word `true`
        // reported it as unmuted, measured on 4.5.1 and 4.7.2 (#853).
        else if (key == "mute") bus.mute = config_file::booleanize(value);
        else if (key == "solo") bus.solo = config_file::booleanize(value);
        else if (key == "bypass_fx") bus.bypass = config_file::booleanize(value);
    }

    // Godot sizes the list to the highest index the file names, so an index it
    // skips is a bus with an empty name, no send and every default. Measured on
    // 4.5.1, 4.6.2 and 4.7.2: a file naming only bus/2 is three buses, and one
    // naming only bus/1/effect/0/enabled is two (#905).
    if (!buses.empty()) {
        const int last = buses.rbegin()->first;
        for (int index = 0; index < last; ++index) buses[index];
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
        if (master.name.empty()) master.name = "Master";
    }

    json array = json::array();
    for (const auto& [index, bus] : buses) {
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
        {"layout_loads", true},
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
