#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// The ten Performance monitors runtime_read_profiler may read, in the order
// the contract emits them. The enum values are pinned by the Phase 7
// feasibility record on Godot 4.5.1 and 4.7.2; availability is the method bind
// plus these values, never a sample being non-zero.
struct ProfilerMetricSpec {
    const char* name;
    const char* unit;
    int64_t monitor;
    const char* category;
};

constexpr std::array<ProfilerMetricSpec, 10> kProfilerMetrics = {{
    {"TIME_FPS", "fps", 0, "frame"},
    {"TIME_PROCESS", "seconds", 1, "process"},
    {"TIME_PHYSICS_PROCESS", "seconds", 2, "process"},
    {"PHYSICS_2D_ACTIVE_OBJECTS", "count", 17, "physics"},
    {"PHYSICS_2D_COLLISION_PAIRS", "count", 18, "physics"},
    {"PHYSICS_3D_ACTIVE_OBJECTS", "count", 20, "physics"},
    {"PHYSICS_3D_COLLISION_PAIRS", "count", 21, "physics"},
    {"RENDER_TOTAL_OBJECTS_IN_FRAME", "count", 11, "render"},
    {"RENDER_TOTAL_PRIMITIVES_IN_FRAME", "count", 12, "render"},
    {"RENDER_TOTAL_DRAW_CALLS_IN_FRAME", "count", 13, "render"},
}};

struct ProfilerRequest {
    int duration_ms{1000};
    int sample_count{30};
    // Indices into kProfilerMetrics, ascending, so output order never depends
    // on the order categories were requested in.
    std::vector<size_t> metric_indices;
};

// Validates a runtime.readProfiler request against the approved contract.
// Errors carry code 400.
Result<ProfilerRequest> parseProfilerRequest(const json& params);

// Aggregates samples without keeping raw history. Owned by the main-thread
// pump; every call happens on the Godot main thread.
class ProfilerCollector {
public:
    explicit ProfilerCollector(ProfilerRequest request);

    const ProfilerRequest& request() const { return m_request; }

    // Monitor enum values to read, one per requested metric, in output order.
    std::vector<int64_t> monitors() const;

    // Elapsed offset the next sample is due at, or -1 once complete.
    int64_t nextOffsetMs() const;

    // True while at least one offset is due at elapsed_ms.
    bool due(int64_t elapsed_ms) const;

    // Records one engine reading for every offset that is due at elapsed_ms.
    // `values` is one reading per requested metric in output order. Returns
    // true when the collection is complete after this observation.
    bool observe(int64_t elapsed_ms, const std::vector<double>& values);

    bool started() const { return m_collected > 0; }
    bool complete() const { return m_collected >= m_request.sample_count; }
    int samplesCollected() const { return m_collected; }

    // The contract success payload, before the router adds provenance.
    json response() const;

private:
    struct MetricStats {
        int valid{0};
        int invalid{0};
        double min{0.0};
        double max{0.0};
        double sum{0.0};
        double last{0.0};
    };

    ProfilerRequest m_request;
    std::vector<int64_t> m_offsets;
    std::vector<MetricStats> m_stats;
    int m_collected{0};
    int64_t m_last_elapsed_ms{0};
};

} // namespace runtime
} // namespace didi
