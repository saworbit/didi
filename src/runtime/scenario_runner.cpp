#include "didi/runtime/scenario_runner.hpp"

#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/common/sha256.hpp"
#include "didi/gdextension/expression_sandbox.hpp"
#include "didi/offline/project_settings_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace didi::runtime {

namespace fs = std::filesystem;

namespace {

// --- Arguments ---------------------------------------------------------------

Error refuse(std::string message, const std::string& field, json extra = json::object()) {
    json data = {{"code", "invalid_arguments"}, {"field", field}};
    for (auto& [key, value] : extra.items()) data[key] = std::move(value);
    return Error(400, std::move(message), std::move(data));
}

bool integerIn(const json& value, int64_t low, int64_t high) {
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>() >= static_cast<uint64_t>(std::max<int64_t>(low, 0)) &&
               value.get<uint64_t>() <= static_cast<uint64_t>(high);
    }
    return value.is_number_integer() && value.get<int64_t>() >= low && value.get<int64_t>() <= high;
}

std::string stepName(size_t index) { return "steps[" + std::to_string(index) + "]"; }

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (!std::isalnum(static_cast<unsigned char>(name.front()))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The fields each kind of step takes. A field another kind uses is refused by
// name rather than ignored: `frames` on an assert step is an author who thought
// the assertion waited, and the run would have meant something else.
const std::vector<std::string>& fieldsOf(ScenarioStep::Kind kind) {
    using Kind = ScenarioStep::Kind;
    static const std::vector<std::string> wait = {"kind", "label", "frames"};
    static const std::vector<std::string> wait_until = {"kind",         "label",   "frames",
                                                        "expression",   "minimum", "maximum",
                                                        "context_node"};
    static const std::vector<std::string> press = {"kind", "label", "action", "frames"};
    static const std::vector<std::string> assert_value = {"kind", "label", "expression",
                                                          "context_node", "minimum", "maximum"};
    static const std::vector<std::string> assert_output = {"kind", "label", "text", "level",
                                                           "absent"};
    static const std::vector<std::string> capture = {"kind", "label"};
    switch (kind) {
        case Kind::wait: return wait;
        case Kind::wait_until: return wait_until;
        case Kind::press: return press;
        case Kind::assert_value: return assert_value;
        case Kind::assert_output: return assert_output;
        case Kind::capture: return capture;
    }
    return capture;
}

std::optional<ScenarioStep::Kind> kindNamed(const std::string& name) {
    using Kind = ScenarioStep::Kind;
    if (name == "wait") return Kind::wait;
    if (name == "wait_until") return Kind::wait_until;
    if (name == "press") return Kind::press;
    if (name == "assert") return Kind::assert_value;
    if (name == "assert_output") return Kind::assert_output;
    if (name == "capture") return Kind::capture;
    return std::nullopt;
}

Result<std::optional<double>> boundOf(const json& step, const char* key, size_t index) {
    if (!step.contains(key)) return std::optional<double>{};
    const auto& value = step[key];
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        return refuse(stepName(index) + "." + key + " must be a finite number.", "steps",
                      {{"step", index}});
    }
    return std::optional<double>{value.get<double>()};
}

Result<void> parseExpression(const json& step, size_t index, ScenarioStep& parsed) {
    if (!step.contains("expression") || !step["expression"].is_string() ||
        step["expression"].get<std::string>().empty() ||
        step["expression"].get<std::string>().size() > 512) {
        return refuse(stepName(index) + ".expression is required: a sandbox expression of 1 to "
                      "512 bytes, such as node.get(\"position\").y.",
                      "steps", {{"step", index}});
    }
    parsed.expression = step["expression"].get<std::string>();
    // The same rules eval_gdscript applies, so a refusal here is the one the
    // game would have given, without a game started to give it.
    const auto policy = godot::ExpressionPolicy::validate(parsed.expression);
    if (policy.isErr()) {
        return refuse(stepName(index) + ".expression is refused by the read-only sandbox: " +
                          policy.error().message,
                      "steps", {{"step", index}});
    }
    if (step.contains("context_node")) {
        if (!step["context_node"].is_string()) {
            return refuse(stepName(index) + ".context_node must be a string.", "steps",
                          {{"step", index}});
        }
        parsed.context_node = step["context_node"].get<std::string>();
        if (const auto problem = paths::runtimeContextPathProblem(parsed.context_node)) {
            return refuse(stepName(index) + ": " + *problem + ".", "steps", {{"step", index}});
        }
    }
    auto minimum = boundOf(step, "minimum", index);
    if (minimum.isErr()) return minimum.error();
    auto maximum = boundOf(step, "maximum", index);
    if (maximum.isErr()) return maximum.error();
    parsed.minimum = minimum.value();
    parsed.maximum = maximum.value();
    if (parsed.minimum && parsed.maximum && *parsed.minimum > *parsed.maximum) {
        return refuse(stepName(index) + ".minimum is greater than its maximum, so no value can "
                      "satisfy it.",
                      "steps", {{"step", index}});
    }
    return Result<void>::ok();
}

Result<ScenarioStep> parseStep(const json& step, size_t index) {
    if (!step.is_object()) {
        return refuse(stepName(index) + " must be an object with a kind.", "steps", {{"step", index}});
    }
    if (!step.contains("kind") || !step["kind"].is_string() ||
        !kindNamed(step["kind"].get<std::string>())) {
        return refuse(stepName(index) + ".kind must be one of wait, wait_until, press, assert, "
                      "assert_output, capture.",
                      "steps", {{"step", index}});
    }
    ScenarioStep parsed;
    parsed.kind = *kindNamed(step["kind"].get<std::string>());
    const auto& allowed = fieldsOf(parsed.kind);
    for (const auto& [key, value] : step.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            const std::string kind = parsed.kindName();
            return refuse(stepName(index) + "." + key + " is not a field of " +
                              (kind.front() == 'a' ? "an " : "a ") + kind + " step.",
                          "steps", {{"step", index}});
        }
    }
    if (step.contains("label")) {
        if (!step["label"].is_string() || step["label"].get<std::string>().empty() ||
            step["label"].get<std::string>().size() > 64) {
            return refuse(stepName(index) + ".label must be a string of 1 to 64 bytes.", "steps",
                          {{"step", index}});
        }
        parsed.label = step["label"].get<std::string>();
    }

    using Kind = ScenarioStep::Kind;
    const bool takes_frames =
        parsed.kind == Kind::wait || parsed.kind == Kind::wait_until || parsed.kind == Kind::press;
    if (takes_frames) {
        const bool required = parsed.kind != Kind::press;
        if (!step.contains("frames")) {
            if (required) {
                return refuse(stepName(index) + ".frames is required for a " +
                                  std::string(parsed.kindName()) + " step: an integer from 1 to " +
                                  std::to_string(kMaxStepFrames) + ".",
                              "steps", {{"step", index}});
            }
            parsed.frames = 1;
        } else if (!integerIn(step["frames"], 1, kMaxStepFrames)) {
            return refuse(stepName(index) + ".frames must be an integer from 1 to " +
                              std::to_string(kMaxStepFrames) + ".",
                          "steps", {{"step", index}});
        } else {
            parsed.frames = static_cast<int>(step["frames"].get<int64_t>());
        }
    }

    switch (parsed.kind) {
        case Kind::wait:
        case Kind::capture:
            break;
        case Kind::press: {
            if (!step.contains("action") || !step["action"].is_string() ||
                step["action"].get<std::string>().empty() ||
                step["action"].get<std::string>().size() > 128) {
                return refuse(stepName(index) + ".action is required: the InputMap action to "
                              "press, 1 to 128 bytes.",
                              "steps", {{"step", index}});
            }
            parsed.action = step["action"].get<std::string>();
            break;
        }
        case Kind::wait_until:
        case Kind::assert_value: {
            auto expression = parseExpression(step, index, parsed);
            if (expression.isErr()) return expression.error();
            break;
        }
        case Kind::assert_output: {
            if (step.contains("text")) {
                if (!step["text"].is_string() || step["text"].get<std::string>().empty() ||
                    step["text"].get<std::string>().size() > 256) {
                    return refuse(stepName(index) + ".text must be a string of 1 to 256 bytes.",
                                  "steps", {{"step", index}});
                }
                parsed.text = step["text"].get<std::string>();
            }
            if (step.contains("level")) {
                static const std::set<std::string> levels = {"debug", "info", "warning", "error"};
                if (!step["level"].is_string() || !levels.count(step["level"].get<std::string>())) {
                    return refuse(stepName(index) + ".level must be debug, info, warning or error.",
                                  "steps", {{"step", index}});
                }
                parsed.level = step["level"].get<std::string>();
            }
            if (step.contains("absent")) {
                if (!step["absent"].is_boolean()) {
                    return refuse(stepName(index) + ".absent must be true or false.", "steps",
                                  {{"step", index}});
                }
                parsed.absent = step["absent"].get<bool>();
            }
            if (parsed.text.empty() && parsed.level.empty()) {
                return refuse(stepName(index) + " names nothing to look for: give text, level, or "
                              "both.",
                              "steps", {{"step", index}});
            }
            break;
        }
    }
    return parsed;
}

// --- The run -----------------------------------------------------------------

json describeError(const Error& error) {
    json described = {{"status", error.code}, {"message", error.message}};
    if (!error.data.is_null()) described["data"] = error.data;
    return described;
}

std::string shortValue(const json& value) {
    auto text = value.dump(-1, ' ', false, json::error_handler_t::replace);
    if (text.size() > 120) text = text.substr(0, 117) + "...";
    return text;
}

std::string numberText(double number) {
    std::ostringstream out;
    out << number;
    return out.str();
}

std::string judgementSentence(const ValueJudgement& judged, const json& value,
                              const std::string& value_type, const ScenarioStep& step) {
    if (judged.reason == "below_minimum") {
        return "the value was " + shortValue(value) + ", below the minimum " +
               numberText(*step.minimum);
    }
    if (judged.reason == "above_maximum") {
        return "the value was " + shortValue(value) + ", above the maximum " +
               numberText(*step.maximum);
    }
    if (judged.reason == "not_a_number") {
        return "the value was a " + (value_type.empty() ? std::string("non-number") : value_type) +
               ", and minimum and maximum compare a number; read one component, such as "
               "node.get(\"position\").y";
    }
    if (judged.reason == "not_finite") return "the value was not a finite number";
    if (judged.reason == "no_value") return "the expression produced no value (null)";
    if (judged.reason == "false") return "the expression was false";
    if (judged.reason == "not_a_boolean") {
        return "the value was " + shortValue(value) +
               ", and with no minimum or maximum the expression has to be true or false";
    }
    return judged.reason;
}

const char* kindOf(const ScenarioStep& step) { return step.kindName(); }

class Run {
public:
    Run(const ScenarioSpec& spec, IScenarioDriver& driver) : m_spec(spec), m_driver(driver) {
        m_steps = json::array();
        for (size_t index = 0; index < spec.steps.size(); ++index) {
            const auto& step = spec.steps[index];
            json entry = {{"index", index}, {"kind", kindOf(step)}, {"outcome", "not_run"}};
            if (!step.label.empty()) entry["label"] = step.label;
            m_steps.push_back(std::move(entry));
        }
    }

    ScenarioOutcome execute() {
        auto launched = guarded([&] { return m_driver.launch(); });
        if (launched.isErr()) {
            fail("launch", std::nullopt, "launch_failed",
                 "The game could not be started: " + launched.error().message, launched.error());
        } else {
            m_game = launched.value();
            for (size_t index = 0; index < m_spec.steps.size() && !m_failure; ++index) {
                if (interrupted(index, false)) break;
                runStep(index);
            }
        }
        // Every path reaches this, the launch that failed included: the driver
        // knows whether a process was started and what is left of it.
        auto torn = guarded([&] { return m_driver.teardown(); });
        json teardown = torn.isOk() && torn.value().is_object() ? torn.value() : json::object();
        teardown["ok"] = torn.isOk();
        if (torn.isErr()) {
            teardown["error"] = describeError(torn.error());
            if (!m_failure) {
                fail("teardown", std::nullopt, "teardown_failed",
                     "Every step ran, but the game it started may still be running: " +
                         torn.error().message,
                     torn.error());
            }
        }
        return finish(std::move(teardown), torn.isOk());
    }

private:
    template <typename Call>
    Result<json> guarded(Call&& call) {
        try {
            return call();
        } catch (const std::exception& thrown) {
            return Error::internal(std::string("The scenario driver failed: ") + thrown.what());
        } catch (...) {
            return Error::internal("The scenario driver failed with an unknown exception");
        }
    }

    void fail(std::string stage, std::optional<size_t> step, std::string reason,
              std::string message, std::optional<Error> cause = std::nullopt) {
        if (m_failure) return;
        m_failure = ScenarioFailure{std::move(stage), step, std::move(reason), std::move(message),
                                    std::move(cause)};
        if (step) {
            auto& entry = m_steps[*step];
            if (m_failure->stage == "assertion") {
                entry["outcome"] = "failed";
            } else {
                entry["outcome"] = "error";
                if (m_failure->cause) entry["error"] = describeError(*m_failure->cause);
            }
            entry["reason"] = m_failure->reason;
        }
    }

    // Asked before each step and between the chunks of a long one. A step
    // that was cut off part way says so; one that never started stays not_run.
    bool interrupted(size_t step, bool started) {
        const char* stage = nullptr;
        std::string message;
        if (m_driver.cancelled()) {
            stage = "cancelled";
            message = "The job running this scenario was cancelled, so it stopped before finishing.";
        } else if (m_driver.elapsedMs() > static_cast<int64_t>(m_spec.timeout_seconds) * 1000) {
            stage = "timeout";
            message = "The scenario ran past its timeout_seconds of " +
                      std::to_string(m_spec.timeout_seconds) + ".";
        }
        if (stage == nullptr) return false;
        fail(stage, step, stage, std::move(message));
        m_steps[step]["outcome"] = started ? "interrupted" : "not_run";
        return true;
    }

    bool stepError(size_t index, const std::string& what, const Error& error) {
        fail("step", index, "engine_refused",
             "Step " + std::to_string(index) + " (" + kindOf(m_spec.steps[index]) + ") " + what +
                 ": " + error.message,
             error);
        return false;
    }

    // Runs `frames` frames of the paused game, at most 60 a call. Checked
    // after every call, the last one too: wait_until runs one frame a call,
    // and a step that ends past the deadline did not finish inside it.
    bool driveFrames(size_t index, int frames) {
        while (frames > 0) {
            const int chunk = std::min(frames, kMaxFramesPerStepCall);
            auto stepped = guarded([&] { return m_driver.step(chunk); });
            if (stepped.isErr()) return stepError(index, "could not run its frames", stepped.error());
            m_frames += chunk;
            frames -= chunk;
            if (interrupted(index, true)) return false;
        }
        return true;
    }

    // An expression's value, judged. Nothing when the engine refused it, in
    // which case the failure is already recorded.
    std::optional<ValueJudgement> judge(size_t index, json& entry) {
        const auto& step = m_spec.steps[index];
        auto evaluated = guarded([&] { return m_driver.evaluate(step.expression, step.context_node); });
        if (evaluated.isErr()) {
            stepError(index, "could not read its expression", evaluated.error());
            return std::nullopt;
        }
        const auto& answer = evaluated.value();
        const json value = answer.is_object() ? answer.value("value", json()) : json();
        const std::string value_type =
            answer.is_object() ? answer.value("value_type", std::string()) : std::string();
        entry["value"] = value;
        if (!value_type.empty()) entry["value_type"] = value_type;
        if (step.minimum) entry["minimum"] = *step.minimum;
        if (step.maximum) entry["maximum"] = *step.maximum;
        auto judged = judgeValue(value, step.minimum, step.maximum);
        m_lastSentence = judgementSentence(judged, value, value_type, step);
        return judged;
    }

    void runStep(size_t index) {
        using Kind = ScenarioStep::Kind;
        const auto& step = m_spec.steps[index];
        auto& entry = m_steps[index];
        const int64_t frames_before = m_frames;
        switch (step.kind) {
            case Kind::wait:
                if (!driveFrames(index, step.frames)) return;
                entry["outcome"] = "done";
                break;
            case Kind::press: {
                auto down = guarded([&] { return m_driver.press(step.action, true); });
                if (down.isErr()) {
                    stepError(index, "could not press " + step.action, down.error());
                    return;
                }
                if (!driveFrames(index, step.frames)) return;
                // Queued like the press, so the release lands in the next frame
                // that runs: the action was down for exactly `frames` frames.
                auto up = guarded([&] { return m_driver.press(step.action, false); });
                if (up.isErr()) {
                    stepError(index, "could not release " + step.action, up.error());
                    return;
                }
                entry["action"] = step.action;
                entry["outcome"] = "done";
                break;
            }
            case Kind::wait_until: {
                // Checked before the first frame, so a condition that already
                // holds costs nothing, and after each frame up to the limit.
                int waited = 0;
                while (true) {
                    const auto judged = judge(index, entry);
                    if (!judged) return;
                    if (judged->held) break;
                    if (waited >= step.frames) {
                        fail("assertion", index, "never_held",
                             "Step " + std::to_string(index) +
                                 " (wait_until) waited " + std::to_string(step.frames) +
                                 " frames and its condition never held: " + m_lastSentence + ".");
                        entry["frames_waited"] = waited;
                        entry["at_frame"] = m_frames;
                        return;
                    }
                    if (!driveFrames(index, 1)) return;
                    ++waited;
                }
                entry["frames_waited"] = waited;
                entry["outcome"] = "done";
                break;
            }
            case Kind::assert_value: {
                const auto judged = judge(index, entry);
                if (!judged) return;
                if (!judged->held) {
                    fail("assertion", index, judged->reason,
                         "Step " + std::to_string(index) + " (assert) did not hold: " +
                             m_lastSentence + ".");
                    entry["at_frame"] = m_frames;
                    return;
                }
                entry["outcome"] = "held";
                break;
            }
            case Kind::assert_output:
                if (!checkOutput(index, entry)) return;
                break;
            case Kind::capture: {
                auto captured = guarded([&] { return m_driver.capture(index); });
                if (captured.isErr()) {
                    stepError(index, "could not capture the frame", captured.error());
                    return;
                }
                entry["capture"] = captured.value();
                m_captures.push_back(captured.value());
                entry["outcome"] = "done";
                break;
            }
        }
        if (m_frames != frames_before || step.kind == Kind::wait || step.kind == Kind::press) {
            entry["frames"] = m_frames - frames_before;
        }
        entry["at_frame"] = m_frames;
    }

    // Reads the game's engine output from its first record. A line that was
    // evicted before it was read cannot be told apart from one that was never
    // printed, so an absence over a gap is not proved.
    bool checkOutput(size_t index, json& entry) {
        const auto& step = m_spec.steps[index];
        const std::string minimum_level = step.level.empty() ? "debug" : step.level;
        constexpr int kMaxPages = 40;
        uint64_t cursor = 0;
        bool dropped = false;
        bool complete = false;
        json matched;
        for (int page = 0; page < kMaxPages; ++page) {
            auto read = guarded([&] { return m_driver.readOutput(cursor, minimum_level); });
            if (read.isErr()) return stepError(index, "could not read the game's output", read.error());
            const auto& body = read.value();
            if (page == 0 && body.value("dropped_before_cursor", false)) dropped = true;
            const auto records = body.value("records", json::array());
            for (const auto& record : records) {
                const auto message = record.value("message", std::string());
                if (step.text.empty() || message.find(step.text) != std::string::npos) {
                    matched = {{"sequence", record.value("sequence", json())},
                               {"level", record.value("level", std::string())},
                               {"message", message.size() > 240 ? message.substr(0, 237) + "..."
                                                                : message}};
                    break;
                }
            }
            if (!matched.is_null()) break;
            const auto next = body.value("next_cursor", cursor);
            if (!body.value("has_more", false) || next <= cursor) {
                complete = true;
                break;
            }
            cursor = next;
        }
        std::string looked_for = step.text.empty() ? std::string("any line")
                                                   : "a line containing \"" + step.text + "\"";
        if (!step.level.empty()) looked_for += " at " + step.level + " or above";
        if (!matched.is_null()) entry["matched"] = matched;
        entry["output_dropped"] = dropped;
        if (!step.absent) {
            if (matched.is_null()) {
                fail("assertion", index, "not_seen",
                     "Step " + std::to_string(index) + " (assert_output) found no " + looked_for +
                         " in the game's output" +
                         (dropped ? ", and some of its earliest output was evicted before it was "
                                    "read"
                                  : "") +
                         ".");
                entry["at_frame"] = m_frames;
                return false;
            }
        } else if (!matched.is_null()) {
            fail("assertion", index, "seen",
                 "Step " + std::to_string(index) + " (assert_output) requires no " + looked_for +
                     ", and the game printed one: " + matched.value("message", std::string()) + ".");
            entry["at_frame"] = m_frames;
            return false;
        } else if (dropped || !complete) {
            fail("assertion", index, "output_unread",
                 "Step " + std::to_string(index) + " (assert_output) cannot show the game printed "
                 "no " + looked_for + ": part of its output was " +
                     (dropped ? std::string("evicted before it was read") : "past what one step reads") +
                     ".");
            entry["at_frame"] = m_frames;
            return false;
        }
        entry["outcome"] = "held";
        return true;
    }

    ScenarioOutcome finish(json teardown, bool torn_down) {
        size_t total = 0, held = 0, failed = 0, not_run = 0, proving_held = 0;
        bool every_step_ran = true;
        for (size_t index = 0; index < m_spec.steps.size(); ++index) {
            const auto outcome = m_steps[index].value("outcome", std::string());
            // Finished, not merely started: done for an action, held for a check.
            if (outcome != "done" && outcome != "held") every_step_ran = false;
            if (!m_spec.steps[index].isAssertion()) continue;
            ++total;
            if (outcome == "held") {
                ++held;
                if (m_spec.steps[index].provesSomething()) ++proving_held;
            } else if (outcome == "failed") {
                ++failed;
            } else {
                ++not_run;
            }
        }
        // P7: a run that proved nothing cannot report a pass, whatever led
        // here. parseScenario refuses such a scenario before it runs; this is
        // the same rule where the verdict is made.
        if (!m_failure && (proving_held == 0 || !every_step_ran)) {
            fail("run", std::nullopt, "nothing_proved",
                 "The run ended without every step running and an assertion holding, so it "
                 "proves nothing.");
        }
        ScenarioOutcome outcome;
        outcome.verdict = !m_failure ? "pass" : (m_failure->stage == "assertion" ? "fail" : "error");
        outcome.failure = m_failure;

        std::string summary;
        if (outcome.verdict == "pass") {
            summary = "Every assertion held (" + std::to_string(held) + " of " +
                      std::to_string(total) + ") over " + std::to_string(m_frames) +
                      " frames, and the game was stopped.";
        } else {
            summary = m_failure->message;
            if (m_failure->stage != "teardown") {
                summary += torn_down ? " The game was stopped."
                                     : " The game may still be running: see teardown.";
            }
        }

        json failure = nullptr;
        if (m_failure) {
            failure = {{"stage", m_failure->stage},
                       {"reason", m_failure->reason},
                       {"message", m_failure->message}};
            if (m_failure->step) {
                failure["step"] = *m_failure->step;
                failure["kind"] = kindOf(m_spec.steps[*m_failure->step]);
            }
            if (m_failure->cause) failure["cause"] = describeError(*m_failure->cause);
        }
        outcome.report = {
            {"scenario", m_spec.name},
            {"verdict", outcome.verdict},
            {"summary", summary},
            {"scene_path", m_spec.scene_path},
            {"headless", m_spec.headless},
            {"frames", m_frames},
            {"assertions", {{"total", total}, {"held", held}, {"failed", failed}, {"not_run", not_run}}},
            {"steps", m_steps},
            {"failure", failure},
            {"game", m_game},
            {"teardown", std::move(teardown)},
            {"captures", m_captures},
        };
        return outcome;
    }

    const ScenarioSpec& m_spec;
    IScenarioDriver& m_driver;
    json m_steps;
    json m_game = nullptr;
    json m_captures = json::array();
    int64_t m_frames{0};
    std::optional<ScenarioFailure> m_failure;
    std::string m_lastSentence;
};

// --- Files -------------------------------------------------------------------

std::string lowerExtension(const std::string& path) {
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// What a scenario's run depends on and this follows. Textures, audio and
// meshes are left out: they change what a frame looks like, and a capture step
// records the frame it took.
bool followed(const std::string& path) {
    static const std::set<std::string> extensions = {"gd",  "cs",       "tscn",       "scn",
                                                     "tres", "res",     "gdshader",   "gdshaderinc"};
    return extensions.count(lowerExtension(path)) != 0;
}

// The file a res:// path names under `root`, or nothing for one that walks out.
std::optional<fs::path> fileUnder(const fs::path& root, const std::string& path) {
    if (path == "project.godot") return root / "project.godot";
    if (path.rfind("res://", 0) != 0) return std::nullopt;
    fs::path relative;
    try {
        relative = paths::projectPathFromUtf8(path.substr(6)).lexically_normal();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        *relative.begin() == "..") {
        return std::nullopt;
    }
    return root / relative;
}

std::optional<std::string> readBytes(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error) || error) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> referencesIn(const std::string& path, const std::string& bytes) {
    std::vector<std::string> found;
    const auto extension = lowerExtension(path);
    if (extension == "tscn" || extension == "tres") {
        static const std::regex ext_resource(R"re(\[ext_resource[^\]]*path="(res://[^"]+)")re");
        for (std::sregex_iterator it(bytes.begin(), bytes.end(), ext_resource), end; it != end; ++it) {
            found.push_back((*it)[1].str());
        }
    } else if (extension == "gd" || extension == "cs" || extension == "gdshader" ||
               extension == "gdshaderinc") {
        // Every res:// string literal: preload, load, a scene to change to, a
        // shader include. Over-including costs a false stale; missing one
        // costs a pass that is true for code it never looked at.
        static const std::regex literal(R"re(["'](res://[^"'\r\n]+)["'])re");
        for (std::sregex_iterator it(bytes.begin(), bytes.end(), literal), end; it != end; ++it) {
            found.push_back((*it)[1].str());
        }
    }
    return found;
}

// The scripts a project names with class_name, so a script that uses a class
// by name reaches the file it is in. Tests reach the code under test this way
// far more often than through a res:// literal. addons/ is left out: a
// framework's or plugin's own classes would fill the record with files the
// project did not write.
std::map<std::string, std::string> classNameIndex(const fs::path& root) {
    static const std::regex declared(R"(^[ \t]*class_name[ \t]+([A-Za-z_][A-Za-z0-9_]*))");
    std::map<std::string, std::string> index;
    std::error_code error;
    size_t visited = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end && visited < 10000; it.increment(error)) {
        const auto name = paths::projectPathToUtf8(it->path().filename());
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            if (it.depth() == 0 && (name == "addons" || name.rfind("build", 0) == 0)) {
                it.disable_recursion_pending();
            } else if (!name.empty() && name.front() == '.') {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->path().extension() != ".gd") continue;
        ++visited;
        const auto bytes = readBytes(it->path());
        if (!bytes || bytes->size() > 512 * 1024) continue;
        // Line by line: std::regex::multiline is not in every standard
        // library this builds with.
        std::istringstream lines(*bytes);
        std::string line;
        while (std::getline(lines, line)) {
            std::smatch match;
            if (std::regex_search(line, match, declared)) {
                const auto relative = it->path().lexically_relative(root);
                index.emplace(match[1].str(), "res://" + paths::projectPathToUtf8(relative.generic_string()));
                break;
            }
        }
    }
    return index;
}

std::vector<std::string> classesUsedIn(const std::string& bytes, const std::map<std::string, std::string>& index) {
    std::vector<std::string> used;
    if (index.empty()) return used;
    static const std::regex identifier(R"([A-Za-z_][A-Za-z0-9_]*)");
    std::set<std::string> seen;
    for (std::sregex_iterator it(bytes.begin(), bytes.end(), identifier), end; it != end; ++it) {
        const auto word = it->str();
        const auto found = index.find(word);
        if (found != index.end() && seen.insert(word).second) used.push_back(found->second);
    }
    return used;
}

std::string currentDigest(const fs::path& root, const std::string& path) {
    const auto file = fileUnder(root, path);
    if (!file) return {};
    const auto bytes = readBytes(*file);
    return bytes ? sha256Hex(*bytes) : std::string();
}

}  // namespace

// --- Public ------------------------------------------------------------------

const char* ScenarioStep::kindName() const {
    switch (kind) {
        case Kind::wait: return "wait";
        case Kind::wait_until: return "wait_until";
        case Kind::press: return "press";
        case Kind::assert_value: return "assert";
        case Kind::assert_output: return "assert_output";
        case Kind::capture: return "capture";
    }
    return "wait";
}

bool ScenarioStep::isAssertion() const {
    return kind == Kind::assert_value || kind == Kind::assert_output;
}

bool ScenarioStep::provesSomething() const {
    return kind == Kind::assert_value || (kind == Kind::assert_output && !absent);
}

Result<ScenarioSpec> parseScenario(const json& arguments) {
    if (!arguments.is_object()) {
        return refuse("runtime_run_scenario arguments must be an object.", "arguments");
    }
    static const std::set<std::string> known = {"name", "scene_path", "steps", "headless",
                                                "timeout_seconds"};
    for (const auto& [key, value] : arguments.items()) {
        (void)value;
        if (!known.count(key)) return refuse("Unknown argument: " + key + ".", key);
    }
    ScenarioSpec spec;
    if (!arguments.contains("name") || !arguments["name"].is_string() ||
        !validName(arguments["name"].get<std::string>())) {
        return refuse("name is required: 1 to 64 letters, digits, '_' or '-', starting with a "
                      "letter or digit. The run is recorded under it.",
                      "name");
    }
    spec.name = arguments["name"].get<std::string>();
    if (!arguments.contains("scene_path") || !arguments["scene_path"].is_string() ||
        arguments["scene_path"].get<std::string>().rfind("res://", 0) != 0 ||
        !(endsWith(arguments["scene_path"].get<std::string>(), ".tscn") ||
          endsWith(arguments["scene_path"].get<std::string>(), ".scn"))) {
        return refuse("scene_path is required: the res:// path of the .tscn or .scn the game "
                      "starts in.",
                      "scene_path");
    }
    spec.scene_path = arguments["scene_path"].get<std::string>();
    if (arguments.contains("headless")) {
        if (!arguments["headless"].is_boolean()) return refuse("headless must be true or false.", "headless");
        spec.headless = arguments["headless"].get<bool>();
    }
    if (arguments.contains("timeout_seconds")) {
        if (!integerIn(arguments["timeout_seconds"], kMinScenarioTimeoutSeconds,
                       kMaxScenarioTimeoutSeconds)) {
            return refuse("timeout_seconds must be an integer from " +
                              std::to_string(kMinScenarioTimeoutSeconds) + " to " +
                              std::to_string(kMaxScenarioTimeoutSeconds) + ".",
                          "timeout_seconds");
        }
        spec.timeout_seconds = static_cast<int>(arguments["timeout_seconds"].get<int64_t>());
    }
    if (!arguments.contains("steps") || !arguments["steps"].is_array() ||
        arguments["steps"].empty() || arguments["steps"].size() > kMaxScenarioSteps) {
        return refuse("steps is required: an array of 1 to " + std::to_string(kMaxScenarioSteps) +
                          " steps.",
                      "steps");
    }
    int64_t frames = 0;
    for (size_t index = 0; index < arguments["steps"].size(); ++index) {
        auto parsed = parseStep(arguments["steps"][index], index);
        if (parsed.isErr()) return parsed.error();
        frames += parsed.value().frames;
        spec.steps.push_back(std::move(parsed.value()));
    }
    if (frames > kMaxScenarioFrames) {
        return refuse("The steps run up to " + std::to_string(frames) + " frames, past the " +
                          std::to_string(kMaxScenarioFrames) + " one scenario may run.",
                      "steps");
    }
    const bool proves = std::any_of(spec.steps.begin(), spec.steps.end(),
                                    [](const ScenarioStep& step) { return step.provesSomething(); });
    if (!proves) {
        const bool any_assertion = std::any_of(spec.steps.begin(), spec.steps.end(),
                                               [](const ScenarioStep& step) { return step.isAssertion(); });
        return refuse(any_assertion
                          ? "Every assertion in this scenario checks that something did not "
                            "happen, which a game that did nothing also passes, so it was not "
                            "run. Add an assert step, or an assert_output step that requires a "
                            "line."
                          : "A scenario with no assertion proves nothing, so it was not run. Add "
                            "an assert step, or an assert_output step that requires a line, "
                            "after the steps that should produce it.",
                      "steps", {{"reason", "no_assertion"}});
    }
    if (spec.headless) {
        for (size_t index = 0; index < spec.steps.size(); ++index) {
            if (spec.steps[index].kind == ScenarioStep::Kind::capture) {
                return refuse("A headless game draws no frame, so " + stepName(index) +
                                  " has nothing to capture. Pass headless: false, or remove the "
                                  "capture step.",
                              "headless", {{"retry_with", {{"headless", false}}}, {"step", index}});
            }
        }
    }
    return spec;
}

ScenarioOutcome runScenario(const ScenarioSpec& spec, IScenarioDriver& driver) {
    return Run(spec, driver).execute();
}

ValueJudgement judgeValue(const json& value, const std::optional<double>& minimum,
                          const std::optional<double>& maximum) {
    if (minimum || maximum) {
        if (value.is_null()) return {false, "no_value"};
        if (!value.is_number()) return {false, "not_a_number"};
        const double number = value.get<double>();
        if (!std::isfinite(number)) return {false, "not_finite"};
        if (minimum && number < *minimum) return {false, "below_minimum"};
        if (maximum && number > *maximum) return {false, "above_maximum"};
        return {true, {}};
    }
    if (value.is_boolean()) return value.get<bool>() ? ValueJudgement{true, {}} : ValueJudgement{false, "false"};
    return {false, value.is_null() ? "no_value" : "not_a_boolean"};
}

ScenarioFiles collectProofFiles(const fs::path& project_root, const std::vector<std::string>& seeds) {
    ScenarioFiles collected;
    std::set<std::string> seen;
    std::set<std::string> seeded(seeds.begin(), seeds.end());
    std::deque<std::string> pending = {"project.godot"};
    // Autoloads run before anything a run starts, whatever it starts.
    auto autoloads = offline::readProjectAutoloads(project_root);
    if (autoloads.isOk()) {
        for (const auto& autoload : autoloads.value()) {
            pending.push_back(autoload.path);
            seeded.insert(autoload.path);
        }
    }
    for (const auto& seed : seeds) pending.push_back(seed);
    const auto classes = classNameIndex(project_root);
    while (!pending.empty()) {
        const auto path = pending.front();
        pending.pop_front();
        if (!seen.insert(path).second) continue;
        // A file the run names is recorded whatever it is, a .gutconfig.json
        // included; what it reaches is followed only into code and scenes.
        if (path != "project.godot" && !seeded.count(path) && !followed(path)) continue;
        // What a run names is recorded wherever it is; what those files reach
        // under addons/ is the framework's or a plugin's, not the project's.
        if (!seeded.count(path) && path.rfind("res://addons/", 0) == 0) continue;
        const auto file = fileUnder(project_root, path);
        if (!file) continue;
        if (collected.files.size() >= kMaxScenarioFiles) {
            collected.truncated = true;
            break;
        }
        const auto bytes = readBytes(*file);
        collected.files.push_back({path, bytes ? sha256Hex(*bytes) : std::string()});
        if (!bytes) continue;
        for (auto& reference : referencesIn(path, *bytes)) {
            if (!seen.count(reference)) pending.push_back(std::move(reference));
        }
        if (lowerExtension(path) == "gd") {
            for (auto& reference : classesUsedIn(*bytes, classes)) {
                if (!seen.count(reference)) pending.push_back(std::move(reference));
            }
        }
    }
    std::sort(collected.files.begin(), collected.files.end(),
              [](const ScenarioFile& left, const ScenarioFile& right) { return left.path < right.path; });
    return collected;
}

ScenarioFiles collectScenarioFiles(const fs::path& project_root, const std::string& scene_path) {
    return collectProofFiles(project_root, {scene_path});
}

std::vector<ScenarioFile> changedScenarioFiles(const fs::path& project_root,
                                               const std::vector<ScenarioFile>& recorded) {
    std::vector<ScenarioFile> changed;
    for (const auto& file : recorded) {
        auto now = currentDigest(project_root, file.path);
        if (now != file.sha256) changed.push_back({file.path, std::move(now)});
    }
    return changed;
}

json scenarioFilesJson(const ScenarioFiles& files) {
    json listed = json::array();
    for (const auto& file : files.files) {
        listed.push_back({{"path", file.path},
                          {"sha256", file.sha256.empty() ? json(nullptr) : json(file.sha256)}});
    }
    return listed;
}

std::vector<ScenarioFile> scenarioFilesFromJson(const json& files, bool* malformed) {
    std::vector<ScenarioFile> read;
    bool bad = !files.is_array();
    if (files.is_array()) {
        for (const auto& entry : files) {
            if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string()) {
                bad = true;
                continue;
            }
            const auto digest = entry.value("sha256", json());
            if (!digest.is_null() && !digest.is_string()) {
                bad = true;
                continue;
            }
            read.push_back({entry["path"].get<std::string>(),
                            digest.is_string() ? digest.get<std::string>() : std::string()});
        }
    }
    if (malformed) *malformed = bad;
    return read;
}

fs::path scenarioRecordDirectory(const fs::path& project_root) {
    return project_root / ".didi" / "scenarios";
}

json scenarioRecordsView(const fs::path& project_root) {
    constexpr size_t kMaxListed = 64;
    const auto directory = scenarioRecordDirectory(project_root);
    json scenarios = json::array();
    std::error_code error;
    std::vector<fs::path> records;
    if (fs::is_directory(directory, error) && !error) {
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            std::error_code type_error;
            if (it->is_regular_file(type_error) && !type_error && it->path().extension() == ".json") {
                records.push_back(it->path());
            }
        }
    }
    for (const auto& path : records) {
        const auto name = paths::projectPathToUtf8(path.stem());
        json listed = {{"name", name},
                       {"record", ".didi/scenarios/" + paths::projectPathToUtf8(path.filename())}};
        const auto bytes = readBytes(path);
        json record = bytes ? json::parse(*bytes, nullptr, false) : json();
        if (!record.is_object()) {
            listed["unreadable"] = true;
            scenarios.push_back(std::move(listed));
            continue;
        }
        bool malformed = false;
        const auto files = scenarioFilesFromJson(record.value("files", json()), &malformed);
        const auto changed = changedScenarioFiles(project_root, files);
        json changed_paths = json::array();
        for (const auto& file : changed) changed_paths.push_back(file.path);
        // A scenario, or a run of the project's tests (project_run_tests),
        // which records under the same names.
        const std::string kind = record.value("kind", std::string("scenario"));
        listed["kind"] = kind;
        listed["verdict"] = record.value("verdict", json());
        listed["ran_at"] = record.value("ran_at", json());
        if (kind == "tests") {
            listed["framework"] = record.value("framework", json());
            listed["paths"] = record.value("paths", json());
            listed["counts"] = record.value("counts", json());
        } else {
            listed["scene_path"] = record.value("scene_path", json());
            listed["assertions"] = record.value("assertions", json());
            listed["frames"] = record.value("frames", json());
        }
        // A record that names no files cannot say what it was true for, so it
        // is stale rather than fresh by default.
        listed["stale"] = !changed.empty() || files.empty() || malformed ||
                          record.value("stale", false);
        listed["changed_files"] = std::move(changed_paths);
        listed["files_truncated"] = record.value("files_truncated", false);
        scenarios.push_back(std::move(listed));
    }
    std::sort(scenarios.begin(), scenarios.end(), [](const json& left, const json& right) {
        const auto a = left.value("ran_at", json());
        const auto b = right.value("ran_at", json());
        const std::string left_at = a.is_string() ? a.get<std::string>() : std::string();
        const std::string right_at = b.is_string() ? b.get<std::string>() : std::string();
        if (left_at != right_at) return left_at > right_at;
        return left.value("name", std::string()) < right.value("name", std::string());
    });
    const bool truncated = scenarios.size() > kMaxListed;
    if (truncated) scenarios.erase(scenarios.begin() + kMaxListed, scenarios.end());
    return {{"scenarios", std::move(scenarios)},
            {"count", records.size()},
            {"truncated", truncated},
            {"directory", ".didi/scenarios"}};
}

}  // namespace didi::runtime
