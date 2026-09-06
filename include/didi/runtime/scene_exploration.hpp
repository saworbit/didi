#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// Driving a running game for a while and reporting what happened.
//
// `runtime_inject_input` already presses a button and `runtime_watch_invariants`
// already answers "did this condition hold". What neither does is keep pressing
// for a while and notice that the game stopped responding to it. Every injected
// event is its own IPC round trip, so an agent driving from outside presses at
// whatever rate the transport allows and looks between presses, which is not a
// playthrough and cannot see a soft lock that resolves in half a second.
//
// So the loop runs in the engine, at frame rate, for a bounded window. It holds
// actions the caller named, samples probes the caller named, and reports where
// those values went and where they stopped going anywhere.
//
// Input actions, not direct movement. An arbitrary project moves its player
// with its own controller, and nothing outside that controller knows how. The
// only general way to move a character that is not this tool's to move is to
// press the project's own InputMap actions and let its own code run.
//
// What this deliberately does not do is judge. It does not know whether a level
// is beatable, whether a stuck interval is a bug or a cutscene, or what the
// player was meant to do next. It reports the intervals in which nothing it
// pressed changed anything, and leaves the reading of them to the caller.

// One value watched while the bot drives. The same bounded sandbox expression
// `runtime_watch_invariants` takes, for the same reason: the caller names what
// counts as movement in their project, because this cannot know.
struct ExplorationProbe {
    std::string name;
    std::string context_node;
    std::string expression;
};

constexpr size_t kMaxExplorationActions = 8;
constexpr size_t kMaxExplorationProbes = 4;
constexpr size_t kMaxExplorationStuckIntervals = 16;
constexpr int kMinExplorationDurationMs = 250;
constexpr int kMaxExplorationDurationMs = 60000;

struct SceneExplorationRequest {
    int duration_ms{5000};
    // How long one action is held before the schedule moves to the next.
    int action_hold_ms{250};
    // How long every probe has to stay still before that counts as stuck.
    int stuck_ms{3000};
    // What counts as a value having moved. A float position never repeats
    // exactly, so "changed at all" would report a game that is standing still
    // as a game that is moving.
    double movement_epsilon{0.001};
    bool pause_on_stuck{true};
    bool stop_on_engine_error{true};
    // The action schedule is drawn from this, so a report names a run that can
    // be made again.
    uint64_t seed{1};
    std::vector<std::string> actions;
    std::vector<ExplorationProbe> probes;
};

// Validates a runtime.exploreScene request against the contract. Errors carry
// code 400.
Result<SceneExplorationRequest> parseSceneExplorationRequest(const json& params);

struct ExplorationReading {
    // Empty when the value could not be read this frame. A probe that never
    // arrived is never treated as a probe that did not move.
    std::optional<double> value;
    std::string read_error;
};

struct ExplorationSample {
    // Parallel to request().probes, in order.
    std::vector<ExplorationReading> readings;
    // Error-level engine records since the run began.
    int64_t engine_errors{0};
};

// Decides, from readings alone, what the run saw and when it should stop.
// Deliberately knows nothing about Godot, so the decision can be exercised
// without one.
class SceneExploration {
public:
    explicit SceneExploration(SceneExplorationRequest request);

    const SceneExplorationRequest& request() const { return m_request; }

    // The schedule. Which slot the window is in, and which action that slot
    // holds. Both are pure functions of the seed, so the same request drives
    // the same sequence on every engine and every run.
    int64_t slotAt(int64_t elapsed_ms) const;
    size_t actionForSlot(int64_t slot) const;

    // Records one frame. Returns true when the run is finished, because the
    // window elapsed, because a stuck interval closed and the caller asked to
    // stop on one, or because the engine reported an error and the caller
    // asked to stop on that.
    bool observe(int64_t elapsed_ms, const ExplorationSample& sample);

    bool stuck() const { return !m_intervals.empty(); }
    int frames() const { return m_frames; }

    // The contract payload, before the router adds provenance. `paused` is
    // whether the game was actually stopped, which is not the same as having
    // asked for it.
    json response(bool paused) const;

private:
    struct Tracked {
        int readings{0};
        double minimum{0.0};
        double maximum{0.0};
        double last{0.0};
        bool seen{false};
        bool moved{false};
        std::string last_read_error;
    };

    struct StuckInterval {
        int64_t started_ms{0};
        int64_t ended_ms{0};
        int64_t slot{0};
    };

    SceneExplorationRequest m_request;
    std::vector<Tracked> m_tracked;
    std::vector<int64_t> m_slotsHeld;
    std::vector<StuckInterval> m_intervals;
    bool m_intervalsTruncated{false};
    // When every probe was last seen to move, which is where a stuck interval
    // is measured from.
    int64_t m_lastMovementMs{0};
    bool m_stillSince{false};
    int64_t m_elapsedMs{0};
    int64_t m_engineErrors{0};
    int m_frames{0};
    bool m_stoppedOnStuck{false};
    bool m_stoppedOnEngineError{false};
};

} // namespace runtime
} // namespace didi
