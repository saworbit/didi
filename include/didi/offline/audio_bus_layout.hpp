#pragma once

#include "didi/common/types.hpp"

#include <string>

namespace didi::offline {

// The audio bus layout as the project file records it.
//
// A muted bus is invisible: the game runs, nothing errors, and no sound comes
// out. An agent asked why a sound effect cannot be heard has no way to look,
// because nothing in Didi could read the bus layout at all.
//
// This is the offline half. It reads default_bus_layout.tres, which is what
// the project ships and what the editor loads at startup, so it answers the
// question without a running engine. What it cannot see is a bus a script
// muted at runtime; the live half reports that.
//
// The layout path comes from project.godot's audio/buses/default_bus_layout
// when it is set, and res://default_bus_layout.tres otherwise, which is the
// same order Godot uses.
Result<json> readAudioBusLayout(const std::string& root_dir);

} // namespace didi::offline
