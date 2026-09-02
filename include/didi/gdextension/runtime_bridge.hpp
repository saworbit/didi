#pragma once

#include "didi/common/types.hpp"
#include "gdextension_interface.h"
#include <string>
#include <string_view>

namespace didi {
namespace godot {

// Caps a UTF-8 string at a byte budget without splitting a sequence, replacing
// anything malformed with '?'. Exposed because the byte bound is a published
// contract on runtime.getTree names, types and paths, and the live integration
// harness cannot measure it: PowerShell's JSON round trip inflates astral
// characters, so a UTF-8 byte count taken there describes the harness rather
// than what the server sent.
struct BoundedUtf8 {
    std::string value;
    bool truncated{false};
};

BoundedUtf8 boundUtf8(std::string_view input, size_t maximum_bytes);

json executeRuntimeBridge(const std::string& method, const json& params,
                          const std::string& session_kind);

// The live SceneTree and its root Window, for the editor and a game alike.
// Spatial queries read the root viewport's existing worlds through these.
Result<GDExtensionObjectPtr> liveSceneTree();
Result<GDExtensionObjectPtr> liveSceneTreeRoot(GDExtensionObjectPtr tree);

// Calls SceneTree.quit. runtime.stop does not call this directly: it hands the
// exit code to EditorHook::requestSceneTreeQuit so the IPC response is written
// before the main loop is allowed to exit.
Result<void> quitSceneTree(int64_t exit_code);

} // namespace godot
} // namespace didi
