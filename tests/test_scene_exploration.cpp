#include "didi/runtime/scene_exploration.hpp"

#include <functional>
#include <set>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::ExplorationReading;
using didi::runtime::ExplorationSample;
using didi::runtime::parseSceneExplorationRequest;
using didi::runtime::SceneExploration;
using didi::runtime::SceneExplorationRequest;

json minimalParams() {
    return {{"actions", json::array({"move_left", "move_right", "jump"})},
            {"probes", json::array({{{"name", "x"},
                                     {"context_node", "/root/Main/Player"},
                                     {"expression", "position.x"}}})}};
}

SceneExplorationRequest parsed(const json& params) {
    auto request = parseSceneExplorationRequest(params);
    ASSERT_TRUE(request.isOk());
    return request.value();
}

bool rejected(const json& params) {
    return parseSceneExplorationRequest(params).isErr();
}

ExplorationSample readingOf(double value, int64_t engine_errors = 0) {
    ExplorationSample sample;
    sample.readings.push_back(ExplorationReading{value, ""});
    sample.engine_errors = engine_errors;
    return sample;
}

ExplorationSample unreadable(const std::string& message) {
    ExplorationSample sample;
    sample.readings.push_back(ExplorationReading{std::nullopt, message});
    return sample;
}

void test_request_defaults_and_refusals() {
    const auto request = parsed(minimalParams());
    ASSERT_EQ(request.duration_ms, 5000);
    ASSERT_EQ(request.action_hold_ms, 250);
    ASSERT_EQ(request.stuck_ms, 3000);
    ASSERT_TRUE(request.pause_on_stuck);
    ASSERT_TRUE(request.stop_on_engine_error);
    ASSERT_EQ(request.actions.size(), 3u);
    ASSERT_EQ(request.probes.size(), 1u);
    ASSERT_EQ(request.probes[0].name, std::string("x"));

    // A probe with no name is still identifiable in the report.
    auto unnamed = minimalParams();
    unnamed["probes"] = json::array({{{"expression", "position.y"}}});
    ASSERT_EQ(parsed(unnamed).probes[0].name, std::string("probe_0"));

    ASSERT_TRUE(rejected(json::array()));
    ASSERT_TRUE(rejected(json::object()));

    // Nothing to press, and nothing to watch. Both are runs that could not
    // report anything the caller came for.
    auto no_actions = minimalParams();
    no_actions["actions"] = json::array();
    ASSERT_TRUE(rejected(no_actions));
    auto no_probes = minimalParams();
    no_probes["probes"] = json::array();
    ASSERT_TRUE(rejected(no_probes));
    auto missing_expression = minimalParams();
    missing_expression["probes"] = json::array({{{"name", "x"}}});
    ASSERT_TRUE(rejected(missing_expression));

    // A repeated action would be pressed twice as often as the caller reads
    // the list as saying, and the per-action counts would not add up.
    auto repeated = minimalParams();
    repeated["actions"] = json::array({"jump", "jump"});
    ASSERT_TRUE(rejected(repeated));

    auto too_many = minimalParams();
    too_many["actions"] = json::array({"a", "b", "c", "d", "e", "f", "g", "h", "i"});
    ASSERT_TRUE(rejected(too_many));

    for (const char* field : {"duration_ms", "action_hold_ms", "stuck_ms"}) {
        auto out_of_range = minimalParams();
        out_of_range[field] = 0;
        ASSERT_TRUE(rejected(out_of_range));
        auto wrong_type = minimalParams();
        wrong_type[field] = "soon";
        ASSERT_TRUE(rejected(wrong_type));
    }

    auto negative_seed = minimalParams();
    negative_seed["seed"] = -1;
    ASSERT_TRUE(rejected(negative_seed));
    auto negative_epsilon = minimalParams();
    negative_epsilon["movement_epsilon"] = -0.5;
    ASSERT_TRUE(rejected(negative_epsilon));

    // A stuck window longer than the run is a condition the run could never
    // report, which is worse than refusing it.
    auto unreportable = minimalParams();
    unreportable["duration_ms"] = 1000;
    unreportable["stuck_ms"] = 2000;
    ASSERT_TRUE(rejected(unreportable));
}

void test_schedule_is_deterministic_and_uses_every_action() {
    // A report names a run. If the schedule were not a pure function of the
    // seed, the run it names could not be made again.
    auto params = minimalParams();
    params["seed"] = 4242;
    SceneExploration first{parsed(params)};
    SceneExploration second{parsed(params)};

    std::set<size_t> used;
    for (int64_t slot = 0; slot < 64; ++slot) {
        ASSERT_EQ(first.actionForSlot(slot), second.actionForSlot(slot));
        ASSERT_TRUE(first.actionForSlot(slot) < 3u);
        used.insert(first.actionForSlot(slot));
    }
    ASSERT_EQ(used.size(), 3u);

    auto other = minimalParams();
    other["seed"] = 7;
    SceneExploration different{parsed(other)};
    bool diverged = false;
    for (int64_t slot = 0; slot < 64; ++slot) {
        if (different.actionForSlot(slot) != first.actionForSlot(slot)) diverged = true;
    }
    ASSERT_TRUE(diverged);

    // Slots follow the hold, so the caller can read a stuck interval's slot
    // back to the action that was down for it.
    ASSERT_EQ(first.slotAt(0), 0);
    ASSERT_EQ(first.slotAt(249), 0);
    ASSERT_EQ(first.slotAt(250), 1);
    ASSERT_EQ(first.slotAt(1000), 4);
}

void test_a_game_that_keeps_moving_is_never_stuck() {
    auto params = minimalParams();
    params["duration_ms"] = 1000;
    params["stuck_ms"] = 400;
    SceneExploration exploration{parsed(params)};

    double position = 0.0;
    bool finished = false;
    for (int64_t elapsed = 0; elapsed <= 1000; elapsed += 100) {
        position += 1.5;
        finished = exploration.observe(elapsed, readingOf(position));
        if (finished) break;
    }
    ASSERT_TRUE(finished);
    ASSERT_TRUE(!exploration.stuck());

    const auto payload = exploration.response(false);
    ASSERT_EQ(payload["stopped_reason"], "duration_elapsed");
    ASSERT_EQ(payload["stuck_intervals"].size(), 0u);
    ASSERT_TRUE(payload["probes"][0]["moved"].get<bool>());
    ASSERT_EQ(payload["probes"][0]["last"].get<double>(), position);
    ASSERT_EQ(payload["verdict"], "none");
}

void test_a_value_that_stops_moving_is_reported_with_the_action_held() {
    auto params = minimalParams();
    params["duration_ms"] = 5000;
    params["stuck_ms"] = 1000;
    SceneExploration exploration{parsed(params)};

    // Moves for 500 ms, then stops dead.
    ASSERT_TRUE(!exploration.observe(0, readingOf(0.0)));
    ASSERT_TRUE(!exploration.observe(250, readingOf(4.0)));
    ASSERT_TRUE(!exploration.observe(500, readingOf(8.0)));
    ASSERT_TRUE(!exploration.observe(1000, readingOf(8.0)));
    ASSERT_TRUE(!exploration.observe(1250, readingOf(8.0)));
    // 500 to 1500 is the full stuck window.
    ASSERT_TRUE(exploration.observe(1500, readingOf(8.0)));

    ASSERT_TRUE(exploration.stuck());
    const auto payload = exploration.response(true);
    ASSERT_EQ(payload["stopped_reason"], "stuck");
    ASSERT_TRUE(payload["paused"].get<bool>());
    ASSERT_EQ(payload["stuck_intervals"].size(), 1u);
    const auto& interval = payload["stuck_intervals"][0];
    ASSERT_EQ(interval["started_ms"].get<int64_t>(), 500);
    ASSERT_EQ(interval["ended_ms"].get<int64_t>(), 1500);
    ASSERT_EQ(interval["duration_ms"].get<int64_t>(), 1000);
    // The report has to name what was being pressed while nothing happened,
    // or it describes a soft lock without saying what provoked it.
    ASSERT_TRUE(interval["action_held"].is_string());
    ASSERT_TRUE(!interval["action_held"].get<std::string>().empty());
}

void test_a_probe_that_cannot_be_read_is_not_a_soft_lock() {
    // The honest case. An expression that fails every frame produces no value,
    // and a value that never arrived is not a value that stayed the same.
    // Counting it as stillness would report a typo in the caller's expression
    // as a frozen game.
    auto params = minimalParams();
    params["duration_ms"] = 3000;
    params["stuck_ms"] = 500;
    SceneExploration exploration{parsed(params)};

    bool finished = false;
    for (int64_t elapsed = 0; elapsed <= 3000; elapsed += 250) {
        finished = exploration.observe(elapsed, unreadable("no property named posiiton"));
        if (finished) break;
    }
    ASSERT_TRUE(finished);
    ASSERT_TRUE(!exploration.stuck());

    const auto payload = exploration.response(false);
    ASSERT_EQ(payload["stopped_reason"], "duration_elapsed");
    ASSERT_EQ(payload["stuck_intervals"].size(), 0u);
    ASSERT_EQ(payload["probes"][0]["readings"].get<int>(), 0);
    ASSERT_EQ(payload["probes"][0]["last_read_error"], "no property named posiiton");
    ASSERT_TRUE(!payload["probes"][0]["moved"].get<bool>());
    // And the run says at the top level that it measured nothing. Everything
    // above reads as a clean exploration on its own: frames observed, no engine
    // errors, no stuck intervals, verdict none. It was a window in which
    // nothing was sampled at all, and a caller skimming has to be able to see
    // the difference without opening every probe.
    ASSERT_TRUE(!payload["measured"].get<bool>());
    ASSERT_EQ(payload["unread_probes"].size(), 1u);
    ASSERT_EQ(payload["unread_probes"][0], payload["probes"][0]["name"]);
}

// A run that read something says so, and names nothing.
void test_a_run_that_reads_its_probe_is_measured() {
    auto params = minimalParams();
    params["duration_ms"] = 1000;
    params["stuck_ms"] = 400;
    SceneExploration exploration{parsed(params)};
    for (int64_t elapsed = 0; elapsed <= 1000; elapsed += 250) {
        if (exploration.observe(elapsed, readingOf(static_cast<double>(elapsed)))) break;
    }
    const auto payload = exploration.response(false);
    ASSERT_TRUE(payload["measured"].get<bool>());
    ASSERT_TRUE(!payload.contains("unread_probes"));
}

void test_a_survey_run_records_more_than_one_interval() {
    auto params = minimalParams();
    params["duration_ms"] = 4000;
    params["stuck_ms"] = 500;
    params["pause_on_stuck"] = false;
    SceneExploration exploration{parsed(params)};

    double position = 0.0;
    bool finished = false;
    for (int64_t elapsed = 0; elapsed <= 4000; elapsed += 250) {
        // Still for the first second of every two, moving for the rest.
        if ((elapsed / 1000) % 2 == 1) position += 3.0;
        finished = exploration.observe(elapsed, readingOf(position));
        if (finished) break;
    }
    ASSERT_TRUE(finished);

    const auto payload = exploration.response(false);
    ASSERT_EQ(payload["stopped_reason"], "duration_elapsed");
    ASSERT_TRUE(payload["stuck_intervals"].size() >= 2u);
    ASSERT_TRUE(!payload["stuck_intervals_truncated"].get<bool>());
}

void test_an_engine_error_stops_the_run_and_is_counted() {
    auto params = minimalParams();
    params["duration_ms"] = 4000;
    params["stuck_ms"] = 3000;
    SceneExploration exploration{parsed(params)};

    ASSERT_TRUE(!exploration.observe(0, readingOf(0.0)));
    ASSERT_TRUE(!exploration.observe(250, readingOf(2.0)));
    ASSERT_TRUE(exploration.observe(500, readingOf(4.0, 2)));

    const auto payload = exploration.response(false);
    ASSERT_EQ(payload["stopped_reason"], "engine_error");
    ASSERT_EQ(payload["engine_errors"].get<int64_t>(), 2);
    // The frame it stopped on is the timestamp of the error, which is what
    // makes the report point at something.
    ASSERT_EQ(payload["explored_ms"].get<int64_t>(), 500);

    // A caller surveying a noisy scene can ask for the whole window instead.
    auto surveying = params;
    surveying["stop_on_engine_error"] = false;
    SceneExploration tolerant{parsed(surveying)};
    ASSERT_TRUE(!tolerant.observe(0, readingOf(0.0, 1)));
    ASSERT_TRUE(!tolerant.observe(250, readingOf(2.0, 1)));
    ASSERT_TRUE(tolerant.observe(4000, readingOf(9.0, 3)));
    const auto tolerated = tolerant.response(false);
    ASSERT_EQ(tolerated["stopped_reason"], "duration_elapsed");
    ASSERT_EQ(tolerated["engine_errors"].get<int64_t>(), 3);
}

void test_frames_held_add_up_to_the_frames_observed() {
    // Every frame the bot ran was a frame some action was down, so the counts
    // have to reconcile or the report is describing a run that did not happen.
    auto params = minimalParams();
    params["duration_ms"] = 2000;
    params["stuck_ms"] = 2000;
    SceneExploration exploration{parsed(params)};

    double position = 0.0;
    int frames = 0;
    for (int64_t elapsed = 0; elapsed <= 2000; elapsed += 100) {
        position += 1.0;
        ++frames;
        if (exploration.observe(elapsed, readingOf(position))) break;
    }

    const auto payload = exploration.response(false);
    ASSERT_EQ(payload["frames"].get<int>(), frames);
    int64_t held = 0;
    for (const auto& action : payload["actions"]) held += action["frames_held"].get<int64_t>();
    ASSERT_EQ(held, static_cast<int64_t>(frames));
}

struct RegisterSceneExplorationTests {
    RegisterSceneExplorationTests() {
        registerTest("SceneExploration.RequestDefaultsAndRefusals",
                     test_request_defaults_and_refusals);
        registerTest("SceneExploration.ScheduleIsDeterministic",
                     test_schedule_is_deterministic_and_uses_every_action);
        registerTest("SceneExploration.MovingIsNeverStuck",
                     test_a_game_that_keeps_moving_is_never_stuck);
        registerTest("SceneExploration.StillnessIsReportedWithItsAction",
                     test_a_value_that_stops_moving_is_reported_with_the_action_held);
        registerTest("SceneExploration.UnreadableProbeIsNotASoftLock",
                     test_a_probe_that_cannot_be_read_is_not_a_soft_lock);
        registerTest("SceneExploration.MeasuredRunNamesNoUnreadProbes",
                     test_a_run_that_reads_its_probe_is_measured);
        registerTest("SceneExploration.SurveyRecordsSeveralIntervals",
                     test_a_survey_run_records_more_than_one_interval);
        registerTest("SceneExploration.EngineErrorStopsTheRun",
                     test_an_engine_error_stops_the_run_and_is_counted);
        registerTest("SceneExploration.FramesHeldReconcile",
                     test_frames_held_add_up_to_the_frames_observed);
    }
} g_register_scene_exploration_tests;

} // namespace
