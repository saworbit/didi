#pragma once

#include <optional>
#include <string>
#include <vector>
#include "didi/common/types.hpp"
#include "didi/common/json.hpp"

namespace didi {
namespace offline {

// What GODOT_BIN and GODOT_PATH resolved to, and what was discarded on the way.
//
// A GODOT_BIN that is set and cannot be used was dropped in silence, resolution
// fell through to the known locations, and script_check_syntax reported that
// fallthrough as engine_executable -- so the one field that could have shown a
// user their variable was ignored named something they never set (#656).
//
// On macOS the thing called Godot is /Applications/Godot.app, a directory, and
// the executable is three levels inside it, so the obvious value to set is
// exactly the one that gets discarded. ADMIN_GUIDE tells people to set this
// variable when "installations use another name or layout", which is precisely
// the population that will set it wrong.
struct GodotExecutableResolution {
    std::string executable;             // what will actually be run
    std::string configured;             // what GODOT_BIN held, empty when unset
    std::string configured_rejected;    // why it was not used, empty when it was
};

GodotExecutableResolution resolveGodotExecutableDetailed();

std::string resolveGodotExecutable();

// The engine that answered, read out of what it printed.
//
// Every Godot process announces itself first: "Godot Engine
// v4.5.1.stable.official.f62fdbde1 - https://godotengine.org". The two tools
// that spawn one to answer "will the engine accept this?" said nothing about
// which engine that was, while resolveGodotExecutable picks newest-first from a
// hardcoded list, so a 4.5 project could be answered about by 4.7 (#617).
//
// Returns the banner without its build hash, in the spelling
// script_reflect_class already uses for api_version -- "Godot Engine
// v4.5.1.stable.official" -- or empty when the output carries no banner.
[[nodiscard]] std::string engineVersionFromOutput(const std::string& output);

#if defined(_WIN32)
namespace detail {

struct WindowsProcessCommand {
    std::wstring application_name;
    std::wstring command_line;
};

std::wstring trustedWindowsCommandInterpreter();
std::optional<WindowsProcessCommand> makeWindowsProcessCommand(
    const std::string& executable,
    const std::vector<std::string>& arguments);

} // namespace detail
#endif

struct TestSessionLog {
    std::string level; // "INFO", "WARN", "ERROR", "SCRIPT_ERROR"
    std::string message;
    std::string timestamp;

    json toJson() const {
        return {
            {"level", level},
            {"message", message},
            {"timestamp", timestamp}
        };
    }
};

struct TestSessionResult {
    bool success{true};
    int exit_code{0};
    double duration_seconds{0.0};
    std::vector<TestSessionLog> logs;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::string summary;
    // Which engine ran the project.
    //
    // This is the tool whose whole answer is "here is what happened when your
    // project ran", and which build ran it is part of that. It appeared only
    // because Godot prints its own banner into the captured logs, so a caller
    // had to string-match the game's stdout to find out that a 4.5 project had
    // been run by 4.7 (#687). Its two siblings have published it since #617.
    std::string engine_executable;
    std::string engine_version;

    json toJson() const {
        json log_arr = json::array();
        for (const auto& l : logs) log_arr.push_back(l.toJson());

        return {
            {"success", success},
            {"exit_code", exit_code},
            {"duration_seconds", duration_seconds},
            {"logs", log_arr},
            {"errors", errors},
            {"warnings", warnings},
            {"summary", summary},
            {"engine_executable",
             engine_executable.empty() ? json(nullptr) : json(engine_executable)},
            {"engine_version", engine_version.empty() ? json(nullptr) : json(engine_version)}
        };
    }
};

class TestRunner {
public:
    static TestSessionResult runSession(const std::string& scene_path,
                                        int timeout_seconds = 10,
                                        bool headless = true,
                                        bool break_on_error = true,
                                        const std::vector<std::string>& extra_args = {});
};

} // namespace offline
} // namespace didi
