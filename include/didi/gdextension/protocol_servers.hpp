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

} // namespace didi::godot
