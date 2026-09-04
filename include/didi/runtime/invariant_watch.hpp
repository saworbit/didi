#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// Watching a running game for conditions that must stay true.
//
// An agent that generates a scene or a script can already drive a game through
// runtime_inject_input and read values back out of it, one round trip at a
// time. What it cannot do from outside is notice a condition that is only false
// for two frames, or freeze the game on the frame that broke it. Polling over
// IPC samples at whatever rate the round trip allows, and by the time a poll
// comes back the frame is gone.
//
// So this is not "eval_gdscript in a loop". It samples every frame the engine
// runs, in the engine, and stops the game on the first violation, which is what
// makes the report a reproduction rather than a description.
//
// The kinds are the ones that can be answered honestly:
//
//   performance_between   a Performance monitor inside a range, which is what
//                         a minimum frame rate is
//   expression_between    a bounded sandbox expression inside a range, which is
//                         what a bounded property is, including a vector
//                         component for a world-boundary check
//   no_engine_errors      no error-level engine output since the watch began,
//                         which is what an unhandled script error looks like
//                         from outside the script
enum class InvariantKind { performance_between, expression_between, no_engine_errors };

struct InvariantSpec {
    std::string name;
    InvariantKind kind{InvariantKind::no_engine_errors};
    // performance_between: an index into kProfilerMetrics, so the monitor enum
    // values stay in the one place the feasibility record pinned them.
    size_t metric_index{0};
    // expression_between
    std::string context_node;
    std::string expression;
    // At least one bound is required for the two range kinds. A one-sided
    // range is the common case: a minimum frame rate has no maximum, and a
    // floor fallout has no upper bound worth naming.
    std::optional<double> minimum;
    std::optional<double> maximum;
};

struct InvariantWatchRequest {
    int duration_ms{2000};
    bool pause_on_violation{true};
    std::vector<InvariantSpec> invariants;
};

// Validates a runtime.watchInvariants request against the contract. Errors
// carry code 400.
Result<InvariantWatchRequest> parseInvariantWatchRequest(const json& params);

// One frame's readings, supplied by whatever can talk to the engine.
struct InvariantReading {
    // Empty when the value could not be read this frame. A reading that never
    // arrived is never treated as a condition that held.
    std::optional<double> value;
    std::string read_error;
};

struct InvariantSample {
    // Parallel to request().invariants, in order.
    std::vector<InvariantReading> readings;
    // Error-level engine records since the watch began.
    int64_t engine_errors{0};
};

// Decides, from readings alone, whether the watch is finished and what to say
// about it. Deliberately knows nothing about Godot so the decision can be
// exercised without one.
class InvariantWatch {
public:
    explicit InvariantWatch(InvariantWatchRequest request);

    const InvariantWatchRequest& request() const { return m_request; }

    // Records one frame. Returns true when the watch is finished, either
    // because an invariant broke or because the window has elapsed.
    bool observe(int64_t elapsed_ms, const InvariantSample& sample);

    bool violated() const { return m_violation.has_value(); }
    bool elapsed(int64_t elapsed_ms) const { return elapsed_ms >= m_request.duration_ms; }
    int samples() const { return m_samples; }

    // The contract payload, before the router adds provenance. `paused` is
    // whether the game was actually stopped, which is not the same as having
    // asked for it.
    json response(bool paused) const;

private:
    struct Observed {
        int readings{0};
        double minimum{0.0};
        double maximum{0.0};
        double last{0.0};
        std::string last_read_error;
    };

    struct Violation {
        size_t index{0};
        double value{0.0};
        std::string bound;   // "minimum" or "maximum"
        double limit{0.0};
        int64_t elapsed_ms{0};
        int sample{0};
        int64_t engine_errors{0};
    };

    InvariantWatchRequest m_request;
    std::vector<Observed> m_observed;
    std::optional<Violation> m_violation;
    int m_samples{0};
    int64_t m_last_elapsed_ms{0};
};

} // namespace runtime
} // namespace didi
