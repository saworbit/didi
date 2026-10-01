#pragma once

// The engine-independent half of the typed object layer (Q7 in
// docs/BUILD_QUEUE.md): how a property path is spelled, which file a change
// inside a sub-resource is saved in, which writes the layer refuses, and when
// two writes in one batch cannot share an undo step. The bridge does the
// engine half; everything here is decided from strings so it can be tested
// without one.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace didi {
namespace godot {

// A property path in Godot's own grammar, the one get_indexed and set_indexed
// take: a property of the node, then a colon before each step into what that
// property holds. A slash belongs to a name, so
// "theme_override_styles/panel:bg_color" is two steps, the theme override slot
// and the StyleBox's bg_color.
inline constexpr size_t kMaxPropertyPathSteps = 8;
inline constexpr size_t kMaxPropertyPathLength = 1024;

// The steps of a path, or the reason it is not one. A path with an empty step
// is refused rather than tidied: ":a", "a:" and "a::b" each mean something
// different in a NodePath, and none of them is a property.
struct PropertyPath {
    std::vector<std::string> steps;
    std::string problem;
};

[[nodiscard]] PropertyPath parsePropertyPath(const std::string& path);

// Where a change to a resource is kept when the edited scene is saved.
//
// Measured on 4.5.1, 4.6.2 and 4.7.2 by writing through a node with
// set_indexed and saving the scene (tools/vibe/probes/indexed_property_engine.py):
//
//   no path, or a sub-resource of the edited scene   saved in the scene
//   an external .tres, or a sub-resource of one      the save rewrites that file
//   a sub-resource of an instanced or inherited      the change is live, reads
//     scene                                          back, and the save drops it
//
// The last row is a write that reports success and is then lost, which is why
// it is refused. A file the editor imports, or one in a format it does not save
// from a scene (.gdshader, .gd, .glb), is refused for the same reason: nothing
// the scene save does keeps the change.
enum class ResourceHome {
    EditedScene,
    ResourceFile,
    OtherScene,
    NotSaved,
};

struct ResourceHomeVerdict {
    ResourceHome home{ResourceHome::EditedScene};
    // The file the resource belongs to, or empty when that is the edited scene.
    std::string file;
};

[[nodiscard]] ResourceHomeVerdict resourceHome(const std::string& resource_path,
                                               const std::string& edited_scene_path);

// Why the layer does not write a property, or nothing when it does. A written
// list, one reason per entry, as Q7 asks; `on_node` is false for a property of
// a sub-resource. Each entry is a property whose setter changes what the scene
// file is rather than what one of its values is.
[[nodiscard]] std::optional<std::string> excludedPropertyWrite(const std::string& property,
                                                               bool on_node);

// The tool that makes an excluded write properly, when there is one.
[[nodiscard]] std::string toolForExcludedWrite(const std::string& property);

// One write of a batch, as the overlap check sees it: the node it resolved to,
// as an identity the caller chooses, and its path's steps.
struct BatchTarget {
    std::string node;
    std::vector<std::string> steps;
};

// The first two writes that touch the same property, or one property inside
// another. Every write in a batch is checked against the scene as it stands
// before the batch, so a write inside a slot another write replaces would be
// checked against the resource that is about to leave. And Godot replays an
// action's undo steps in the order they were added, so the later write's old
// value would be restored onto whatever the earlier one's undo left.
[[nodiscard]] std::optional<std::pair<size_t, size_t>> overlappingWrites(
    const std::vector<BatchTarget>& targets);

// What the engine declares about the values a property takes, in the shape
// not_applied's engine_constraint already uses: the kind and Godot's own
// hint_string. Range, enum and enum suggestion say which values fit;
// resource_type says which classes a slot takes. Nothing for any other hint,
// which is an editor affordance rather than a statement about values.
[[nodiscard]] std::optional<json> declaredConstraint(int hint, const std::string& hint_string);

// The names a caller probably meant, for a step that named nothing. An exact
// match ignoring case first, then names containing the step or contained in
// it, then names sharing its first word, then the rest, each group in the
// order given. At most `limit`.
[[nodiscard]] std::vector<std::string> propertyNameCandidates(
    const std::string& missing, const std::vector<std::string>& available, size_t limit);

}  // namespace godot
}  // namespace didi
