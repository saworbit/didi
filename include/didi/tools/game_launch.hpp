#pragma once

// Finding the game a launch just started. runtime_launch with detach: true and
// runtime_run_scenario both start a Godot and then wait for the session it
// publishes, and they have to agree about which session that is.

#include "didi/common/json.hpp"
#include "didi/runtime/session_client.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace didi::mcp {

// The server's project, spelled the way a session descriptor spells its
// project_path.
std::string launchedProjectPath();

// The session a game started at `launched_at_ms` published on this project:
// the one whose pid is `spawned_pid`, or else the first game session that
// started after the launch did. Null if none appeared by `deadline`.
json awaitLaunchedGameSession(const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                              uint64_t spawned_pid, int64_t launched_at_ms,
                              std::chrono::steady_clock::time_point deadline);

}  // namespace didi::mcp
