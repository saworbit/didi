// runtime_run_scenario (Q9 in docs/BUILD_QUEUE.md, principle P7).
//
// The run loop and its rules live in src/runtime/scenario_runner.cpp. This is
// the half that touches the world: it starts the game with runtime_launch's
// own process start, drives it over a route of its own through the bridge
// methods a game session already admits, writes the record under .didi, and
// turns a run that did not pass into an error a caller cannot read as success.

#include "didi/common/atomic_write.hpp"
#include "didi/common/base64.hpp"
#include "didi/common/cancellation.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/sha256.hpp"
#include "didi/common/version.hpp"
#include "didi/mcp/jobs.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/offline/project_settings_file.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/runtime/scenario_runner.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/tools/game_launch.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace didi {
namespace mcp {

namespace {

namespace fs = std::filesystem;

int64_t wallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// One stepped frame is one physics tick when the game runs at the project's
// tick rate. Without a fixed rate a frame runs as many ticks as its wall-clock
// time covers: 5 to 6 for a 100 ms frame on 4.5.1, 4.6.2 and 4.7.2, measured.
int fixedFramesPerSecond(const fs::path& root) {
    auto setting = offline::readProjectSetting(root, "physics/common/physics_ticks_per_second");
    if (setting.isErr() || !setting.value().existed) return 60;
    try {
        const int ticks = std::stoi(setting.value().literal);
        return ticks >= 1 && ticks <= 1000 ? ticks : 60;
    } catch (const std::exception&) {
        return 60;
    }
}

class LiveScenarioDriver : public runtime::IScenarioDriver {
public:
    LiveScenarioDriver(const runtime::ScenarioSpec& spec,
                       std::shared_ptr<runtime::IRuntimeSessionClient> sessions,
                       std::shared_ptr<ipc::IIpcClient> router, fs::path root, int fixed_fps)
        : m_spec(spec),
          m_sessions(std::move(sessions)),
          m_router(std::move(router)),
          m_root(std::move(root)),
          m_fixedFps(fixed_fps),
          m_started(std::chrono::steady_clock::now()) {}

    Result<json> launch() override {
        if (!m_sessions) {
            return Error(503, "This server cannot reach game sessions, so it cannot drive a game.",
                         {{"code", "not_connected"}, {"retryable", false}});
        }
        const int64_t launched_at_ms = wallClockMs();
        // A fixed rate also unthrottles the game: Godot ignores --max-fps
        // beside --fixed-fps (120 frames in 1 ms on 4.7.2 with both), so a
        // headless game runs as fast as it can until the pause lands. That
        // was 1 to 2947 frames before the first step here, which is why a
        // scenario syncs with wait_until rather than a guessed wait.
        const std::vector<std::string> extra = {"--fixed-fps", std::to_string(m_fixedFps)};
        auto started = offline::TestRunner::runSession(m_spec.scene_path, m_spec.timeout_seconds,
                                                       m_spec.headless, false, extra, true);
        if (started.launch_failed || started.pid == 0) {
            const auto cause = started.launch_error.empty() ? std::string("the process did not start")
                                                            : started.launch_error;
            return Error(503,
                         "Godot could not be started: " + cause + ". Engine tried: " +
                             (started.engine_executable.empty() ? std::string("none found")
                                                                : started.engine_executable) +
                             ". Set GODOT_BIN to a Godot executable.",
                         {{"code", "engine_unavailable"},
                          {"engine_executable", started.engine_executable.empty()
                                                    ? json(nullptr)
                                                    : json(started.engine_executable)},
                          {"retryable", false}});
        }
        m_spawnedPid = started.pid;
        m_engineExecutable = started.engine_executable;

        // Half the run's time at most, so a game that is slow to publish
        // leaves time for the steps it was started for.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(std::max(5, m_spec.timeout_seconds / 2));
        const json published = awaitLaunchedGameSession(m_sessions, m_spawnedPid, launched_at_ms, deadline);
        if (published.is_null()) {
            return Error(504,
                         "Process " + std::to_string(m_spawnedPid) +
                             " was started and published no game session in time. The Didi addon "
                             "may not be enabled in this project, or the scene failed to load.",
                         {{"code", "timeout"}, {"field", "timeout_seconds"}, {"retryable", true}});
        }
        m_sessionId = published.value("session_id", std::string());
        m_gamePid = published.value("pid", m_spawnedPid);

        // A route of its own, so the caller's selection never moves: the
        // editor it was working in is still the one its next call reaches.
        auto opened = m_sessions->openSessionRoute(m_sessionId);
        if (opened.isErr()) return opened.error();
        m_routeOpen = true;
        m_lease = runtime::acquireRuntimeRouteLeaseFor(m_router, m_sessionId);
        if (!m_lease) {
            return Error(503, "The game published session " + m_sessionId +
                                  " and no route to it could be held.",
                         {{"code", "not_connected"}, {"retryable", true}});
        }
        auto paused = send("runtime.setPaused", {{"paused", true}}, 10000, false);
        if (paused.isErr()) return paused.error();
        if (!paused.value().value("paused", false)) {
            return Error(500, "The game did not report itself paused, so its frames cannot be "
                              "counted from here.");
        }

        json game = {{"pid", m_gamePid},
                     {"session_id", m_sessionId},
                     {"engine_version", published.value("engine_version", json())},
                     {"engine_executable", m_engineExecutable},
                     {"build_id", published.value("build_id", json())},
                     {"server_build_id", kBuildId},
                     {"fixed_fps", m_fixedFps},
                     {"headless", m_spec.headless}};
        if (published.value("build_id", std::string()) != kBuildId) {
            game["bridge_build_matches"] = false;
        }
        return game;
    }

    Result<json> step(int frames) override {
        // Generous: a headless game at a fixed rate runs a frame in well under
        // a millisecond, and a software-rendered one can take most of a second.
        return send("runtime.step", {{"frames", frames}}, 15000 + frames * 1000, false);
    }

    Result<json> press(const std::string& action, bool pressed) override {
        auto answer = send("runtime.injectInput",
                           {{"events", json::array({{{"type", "action"},
                                                     {"action_name", action},
                                                     {"pressed", pressed}}})}},
                           10000, false);
        if (answer.isErr()) return answer;
        // Held for the next frame that runs, which is what makes a press land
        // on a frame this run counts. Delivered at once means the game was not
        // paused, so the frame it landed in is not one this run can name.
        if (answer.value().value("delivery", std::string()) != "next_unpaused_frame") {
            return Error(409,
                         "The game was not paused when the event was sent, so it landed in a frame "
                         "this run did not step. Something in the game unpaused its tree.",
                         {{"code", "game_not_paused"}, {"retryable", false}});
        }
        return answer;
    }

    Result<json> evaluate(const std::string& expression, const std::string& context_node) override {
        json params = {{"expression", expression}, {"timeout_ms", 2000}};
        if (!context_node.empty()) params["context_node"] = context_node;
        return send("runtime.evalGdscript", params, 10000, true);
    }

    Result<json> readOutput(uint64_t cursor, const std::string& minimum_level) override {
        return send("runtime.getOutput",
                    {{"cursor", cursor}, {"limit", 500}, {"minimum_level", minimum_level}}, 10000,
                    true);
    }

    Result<json> capture(size_t step_index) override {
        auto answer = send("vision.captureViewport", json::object(), 20000, false);
        if (answer.isErr()) return answer;
        const auto& frame = answer.value();
        if (!frame.contains("image_base64") || !frame["image_base64"].is_string()) {
            return Error(500, "The capture answered without an image.");
        }
        const auto bytes = base64::decode(frame["image_base64"].get<std::string>());
        const std::string png(bytes.begin(), bytes.end());
        const auto directory = runtime::scenarioRecordDirectory(m_root) / m_spec.name;
        std::error_code error;
        fs::create_directories(directory, error);
        if (error) return Error(500, "The capture directory cannot be created: " + error.message());
        const std::string file = "capture-" + std::to_string(step_index) + ".png";
        auto written = files::writeFileAtomically(directory / file, png);
        if (written.isErr()) return written.error();
        json kept = {{"path", ".didi/scenarios/" + m_spec.name + "/" + file},
                     {"sha256", sha256Hex(png)},
                     {"bytes", png.size()}};
        for (const char* key : {"width", "height", "camera_identifier"}) {
            if (frame.contains(key)) kept[key] = frame[key];
        }
        return kept;
    }

    Result<json> teardown() override {
        json report = {{"started", m_spawnedPid != 0}};
        if (m_spawnedPid == 0) return report;
        if (m_sessionId.empty()) {
            // Nothing published, so there is no route to ask it to stop on.
            return Error(409,
                         "Process " + std::to_string(m_spawnedPid) +
                             " was started and never published a session, so it could not be "
                             "asked to stop. It may still be running.",
                         {{"code", "teardown_failed"}, {"pid", m_spawnedPid}});
        }
        report["session_id"] = m_sessionId;
        report["pid"] = m_gamePid;
        bool asked = false;
        if (m_lease) {
            auto stopped = send("runtime.stop", {{"exit_code", 0}}, 10000, false);
            asked = stopped.isOk();
            if (stopped.isErr()) report["stop_error"] = stopped.error().message;
            // The exit that follows is this run's doing, not a crash, and a
            // later call that meets it should say so (#595). Only when the
            // game took the request, as runtime_stop records it: a stop that
            // failed may be a game that had already died.
            if (asked) runtime::recordRequestedStop({m_gamePid, m_sessionId, 0, 0});
        }
        report["stop_requested"] = asked;
        if (m_routeOpen) {
            (void)m_sessions->closeSessionRoute(m_sessionId);
            m_routeOpen = false;
        }
        m_lease.reset();

        // Gone means its descriptor is gone, which is how a game that exits
        // retires it, or that the process behind it is dead, which is how a
        // crashed one leaves it: listed, with alive: false.
        const auto waited_from = std::chrono::steady_clock::now();
        const auto deadline = waited_from + std::chrono::seconds(10);
        bool gone = false;
        while (true) {
            auto listed = m_sessions->listSessions(launchedProjectPath());
            if (listed.isOk() && listed.value().is_object() && listed.value()["sessions"].is_array()) {
                gone = true;
                for (const auto& entry : listed.value()["sessions"]) {
                    if (entry.value("session_id", std::string()) == m_sessionId &&
                        entry.value("alive", true)) {
                        gone = false;
                    }
                }
            }
            if (gone || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        report["session_gone"] = gone;
        report["waited_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - waited_from)
                                  .count();
        if (!gone) {
            return Error(409,
                         "Game " + std::to_string(m_gamePid) + " (session " + m_sessionId +
                             ") was still published 10 seconds after it was asked to stop.",
                         {{"code", "teardown_failed"}, {"pid", m_gamePid}, {"session_id", m_sessionId}});
        }
        return report;
    }

    bool cancelled() const override { return cancellationRequested(); }

    int64_t elapsedMs() const override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - m_started)
            .count();
    }

    const std::string& sessionId() const { return m_sessionId; }
    uint64_t gamePid() const { return m_gamePid != 0 ? m_gamePid : m_spawnedPid; }

private:
    Result<json> send(const std::string& method, const json& params, int timeout_ms, bool repeatable) {
        if (!m_lease) {
            return Error(503, "The scenario holds no route to its game.",
                         {{"code", "not_connected"}, {"retryable", false}});
        }
        auto sent = runtime::sendLiveRouteRequest(*m_lease, method, params, timeout_ms, repeatable);
        if (sent.response.isErr()) return sent.response.error();
        const auto& value = sent.response.value();
        // The bridge answers a refusal as an error object; the transport hands
        // most of them over as a failure already, and this catches the rest.
        if (value.is_object() && value.contains("error") && value["error"].is_object()) {
            const auto& error = value["error"];
            return Error(error.value("code", 500), error.value("message", std::string("The game refused")),
                         error.value("data", json()));
        }
        return value;
    }

    const runtime::ScenarioSpec& m_spec;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_sessions;
    std::shared_ptr<ipc::IIpcClient> m_router;
    fs::path m_root;
    int m_fixedFps;
    std::chrono::steady_clock::time_point m_started;
    uint64_t m_spawnedPid{0};
    uint64_t m_gamePid{0};
    std::string m_engineExecutable;
    std::string m_sessionId;
    bool m_routeOpen{false};
    std::optional<runtime::RuntimeRouteLease> m_lease;
};

// The error a run that did not pass answers with. The report stays beside it,
// whole, the way project_apply_changes keeps its verification report beside
// its 422.
json scenarioError(const runtime::ScenarioOutcome& outcome, const LiveScenarioDriver& driver) {
    const auto& failure = *outcome.failure;
    if (failure.stage == "assertion") {
        json data = {{"code", "scenario_failed"}, {"field", "steps"}, {"retryable", false}};
        if (failure.step) data["step"] = *failure.step;
        return {{"code", 422}, {"message", failure.message}, {"data", std::move(data)}};
    }
    if (failure.stage == "teardown") {
        json data = {{"code", "teardown_failed"}, {"retryable", false}};
        if (!driver.sessionId().empty()) {
            data["next_call"] = {{"tool", "runtime_attach_session"},
                                 {"arguments", {{"session_id", driver.sessionId()}}},
                                 {"reason", "The game this scenario started may still be running: "
                                            "attach to it and call runtime_stop."}};
        }
        if (driver.gamePid() != 0) data["pid"] = driver.gamePid();
        return {{"code", 409}, {"message", failure.message}, {"data", std::move(data)}};
    }
    if (failure.stage == "cancelled") {
        return {{"code", 409},
                {"message", failure.message},
                {"data", {{"code", "job_cancelled"}, {"field", "request_id"}, {"retryable", false}}}};
    }
    if (failure.stage == "timeout") {
        return {{"code", 504},
                {"message", failure.message},
                {"data", {{"code", "timeout"}, {"field", "timeout_seconds"}, {"retryable", true}}}};
    }
    // launch, step, run: what the engine or the launch said, with the step it
    // happened at.
    const int status = failure.cause ? failure.cause->code : 500;
    json data = failure.cause && failure.cause->data.is_object() ? failure.cause->data : json::object();
    // The route's own framing describes the game, which failure.cause keeps
    // whole; the envelope keeps what says what to do.
    data.erase("session");
    data.erase("execution_mode");
    if (failure.step) {
        data["step"] = *failure.step;
        // The step is what the caller changes, unless the engine already named
        // a fix of its own.
        if (!data.contains("field") && !data.contains("next_call") && !data.contains("retry_with")) {
            data["field"] = "steps";
        }
    }
    data["stage"] = failure.stage;
    return {{"code", status}, {"message", failure.message}, {"data", std::move(data)}};
}

}  // namespace

CallToolResult handleRuntimeRunScenario(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    auto spec = runtime::parseScenario(args);
    if (spec.isErr()) return CallToolResult::fromError(spec.error());

    auto scene = paths::resolveProjectFile(spec.value().scene_path);
    if (scene.isErr()) {
        return CallToolResult::errorJson(
            404, "No resource at " + spec.value().scene_path + ": " + scene.error().message + ".",
            {{"code", "not_found"}, {"retryable", false}});
    }
    std::error_code error;
    const auto root = fs::weakly_canonical(fs::current_path(error), error);
    if (error) return CallToolResult::errorJson(500, "The project directory cannot be read.");

    const auto started_at_ms = wallClockMs();
    const auto ran_against = runtime::collectScenarioFiles(root, spec.value().scene_path);
    // Captures from an earlier run of this scenario are not this run's
    // evidence. The name is checked to be a plain file name, so this stays
    // inside .didi/scenarios.
    fs::remove_all(runtime::scenarioRecordDirectory(root) / spec.value().name, error);

    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    LiveScenarioDriver driver(spec.value(), sessions, ipc, root, fixedFramesPerSecond(root));
    const auto outcome = runtime::runScenario(spec.value(), driver);

    json report = outcome.report;
    report["files"] = runtime::scenarioFilesJson(ran_against);
    report["files_truncated"] = ran_against.truncated;
    // A file changed while the run was going is a pass for code that is no
    // longer there, which is what stale means.
    const auto changed = runtime::changedScenarioFiles(root, ran_against.files);
    json changed_paths = json::array();
    for (const auto& file : changed) changed_paths.push_back(file.path);
    report["stale"] = !changed.empty();
    if (!changed.empty()) report["changed_files"] = std::move(changed_paths);
    report["ran_at"] = isoTimestamp(started_at_ms);
    report["duration_ms"] = wallClockMs() - started_at_ms;

    const auto record_path = runtime::scenarioRecordDirectory(root) / (spec.value().name + ".json");
    fs::create_directories(record_path.parent_path(), error);
    auto written = error ? Result<void>(Error(500, error.message()))
                         : files::writeFileAtomically(
                               record_path, report.dump(2, ' ', false, json::error_handler_t::replace));
    if (written.isOk()) {
        report["record"] = ".didi/scenarios/" + spec.value().name + ".json";
    } else {
        report["record"] = nullptr;
        report["record_error"] = "The record could not be written, so godot://project/scenarios "
                                 "will not list this run: " + written.error().message;
    }

    if (outcome.verdict == "pass") return CallToolResult::successJson(report);
    report["error"] = scenarioError(outcome, driver);
    auto result = CallToolResult::successJson(report);
    result.isError = true;
    return result;
}

}  // namespace mcp
}  // namespace didi
