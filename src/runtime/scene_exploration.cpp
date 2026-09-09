#include "didi/runtime/scene_exploration.hpp"

#include <algorithm>
#include <cmath>

namespace didi {
namespace runtime {

namespace {

constexpr int kMinActionHoldMs = 16;
constexpr int kMaxActionHoldMs = 10000;
constexpr int kMinStuckMs = 100;
constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxActionBytes = 128;
constexpr size_t kMaxExpressionBytes = 512;
constexpr size_t kMaxContextBytes = 256;

Result<std::string> boundedString(const json& value, const char* field, size_t maximum) {
    if (!value.is_string()) {
        return Error::invalidArgument(std::string(field) + " must be a string");
    }
    auto text = value.get<std::string>();
    if (text.empty() || text.size() > maximum) {
        return Error::invalidArgument(std::string(field) + " must be 1 to " +
                                      std::to_string(maximum) + " bytes");
    }
    return text;
}

Result<int> boundedInt(const json& params, const char* field, int minimum, int maximum,
                       int fallback) {
    if (!params.contains(field)) return fallback;
    const auto& value = params[field];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return Error::invalidArgument(std::string(field) + " must be an integer from " +
                                      std::to_string(minimum) + " to " + std::to_string(maximum));
    }
    const auto number = value.get<int64_t>();
    if (number < minimum || number > maximum) {
        return Error::invalidArgument(std::string(field) + " must be an integer from " +
                                      std::to_string(minimum) + " to " + std::to_string(maximum));
    }
    return static_cast<int>(number);
}

Result<bool> boundedBool(const json& params, const char* field, bool fallback) {
    if (!params.contains(field)) return fallback;
    if (!params[field].is_boolean()) {
        return Error::invalidArgument(std::string(field) + " must be a boolean");
    }
    return params[field].get<bool>();
}

Result<ExplorationProbe> parseProbe(const json& value, size_t position) {
    if (!value.is_object()) {
        return Error::invalidArgument("Each probe must be an object");
    }
    ExplorationProbe probe;
    if (value.contains("name")) {
        auto name = boundedString(value["name"], "name", kMaxNameBytes);
        if (name.isErr()) return name.error();
        probe.name = name.value();
    } else {
        probe.name = "probe_" + std::to_string(position);
    }
    auto expression = boundedString(value.value("expression", json()), "expression",
                                    kMaxExpressionBytes);
    if (expression.isErr()) return expression.error();
    probe.expression = expression.value();

    if (value.contains("context_node")) {
        auto context = boundedString(value["context_node"], "context_node", kMaxContextBytes);
        if (context.isErr()) return context.error();
        probe.context_node = context.value();
    }
    return probe;
}

// A small deterministic mix, so a slot's action depends on the seed and the
// slot and on nothing else. std::mt19937 would do as well; this keeps the
// schedule identical across standard library implementations, which matters
// because a report names a run somebody else has to be able to repeat.
uint64_t mixed(uint64_t seed, uint64_t slot) {
    uint64_t value = seed ^ (slot + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31;
    return value;
}

} // namespace

Result<SceneExplorationRequest> parseSceneExplorationRequest(const json& params) {
    if (!params.is_object()) {
        return Error::invalidArgument("Scene exploration params must be an object");
    }
    SceneExplorationRequest request;

    auto duration = boundedInt(params, "duration_ms", kMinExplorationDurationMs,
                               kMaxExplorationDurationMs, request.duration_ms);
    if (duration.isErr()) return duration.error();
    request.duration_ms = duration.value();

    auto hold = boundedInt(params, "action_hold_ms", kMinActionHoldMs, kMaxActionHoldMs,
                           request.action_hold_ms);
    if (hold.isErr()) return hold.error();
    request.action_hold_ms = hold.value();

    auto stuck = boundedInt(params, "stuck_ms", kMinStuckMs, kMaxExplorationDurationMs,
                            request.stuck_ms);
    if (stuck.isErr()) return stuck.error();
    request.stuck_ms = stuck.value();

    auto pause = boundedBool(params, "pause_on_stuck", request.pause_on_stuck);
    if (pause.isErr()) return pause.error();
    request.pause_on_stuck = pause.value();

    auto stop_on_error = boundedBool(params, "stop_on_engine_error", request.stop_on_engine_error);
    if (stop_on_error.isErr()) return stop_on_error.error();
    request.stop_on_engine_error = stop_on_error.value();

    if (params.contains("movement_epsilon")) {
        const auto& value = params["movement_epsilon"];
        if (!value.is_number()) {
            return Error::invalidArgument("movement_epsilon must be a number");
        }
        const auto epsilon = value.get<double>();
        if (!std::isfinite(epsilon) || epsilon < 0.0) {
            return Error::invalidArgument("movement_epsilon must be finite and not negative");
        }
        request.movement_epsilon = epsilon;
    }

    if (params.contains("seed")) {
        const auto& value = params["seed"];
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return Error::invalidArgument("seed must be a non-negative integer");
        }
        const auto seed = value.get<int64_t>();
        if (seed < 0) return Error::invalidArgument("seed must be a non-negative integer");
        request.seed = static_cast<uint64_t>(seed);
    }

    // A run that cannot press anything is a run that watches a game nobody is
    // playing, which runtime_watch_invariants already does and says so.
    if (!params.contains("actions") || !params["actions"].is_array()) {
        return Error::invalidArgument("actions must be an array");
    }
    const auto& actions = params["actions"];
    if (actions.empty() || actions.size() > kMaxExplorationActions) {
        return Error::invalidArgument("actions must contain 1 to " +
                                      std::to_string(kMaxExplorationActions) + " entries");
    }
    for (const auto& action : actions) {
        auto name = boundedString(action, "action", kMaxActionBytes);
        if (name.isErr()) return name.error();
        if (std::find(request.actions.begin(), request.actions.end(), name.value()) !=
            request.actions.end()) {
            return Error::invalidArgument("actions must not repeat: " + name.value());
        }
        request.actions.push_back(name.value());
    }

    // Without a probe there is nothing to call stuck, and a run that reports
    // only that it pressed buttons has answered a question nobody asked.
    if (!params.contains("probes") || !params["probes"].is_array()) {
        return Error::invalidArgument("probes must be an array");
    }
    const auto& probes = params["probes"];
    if (probes.empty() || probes.size() > kMaxExplorationProbes) {
        return Error::invalidArgument("probes must contain 1 to " +
                                      std::to_string(kMaxExplorationProbes) + " entries");
    }
    for (size_t index = 0; index < probes.size(); ++index) {
        auto probe = parseProbe(probes[index], index);
        if (probe.isErr()) return probe.error();
        request.probes.push_back(std::move(probe.value()));
    }

    if (request.stuck_ms > request.duration_ms) {
        return Error::invalidArgument(
            "stuck_ms must not exceed duration_ms; a stuck interval that cannot close "
            "inside the window is one this run could never report");
    }
    return request;
}

SceneExploration::SceneExploration(SceneExplorationRequest request)
    : m_request(std::move(request)),
      m_tracked(m_request.probes.size()),
      m_slotsHeld(m_request.actions.size(), 0) {}

int64_t SceneExploration::slotAt(int64_t elapsed_ms) const {
    if (elapsed_ms < 0) return 0;
    return elapsed_ms / m_request.action_hold_ms;
}

size_t SceneExploration::actionForSlot(int64_t slot) const {
    if (m_request.actions.size() == 1) return 0;
    return static_cast<size_t>(mixed(m_request.seed, static_cast<uint64_t>(slot)) %
                               m_request.actions.size());
}

bool SceneExploration::observe(int64_t elapsed_ms, const ExplorationSample& sample) {
    if (m_stoppedOnStuck || m_stoppedOnEngineError) return true;
    m_elapsedMs = elapsed_ms;
    ++m_frames;
    m_engineErrors = sample.engine_errors;

    const auto slot = slotAt(elapsed_ms);
    const auto action = actionForSlot(slot);
    if (action < m_slotsHeld.size()) {
        // Frames, not slots. Two frames in one slot are two frames of that
        // action being held, which is what the caller wants to know about the
        // time the bot spent on it.
        ++m_slotsHeld[action];
    }

    // Movement is any probe moving. A player that slides down a wall is not
    // stuck, and requiring every probe to move would call it stuck the moment
    // one axis settled.
    bool moved = false;
    for (size_t index = 0; index < m_request.probes.size() && index < sample.readings.size();
         ++index) {
        const auto& reading = sample.readings[index];
        auto& tracked = m_tracked[index];
        if (!reading.value.has_value()) {
            // A probe that could not be read says nothing about movement. It
            // must not be counted as a value that stayed the same, because
            // that would report a broken expression as a soft lock.
            if (!reading.read_error.empty()) tracked.last_read_error = reading.read_error;
            moved = true;
            continue;
        }
        const auto value = *reading.value;
        if (!tracked.seen) {
            tracked.seen = true;
            tracked.minimum = value;
            tracked.maximum = value;
            // The first reading is not movement. There was nothing to move from.
        } else if (std::fabs(value - tracked.last) > m_request.movement_epsilon) {
            moved = true;
            tracked.moved = true;
            tracked.minimum = std::min(tracked.minimum, value);
            tracked.maximum = std::max(tracked.maximum, value);
        } else {
            tracked.minimum = std::min(tracked.minimum, value);
            tracked.maximum = std::max(tracked.maximum, value);
        }
        tracked.last = value;
        ++tracked.readings;
    }

    if (moved) {
        m_lastMovementMs = elapsed_ms;
        m_stillSince = false;
    } else if (!m_stillSince) {
        m_stillSince = true;
        // Stillness is measured from the last frame that moved, not from this
        // one, or the first still frame would restart the clock every time.
    }

    if (m_stillSince && elapsed_ms - m_lastMovementMs >= m_request.stuck_ms) {
        if (m_intervals.size() < kMaxExplorationStuckIntervals) {
            m_intervals.push_back(StuckInterval{m_lastMovementMs, elapsed_ms, slot});
        } else {
            m_intervalsTruncated = true;
        }
        // The clock restarts whether or not the run stops here, so a window
        // that keeps going reports the next interval rather than one long one.
        m_lastMovementMs = elapsed_ms;
        m_stillSince = false;
        if (m_request.pause_on_stuck) {
            m_stoppedOnStuck = true;
            return true;
        }
    }

    if (m_request.stop_on_engine_error && sample.engine_errors > 0) {
        m_stoppedOnEngineError = true;
        return true;
    }

    return elapsed_ms >= m_request.duration_ms;
}

json SceneExploration::response(bool paused) const {
    json actions = json::array();
    for (size_t index = 0; index < m_request.actions.size(); ++index) {
        actions.push_back({{"action", m_request.actions[index]},
                           {"frames_held", m_slotsHeld[index]}});
    }

    json probes = json::array();
    // A probe that never read is not a probe that saw nothing move. Field trial
    // 03 got frames, engine_errors 0, no stuck intervals and every probe at
    // readings 0, which skims as a clean exploration and was a run that
    // measured nothing at all. Naming them at the top level is what separates
    // the two.
    json unread = json::array();
    for (size_t index = 0; index < m_request.probes.size(); ++index) {
        const auto& tracked = m_tracked[index];
        if (tracked.readings == 0) unread.push_back(m_request.probes[index].name);
        json entry = {{"name", m_request.probes[index].name},
                      {"expression", m_request.probes[index].expression},
                      {"readings", tracked.readings},
                      {"moved", tracked.moved}};
        if (!m_request.probes[index].context_node.empty()) {
            entry["context_node"] = m_request.probes[index].context_node;
        }
        if (tracked.seen) {
            entry["minimum"] = tracked.minimum;
            entry["maximum"] = tracked.maximum;
            entry["last"] = tracked.last;
        }
        if (!tracked.last_read_error.empty()) {
            entry["last_read_error"] = tracked.last_read_error;
        }
        probes.push_back(std::move(entry));
    }

    json intervals = json::array();
    for (const auto& interval : m_intervals) {
        const auto action = actionForSlot(interval.slot);
        intervals.push_back({{"started_ms", interval.started_ms},
                             {"ended_ms", interval.ended_ms},
                             {"duration_ms", interval.ended_ms - interval.started_ms},
                             {"action_held", m_request.actions[action]}});
    }

    const char* stopped_reason = "duration_elapsed";
    if (m_stoppedOnStuck) stopped_reason = "stuck";
    else if (m_stoppedOnEngineError) stopped_reason = "engine_error";

    const bool measured = unread.size() < m_request.probes.size();
    json response = {{"explored_ms", m_elapsedMs},
            {"frames", m_frames},
            {"stopped_reason", stopped_reason},
            // Whether the game was actually stopped, which is not the same as
            // having asked for it.
            {"paused", paused},
            {"engine_errors", m_engineErrors},
            {"seed", m_request.seed},
            {"actions", std::move(actions)},
            {"probes", std::move(probes)},
            {"stuck_intervals", std::move(intervals)},
            {"stuck_intervals_truncated", m_intervalsTruncated},
            // Whether anything was actually sampled. False means the whole
            // window is unobserved: no probe returned a value, so no frame
            // could be still and no interval could be stuck, and every other
            // number here is about driving rather than about the game.
            {"measured", measured},
            // Said plainly, because the whole report is observations and a
            // caller reading "stuck" as "broken" would be reading in a verdict
            // that nothing here is entitled to give.
            {"verdict", "none"}};
    if (!unread.empty()) response["unread_probes"] = std::move(unread);
    return response;
}

} // namespace runtime
} // namespace didi
