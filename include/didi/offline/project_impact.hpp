#pragma once

#include "didi/common/types.hpp"

#include <string>

namespace didi::offline {

// What else changes if this changes.
//
// Renaming a variable or a signal in one script can break a scene that wires
// it, an animation track that keyframes it, or an autoload that loads it.
// Godot reports none of that until the game runs, and a lexical search does
// not report it either: the connection lives in a .tscn as an attribute, and
// the keyframe lives in a NodePath inside a quoted string.
//
// So an agent asked to rename a symbol edits the script, sees a clean search,
// and ships a project that is broken at runtime. This answers the question the
// search cannot: every place that names this thing, and how it names it.
//
// The kinds reported are the forms Godot actually writes:
//
//   ext_resource         a scene or resource naming a file
//   script_attachment    a scene attaching this script to a node
//   script_load          preload/load/Load<T> naming a file from code
//   autoload             a project.godot autoload entry naming a file
//   scene_connection     [connection] wiring this signal or this method
//   animation_track      a NodePath in a track keyframing this property
//   code_reference       the name used in GDScript or C#
struct ProjectImpactOptions {
    // The symbol, signal, or res:// path to trace. A target starting with
    // res:// or uid:// is treated as a file; anything else as a name.
    std::string target;
    // A cap so one call cannot return an unbounded list.
    size_t max_impacts{500};
};

// Errors on an empty or malformed target rather than returning an empty
// report, because "nothing depends on this" and "you asked the wrong question"
// must not look the same to a caller about to delete something.
Result<json> analyzeImpact(const std::string& root_dir, const ProjectImpactOptions& options);

} // namespace didi::offline
