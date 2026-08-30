#pragma once

#include "didi/common/types.hpp"
#include <string>

namespace didi {
namespace godot {

json executeRuntimeBridge(const std::string& method, const json& params,
                          const std::string& session_kind);

// Calls SceneTree.quit. runtime.stop does not call this directly: it hands the
// exit code to EditorHook::requestSceneTreeQuit so the IPC response is written
// before the main loop is allowed to exit.
Result<void> quitSceneTree(int64_t exit_code);

} // namespace godot
} // namespace didi
