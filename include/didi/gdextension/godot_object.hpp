#pragma once

#include "didi/gdextension/gdextension_interface.h"

namespace didi {
namespace godot {

// Constructs a Godot object and finishes constructing it, or fails.
//
// `GodotApi::classdb_construct_object` is bound to the interface entry point
// `classdb_construct_object2`, which the engine implements as
// `ClassDB::instantiate_without_postinitialization`. The name is the whole
// story: the object comes back before NOTIFICATION_POSTINITIALIZE has been
// sent. Godot's own `memnew` path sends it for built-in classes, so a class
// that does real work there, a themed Control resolving theme items being the
// case that found this, is otherwise handed back half-built. Constructing a
// Label that way segfaults the editor.
//
// Every construction goes through here so a new call site cannot reintroduce
// the omission, and so no copy of it can drift: the bridge, the expression
// sandbox and the engine output logger each had one, and two of them handed
// the half-built object back when the notification could not be sent (#1166).
// `classdb_construct_object3` carries the same requirement, so this stays
// correct across that migration.
//
// Never call it from inside a class's own create_instance_func: the engine
// sends the notification itself once that returns.
GDExtensionObjectPtr constructObject(GDExtensionConstStringNamePtr class_name);

} // namespace godot
} // namespace didi
