#pragma once

// Where an editor's own protocol servers listen (Q11 in docs/BUILD_QUEUE.md).
//
// A Godot editor runs the GDScript language server and a Debug Adapter
// Protocol server, on the ports its editor settings name. Started with
// --lsp-port or --dap-port it listens there instead, and nothing an extension
// can call reports that: the engine consumes both options, so
// OS.get_cmdline_args() no longer holds them (measured on 4.5.1, 4.6.2 and
// 4.7.2). The process's own command line still does, so it is read here the
// way Godot's main reads it.

#include <optional>
#include <string>
#include <vector>

namespace didi::godot {

struct ProtocolPortOverrides {
    std::optional<int> language_server;
    std::optional<int> debug_adapter;
};

// This process's arguments as the operating system holds them, without the
// executable. Empty when they cannot be read.
std::vector<std::string> processArguments();

// The --lsp-port and --dap-port values in `arguments`, read as Godot's main
// reads them: an option is a whole argument, the value is the next one, the
// last occurrence wins, and nothing after `--` or `++` is an engine option.
ProtocolPortOverrides protocolPortOverrides(const std::vector<std::string>& arguments);

// The rate --fixed-fps sets, read the same way, or nothing when the game runs
// in real time. Godot treats a value below 1 as no fixed rate.
std::optional<int> fixedFpsArgument(const std::vector<std::string>& arguments);

// Whether the game's own arguments, after `--` or `++`, carry the marker a
// scenario run starts its game with, so the tree is paused before the first
// physics frame (#1208).
inline constexpr const char* kStartPausedArgument = "--didi-start-paused";
bool startPausedRequested(const std::vector<std::string>& arguments);

} // namespace didi::godot
