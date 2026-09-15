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
//   node_path_reference  another serialized NodePath property
//   code_reference       the name or static node path used in GDScript or C#
struct ProjectImpactOptions {
    // The symbol, signal, resource path, or node path to trace. A target
    // starting with res:// or uid:// is treated as a file. A validated Godot
    // node path is treated separately from a single identifier.
    std::string target;
    // A cap so one call cannot return an unbounded list.
    size_t max_impacts{500};
};

// Errors on an empty or malformed target rather than returning an empty
// report, because "nothing depends on this" and "you asked the wrong question"
// must not look the same to a caller about to delete something.
Result<json> analyzeImpact(const std::string& root_dir, const ProjectImpactOptions& options);

// Renames a symbol everywhere Godot serializes it, and says where it did not.
//
// The serialized forms are unambiguous: a [connection] names a signal and a
// method in named attributes, and an animation track names a property in the
// segment of a NodePath after the colon. Those are exactly the references a
// text search finds but cannot explain, and exactly the ones an agent renaming
// a variable in a script forgets. They can be rewritten without guessing.
//
// GDScript references cannot. The language is dynamically typed, so
// `player.character_health` cannot be resolved to a particular class's member
// without inference this does not have, and a whole-word match may equally be a
// local variable in an unrelated file that happens to share the name. Renaming
// those would be a second source of the silent breakage this exists to prevent,
// so they are reported with their file and line and left alone; script_patch_method
// is the tool for them.
// There is deliberately no dry_run here. The mutation envelope owns previews and
// intercepts before a handler runs, so a flag on this would be dead. What that
// envelope's preview cannot show is which files are about to change, and for a
// multi-file write that is the thing worth seeing: project_analyze_impact on the
// same target lists every site, and is the step to take first.
struct ProjectRenameOptions {
    std::string target;    // the identifier to rename
    std::string new_name;  // what to call it
    size_t max_impacts{500};
};

// What renameReferences would do, without doing any of it: which files change,
// how many lines in each, and the references it will report and never rewrite.
//
// The confirmation preview calls this. project_rename_references is always
// confirmed because the preview is the only chance to see which files it is
// about to touch, and the preview had no probe, so it showed the two
// identifiers the caller had just typed and nothing else (#662). It refuses
// exactly what the real call refuses, so a preview does not mint a token for a
// call that cannot run.
//
// The scan runs again on the confirm. That is the price of a preview that
// means something, and the confirm insists on a fresh crawl anyway.
Result<json> planRenameReferences(const std::string& root_dir,
                                  const ProjectRenameOptions& options);

Result<json> renameReferences(const std::string& root_dir, const ProjectRenameOptions& options);

} // namespace didi::offline
