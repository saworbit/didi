#pragma once

#include "didi/common/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace didi::offline {

// One signal connection to write into a scene file, which signal_connect does
// when no editor is attached (#1387). The node paths take the forms the live
// tool takes: ".", "/root/<Root>/Child", "<Root>/Child" or "Child".
struct SceneConnectionRequest {
    std::string scene_path;
    std::string emitter_node;
    std::string signal_name;
    std::string target_node;
    std::string target_method;
    int64_t flags{2};
};

struct SceneConnectionWrite {
    std::string scene_path;
    std::string from;  // scene-relative, "." for the root
    std::string to;
    std::string line;  // the [connection] line written
    // Whether the signal's arguments were matched against the method's. An
    // engine class's signals are known by name alone, so for one of those they
    // were not, and the scene loads either way.
    bool arity_checked{false};
};

// Writes the connection or refuses it. Everything it writes has to be certain,
// because the engine says nothing when it loads most bad connections: an
// emitter or target that is not in the scene is dropped, and a method the
// target does not have is connected and fails only when the signal fires;
// only a signal the emitter does not declare prints an ERROR, measured on
// 4.5.1, 4.6.2 and 4.7.2. So the emitter's signal and the target's method are
// judged through each node's GDScript, each script it extends by path or by
// class_name, and the engine class the chain ends on, and a step that does not
// resolve, such as a node inside an instanced scene, refuses the call rather
// than guessing (422 connection_not_judged).
Result<SceneConnectionWrite> connectSignalInScene(const std::filesystem::path& project_root,
                                                  const SceneConnectionRequest& request);

// The line Godot writes for a connection, which leaves flags out when they are
// CONNECT_PERSIST alone.
std::string connectionLine(const std::string& signal, const std::string& from, const std::string& to,
                           const std::string& method, int64_t flags);

} // namespace didi::offline
