#include "didi/runtime/scenario_runner.hpp"
#include "didi/common/sha256.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

namespace fs = std::filesystem;
using didi::Error;
using didi::json;
using didi::Result;
using didi::runtime::IScenarioDriver;
using didi::runtime::ScenarioSpec;
using didi::runtime::ScenarioStep;
using didi::runtime::changedScenarioFiles;
using didi::runtime::collectScenarioFiles;
using didi::runtime::judgeValue;
using didi::runtime::parseScenario;
using didi::runtime::runScenario;
using didi::runtime::scenarioFilesJson;
using didi::runtime::scenarioRecordsView;

// Sandbox expressions, as an author writes them. The fake answers each one.
const std::string kY = "node.get(\"position\").y";
const std::string kOnFloor = "node.get(\"velocity\").y == 0";
const std::string kNothing = "node.get(\"missing\")";
const std::string kPosition = "node.get(\"position\")";

// A game small enough to reason about: a player on a floor at y = 0 who jumps
// on "jump", up to twice before landing again. Up is negative, as in Godot 2D.
// Input pressed while paused lands in the next frame that runs, the way the
// bridge's held queue delivers it.
class FakeGame : public IScenarioDriver {
public:
    // Configuration.
    std::optional<Error> launch_error;
    std::optional<Error> teardown_error;
    int fail_step_call = -1;          // 0-based index of the step() call that fails
    bool throw_on_evaluate = false;
    int cancel_after_calls = -1;      // cancelled() turns true after this many driver calls
    int64_t ms_per_call = 0;
    bool output_dropped = false;
    size_t output_page_size = 2;

    // What happened.
    int launches = 0;
    int teardowns = 0;
    int calls = 0;
    std::vector<std::string> log;

    double y = 0.0;
    double vy = 0.0;
    int jumps_left = 2;
    bool jump_down = false;
    bool queued_press = false;
    bool queued_release = false;
    std::vector<std::string> output = {"Godot Engine v4.7.2", "ready"};

    Result<json> launch() override {
        ++launches;
        log.push_back("launch");
        if (launch_error) return *launch_error;
        return json{{"pid", 4242}, {"session_id", "game-1"}, {"fixed_fps", 60}};
    }

    Result<json> step(int frames) override {
        const int index = step_calls++;
        ++calls;
        log.push_back("step " + std::to_string(frames));
        if (index == fail_step_call) {
            return Error(503, "The game stopped answering", json{{"code", "not_connected"}});
        }
        for (int frame = 0; frame < frames; ++frame) runFrame();
        return json{{"frames", frames}};
    }

    Result<json> press(const std::string& action, bool pressed) override {
        ++calls;
        log.push_back(std::string(pressed ? "press " : "release ") + action);
        if (action != "jump") {
            return Error(400, "The game's InputMap does not define \"" + action + "\"",
                         json{{"code", "invalid_arguments"}});
        }
        if (pressed) queued_press = true; else queued_release = true;
        return json{{"outcome", "queued"}};
    }

    Result<json> evaluate(const std::string& expression, const std::string&) override {
        ++calls;
        log.push_back("eval " + expression);
        if (throw_on_evaluate) throw std::runtime_error("pipe broke");
        if (expression == kY) return json{{"value", y}, {"value_type", "float"}};
        if (expression == kOnFloor) return json{{"value", y >= 0.0 && vy == 0.0}, {"value_type", "bool"}};
        if (expression == kNothing) return json{{"value", nullptr}, {"value_type", "null"}};
        if (expression == kPosition) {
            return json{{"value", {{"type", "Vector2"}, {"x", 0.0}, {"y", y}}}, {"value_type", "Vector2"}};
        }
        return Error(404, "Node not found: " + expression);
    }

    Result<json> readOutput(uint64_t cursor, const std::string&) override {
        ++calls;
        json records = json::array();
        uint64_t next = cursor == 0 ? 1 : cursor;
        while (records.size() < output_page_size && next <= output.size()) {
            records.push_back({{"sequence", next}, {"level", "info"}, {"message", output[next - 1]}});
            ++next;
        }
        return json{{"records", records},
                    {"next_cursor", next},
                    {"has_more", next <= output.size()},
                    {"dropped_before_cursor", cursor == 0 && output_dropped}};
    }

    Result<json> capture(size_t step_index) override {
        ++calls;
        return json{{"path", ".didi/scenarios/t/capture-" + std::to_string(step_index) + ".png"},
                    {"sha256", std::string(64, 'a')}, {"width", 64}, {"height", 64}};
    }

    Result<json> teardown() override {
        ++teardowns;
        log.push_back("teardown");
        if (teardown_error) return *teardown_error;
        return json{{"stopped", launches > 0 && !launch_error}, {"session_gone", true}};
    }

    bool cancelled() const override { return cancel_after_calls >= 0 && calls >= cancel_after_calls; }
    int64_t elapsedMs() const override { return ms_per_call * calls; }

private:
    void runFrame() {
        bool just_pressed = false;
        if (queued_press) { just_pressed = !jump_down; jump_down = true; queued_press = false; }
        else if (queued_release) { jump_down = false; queued_release = false; }
        if (just_pressed && jumps_left > 0) {
            vy = -10.0;
            --jumps_left;
            if (jumps_left == 0) output.push_back("double jump");
        }
        vy += 1.0;
        y += vy;
        if (y >= 0.0) { y = 0.0; vy = 0.0; jumps_left = 2; }
    }

    int step_calls = 0;
};

json scenario(json steps) {
    return {{"name", "double_jump"}, {"scene_path", "res://main.tscn"}, {"steps", std::move(steps)}};
}

// Two presses, the second near the top of the first, then a look at the
// highest point. One jump peaks at -45 here, so -60 is out of its reach.
json doubleJump(int presses = 2) {
    json steps = json::array({{{"kind", "wait"}, {"frames", 3}},
                              {{"kind", "press"}, {"action", "jump"}}});
    steps.push_back({{"kind", "wait"}, {"frames", 8}});
    if (presses == 2) steps.push_back({{"kind", "press"}, {"action", "jump"}});
    steps.push_back({{"kind", "wait"}, {"frames", 6}});
    steps.push_back({{"kind", "assert"}, {"label", "above one jump"}, {"expression", kY}, {"maximum", -60}});
    steps.push_back({{"kind", "assert_output"}, {"text", "double jump"}});
    return scenario(steps);
}

ScenarioSpec parsed(const json& arguments) {
    auto spec = parseScenario(arguments);
    if (spec.isErr()) throw std::runtime_error("parseScenario refused: " + spec.error().message);
    return spec.value();
}

Error refusal(const json& arguments) {
    auto spec = parseScenario(arguments);
    ASSERT_TRUE(spec.isErr());
    return spec.error();
}

std::string fieldOf(const Error& error) { return error.data.value("field", std::string()); }

void test_a_scenario_with_no_assertion_is_refused() {
    const auto none = refusal(scenario(json::array({{{"kind", "wait"}, {"frames", 10}},
                                                    {{"kind", "press"}, {"action", "jump"}}})));
    ASSERT_EQ(none.code, 400);
    ASSERT_EQ(fieldOf(none), "steps");
    ASSERT_EQ(none.data.value("reason", std::string()), "no_assertion");
    ASSERT_TRUE(none.message.find("proves nothing") != std::string::npos);

    // An absence alone is passed by a game that never ran its loop.
    const auto absence = refusal(scenario(json::array(
        {{{"kind", "wait"}, {"frames", 10}},
         {{"kind", "assert_output"}, {"level", "error"}, {"absent", true}}})));
    ASSERT_EQ(absence.data.value("reason", std::string()), "no_assertion");
    ASSERT_TRUE(absence.message.find("did not happen") != std::string::npos);

    // The same absence beside a positive check is a scenario.
    parsed(scenario(json::array({{{"kind", "assert_output"}, {"level", "error"}, {"absent", true}},
                                 {{"kind", "assert"}, {"expression", kY}, {"maximum", 0}}})));
}

void test_arguments_are_refused_before_a_game_starts() {
    const json good_assert = {{"kind", "assert"}, {"expression", kY}, {"maximum", 0}};
    auto with = [&](json step) { return scenario(json::array({std::move(step), good_assert})); };

    ASSERT_EQ(fieldOf(refusal(with({{"kind", "jump"}}))), "steps");
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "wait"}}))), "steps");                   // frames required
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "wait"}, {"frames", 601}}))), "steps");
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "wait"}, {"frames", 0}}))), "steps");
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "press"}}))), "steps");                 // action required
    // A field of another kind is named, not ignored.
    const auto stray = refusal(with({{"kind", "assert"}, {"expression", kY}, {"frames", 5}}));
    ASSERT_TRUE(stray.message.find("frames is not a field of an assert step") != std::string::npos);
    ASSERT_EQ(stray.data.value("step", -1), 0);
    // The sandbox's own refusal, without a game.
    const auto sandboxed = refusal(with({{"kind", "assert"}, {"expression", "OS.get_name()"}}));
    ASSERT_TRUE(sandboxed.message.find("read-only sandbox") != std::string::npos);
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "assert"}, {"expression", kY}, {"context_node", "Player"}}))),
              "steps");
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "assert"}, {"expression", kY}, {"minimum", 2}, {"maximum", 1}}))),
              "steps");
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "assert_output"}}))), "steps");          // nothing to look for
    ASSERT_EQ(fieldOf(refusal(with({{"kind", "assert_output"}, {"text", "x"}, {"level", "fatal"}}))), "steps");

    // A run longer than the bound, in steps that are each within theirs.
    json long_run = json::array();
    for (int i = 0; i < 7; ++i) long_run.push_back({{"kind", "wait"}, {"frames", 600}});
    long_run.push_back(good_assert);
    ASSERT_EQ(fieldOf(refusal(scenario(long_run))), "steps");

    auto args = scenario(json::array({good_assert}));
    args["name"] = "../escape";
    ASSERT_EQ(fieldOf(refusal(args)), "name");
    args = scenario(json::array({good_assert}));
    args["scene_path"] = "res://player.gd";
    ASSERT_EQ(fieldOf(refusal(args)), "scene_path");
    args = scenario(json::array({good_assert}));
    args["timeout_seconds"] = 301;
    ASSERT_EQ(fieldOf(refusal(args)), "timeout_seconds");
    args = scenario(json::array({good_assert}));
    args["detach"] = true;
    ASSERT_EQ(fieldOf(refusal(args)), "detach");
}

void test_a_capture_in_a_headless_game_names_the_argument_that_fixes_it() {
    auto args = scenario(json::array({{{"kind", "capture"}},
                                      {{"kind", "assert"}, {"expression", kY}, {"maximum", 0}}}));
    const auto error = refusal(args);
    ASSERT_EQ(fieldOf(error), "headless");
    ASSERT_EQ(error.data["retry_with"], json({{"headless", false}}));
    args["headless"] = false;
    const auto spec = parsed(args);
    ASSERT_TRUE(!spec.headless);
}

void test_the_double_jump_passes_with_its_evidence() {
    FakeGame game;
    const auto outcome = runScenario(parsed(doubleJump()), game);
    ASSERT_EQ(outcome.verdict, "pass");
    ASSERT_TRUE(!outcome.failure.has_value());
    const auto& report = outcome.report;
    ASSERT_EQ(report["assertions"]["held"].get<int>(), 2);
    ASSERT_EQ(report["assertions"]["total"].get<int>(), 2);
    ASSERT_EQ(report["frames"].get<int>(), 3 + 1 + 8 + 1 + 6);
    ASSERT_EQ(report["steps"][5]["outcome"], "held");
    ASSERT_EQ(report["steps"][5]["label"], "above one jump");
    ASSERT_TRUE(report["steps"][5]["value"].get<double>() <= -60.0);
    ASSERT_EQ(report["steps"][6]["matched"]["message"], "double jump");
    ASSERT_EQ(report["teardown"]["ok"], true);
    ASSERT_EQ(game.teardowns, 1);
    ASSERT_EQ(game.log.back(), "teardown");
}

void test_one_press_fails_on_the_assertion_it_names() {
    FakeGame game;
    const auto outcome = runScenario(parsed(doubleJump(1)), game);
    ASSERT_EQ(outcome.verdict, "fail");
    ASSERT_TRUE(outcome.failure.has_value());
    ASSERT_EQ(outcome.failure->stage, "assertion");
    ASSERT_EQ(*outcome.failure->step, 4u);
    ASSERT_EQ(outcome.failure->reason, "above_maximum");
    const auto& report = outcome.report;
    ASSERT_EQ(report["failure"]["kind"], "assert");
    ASSERT_EQ(report["steps"][4]["outcome"], "failed");
    // A fail-fast run: the output check after it never ran, and says so.
    ASSERT_EQ(report["steps"][5]["outcome"], "not_run");
    ASSERT_EQ(report["assertions"]["not_run"].get<int>(), 1);
    ASSERT_EQ(game.teardowns, 1);
}

void test_a_press_is_held_for_exactly_its_frames() {
    FakeGame game;
    runScenario(parsed(scenario(json::array({{{"kind", "press"}, {"action", "jump"}, {"frames", 3}},
                                             {{"kind", "assert"}, {"expression", kY}, {"maximum", 0}}}))),
                game);
    const std::vector<std::string> expected = {"launch", "press jump", "step 3", "release jump",
                                               "eval " + kY, "teardown"};
    ASSERT_EQ(game.log, expected);
}

void test_frames_past_sixty_are_run_in_chunks() {
    FakeGame game;
    const auto outcome = runScenario(
        parsed(scenario(json::array({{{"kind", "wait"}, {"frames", 130}},
                                     {{"kind", "assert"}, {"expression", kOnFloor}}}))),
        game);
    ASSERT_EQ(outcome.verdict, "pass");
    const std::vector<std::string> expected = {"launch", "step 60", "step 60", "step 10",
                                               "eval " + kOnFloor, "teardown"};
    ASSERT_EQ(game.log, expected);
}

void test_teardown_runs_on_every_path() {
    {   // The launch failed: nothing ran, and teardown is still asked.
        FakeGame game;
        game.launch_error = Error(503, "Godot could not be started", json{{"code", "engine_unavailable"}});
        const auto outcome = runScenario(parsed(doubleJump()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "launch");
        ASSERT_EQ(game.teardowns, 1);
        ASSERT_EQ(outcome.report["steps"][0]["outcome"], "not_run");
    }
    {   // The engine stopped answering under a step.
        FakeGame game;
        game.fail_step_call = 1;
        const auto outcome = runScenario(parsed(doubleJump()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "step");
        ASSERT_EQ(outcome.failure->cause->code, 503);
        ASSERT_EQ(outcome.report["steps"][1]["outcome"], "error");
        ASSERT_EQ(game.teardowns, 1);
    }
    {   // The game refused the action.
        FakeGame game;
        const auto outcome = runScenario(
            parsed(scenario(json::array({{{"kind", "press"}, {"action", "dash"}},
                                         {{"kind", "assert"}, {"expression", kY}, {"maximum", 0}}}))),
            game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_TRUE(outcome.failure->message.find("could not press dash") != std::string::npos);
        ASSERT_EQ(game.teardowns, 1);
    }
    {   // The driver threw.
        FakeGame game;
        game.throw_on_evaluate = true;
        const auto outcome = runScenario(parsed(doubleJump()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_TRUE(outcome.failure->message.find("pipe broke") != std::string::npos);
        ASSERT_EQ(game.teardowns, 1);
    }
    {   // The job was cancelled.
        FakeGame game;
        game.cancel_after_calls = 2;
        const auto outcome = runScenario(parsed(doubleJump()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "cancelled");
        ASSERT_EQ(game.teardowns, 1);
    }
    {   // The deadline passed.
        FakeGame game;
        game.ms_per_call = 30000;
        const auto outcome = runScenario(parsed(doubleJump()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "timeout");
        ASSERT_EQ(game.teardowns, 1);
    }
}

void test_a_failed_teardown_fails_a_run_whose_assertions_held() {
    FakeGame game;
    game.teardown_error = Error(409, "The game was still running after runtime.stop");
    const auto outcome = runScenario(parsed(doubleJump()), game);
    ASSERT_EQ(outcome.verdict, "error");
    ASSERT_EQ(outcome.failure->stage, "teardown");
    ASSERT_EQ(outcome.report["teardown"]["ok"], false);
    ASSERT_EQ(outcome.report["assertions"]["held"].get<int>(), 2);

    // A disproved behaviour stays disproved, and the teardown says what it left.
    FakeGame failing;
    failing.teardown_error = Error(409, "The game was still running after runtime.stop");
    const auto failed = runScenario(parsed(doubleJump(1)), failing);
    ASSERT_EQ(failed.verdict, "fail");
    ASSERT_EQ(failed.report["teardown"]["ok"], false);
    ASSERT_TRUE(failed.report["summary"].get<std::string>().find("may still be running") != std::string::npos);
}

void test_a_run_that_proved_nothing_cannot_pass() {
    // parseScenario refuses this; a spec built by hand reaches the verdict and
    // is refused there too.
    ScenarioSpec spec;
    spec.name = "smoke";
    spec.scene_path = "res://main.tscn";
    ScenarioStep wait;
    wait.kind = ScenarioStep::Kind::wait;
    wait.frames = 5;
    spec.steps.push_back(wait);
    FakeGame game;
    const auto outcome = runScenario(spec, game);
    ASSERT_EQ(outcome.verdict, "error");
    ASSERT_EQ(outcome.failure->reason, "nothing_proved");
    ASSERT_EQ(game.teardowns, 1);
}

void test_values_are_judged_strictly() {
    ASSERT_TRUE(judgeValue(true, std::nullopt, std::nullopt).held);
    ASSERT_EQ(judgeValue(false, std::nullopt, std::nullopt).reason, "false");
    ASSERT_EQ(judgeValue(1, std::nullopt, std::nullopt).reason, "not_a_boolean");
    ASSERT_EQ(judgeValue(nullptr, std::nullopt, std::nullopt).reason, "no_value");
    ASSERT_EQ(judgeValue(nullptr, 0.0, std::nullopt).reason, "no_value");
    ASSERT_EQ(judgeValue(json{{"x", 1}}, 0.0, std::nullopt).reason, "not_a_number");
    ASSERT_EQ(judgeValue(true, 0.0, std::nullopt).reason, "not_a_number");
    ASSERT_EQ(judgeValue(-1.5, 0.0, std::nullopt).reason, "below_minimum");
    ASSERT_EQ(judgeValue(3, std::nullopt, 2.0).reason, "above_maximum");
    ASSERT_TRUE(judgeValue(2, 2.0, 2.0).held);

    // Through a run: a Vector2 against a bound fails and says to read a component.
    FakeGame game;
    const auto outcome = runScenario(
        parsed(scenario(json::array({{{"kind", "assert"}, {"expression", kPosition}, {"maximum", 0}}}))),
        game);
    ASSERT_EQ(outcome.verdict, "fail");
    ASSERT_EQ(outcome.failure->reason, "not_a_number");
    ASSERT_TRUE(outcome.failure->message.find("read one component") != std::string::npos);
    FakeGame nulls;
    const auto null_outcome = runScenario(
        parsed(scenario(json::array({{{"kind", "assert"}, {"expression", kNothing}}}))), nulls);
    ASSERT_EQ(null_outcome.verdict, "fail");
    ASSERT_EQ(null_outcome.failure->reason, "no_value");
}

void test_wait_until_waits_for_its_condition_and_fails_when_it_never_holds() {
    FakeGame game;
    // In the air after the press; back on the floor some frames later.
    const auto outcome = runScenario(
        parsed(scenario(json::array({{{"kind", "press"}, {"action", "jump"}},
                                     {{"kind", "wait_until"}, {"expression", kOnFloor}, {"frames", 60}},
                                     {{"kind", "assert"}, {"expression", kY}, {"minimum", 0}, {"maximum", 0}}}))),
        game);
    ASSERT_EQ(outcome.verdict, "pass");
    const auto waited = outcome.report["steps"][1]["frames_waited"].get<int>();
    ASSERT_TRUE(waited > 0 && waited < 60);

    FakeGame never;
    const auto failed = runScenario(
        parsed(scenario(json::array({{{"kind", "wait_until"}, {"expression", kY}, {"maximum", -100}, {"frames", 20}},
                                     {{"kind", "assert"}, {"expression", kOnFloor}}}))),
        never);
    ASSERT_EQ(failed.verdict, "fail");
    ASSERT_EQ(failed.failure->reason, "never_held");
    ASSERT_EQ(failed.report["steps"][1]["outcome"], "not_run");
}

// Driver calls before the wait_until polls: eval, press, step 1, release.
// Its first poll is eval (call 5) and then step 1 (call 6), and the player is
// in the air for about twenty frames after that.
json jumpThenWaitForTheFloor() {
    json spec = scenario(json::array({{{"kind", "assert"}, {"expression", kOnFloor}},
                                      {{"kind", "press"}, {"action", "jump"}, {"frames", 1}},
                                      {{"kind", "wait_until"}, {"expression", kOnFloor}, {"frames", 60}}}));
    spec["timeout_seconds"] = 5;
    return spec;
}

size_t framesStepped(const FakeGame& game) {
    return static_cast<size_t>(std::count(game.log.begin(), game.log.end(), "step 1"));
}

void test_a_cancel_or_deadline_stops_a_step_that_is_already_running() {
    {   // Cancelled during the polls: the poll that saw it is the last.
        FakeGame game;
        game.cancel_after_calls = 6;
        const auto outcome = runScenario(parsed(jumpThenWaitForTheFloor()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "cancelled");
        ASSERT_EQ(outcome.failure->step, std::optional<size_t>(2));
        ASSERT_EQ(outcome.report["steps"][2]["outcome"], "interrupted");
        ASSERT_EQ(framesStepped(game), 2u);
        ASSERT_EQ(game.teardowns, 1);
    }
    {   // The deadline passed during the last step.
        FakeGame game;
        game.ms_per_call = 1000;
        const auto outcome = runScenario(parsed(jumpThenWaitForTheFloor()), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "timeout");
        ASSERT_EQ(outcome.report["steps"][2]["outcome"], "interrupted");
        ASSERT_EQ(framesStepped(game), 2u);
        ASSERT_EQ(game.teardowns, 1);
    }
    {   // A wait of one chunk is checked after it too, not only between chunks.
        FakeGame game;
        game.ms_per_call = 3000;
        json spec = scenario(json::array({{{"kind", "assert"}, {"expression", kOnFloor}},
                                          {{"kind", "wait"}, {"frames", 60}}}));
        spec["timeout_seconds"] = 5;
        const auto outcome = runScenario(parsed(spec), game);
        ASSERT_EQ(outcome.verdict, "error");
        ASSERT_EQ(outcome.failure->stage, "timeout");
        ASSERT_EQ(outcome.report["steps"][1]["outcome"], "interrupted");
        ASSERT_EQ(game.teardowns, 1);
    }
}

void test_output_assertions_page_and_refuse_to_prove_an_absence_over_a_gap() {
    {   // Found on a later page.
        FakeGame game;
        game.output = {"a", "b", "c", "d", "the line"};
        const auto outcome = runScenario(
            parsed(scenario(json::array({{{"kind", "assert_output"}, {"text", "the line"}}}))), game);
        ASSERT_EQ(outcome.verdict, "pass");
        ASSERT_EQ(outcome.report["steps"][0]["matched"]["sequence"].get<int>(), 5);
    }
    const json absence_and_check = json::array(
        {{{"kind", "assert_output"}, {"text", "ERROR"}, {"absent", true}},
         {{"kind", "assert"}, {"expression", kOnFloor}}});
    {   // Never printed: the absence holds.
        FakeGame game;
        ASSERT_EQ(runScenario(parsed(scenario(absence_and_check)), game).verdict, "pass");
    }
    {   // Printed: it does not.
        FakeGame game;
        game.output.push_back("ERROR: boom");
        const auto outcome = runScenario(parsed(scenario(absence_and_check)), game);
        ASSERT_EQ(outcome.verdict, "fail");
        ASSERT_EQ(outcome.failure->reason, "seen");
    }
    {   // Some output was evicted before it was read: nothing is proved.
        FakeGame game;
        game.output_dropped = true;
        const auto outcome = runScenario(parsed(scenario(absence_and_check)), game);
        ASSERT_EQ(outcome.verdict, "fail");
        ASSERT_EQ(outcome.failure->reason, "output_unread");
    }
}

// --- Files -------------------------------------------------------------------

struct TempProject {
    fs::path root;
    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("didi_scenario_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(counter++));
        fs::create_directories(root);
    }
    ~TempProject() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    void write(const std::string& relative, const std::string& text) const {
        const auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
};

void writeProject(const TempProject& project) {
    project.write("project.godot",
                  "config_version=5\n\n[autoload]\n\nGame=\"*res://autoload/game.gd\"\n");
    project.write("autoload/game.gd", "extends Node\n");
    project.write("main.tscn",
                  "[gd_scene load_steps=3 format=3]\n\n"
                  "[ext_resource type=\"Script\" path=\"res://player.gd\" id=\"1\"]\n"
                  "[ext_resource type=\"PackedScene\" path=\"res://hud.tscn\" id=\"2\"]\n"
                  "[ext_resource type=\"Texture2D\" path=\"res://icon.png\" id=\"3\"]\n\n"
                  "[node name=\"Main\" type=\"Node2D\"]\n");
    project.write("hud.tscn", "[gd_scene format=3]\n\n[node name=\"Hud\" type=\"Control\"]\n");
    project.write("player.gd",
                  "extends CharacterBody2D\nconst Lib = preload(\"res://lib/jump.gd\")\n"
                  "var sound = 'res://sounds/jump.wav'\n");
    project.write("lib/jump.gd", "extends RefCounted\n");
    project.write("icon.png", "not really a png");
}

std::vector<std::string> pathsOf(const std::vector<didi::runtime::ScenarioFile>& files) {
    std::vector<std::string> listed;
    for (const auto& file : files) listed.push_back(file.path);
    return listed;
}

void test_the_record_names_the_scripts_and_scenes_a_run_reaches() {
    TempProject project;
    writeProject(project);
    const auto collected = collectScenarioFiles(project.root, "res://main.tscn");
    const std::vector<std::string> expected = {"project.godot", "res://autoload/game.gd", "res://hud.tscn",
                                               "res://lib/jump.gd", "res://main.tscn", "res://player.gd"};
    ASSERT_EQ(pathsOf(collected.files), expected);
    ASSERT_TRUE(!collected.truncated);
    for (const auto& file : collected.files) ASSERT_EQ(file.sha256.size(), 64u);
    ASSERT_EQ(collected.files[5].sha256,
              didi::sha256Hex("extends CharacterBody2D\nconst Lib = preload(\"res://lib/jump.gd\")\n"
                              "var sound = 'res://sounds/jump.wav'\n"));
    ASSERT_TRUE(changedScenarioFiles(project.root, collected.files).empty());
}

void test_a_changed_file_marks_the_record_stale() {
    TempProject project;
    writeProject(project);
    const auto collected = collectScenarioFiles(project.root, "res://main.tscn");
    fs::create_directories(project.root / ".didi" / "scenarios");
    json record = {{"scenario", "double_jump"}, {"verdict", "pass"}, {"ran_at", "2026-10-08T00:00:00.000Z"},
                   {"scene_path", "res://main.tscn"}, {"files", scenarioFilesJson(collected)}};
    project.write(".didi/scenarios/double_jump.json", record.dump());
    // A record that names nothing it ran against cannot be fresh.
    project.write(".didi/scenarios/empty.json", json{{"verdict", "pass"}, {"ran_at", "2026-10-07T00:00:00.000Z"}}.dump());
    project.write(".didi/scenarios/broken.json", "{ not json");

    auto view = scenarioRecordsView(project.root);
    ASSERT_EQ(view["count"].get<int>(), 3);
    ASSERT_EQ(view["scenarios"][0]["name"], "double_jump");   // newest first
    ASSERT_EQ(view["scenarios"][0]["stale"], false);
    json empty_entry, broken_entry;
    for (const auto& entry : view["scenarios"]) {
        if (entry["name"] == "empty") empty_entry = entry;
        if (entry["name"] == "broken") broken_entry = entry;
    }
    ASSERT_EQ(empty_entry["stale"], true);
    ASSERT_EQ(broken_entry["unreadable"], true);

    project.write("lib/jump.gd", "extends RefCounted\nconst HEIGHT = 2\n");
    view = scenarioRecordsView(project.root);
    ASSERT_EQ(view["scenarios"][0]["stale"], true);
    ASSERT_EQ(view["scenarios"][0]["changed_files"], json::array({"res://lib/jump.gd"}));

    fs::remove(project.root / "hud.tscn");
    const auto changed = changedScenarioFiles(project.root, collected.files);
    ASSERT_EQ(changed.size(), 2u);
    ASSERT_TRUE(changed[0].path == "res://hud.tscn" && changed[0].sha256.empty());
}

void test_a_reference_outside_the_project_is_not_followed() {
    TempProject project;
    project.write("project.godot", "config_version=5\n");
    project.write("main.tscn", "[ext_resource type=\"Script\" path=\"res://../outside.gd\" id=\"1\"]\n");
    const auto collected = collectScenarioFiles(project.root, "res://main.tscn");
    const std::vector<std::string> expected = {"project.godot", "res://main.tscn"};
    ASSERT_EQ(pathsOf(collected.files), expected);
}

// --- Through the registry ----------------------------------------------------

json payloadOf(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(!result.content.empty());
    return json::parse(result.content[0].text);
}

void test_the_tool_is_a_mutation_that_runs_project_code() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("runtime_run_scenario");
    ASSERT_TRUE(tool != nullptr);
    ASSERT_TRUE(tool->capability.implemented);
    // It starts a Godot that runs the project's own code and presses its
    // actions: never safe to auto-approve, and not a closed local call.
    ASSERT_TRUE(!tool->annotations.read_only);
    ASSERT_TRUE(tool->annotations.open_world);
}

void test_refusals_reach_the_caller_with_what_fixes_them() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    // Refused before any game: nothing here needs Godot.
    const auto smoke = registry.callTool(
        "runtime_run_scenario",
        scenario(json::array({{{"kind", "wait"}, {"frames", 30}}, {{"kind", "press"}, {"action", "jump"}}})));
    ASSERT_TRUE(smoke.isError);
    const auto refused = payloadOf(smoke)["error"];
    ASSERT_EQ(refused["code"], 400);
    ASSERT_EQ(refused["data"]["reason"], "no_assertion");
    ASSERT_EQ(refused["data"]["field"], "steps");
    ASSERT_EQ(refused["data"]["canonical_tool"], "runtime_run_scenario");

    // A scene that is not in the project is named, with the call that finds one.
    auto args = scenario(json::array({{{"kind", "assert"}, {"expression", kY}, {"maximum", 0}}}));
    args["scene_path"] = "res://no_such_scene_for_a_scenario.tscn";
    const auto missing = registry.callTool("runtime_run_scenario", args);
    ASSERT_TRUE(missing.isError);
    const auto absent = payloadOf(missing)["error"];
    ASSERT_EQ(absent["code"], 404);
    ASSERT_EQ(absent["data"]["next_call"]["tool"], "project_list_resources");
}

struct RegisterScenarioRunnerTests {
    RegisterScenarioRunnerTests() {
        registerTest("ScenarioRunner.NoAssertionIsRefused", test_a_scenario_with_no_assertion_is_refused);
        registerTest("ScenarioRunner.ArgumentsAreRefusedBeforeLaunch",
                     test_arguments_are_refused_before_a_game_starts);
        registerTest("ScenarioRunner.HeadlessCaptureNamesTheFix",
                     test_a_capture_in_a_headless_game_names_the_argument_that_fixes_it);
        registerTest("ScenarioRunner.DoubleJumpPasses", test_the_double_jump_passes_with_its_evidence);
        registerTest("ScenarioRunner.OnePressFails", test_one_press_fails_on_the_assertion_it_names);
        registerTest("ScenarioRunner.PressIsHeldForItsFrames", test_a_press_is_held_for_exactly_its_frames);
        registerTest("ScenarioRunner.LongWaitsAreChunked", test_frames_past_sixty_are_run_in_chunks);
        registerTest("ScenarioRunner.TeardownRunsOnEveryPath", test_teardown_runs_on_every_path);
        registerTest("ScenarioRunner.FailedTeardownFailsTheRun",
                     test_a_failed_teardown_fails_a_run_whose_assertions_held);
        registerTest("ScenarioRunner.NothingProvedCannotPass", test_a_run_that_proved_nothing_cannot_pass);
        registerTest("ScenarioRunner.ValuesAreJudgedStrictly", test_values_are_judged_strictly);
        registerTest("ScenarioRunner.WaitUntil",
                     test_wait_until_waits_for_its_condition_and_fails_when_it_never_holds);
        registerTest("ScenarioRunner.CancelOrDeadlineStopsARunningStep",
                     test_a_cancel_or_deadline_stops_a_step_that_is_already_running);
        registerTest("ScenarioRunner.OutputAssertions",
                     test_output_assertions_page_and_refuse_to_prove_an_absence_over_a_gap);
        registerTest("ScenarioRunner.RecordNamesWhatRan",
                     test_the_record_names_the_scripts_and_scenes_a_run_reaches);
        registerTest("ScenarioRunner.ChangedFileMarksStale", test_a_changed_file_marks_the_record_stale);
        registerTest("ScenarioRunner.OutsideReferencesAreNotFollowed",
                     test_a_reference_outside_the_project_is_not_followed);
        registerTest("ScenarioRunner.ToolIsAMutationThatRunsProjectCode",
                     test_the_tool_is_a_mutation_that_runs_project_code);
        registerTest("ScenarioRunner.RefusalsNameTheirFix", test_refusals_reach_the_caller_with_what_fixes_them);
    }
} g_register_scenario_runner_tests;

}  // namespace
