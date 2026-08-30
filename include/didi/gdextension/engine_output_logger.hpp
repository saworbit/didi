#pragma once

namespace didi {
namespace godot {

// Subscribes to Godot's own output so `runtime_read_output` has something to
// read. Godot exposes this through `OS.add_logger`, which takes an instance of
// a class deriving from `Logger` -- so this is the first custom class Didi
// registers with the engine rather than merely calling into.
//
// Both calls are no-ops when the engine does not expose the class-registration
// interface, or when registration fails. A missing capture stream degrades the
// output tool to empty; it must never prevent the extension from loading.
//
// Call from the extension's SCENE-level initialize/deinitialize, on the main
// thread. The logger itself is invoked by the engine from arbitrary threads.
void installEngineOutputLogger();
void removeEngineOutputLogger();

} // namespace godot
} // namespace didi
