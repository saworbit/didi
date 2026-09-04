#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <vector>

namespace didi {
namespace runtime {

// A finite, bounded point in the attached session's world. `dimension` is 2
// or 3 and z is only meaningful for 3.
struct SpatialPoint {
    int dimension{3};
    double x{0.0};
    double y{0.0};
    double z{0.0};

    json toJson() const;
};

constexpr double kSpatialCoordinateLimit = 1000000.0;

struct RaycastRequest {
    SpatialPoint from;
    SpatialPoint to;
    int64_t collision_mask{1};
    int dimension() const { return from.dimension; }
};

struct NavPathRequest {
    SpatialPoint start_point;
    SpatialPoint end_point;
    int64_t navigation_layers{1};
    bool optimize{true};
    int dimension() const { return start_point.dimension; }
};

// A batch is exactly N of the single-ray contract above, dispatched together.
//
// The value is not only one round trip instead of N. The direct space state and
// the method binds are resolved once for the whole batch, so fifty sightlines
// cost one lookup rather than fifty, and every ray is answered against the same
// physics state rather than against fifty successive frames.
//
// Every ray in a batch shares one dimension, because a 2D and a 3D ray are
// answered by different space states, and a batch that silently split across
// two would be answering from two different worlds.
constexpr size_t kMaxRaycastBatch = 64;

struct RaycastBatchRequest {
    std::vector<RaycastRequest> rays;
    int dimension() const { return rays.empty() ? 3 : rays.front().dimension(); }
};

// Both validate against the approved Phase 7B contracts and return 400 for
// anything that does not match exactly: unknown keys, mixed dimensions,
// non-finite or out-of-range coordinates, a zero-length ray, a mask outside
// 1..2147483647.
Result<RaycastRequest> parseRaycastRequest(const json& params);
// Errors name the index of the ray that failed, because a batch rejected
// without saying which entry is wrong is a batch the caller has to bisect.
Result<RaycastBatchRequest> parseRaycastBatchRequest(const json& params);
Result<NavPathRequest> parseNavPathRequest(const json& params);

} // namespace runtime
} // namespace didi
