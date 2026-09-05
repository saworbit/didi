#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include "didi/runtime/spatial_queries.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// A shape drawn in the editor viewport to show what a mutation would look like,
// before anything on disk changes.
//
// The point of these is that they are not in the scene. They are handed to the
// rendering server directly, so the scene tree, the scene dock and the saved
// file are all untouched and the editor never becomes dirty. Nothing here can
// be undone because nothing here was done.
enum class GhostKind { addition, translation, deletion };

// The colours the issue asked for, and the reason a caller does not have to
// pick one: cyan for something being added, yellow for something moving, red
// for something going away.
struct GhostColor {
    double red{0.0};
    double green{1.0};
    double blue{1.0};

    json toJson() const;
};

GhostColor colorForGhostKind(GhostKind kind);
const char* nameForGhostKind(GhostKind kind);

struct GhostShape {
    SpatialPoint position;
    // Full extents, not half extents: the size a person would type into the
    // inspector.
    SpatialPoint size;
    // 3D only, and only meaningful there. A 2D preview is an axis-aligned
    // rectangle.
    double rotation_degrees[3]{0.0, 0.0, 0.0};
    bool rotated{false};
    GhostKind kind{GhostKind::addition};
    GhostColor color;
    bool color_given{false};
    std::string label;

    int dimension() const { return position.dimension; }
};

constexpr size_t kMaxGhostShapesPerCall = 64;
// Previews stay up until they are cleared, which is what makes them useful to
// look at. This is the ceiling on how many can be up at once, so a session that
// forgets to clear cannot fill the viewport without limit.
constexpr size_t kMaxLiveGhostShapes = 256;

struct GhostPreviewRequest {
    std::vector<GhostShape> shapes;
    // Whether this call replaces what is already on screen. A preview usually
    // stands for one proposal, so replacing is the default; accumulating is the
    // thing a caller has to ask for.
    bool replace{true};

    int dimension() const { return shapes.empty() ? 3 : shapes.front().dimension(); }
};

struct GhostClearRequest {
    // Empty clears everything, which is the call that always works no matter
    // what left the previews behind.
    std::string preview_id;
};

// Every shape in one call shares one dimension, because a 2D rectangle and a 3D
// box are drawn by different servers into different worlds, and a call that
// silently split across both would be drawing into two places at once.
Result<GhostPreviewRequest> parseGhostPreviewRequest(const json& params);
Result<GhostClearRequest> parseGhostClearRequest(const json& params);

} // namespace runtime
} // namespace didi
