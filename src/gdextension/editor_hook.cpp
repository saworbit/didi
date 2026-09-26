#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/engine_diagnostics.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/expression_sandbox.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/types.hpp"
#include <unordered_set>

namespace didi {
namespace godot {

namespace {

std::string sessionKindName(std::optional<runtime::SessionKind> kind) {
    if (!kind.has_value()) return {};
    return *kind == runtime::SessionKind::editor ? "editor" : "game";
}

void fulfillCommand(const std::shared_ptr<std::promise<json>>& promise,
                    const std::shared_ptr<CommandControl>& control,
                    json response) {
    if (promise && control && control->tryClaimResponse()) {
        // Everything the engine printed at warning level or above since this
        // command started, on its answer. One place, so a deferred command --
        // a reimport, a coroutine, a capture -- carries what printed across
        // the frames it waited for as well as a synchronous one does.
        const auto [method, cursor] = control->started();
        if (cursor != 0 && !methodReportsEngineOutputItself(method)) {
            auto& ring = EditorHook::instance().engineOutput();
            const size_t total = ring.countFrom(cursor, "warning");
            if (total > 0) {
                auto records = ring.read(cursor, kMaxEngineDiagnostics, "warning");
                if (records.isOk()) {
                    const auto found = records.value().find("records");
                    if (found != records.value().end()) {
                        attachEngineDiagnostics(response, *found, total);
                    }
                }
            }
        }
        promise->set_value(std::move(response));
    }
}

} // namespace

std::optional<json> validateSessionKindForMethod(
    std::string_view method, std::optional<runtime::SessionKind> session_kind) {
    const auto policy = runtime::livePolicyForMethod(method);
    const auto selected = session_kind == runtime::SessionKind::editor
                              ? std::optional<std::string_view>("editor")
                              : session_kind == runtime::SessionKind::game
                                    ? std::optional<std::string_view>("game")
                                    : std::nullopt;
    if (selected.has_value() && runtime::allowsSessionKind(policy, *selected)) {
        return std::nullopt;
    }
    json allowed = policy == runtime::LiveSessionKindPolicy::editor_only
                       ? json::array({"editor"})
                       : policy == runtime::LiveSessionKindPolicy::game_only
                             ? json::array({"game"})
                             : json::array({"editor", "game"});
    // A sentence, with the identifier kept as a stable code in data. This is
    // the most-reached of the raw identifiers #441 was about: the hook rejects
    // before the bridge is entered, so this is the one a caller actually sees.
    std::string allowed_text;
    for (size_t index = 0; index < allowed.size(); ++index) {
        if (index > 0) allowed_text += " or ";
        allowed_text += allowed[index].get<std::string>();
    }
    const std::string selected_text =
        selected.has_value() ? "a " + std::string(*selected) + " session" : "no session";
    const std::string sentence = std::string(method) + " needs " +
                                 (allowed.size() == 1 ? "an " : "a ") + allowed_text +
                                 " session, and " + selected_text + " is selected.";
    return json{{"error", {{"code", 409},
                            {"message", sentence},
                            {"data", {{"code", "session_kind_rejected"},
                                      {"method", method},
                                      {"selected_session_kind",
                                       selected.has_value() ? json(*selected) : json(nullptr)},
                                      {"allowed_session_kinds", std::move(allowed)},
                                      {"retryable", false}}}}}};
}

EditorHook& EditorHook::instance() {
    static EditorHook s_instance;
    return s_instance;
}

EditorHook::EditorHook() {
    std::weak_ptr<RuntimeLogRing> logs = m_runtimeLogs;
    Logger::instance().setSink([logs](LogLevel level, std::string_view source, std::string_view message) {
        const auto ring = logs.lock();
        if (!ring) return;
        const char* name = "info";
        switch (level) {
            case LogLevel::Debug: name = "debug"; break;
            case LogLevel::Info: name = "info"; break;
            case LogLevel::Warn: name = "warning"; break;
            case LogLevel::Error: name = "error"; break;
            case LogLevel::None: return;
        }
        ring->append(name, source, message);
    });
    DIDI_LOG_INFO("EDITOR_HOOK", "Runtime log stream initialized");
}

EditorHook::~EditorHook() {
    Logger::instance().setSink({});
}

CommandTicket EditorHook::postCommand(const std::string& method, const json& params) {
    auto prom = std::make_shared<std::promise<json>>();
    auto fut = prom->get_future();
    auto control = std::make_shared<CommandControl>();

    EngineCommand cmd;
    cmd.method = method;
    cmd.params = params;
    cmd.response_promise = prom;
    cmd.control = control;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!GodotApi::instance().isLiveReady()) {
            control->markCompleted();
            fulfillCommand(prom, control, {{"error", {{"code", 503},
                                       {"message", "Godot main-loop bridge is not ready"},
                                       {"data", {{"code", "bridge_not_ready"},
                                                 {"retryable", true}}}}}});
            return {std::move(fut), std::move(prom), std::move(control)};
        }
        m_commandQueue.push(std::move(cmd));
    }

    return {std::move(fut), std::move(prom), std::move(control)};
}

std::optional<runtime::SessionKind> EditorHook::sessionKind() const {
    return m_sessionKind;
}

void EditorHook::setSessionKind(const std::string& session_kind) {
    if (session_kind == "editor") m_sessionKind = runtime::SessionKind::editor;
    else if (session_kind == "game") m_sessionKind = runtime::SessionKind::game;
    else m_sessionKind.reset();
}

void EditorHook::processQueue() {
    // EditorFileSystem.reimport_files and RenderingServer.force_draw both
    // re-enter the main-loop callback synchronously. A nested pump must observe
    // progress and nothing else: dequeuing there would run unrelated scene and
    // runtime commands against a tree that is mid-reimport, or one with
    // unrelated nodes hidden for an isolated viewport capture.
    //
    // The editor's own import pass re-enters it the same way, from a pass Didi
    // did not start (#914). Dequeuing there started a second reimport inside
    // the editor's, which opened a second "reimport" progress task and left the
    // engine printing three errors, so a frame inside any import pass is
    // treated as a nested one.
    if (m_pumping || editorImportPassOpen()) {
        processRuntimeStepFrame();
        processAssetReimportFrame();
        processProfilerFrame();
        processInvariantWatchFrame();
        processSceneExplorationFrame();
        GodotBridge::instance().processDeferredReindexFrame();
        processMainScreenCaptureFrame();
        processScriptCallFrame();
        return;
    }
    m_pumping = true;
    struct PumpGuard {
        bool& pumping;
        ~PumpGuard() { pumping = false; }
    } pump_guard{m_pumping};

    struct QueuedCommand {
        EngineCommand command;
        std::optional<json> session_rejection;
    };
    std::vector<QueuedCommand> commands;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        constexpr size_t kMaxCommandsPerFrame = 64;
        while (!m_commandQueue.empty() && commands.size() < kMaxCommandsPerFrame) {
            auto session_rejection = validateSessionKindForMethod(
                m_commandQueue.front().method, m_sessionKind);
            commands.push_back(
                {std::move(m_commandQueue.front()), std::move(session_rejection)});
            m_commandQueue.pop();
        }
    }

    for (auto& queued : commands) {
        auto& cmd = queued.command;
        if (queued.session_rejection.has_value()) {
            fulfillCommand(cmd.response_promise, cmd.control,
                           std::move(*queued.session_rejection));
            continue;
        }
        if (!cmd.control || !cmd.control->tryStart()) {
            fulfillCommand(cmd.response_promise, cmd.control,
                           {{"error", {{"code", 504},
                                        {"message", "Command cancelled before execution"},
                                        // Without a code of its own the 504 floor
                                        // calls this a timeout, and it is not one:
                                        // nothing waited and nothing ran.
                                        {"data", {{"code", "command_cancelled"},
                                                  {"retryable", true}}}}}});
            continue;
        }
        cmd.control->noteStart(cmd.method, engineOutput().nextSequence());
        try {
            if (cmd.method == "asset.reimport") {
                scheduleAssetReimport(cmd.params, cmd.response_promise, cmd.control);
                continue;
            }
            if (cmd.method == "runtime.readProfiler") {
                scheduleProfilerRead(cmd.params, cmd.response_promise, cmd.control);
                continue;
            }
            if (cmd.method == "runtime.watchInvariants") {
                scheduleInvariantWatch(cmd.params, cmd.response_promise, cmd.control);
                continue;
            }
            if (cmd.method == "runtime.exploreScene") {
                scheduleSceneExploration(cmd.params, cmd.response_promise, cmd.control);
                continue;
            }
            if (cmd.method == "runtime.step") {
                if (!cmd.params.is_object() ||
                    (cmd.params.contains("frames") &&
                     !cmd.params["frames"].is_number_integer() &&
                     !cmd.params["frames"].is_number_unsigned())) {
                    cmd.control->markCompleted();
                    fulfillCommand(cmd.response_promise, cmd.control,
                                   {{"error", {{"code", 400},
                                                {"message", "frames must be an integer from 1 to 60"}}}});
                    continue;
                }
                int frames = 1;
                if (cmd.params.contains("frames")) {
                    if ((cmd.params["frames"].is_number_integer() &&
                         (cmd.params["frames"].get<int64_t>() < 1 ||
                          cmd.params["frames"].get<int64_t>() > 60)) ||
                        (cmd.params["frames"].is_number_unsigned() &&
                         (cmd.params["frames"].get<uint64_t>() < 1 ||
                          cmd.params["frames"].get<uint64_t>() > 60))) {
                        cmd.control->markCompleted();
                        fulfillCommand(cmd.response_promise, cmd.control,
                                       {{"error", {{"code", 400},
                                                    {"message", "frames must be an integer from 1 to 60"}}}});
                        continue;
                    }
                    frames = static_cast<int>(cmd.params["frames"].get<uint64_t>());
                }
                scheduleRuntimeStep(frames, cmd.response_promise, cmd.control);
                continue;
            }
            // Every method that takes a frame off an editor viewport, not just
            // the first one that needed it. viewport_diff_capture takes its own
            // comparison capture, so without this it compared the baseline
            // against whatever was last rendered in a viewport with no size and
            // reported bit_identical for a frame that had wholly changed (#568).
            if ((cmd.method == "vision.captureViewport" || cmd.method == "vision.diffViewport" ||
                 cmd.method == "vision.capturePasses") &&
                scheduleMainScreenCapture(cmd.method, cmd.params, cmd.response_promise,
                                          cmd.control)) {
                continue;
            }
            if (cmd.method == "scene.callMethod" &&
                scheduleScriptCall(cmd.params, cmd.response_promise, cmd.control)) {
                continue;
            }
            json result = executeOnMainThread(cmd.method, cmd.params);
            cmd.control->markCompleted();
            fulfillCommand(cmd.response_promise, cmd.control, std::move(result));
        } catch (const std::exception& e) {
            DIDI_LOG_ERROR("EDITOR_HOOK", "Exception executing command: ", cmd.method);
            cmd.control->markCompleted();
            fulfillCommand(cmd.response_promise, cmd.control,
                           {{"error", {{"code", 500},
                                        {"message", e.what()},
                                        {"data", {{"code", "command_threw"},
                                                  {"retryable", false}}}}}});
        }
    }
    processRuntimeStepFrame();
    processAssetReimportFrame();
    processProfilerFrame();
    processInvariantWatchFrame();
    processSceneExplorationFrame();
    GodotBridge::instance().processDeferredReindexFrame();
    processMainScreenCaptureFrame();
    processScriptCallFrame();
    processPendingQuitFrame();
}

void EditorHook::scheduleProfilerRead(
    const json& params,
    const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    auto request = runtime::parseProfilerRequest(params);
    if (request.isErr()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", request.error().code},
                                    {"message", request.error().message}}}});
        return;
    }
    // Availability is the pinned bind existing, checked before any state is
    // published. A zero reading later is a valid sample, not a missing API.
    auto preflight = GodotBridge::instance().preflightPerformanceMonitors();
    if (preflight.isErr()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 501},
                                    {"message", "Performance.get_monitor is unavailable: " +
                                                    preflight.error().message},
                                    {"data", {{"code", "required_bind_unavailable"},
                                              {"retryable", false}}}}}});
        return;
    }
    int sample_count = 0;
    int duration_ms = 0;
    {
        std::lock_guard<std::mutex> lock(m_profilerMutex);
        if (m_pendingProfilerRead.has_value()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 423},
                                        {"message", "A profiler collection is already active"},
                                        {"data", {{"code", "profiler_read_active"},
                                                  {"retryable", true}}}}}});
            return;
        }
        sample_count = request.value().sample_count;
        duration_ms = request.value().duration_ms;
        m_pendingProfilerRead = PendingProfilerRead{
            runtime::ProfilerCollector(std::move(request.value())),
            std::chrono::steady_clock::now(), true, promise, control};
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Scheduled profiler collection of ", sample_count,
                  " sample(s) over ", duration_ms, " ms");
}

void EditorHook::processProfilerFrame() {
    // The engine call happens outside the lock. Performance.get_monitor does
    // not re-enter the pump today, but the step and reimport paths keep the
    // same discipline for the same reason: a nested callback that reaches this
    // function must never find the mutex held by its own thread.
    std::vector<int64_t> monitors;
    std::shared_ptr<CommandControl> sampling_for;
    int64_t elapsed = 0;
    {
        std::lock_guard<std::mutex> lock(m_profilerMutex);
        if (!m_pendingProfilerRead.has_value()) return;
        auto& pending = *m_pendingProfilerRead;
        if (pending.awaiting_next_callback) {
            // The command was dequeued this callback. The first sample belongs
            // to the next one, so the window starts at a frame boundary.
            pending.awaiting_next_callback = false;
            pending.started_at = std::chrono::steady_clock::now();
            return;
        }
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - pending.started_at)
                      .count();
        if (!pending.collector.due(elapsed)) return;
        monitors = pending.collector.monitors();
        sampling_for = pending.control;
    }

    auto reading = GodotBridge::instance().samplePerformanceMonitors(monitors);

    std::optional<PendingProfilerRead> completed;
    json failure;
    {
        std::lock_guard<std::mutex> lock(m_profilerMutex);
        // Shutdown may have taken the read while the engine was being asked.
        if (!m_pendingProfilerRead.has_value() ||
            m_pendingProfilerRead->control != sampling_for) {
            return;
        }
        auto& pending = *m_pendingProfilerRead;
        if (reading.isErr()) {
            failure = {{"error", {{"code", reading.error().code},
                                  {"message", reading.error().message},
                                  {"data", {{"outcome", pending.collector.started()
                                                            ? "unknown_outcome"
                                                            : "not_started"},
                                            {"retryable", false}}}}}};
        } else if (!pending.collector.observe(elapsed, reading.value())) {
            return;
        }
        completed = std::move(m_pendingProfilerRead);
        m_pendingProfilerRead.reset();
    }

    completed->control->markCompleted();
    if (!failure.is_null()) {
        fulfillCommand(completed->response_promise, completed->control, std::move(failure));
        return;
    }
    auto response = completed->collector.response();
    response["execution_mode"] = "live";
    response["is_live_engine"] = true;
    response["session_kind"] = sessionKindName(m_sessionKind);
    fulfillCommand(completed->response_promise, completed->control, std::move(response));
}

void EditorHook::scheduleInvariantWatch(
    const json& params, const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    auto request = runtime::parseInvariantWatchRequest(params);
    if (request.isErr()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", request.error().code},
                                    {"message", request.error().message}}}});
        return;
    }
    // A performance invariant reads the same monitors the profiler does, so it
    // needs the same bind, and finding that out on the first frame instead of
    // now would report a watch that started and observed nothing.
    for (const auto& invariant : request.value().invariants) {
        if (invariant.kind != runtime::InvariantKind::performance_between) continue;
        auto preflight = GodotBridge::instance().preflightPerformanceMonitors();
        if (preflight.isErr()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 501},
                                        {"message", "Performance.get_monitor is unavailable: " +
                                                        preflight.error().message},
                                        {"data", {{"code", "required_bind_unavailable"},
                                                  {"retryable", false}}}}}});
            return;
        }
        break;
    }

    int duration_ms = 0;
    size_t invariant_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_invariantMutex);
        if (m_pendingInvariantWatch.has_value()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 423},
                                        {"message", "An invariant watch is already active"},
                                        {"data", {{"code", "invariant_watch_active"},
                                                  {"retryable", true}}}}}});
            return;
        }
        duration_ms = request.value().duration_ms;
        invariant_count = request.value().invariants.size();
        m_pendingInvariantWatch = PendingInvariantWatch{
            runtime::InvariantWatch(std::move(request.value())),
            std::chrono::steady_clock::now(), true, engineOutput().nextSequence(), promise, control};
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Scheduled an invariant watch of ", invariant_count,
                  " condition(s) over ", duration_ms, " ms");
}

void EditorHook::processInvariantWatchFrame() {
    // Same discipline as processProfilerFrame: the engine work happens outside
    // the lock, because a nested callback that reaches this function must never
    // find the mutex held by its own thread.
    std::vector<runtime::InvariantSpec> invariants;
    std::shared_ptr<CommandControl> watching_for;
    uint64_t error_cursor = 0;
    int64_t elapsed = 0;
    {
        std::lock_guard<std::mutex> lock(m_invariantMutex);
        if (!m_pendingInvariantWatch.has_value()) return;
        auto& pending = *m_pendingInvariantWatch;
        if (pending.awaiting_next_callback) {
            // The command was dequeued this callback. The window starts at a
            // frame boundary, the same as the profiler's.
            pending.awaiting_next_callback = false;
            pending.started_at = std::chrono::steady_clock::now();
            pending.error_cursor = engineOutput().nextSequence();
            return;
        }
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - pending.started_at)
                      .count();
        invariants = pending.watch.request().invariants;
        watching_for = pending.control;
        error_cursor = pending.error_cursor;
    }

    runtime::InvariantSample sample;
    sample.readings.resize(invariants.size());

    std::vector<int64_t> monitors;
    std::vector<size_t> monitor_targets;
    for (size_t index = 0; index < invariants.size(); ++index) {
        if (invariants[index].kind != runtime::InvariantKind::performance_between) continue;
        monitors.push_back(runtime::kProfilerMetrics[invariants[index].metric_index].monitor);
        monitor_targets.push_back(index);
    }
    if (!monitors.empty()) {
        auto reading = GodotBridge::instance().samplePerformanceMonitors(monitors);
        for (size_t slot = 0; slot < monitor_targets.size(); ++slot) {
            auto& target = sample.readings[monitor_targets[slot]];
            if (reading.isErr()) {
                target.read_error = reading.error().message;
            } else {
                target.value = reading.value()[slot];
            }
        }
    }

    for (size_t index = 0; index < invariants.size(); ++index) {
        const auto& invariant = invariants[index];
        if (invariant.kind != runtime::InvariantKind::expression_between) continue;
        json params = {{"expression", invariant.expression},
                       // Short on purpose. This runs inside a frame, and an
                       // expression allowed to take a second would be measuring
                       // a game it had itself stopped.
                       {"timeout_ms", 50}};
        if (!invariant.context_node.empty()) params["context_node"] = invariant.context_node;
        const auto evaluated = executeExpression(params, sessionKindName(m_sessionKind));
        auto& target = sample.readings[index];
        if (evaluated.contains("error")) {
            target.read_error = evaluated["error"].value("message", "expression failed");
            continue;
        }
        const json value = evaluated.value("value", json());
        if (value.is_number()) {
            target.value = value.get<double>();
        } else if (value.is_boolean()) {
            // A condition is a number here, and true is one. Saying so keeps a
            // boolean invariant expressible without a second kind.
            target.value = value.get<bool>() ? 1.0 : 0.0;
        } else {
            target.read_error = "expression did not evaluate to a number or a boolean";
        }
    }

    sample.engine_errors = static_cast<int64_t>(
        engineOutput().countFrom(error_cursor, "error"));

    std::optional<PendingInvariantWatch> completed;
    bool violated = false;
    {
        std::lock_guard<std::mutex> lock(m_invariantMutex);
        if (!m_pendingInvariantWatch.has_value() ||
            m_pendingInvariantWatch->control != watching_for) {
            return;
        }
        auto& pending = *m_pendingInvariantWatch;
        if (!pending.watch.observe(elapsed, sample)) return;
        violated = pending.watch.violated();
        completed = std::move(m_pendingInvariantWatch);
        m_pendingInvariantWatch.reset();
    }

    // The pause happens on this frame, which is the frame that broke it. That
    // is the whole difference between a report and a reproduction.
    bool paused = false;
    if (violated && completed->watch.request().pause_on_violation) {
        auto stopped = setLiveSceneTreePaused(true);
        paused = stopped.isOk();
        if (stopped.isErr()) {
            DIDI_LOG_WARN("EDITOR_HOOK", "Could not pause on an invariant violation: ",
                          stopped.error().message);
        }
    }

    completed->control->markCompleted();
    auto response = completed->watch.response(paused);
    response["execution_mode"] = "live";
    response["is_live_engine"] = true;
    response["session_kind"] = sessionKindName(m_sessionKind);
    fulfillCommand(completed->response_promise, completed->control, std::move(response));
}

void EditorHook::scheduleSceneExploration(
    const json& params, const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    auto request = runtime::parseSceneExplorationRequest(params);
    if (request.isErr()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", request.error().code},
                                    {"message", request.error().message}}}});
        return;
    }

    // An action nobody declared dispatches without complaint and moves nothing,
    // so a run driving one would report its own stillness as a stuck interval.
    // Checked here rather than on the first frame, because a run that has
    // already started has already told the caller it was driving the game.
    json action_names = json::array();
    for (const auto& action : request.value().actions) action_names.push_back(action);
    const auto missing = GodotBridge::instance().execute(
        "runtime.missingInputActions", {{"actions", std::move(action_names)}},
        sessionKindName(m_sessionKind));
    if (missing.contains("error")) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       runtime::relayedExplorationRefusal(
                           "Could not check the input actions", missing,
                           "input_action_check_failed"));
        return;
    }
    const auto undefined = missing.value("missing", json::array());
    if (!undefined.empty()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 400},
                                    {"message", "The project's InputMap does not define every action "
                                                "this run was told to press, so it would have driven "
                                                "nothing and reported the stillness as a stuck "
                                                "interval"},
                                    {"data", {{"undefined_actions", undefined}}}}}});
        return;
    }

    // A paused tree does not deliver an injected event to a node that pauses,
    // so every action this run holds would be queued and the whole window would
    // be still because nothing was pressed. That is the finding the check above
    // refuses for an action the InputMap does not declare, one cause along, and
    // the mirror of scheduleRuntimeStep refusing a game that is not paused.
    auto tree_state = executeRuntimeBridge("runtime.getTree",
                                           {{"root_path", "/root"}, {"max_depth", 0}},
                                           sessionKindName(m_sessionKind));
    if (tree_state.contains("error")) {
        control->markCompleted();
        fulfillCommand(promise, control, std::move(tree_state));
        return;
    }
    if (tree_state.value("paused", false)) {
        control->markCompleted();
        fulfillCommand(promise, control, runtime::pausedExplorationRefusal(false));
        return;
    }

    int duration_ms = 0;
    size_t action_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_explorationMutex);
        if (m_pendingSceneExploration.has_value()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 423},
                                        {"message", "A scene exploration is already running"},
                                        {"data", {{"code", "scene_exploration_active"},
                                                  {"retryable", true}}}}}});
            return;
        }
        duration_ms = request.value().duration_ms;
        action_count = request.value().actions.size();
        m_pendingSceneExploration = PendingSceneExploration{
            runtime::SceneExploration(std::move(request.value())),
            std::chrono::steady_clock::now(), true, engineOutput().nextSequence(),
            std::nullopt, -1, promise, control};
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Scheduled a scene exploration pressing ", action_count,
                  " action(s) over ", duration_ms, " ms");
}

void EditorHook::releaseExplorationAction(size_t action_index) {
    std::string action_name;
    {
        std::lock_guard<std::mutex> lock(m_explorationMutex);
        if (!m_pendingSceneExploration.has_value()) return;
        const auto& actions = m_pendingSceneExploration->exploration.request().actions;
        if (action_index >= actions.size()) return;
        action_name = actions[action_index];
    }
    const json release = {{"events", json::array({{{"type", "action"},
                                                   {"action_name", action_name},
                                                   {"pressed", false}}})}};
    const auto result = GodotBridge::instance().execute("runtime.injectInput", release,
                                                        sessionKindName(m_sessionKind));
    if (result.contains("error")) {
        DIDI_LOG_WARN("EDITOR_HOOK", "Could not release the exploration action ", action_name,
                      ": ", result["error"].value("message", "unknown"));
    }
}

void EditorHook::processSceneExplorationFrame() {
    // Same discipline as processInvariantWatchFrame: every call into the engine
    // happens outside the lock, because a nested callback reaching this
    // function must never find the mutex held by its own thread.
    std::vector<runtime::ExplorationProbe> probes;
    std::shared_ptr<CommandControl> running_for;
    uint64_t error_cursor = 0;
    int64_t elapsed = 0;
    int64_t slot = 0;
    std::optional<size_t> previously_held;
    int64_t previously_held_slot = -1;
    size_t action_now = 0;
    std::string action_now_name;
    {
        std::lock_guard<std::mutex> lock(m_explorationMutex);
        if (!m_pendingSceneExploration.has_value()) return;
        auto& pending = *m_pendingSceneExploration;
        if (pending.awaiting_next_callback) {
            // The command was dequeued this callback. The window starts at a
            // frame boundary, the same as the watch's, so the first press and
            // the first sample belong to the same frame.
            pending.awaiting_next_callback = false;
            pending.started_at = std::chrono::steady_clock::now();
            pending.error_cursor = engineOutput().nextSequence();
            return;
        }
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - pending.started_at)
                      .count();
        probes = pending.exploration.request().probes;
        running_for = pending.control;
        error_cursor = pending.error_cursor;
        slot = pending.exploration.slotAt(elapsed);
        previously_held = pending.held_action;
        previously_held_slot = pending.held_slot;
        action_now = pending.exploration.actionForSlot(slot);
        action_now_name = pending.exploration.request().actions[action_now];
    }

    // The schedule moved, so what was down comes up before the next thing goes
    // down. Two actions held at once would be a combination the caller never
    // asked for, and a report naming one of them would be describing the wrong
    // input.
    if (slot != previously_held_slot) {
        if (previously_held.has_value()) releaseExplorationAction(*previously_held);
        const json press = {{"events", json::array({{{"type", "action"},
                                                     {"action_name", action_now_name},
                                                     {"pressed", true}}})}};
        const auto pressed = GodotBridge::instance().execute("runtime.injectInput", press,
                                                             sessionKindName(m_sessionKind));
        // "queued" is runtime_inject_input succeeding at holding an event a
        // paused tree will not deliver, and it is the end of this run: the
        // window is open and nothing it presses can land. The tree was running
        // when the run was scheduled, so it was paused since. Queue the release
        // behind the press, outside the lock releaseExplorationAction takes, so
        // a refused run does not leave an action down in the frame the tree
        // resumes.
        const bool press_queued = pressed.value("outcome", std::string()) == "queued";
        if (press_queued) {
            const json release = {{"events", json::array({{{"type", "action"},
                                                           {"action_name", action_now_name},
                                                           {"pressed", false}}})}};
            const auto released = GodotBridge::instance().execute(
                "runtime.injectInput", release, sessionKindName(m_sessionKind));
            if (released.contains("error")) {
                DIDI_LOG_WARN("EDITOR_HOOK", "Could not queue the release for ", action_now_name,
                              ": ", released["error"].value("message", "unknown"));
            }
        }
        std::lock_guard<std::mutex> lock(m_explorationMutex);
        if (!m_pendingSceneExploration.has_value() ||
            m_pendingSceneExploration->control != running_for) {
            return;
        }
        if (press_queued) {
            auto finished = std::move(m_pendingSceneExploration);
            m_pendingSceneExploration.reset();
            finished->control->markCompleted();
            fulfillCommand(finished->response_promise, finished->control,
                           runtime::pausedExplorationRefusal(true));
            return;
        }
        if (pressed.contains("error")) {
            // The window is open and the press did not land, so reporting the
            // run as if it had driven the game would be the dishonest answer.
            // The InputMap check already refused an action nobody declared, so
            // whatever is left is the engine refusing, not the request being
            // wrong: it is reported under the bridge's own status and code
            // rather than flattened to a 400 the floor calls invalid_arguments.
            auto finished = std::move(m_pendingSceneExploration);
            m_pendingSceneExploration.reset();
            finished->control->markCompleted();
            fulfillCommand(finished->response_promise, finished->control,
                           runtime::relayedExplorationRefusal(
                               "Could not press the action " + action_now_name, pressed,
                               "press_refused", {{"action", action_now_name}}));
            return;
        }
        m_pendingSceneExploration->held_action = action_now;
        m_pendingSceneExploration->held_slot = slot;
    }

    runtime::ExplorationSample sample;
    sample.readings.resize(probes.size());
    for (size_t index = 0; index < probes.size(); ++index) {
        json expression_params = {{"expression", probes[index].expression},
                                  // Short on purpose, for the reason the watch
                                  // gives: this runs inside a frame, and an
                                  // expression allowed a second would be
                                  // measuring a game it had itself stalled.
                                  {"timeout_ms", 50}};
        if (!probes[index].context_node.empty()) {
            expression_params["context_node"] = probes[index].context_node;
        }
        const auto evaluated = executeExpression(expression_params,
                                                 sessionKindName(m_sessionKind));
        auto& target = sample.readings[index];
        if (evaluated.contains("error")) {
            target.read_error = evaluated["error"].value("message", "expression failed");
            continue;
        }
        const json value = evaluated.value("value", json());
        if (value.is_number()) {
            target.value = value.get<double>();
        } else if (value.is_boolean()) {
            target.value = value.get<bool>() ? 1.0 : 0.0;
        } else {
            target.read_error = "expression did not evaluate to a number or a boolean";
        }
    }

    sample.engine_errors = static_cast<int64_t>(
        engineOutput().countFrom(error_cursor, "error"));

    std::optional<PendingSceneExploration> completed;
    bool stuck = false;
    {
        std::lock_guard<std::mutex> lock(m_explorationMutex);
        if (!m_pendingSceneExploration.has_value() ||
            m_pendingSceneExploration->control != running_for) {
            return;
        }
        auto& pending = *m_pendingSceneExploration;
        if (!pending.exploration.observe(elapsed, sample)) return;
        stuck = pending.exploration.stuck();
        completed = std::move(m_pendingSceneExploration);
        m_pendingSceneExploration.reset();
    }

    // Whatever is down comes up. The run is over either way, and an action
    // left pressed would go on driving the game after the report said the bot
    // had stopped.
    if (completed->held_action.has_value()) {
        const auto& actions = completed->exploration.request().actions;
        if (*completed->held_action < actions.size()) {
            const json release = {{"events", json::array({{{"type", "action"},
                                                           {"action_name",
                                                            actions[*completed->held_action]},
                                                           {"pressed", false}}})}};
            const auto released = GodotBridge::instance().execute(
                "runtime.injectInput", release, sessionKindName(m_sessionKind));
            if (released.contains("error")) {
                DIDI_LOG_WARN("EDITOR_HOOK", "Could not release the last exploration action: ",
                              released["error"].value("message", "unknown"));
            }
        }
    }

    // The pause happens on the frame the run stopped on, which is what makes
    // a stuck interval a reproduction somebody can look at rather than a note
    // about a game that has since moved on.
    bool paused = false;
    if (stuck && completed->exploration.request().pause_on_stuck) {
        auto stopped = setLiveSceneTreePaused(true);
        paused = stopped.isOk();
        if (stopped.isErr()) {
            DIDI_LOG_WARN("EDITOR_HOOK", "Could not pause on a stuck interval: ",
                          stopped.error().message);
        }
    }

    completed->control->markCompleted();
    auto response = completed->exploration.response(paused);
    response["execution_mode"] = "live";
    response["is_live_engine"] = true;
    response["session_kind"] = sessionKindName(m_sessionKind);
    fulfillCommand(completed->response_promise, completed->control, std::move(response));
}

void EditorHook::requestSceneTreeQuit(int64_t exit_code) {
    m_pendingQuitExitCode = exit_code;
    // One full frame of margin. The response is framed and written by the IPC
    // worker as soon as this command's handler returns, which is microseconds;
    // a frame is milliseconds. This does not make the ordering certain, only
    // very likely, and the comment on processPendingQuitFrame says why.
    m_pendingQuitFrames = 2;
}

void EditorHook::processPendingQuitFrame() {
    if (!m_pendingQuitExitCode.has_value()) return;
    if (--m_pendingQuitFrames > 0) return;
    const int64_t exit_code = *m_pendingQuitExitCode;
    m_pendingQuitExitCode.reset();
    DIDI_LOG_INFO("EDITOR_HOOK", "Quitting the scene tree with exit code ", exit_code);
    auto requested = quitSceneTree(exit_code);
    if (requested.isErr()) {
        DIDI_LOG_ERROR("EDITOR_HOOK", "Deferred SceneTree.quit failed: ", requested.error().message);
    }
}

void EditorHook::scheduleAssetReimport(
    const json& params,
    const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    if (m_sessionKind != runtime::SessionKind::editor) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 409},
                                    {"message", "Asset reimport is available only in editor sessions"},
                                    {"data", {{"code", "session_kind_rejected"},
                                              {"method", "asset.reimport"},
                                              {"retryable", false}}}}}});
        return;
    }
    if (!params.is_object() || !params.contains("paths") || !params["paths"].is_array() ||
        params["paths"].empty() || params["paths"].size() > 256) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 400},
                                    {"message", "paths must contain 1 to 256 source assets"}}}});
        return;
    }
    std::vector<std::string> paths;
    paths.reserve(params["paths"].size());
    for (const auto& value : params["paths"]) {
        if (!value.is_string()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 400},
                                        {"message", "paths must contain only strings"}}}});
            return;
        }
        paths.push_back(value.get<std::string>());
    }
    int64_t timeout_ms = 10000;
    if (params.contains("timeout_ms")) {
        if (!params["timeout_ms"].is_number_integer() ||
            params["timeout_ms"].get<int64_t>() < 1 ||
            params["timeout_ms"].get<int64_t>() > 10000) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 400},
                                        {"message", "timeout_ms must be an integer from 1 to 10000"}}}});
            return;
        }
        timeout_ms = params["timeout_ms"].get<int64_t>();
    }

    std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
    if (m_pendingAssetReimport.has_value()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 409},
                                    {"message", "An asset reimport request is already active"},
                                    {"data", {{"code", "asset_reimport_active"},
                                              {"retryable", true}}}}}});
        return;
    }
    auto resolved = GodotBridge::instance().resolveReimportPaths(paths);
    if (resolved.isErr()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", resolved.error().code},
                                    {"message", resolved.error().message}}}});
        return;
    }

    // Publish the request before starting the reimport. reimport_files
    // re-enters the main-loop callback, and a nested frame that cannot see a
    // pending request skips straight past the scanning window, so the request
    // later either times out or reports idle without ever having observed the
    // scan it was waiting for.
    const auto now = std::chrono::steady_clock::now();
    m_pendingAssetReimport.emplace(PendingAssetReimport{
        resolved.value().paths, resolved.value().reimported, resolved.value().refreshed,
        ReimportProgress(now, std::chrono::milliseconds(timeout_ms)),
        promise, control, resolved.value().needs_scan
    });

    auto started = GodotBridge::instance().startAssetReimport(resolved.value());
    if (started.isErr()) {
        m_pendingAssetReimport.reset();
        control->markCompleted();
        json error = {{"code", started.error().code}, {"message", started.error().message}};
        if (!started.error().data.is_null()) error["data"] = started.error().data;
        fulfillCommand(promise, control, {{"error", std::move(error)}});
        return;
    }
    // A held reimport called nothing that runs frames, so no nested frame has
    // seen the request since it was published. One that was not held did run
    // them, and a nested frame may have answered on its deadline.
    if (m_pendingAssetReimport.has_value() && m_pendingAssetReimport->control == control) {
        m_pendingAssetReimport->reimport_deferred = started.value().held;
        m_pendingAssetReimport->settle = started.value().settle;
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Started bounded asset reimport for ",
                  resolved.value().reimported.size(), " imported and ",
                  resolved.value().refreshed.size(), " refreshed path(s)",
                  started.value().held ? ", the reimport held until the editor's scan is over" : "");
}

bool EditorHook::scheduleMainScreenCapture(
    const std::string& method,
    const json& params,
    const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    if (!params.is_object() || params.value("select_main_screen", false) != true) return false;

    const auto refuse = [&](int code, const std::string& message) {
        control->markCompleted();
        fulfillCommand(promise, control, {{"error", {{"code", code}, {"message", message}}}});
        return true;
    };

    if (m_sessionKind != runtime::SessionKind::editor) {
        return refuse(409, "select_main_screen applies to an editor session. A game has one "
                           "viewport and no main screen to choose.");
    }
    // The same default the renderers apply, so a call that names no camera
    // selects the screen the frame will actually be taken from rather than
    // being told to name one the tool already defaults to.
    const auto identifier = params.value("camera_identifier", std::string("active_editor_view"));
    const auto viewport = selectEditorViewport(identifier);
    if (!viewport.has_value()) {
        return refuse(400, "select_main_screen needs a camera_identifier naming an editor "
                           "viewport, one of " + editorViewportIdentifierList() + "; received \"" +
                           identifier + "\"");
    }
    const std::string target = *viewport == EditorViewport::TwoD ? "2D" : "3D";

    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (m_pendingMainScreenCapture.has_value()) {
            return refuse(409, "A main-screen capture is already in flight");
        }
    }

    // Read before switching, or there is nothing left to go back to.
    const auto previous = GodotBridge::instance().currentMainScreenName();
    if (previous.has_value() && *previous == target) {
        // Already there, so nothing to select, wait for or restore. The
        // ordinary synchronous path is the honest answer.
        return false;
    }
    auto selected = GodotBridge::instance().selectMainScreen(target);
    if (selected.isErr()) {
        return refuse(selected.error().code, selected.error().message);
    }

    std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
    m_pendingMainScreenCapture = PendingMainScreenCapture{
        method, params, target, previous.value_or(std::string()), 1, promise, control
    };
    DIDI_LOG_INFO("EDITOR_HOOK", "Selected the ", target,
                  " main screen for a capture; answering next frame");
    return true;
}

void EditorHook::processMainScreenCaptureFrame() {
    std::optional<PendingMainScreenCapture> ready;
    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (!m_pendingMainScreenCapture.has_value()) return;
        if (m_pendingMainScreenCapture->remaining_frames > 0) {
            --m_pendingMainScreenCapture->remaining_frames;
            return;
        }
        ready = std::move(m_pendingMainScreenCapture);
        m_pendingMainScreenCapture.reset();
    }

    // Through the ordinary dispatcher, so the deferred answer is the same
    // answer the synchronous path gives, guards included.
    json result = executeOnMainThread(ready->method, ready->params);

    // Put the editor back the way it was found, whatever the capture did. A
    // main screen an addon owns cannot be named back, so that is said rather
    // than silently left on the one this selected.
    bool restored = false;
    if (!ready->previous_screen.empty()) {
        restored = GodotBridge::instance().selectMainScreen(ready->previous_screen).isOk();
    }
    if (result.is_object()) {
        result["main_screen_selected"] = ready->selected_screen;
        result["main_screen_restored"] = restored;
        if (!ready->previous_screen.empty()) {
            result["previous_main_screen"] = ready->previous_screen;
        } else {
            result["main_screen_restore_note"] =
                "The main screen that was showing is one Didi cannot name back, which is any "
                "screen an addon contributes, so the editor is left on " + ready->selected_screen +
                ".";
        }
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Captured on the ", ready->selected_screen,
                  " main screen; previous screen ",
                  ready->previous_screen.empty() ? std::string("could not be named")
                                                 : ready->previous_screen,
                  restored ? " restored" : " not restored");
    ready->control->markCompleted();
    fulfillCommand(ready->response_promise, ready->control, std::move(result));
}

bool EditorHook::scheduleScriptCall(
    const json& params,
    const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    const auto refuse = [&](int code, const std::string& message) {
        control->markCompleted();
        fulfillCommand(promise, control, {{"error", {{"code", code}, {"message", message}}}});
        return true;
    };
    if (m_sessionKind != runtime::SessionKind::editor) {
        return refuse(409, "scene_call_method resolves against the edited scene, so it needs an "
                           "editor session.");
    }

    int timeout_seconds = 10;
    if (params.is_object() && params.contains("timeout_seconds")) {
        const auto& value = params["timeout_seconds"];
        if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
            value.get<int64_t>() < 1 || value.get<int64_t>() > 120) {
            return refuse(400, "timeout_seconds must be an integer from 1 to 120");
        }
        timeout_seconds = static_cast<int>(value.get<int64_t>());
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (m_pendingScriptCall.has_value()) {
            return refuse(409, "A coroutine started by scene_call_method is still running");
        }
    }

    std::optional<GodotBridge::PendingScriptCall> pending;
    json result = GodotBridge::instance().callScriptMethod(params, pending);
    if (!pending.has_value()) {
        // Either it finished or it failed; both are answers.
        control->markCompleted();
        fulfillCommand(promise, control, std::move(result));
        return true;
    }

    std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
    m_pendingScriptCall = PendingScriptCallRequest{
        pending->await_id, pending->target_node, pending->method_name,
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds),
        promise, control};
    DIDI_LOG_INFO("EDITOR_HOOK", "Awaiting the coroutine ", pending->method_name,
                  " started on ", pending->target_node);
    return true;
}

void EditorHook::processScriptCallFrame() {
    std::optional<PendingScriptCallRequest> ready;
    std::optional<json> value;
    bool timed_out = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (!m_pendingScriptCall.has_value()) return;
        value = GodotBridge::instance().collectScriptCall(m_pendingScriptCall->await_id);
        timed_out = !value.has_value() &&
                    std::chrono::steady_clock::now() > m_pendingScriptCall->deadline;
        if (!value.has_value() && !timed_out) return;
        ready = std::move(m_pendingScriptCall);
        m_pendingScriptCall.reset();
    }

    json response;
    if (timed_out) {
        // The coroutine is still running and Didi has stopped waiting. Saying
        // that is the answer; claiming a result would be a lie, and claiming a
        // failure would be one too, because the method may yet finish.
        GodotBridge::instance().abandonScriptCall(ready->await_id);
        response = {{"error",
                     {{"code", 504},
                      {"message", "\"" + ready->method_name +
                                      "\" is a coroutine that had not finished when the timeout "
                                      "ran out. It has started and may still complete; nothing "
                                      "was rolled back. Verify the effect rather than retrying "
                                      "blindly."},
                      {"data", {{"outcome", "unknown_outcome"},
                                {"awaited", true},
                                {"target_node", ready->target_node},
                                {"method_name", ready->method_name}}}}}};
    } else {
        response = {{"status", "success"},
                    {"target_node", ready->target_node},
                    {"method_name", ready->method_name},
                    {"awaited", true},
                    {"returned", *value},
                    {"execution_mode", "live"},
                    {"is_live_engine", true},
                    {"session_kind", "editor"}};
        DIDI_LOG_INFO("EDITOR_HOOK", "Coroutine ", ready->method_name, " completed");
    }
    ready->control->markCompleted();
    fulfillCommand(ready->response_promise, ready->control, std::move(response));
}

bool EditorHook::editorImportPassOpen() {
    if (m_importPassOverride.has_value()) return *m_importPassOverride;
    // A game has no EditorFileSystem and runs no import pass.
    if (m_sessionKind != runtime::SessionKind::editor) return false;
    return importPassOpen(GodotBridge::instance().observeEditorImportPass());
}

void EditorHook::processAssetReimportFrame() {
    std::optional<PendingAssetReimport> completed;
    json response;
    std::shared_ptr<CommandControl> observed_control;
    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (!m_pendingAssetReimport.has_value()) return;
        observed_control = m_pendingAssetReimport->control;
    }
    const auto now = std::chrono::steady_clock::now();
    auto scanning = GodotBridge::instance().isEditorFilesystemScanning();
    // Nothing is answered from inside an import pass, the editor's or Didi's
    // own (#914). The scanning flag and the sidecars both settle while the
    // pass still has its last progress task open and has not yet emitted
    // resources_reimported, which is what reloads the resources a caller reads
    // next, and a caller that is answered then sends its next request into the
    // pass. The deadline still applies: a timeout claims nothing about the work.
    const bool inside_pass = editorImportPassOpen();
    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (!m_pendingAssetReimport.has_value() ||
            m_pendingAssetReimport->control != observed_control) {
            return;
        }
        if (inside_pass) {
            if (!m_pendingAssetReimport->progress.expired(now)) return;
            completed = std::move(m_pendingAssetReimport);
            m_pendingAssetReimport.reset();
            response = {{"error", {{"code", 504},
                                    {"message", "Asset reimport did not finish before timeout: the "
                                                "editor was still inside an import pass"},
                                    {"data", {{"code", "reimport_idle_timeout"},
                                               {"outcome", "unknown_outcome"},
                                               {"editor_import_pass_open", true},
                                               {"route_quarantine", false}}}}}};
        } else if (scanning.isErr()) {
            completed = std::move(m_pendingAssetReimport);
            m_pendingAssetReimport.reset();
            response = {{"error", {{"code", scanning.error().code},
                                    {"message", scanning.error().message}}}};
        } else if (m_pendingAssetReimport->reimport_deferred) {
            // The reimport was held because a scan was running or its results
            // were not applied yet, the editor was inside a progress task, or
            // an asset was not listed yet. reimport_files cannot find a file
            // while the editor scans and skipped it with nothing but an engine
            // line to show, and one started inside the work that applies a
            // scan collided with it. Nothing has been reimported, so an answer
            // from here says so.
            auto& settle = m_pendingAssetReimport->settle;
            if (scanning.value() && !settle.has_value()) settle = GodotBridge::instance().beginScanSettle();
            const bool busy = scanning.value() || GodotBridge::instance().editorProgressOpen() ||
                              (settle.has_value() && !GodotBridge::instance().scanSettled(*settle));
            if (!busy && ++m_pendingAssetReimport->deferred_idle_frames < 2 &&
                !m_pendingAssetReimport->progress.expired(now)) {
                return;
            }
            if (busy) {
                m_pendingAssetReimport->deferred_idle_frames = 0;
                if (!m_pendingAssetReimport->progress.expired(now)) return;
                completed = std::move(m_pendingAssetReimport);
                m_pendingAssetReimport.reset();
                response = {{"error", {{"code", 504},
                                        {"message", "The editor was still scanning its files, or finishing "
                                                    "the work a scan leaves, when the timeout ran out, and "
                                                    "Godot cannot reimport a file then, so nothing was "
                                                    "reimported. Retry once the editor is idle."},
                                        {"data", {{"code", "editor_scanning"},
                                                   {"outcome", "not_imported"},
                                                   {"retryable", true},
                                                   {"route_quarantine", false}}}}}};
            } else if (const auto unlisted = GodotBridge::instance().unindexedAssets(
                           m_pendingAssetReimport->reimported);
                       !unlisted.empty()) {
                completed = std::move(m_pendingAssetReimport);
                m_pendingAssetReimport.reset();
                const json unlisted_json = unlisted;
                response = {{"error", {{"code", 409},
                                        {"message", "The editor does not list " + unlisted_json.dump() +
                                                    " among its files, even with its scan over, so "
                                                    "Godot cannot reimport them. It skips a directory "
                                                    "that holds a .gdignore file, for one. Nothing "
                                                    "was reimported."},
                                        {"data", {{"code", "asset_not_indexed"},
                                                   {"not_indexed", unlisted_json},
                                                   {"outcome", "not_imported"},
                                                   {"retryable", false}}}}}};
            } else {
                // reimport_files runs frames of its own, and a nested frame
                // must see a reimport under way rather than one still held.
                m_pendingAssetReimport->reimport_deferred = false;
                const auto reimported = m_pendingAssetReimport->reimported;
                auto started = GodotBridge::instance().reimportIndexedAssets(reimported);
                // A nested frame may have answered, on its deadline.
                if (!m_pendingAssetReimport.has_value() ||
                    m_pendingAssetReimport->control != observed_control) {
                    return;
                }
                if (started.isOk()) return;
                completed = std::move(m_pendingAssetReimport);
                m_pendingAssetReimport.reset();
                json error = {{"code", started.error().code}, {"message", started.error().message}};
                if (!started.error().data.is_null()) error["data"] = started.error().data;
                response = {{"error", std::move(error)}};
            }
        } else {
            const auto state = m_pendingAssetReimport->progress.observe(scanning.value(), now);
            if (state == ReimportProgressState::Pending) return;
            // Idle is not finished when a scan was asked for. The editor clears
            // its scanning flag before the importer has written the sidecars
            // the answer is about, so the answer waits for those to settle
            // instead, bounded by the same timeout as everything else.
            // Nor is it finished while the scan's results wait to be applied.
            // The editor applies them in frames of its own, and a caller
            // answered from one of those frames sent its next request into
            // that work.
            if (state == ReimportProgressState::Idle && m_pendingAssetReimport->settle.has_value() &&
                !GodotBridge::instance().scanSettled(*m_pendingAssetReimport->settle)) {
                return;
            }
            if (state == ReimportProgressState::Idle && m_pendingAssetReimport->needs_scan) {
                // Ask the editor, do not wait out a guess. The scanning flag
                // clears before the importer has written its sidecars, so the
                // first answer after idle called a freshly imported asset
                // unimported. A timer instead of a question is the same bug
                // with a number on it: it passed here and failed on a slower
                // machine.
                bool settled = true;
                for (const auto& path : m_pendingAssetReimport->refreshed) {
                    if (!GodotBridge::instance().assetImportSettled(path)) {
                        settled = false;
                        break;
                    }
                }
                if (!settled) return;
            }
            const auto elapsed = m_pendingAssetReimport->progress.elapsedMs(now);
            completed = std::move(m_pendingAssetReimport);
            m_pendingAssetReimport.reset();
            if (state == ReimportProgressState::TimedOut) {
                response = {{"error", {{"code", 504},
                                        {"message", "Asset reimport did not reach editor idle before timeout"},
                                        {"data", {{"code", "reimport_idle_timeout"},
                                                   {"outcome", "unknown_outcome"},
                                                   {"route_quarantine", false}}}}}};
            } else {
                // What the scan did, read off the filesystem rather than
                // inferred from the call that was made. A path with no .import
                // sidecar is one of two different things -- a .gd or a .tscn,
                // which never gets one, or an asset the editor had never seen,
                // which is unusable until it is imported -- and both were
                // reported as refreshed and idle, which reads as "done, nothing
                // was stale" (#731). Whether a sidecar exists now tells them
                // apart without guessing at the file's type.
                json imported = json::array();
                json announced = json::array();
                json failed = json::array();
                for (const auto& path : completed->refreshed) {
                    if (!GodotBridge::instance().assetIsImported(path)) {
                        announced.push_back(path);
                    } else if (GodotBridge::instance().assetImportFailed(path)) {
                        failed.push_back(path);
                    } else {
                        imported.push_back(path);
                    }
                }
                // A reimported path can fail as well as a new one: the engine
                // rewrites the sidecar with valid=false either way.
                for (const auto& path : completed->reimported) {
                    if (GodotBridge::instance().assetImportFailed(path)) failed.push_back(path);
                }
                response = {{"paths", completed->paths},
                            {"accepted_count", completed->paths.size()},
                            {"reimported", completed->reimported},
                            {"refreshed", completed->refreshed},
                            {"imported", imported},
                            {"announced", announced},
                            {"elapsed_ms", elapsed}, {"idle", true},
                            {"execution_mode", "live"}, {"is_live_engine", true},
                            {"session_kind", "editor"}};
                if (!failed.empty()) {
                    // The engine refused the import, and says why in the lines
                    // the answer carries (engine_diagnostics). Reported as a
                    // refusal, because an asset that did not import is one
                    // nothing can load.
                    response = {{"error", {{"code", 422},
                                           {"message", "Godot could not import " + failed.dump() +
                                                           ". The .import sidecar records valid=false; "
                                                           "the engine's reason is under engine_diagnostics."},
                                           {"data", {{"code", "asset_import_failed"},
                                                     {"failed", failed},
                                                     {"imported", imported},
                                                     {"announced", announced},
                                                     {"outcome", imported.empty() ? "not_imported"
                                                                                  : "partially_imported"},
                                                     {"retryable", false}}}}}};
                } else if (!announced.empty()) {
                    response["limitation"] =
                        "Paths under announced carry no .import sidecar after the scan. For a "
                        "script, a scene or a text resource that is the whole story: Godot's "
                        "import system does not own those and they are usable as they are. For "
                        "an asset that needs importing -- an image, an audio file, a font -- it "
                        "means the editor did not import it, and anything referencing it will "
                        "load nothing.";
                }
            }
        }
    }
    if (!completed.has_value()) return;
    completed->control->markCompleted();
    fulfillCommand(completed->response_promise, completed->control, std::move(response));
}

void EditorHook::scheduleRuntimeStep(
    int frames,
    const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    if (m_sessionKind != runtime::SessionKind::game) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 409},
                                    {"message", "Frame stepping is available only for game sessions"},
                                    {"data", {{"code", "session_kind_rejected"},
                                              {"method", "runtime.step"},
                                              {"retryable", false}}}}}});
        return;
    }

    auto state = executeRuntimeBridge("runtime.getTree",
                                      {{"root_path", "/root"}, {"max_depth", 0}},
                                      sessionKindName(m_sessionKind));
    if (state.contains("error")) {
        control->markCompleted();
        fulfillCommand(promise, control, std::move(state));
        return;
    }
    if (!state.value("paused", false)) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 409},
                                    {"message", "Frame stepping requires a paused game session"},
                                    {"data", {{"code", "game_not_paused"},
                                              {"paused", false},
                                              {"retryable", false}}}}}});
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stepMutex);
        if (!m_runtimeStepGate.tryAcquire()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 409},
                                        {"message", "A runtime frame step is already active"},
                                        {"data", {{"code", "runtime_step_active"},
                                                  {"retryable", true}}}}}});
            return;
        }
        if (m_pendingRuntimeStep.has_value()) {
            m_runtimeStepGate.release();
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 409},
                                        {"message", "A runtime frame step is already active"},
                                        {"data", {{"code", "runtime_step_active"},
                                                  {"retryable", true}}}}}});
            return;
        }
        m_pendingRuntimeStep = PendingRuntimeStep{
            frames, frames, true, promise, control
        };
    }

    auto resumed = executeRuntimeBridge("runtime.setPaused", {{"paused", false}},
                                        sessionKindName(m_sessionKind));
    if (resumed.contains("error")) {
        {
            std::lock_guard<std::mutex> lock(m_stepMutex);
            if (m_pendingRuntimeStep.has_value() &&
                m_pendingRuntimeStep->control == control) {
                m_pendingRuntimeStep.reset();
                m_runtimeStepGate.release();
            }
        }
        control->markCompleted();
        fulfillCommand(promise, control, std::move(resumed));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_stepMutex);
        if (m_pendingRuntimeStep.has_value() && m_pendingRuntimeStep->control == control) {
            m_pendingRuntimeStep->released_input_events =
                resumed.value("released_input_events", int64_t{0});
        }
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Scheduled runtime frame step for ", frames, " frame(s)");
}

void EditorHook::processRuntimeStepFrame() {
    std::optional<PendingRuntimeStep> completed;
    {
        std::lock_guard<std::mutex> lock(m_stepMutex);
        if (!m_pendingRuntimeStep.has_value()) return;
        if (m_pendingRuntimeStep->awaiting_next_callback) {
            m_pendingRuntimeStep->awaiting_next_callback = false;
            return;
        }
        --m_pendingRuntimeStep->remaining_frames;
        if (m_pendingRuntimeStep->remaining_frames > 0) return;
        completed = std::move(m_pendingRuntimeStep);
        m_pendingRuntimeStep.reset();
        m_runtimeStepGate.release();
    }

    auto paused = executeRuntimeBridge("runtime.setPaused", {{"paused", true}},
                                       sessionKindName(m_sessionKind));
    if (paused.contains("error") || !paused.value("paused", false)) {
        completed->control->markCompleted();
        if (!paused.contains("error")) {
            paused = {{"error", {{"code", 500},
                                  {"message", "Godot did not re-pause after the runtime frame step"},
                                  {"data", {{"code", "repause_failed"},
                                            {"retryable", false}}}}}};
        }
        fulfillCommand(completed->response_promise, completed->control, std::move(paused));
        return;
    }

    completed->control->markCompleted();
    fulfillCommand(completed->response_promise, completed->control,
                   {{"status", "success"}, {"frames", completed->requested_frames},
                    {"paused", true}, {"released_input_events", completed->released_input_events},
                    {"execution_mode", "live"},
                    {"is_live_engine", true}, {"session_kind", sessionKindName(m_sessionKind)}});
}

void EditorHook::cancelPendingCommands(const std::string& reason) {
    std::queue<EngineCommand> pending;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        pending.swap(m_commandQueue);
    }
    while (!pending.empty()) {
        auto command = std::move(pending.front());
        pending.pop();
        if (command.control && command.control->tryCancelPending()) {
            fulfillCommand(command.response_promise, command.control,
                           {{"error", {{"code", 503},
                                        {"message", reason},
                                        {"data", {{"code", "live_session_ended"},
                                                  {"retryable", false}}}}}});
        }
    }
    std::optional<PendingRuntimeStep> active_step;
    {
        std::lock_guard<std::mutex> lock(m_stepMutex);
        if (m_pendingRuntimeStep.has_value()) {
            active_step = std::move(m_pendingRuntimeStep);
            m_pendingRuntimeStep.reset();
            m_runtimeStepGate.release();
        }
    }
    if (active_step.has_value() && active_step->control &&
        active_step->control->tryCancelRunning()) {
        fulfillCommand(active_step->response_promise, active_step->control,
                       {{"error", {{"code", 503},
                                        {"message", reason},
                                        {"data", {{"code", "live_session_ended"},
                                                  {"retryable", false}}}}}});
    }
    std::optional<PendingAssetReimport> active_reimport;
    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (m_pendingAssetReimport.has_value()) {
            active_reimport = std::move(m_pendingAssetReimport);
            m_pendingAssetReimport.reset();
        }
    }
    if (active_reimport.has_value() && active_reimport->control &&
        active_reimport->control->tryCancelRunning()) {
        fulfillCommand(active_reimport->response_promise, active_reimport->control,
                       {{"error", {{"code", 503},
                                        {"message", reason},
                                        {"data", {{"code", "live_session_ended"},
                                                  {"retryable", false}}}}}});
    }
    std::optional<PendingProfilerRead> active_profiler;
    {
        std::lock_guard<std::mutex> lock(m_profilerMutex);
        if (m_pendingProfilerRead.has_value()) {
            active_profiler = std::move(m_pendingProfilerRead);
            m_pendingProfilerRead.reset();
        }
    }
    // Taking the pending read out under the lock is what stops a late frame
    // callback from publishing a partial window after shutdown began.
    if (active_profiler.has_value() && active_profiler->control &&
        active_profiler->control->tryCancelRunning()) {
        fulfillCommand(active_profiler->response_promise, active_profiler->control,
                       {{"error", {{"code", 504}, {"message", reason},
                                    {"data", {{"code", "live_session_ended"},
                                              {"outcome", active_profiler->collector.started()
                                                              ? "unknown_outcome"
                                                              : "not_started"},
                                              {"retryable", false}}}}}});
    }

    // An invariant watch and an exploration run both live across frames, so a
    // shutdown that drains only the queue leaves their callers waiting on a
    // promise nothing will ever fulfil. They wait for the transport timeout
    // instead of being told the engine went away.
    std::optional<PendingInvariantWatch> active_watch;
    {
        std::lock_guard<std::mutex> lock(m_invariantMutex);
        if (m_pendingInvariantWatch.has_value()) {
            active_watch = std::move(m_pendingInvariantWatch);
            m_pendingInvariantWatch.reset();
        }
    }
    if (active_watch.has_value() && active_watch->control &&
        active_watch->control->tryCancelRunning()) {
        fulfillCommand(active_watch->response_promise, active_watch->control,
                       {{"error", {{"code", 503}, {"message", reason},
                                    {"data", {{"code", "live_session_ended"},
                                              {"outcome", "unknown_outcome"},
                                              {"retryable", false}}}}}});
    }

    std::optional<PendingSceneExploration> active_exploration;
    {
        std::lock_guard<std::mutex> lock(m_explorationMutex);
        if (m_pendingSceneExploration.has_value()) {
            active_exploration = std::move(m_pendingSceneExploration);
            m_pendingSceneExploration.reset();
        }
    }
    if (active_exploration.has_value()) {
        // Whatever the bot was holding comes up. An action left down outlives
        // the run that pressed it and goes on driving the game, and on a
        // shutdown path there is nothing else left to release it.
        if (active_exploration->held_action.has_value()) {
            const auto& actions = active_exploration->exploration.request().actions;
            if (*active_exploration->held_action < actions.size()) {
                const json release = {
                    {"events", json::array({{{"type", "action"},
                                             {"action_name",
                                              actions[*active_exploration->held_action]},
                                             {"pressed", false}}})}};
                const auto released = GodotBridge::instance().execute(
                    "runtime.injectInput", release, sessionKindName(m_sessionKind));
                if (released.contains("error")) {
                    DIDI_LOG_WARN("EDITOR_HOOK",
                                  "Could not release the exploration action on shutdown: ",
                                  released["error"].value("message", "unknown"));
                }
            }
        }
        if (active_exploration->control && active_exploration->control->tryCancelRunning()) {
            fulfillCommand(active_exploration->response_promise, active_exploration->control,
                           {{"error", {{"code", 503}, {"message", reason},
                                        {"data", {{"code", "live_session_ended"},
                                                  {"outcome", "unknown_outcome"},
                                                  {"retryable", false}}}}}});
        }
    }
}

json EditorHook::executeOnMainThread(const std::string& method, const json& params) {
    if (auto rejected = validateSessionKindForMethod(method, m_sessionKind);
        rejected.has_value()) {
        return std::move(*rejected);
    }
    DIDI_LOG_DEBUG("EDITOR_HOOK", "Executing command on Godot main thread: ", method);

    static const std::unordered_set<std::string> live_bridge_methods = {
        "editor.getState", "editor.getRecoveryState", "editor.getSelection", "scene.getHierarchy", "scene.instantiateNode",
        "scene.removeNode", "scene.reparentNode", "scene.setProperty",
        "scene.getProperty", "scene.duplicateNode", "editor.undo", "editor.redo",
        "editor.saveScene", "editor.reloadProject", "script.attachToNode",
        "script.detachFromNode", "project.listAutoloads", "project.setAutoload",
        "project.removeAutoload", "project.listInputActions", "project.setInputAction",
        "project.removeInputAction", "project.getSetting", "project.setSetting",
        "project.resolveUids", "engine.classExists",
        "scene.listGroups", "scene.addToGroup", "scene.removeFromGroup",
        "scene.getGroupMembers", "scene.create", "scene.open", "scene.close",
        "scene.packBranch", "runtime.getTree", "runtime.setPaused", "runtime.stop",
        "runtime.evalGdscript", "ui.hitTest", "ui.listControls", "audio.listBuses", "audio.configureBus",
        // Editor only, by not being in game_admitted below: a bus added to a
        // running game is in no file and gone when the game stops (#771).
        "audio.addBus",
        // Editor only: asset_configure_import reads the stream the editor's
        // reimport just wrote, and a game cannot reimport (#958).
        "asset.readImportedStream",
        // Admission is deliberately separate from the failure-injection seams.
        // One macro previously controlled both, so the feature could not be
        // admitted to production without also compiling test seams into a
        // shipping binary. Only the seam configurator stays gated.
        "signal.listConnections", "signal.connect", "signal.disconnect", "signal.emit",
        "runtime.injectInput", "physics.raycast", "physics.raycastBatch",
        "physics.clearance", "vision.frustumQuery", "nav.queryPath",
        "anim.listTracks", "anim.playTrack", "anim.addLibrary", "resource.refreshCached",
        "export.reloadPresets",
        "vision.setCameraTransform",
        "vision.toggleDebugDraw", "tilemap.setCells", "tilemap.getUsedRect",
        "gridmap.setCells", "shader.listUniforms", "shader.setUniform",
        "preview.renderGhost", "preview.clearGhosts",
        "shader.getVisualGraph"
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
        , "phase7SignalTest.configure"
#endif
    };
    if (live_bridge_methods.count(method)) {
        // Spatial reads are editor-or-game by policy; everything else that is
        // not runtime.* is editor-only here.
        const bool game_admitted = method.rfind("runtime.", 0) == 0 ||
                                   method == "physics.raycast" ||
                                   method == "physics.raycastBatch" ||
                                   method == "physics.clearance" ||
                                   method == "vision.frustumQuery" || method == "nav.queryPath" ||
                                   method == "anim.listTracks" || method == "anim.playTrack" ||
                                   // Enumerating Controls and hit-testing a point
                                   // are reads, and the running game is where a
                                   // caller most needs them (#592).
                                   method == "ui.listControls" || method == "ui.hitTest" ||
                                   // The game's own mix, and the only place a
                                   // bus a script muted at runtime is muted.
                                   method == "audio.listBuses" || method == "audio.configureBus" ||
                                   // A ClassDB read. Every session has one, and
                                   // the answer is about the process rather
                                   // than about an open scene (#766).
                                   method == "engine.classExists";
        if (m_sessionKind == runtime::SessionKind::game && !game_admitted) {
            return {{"error", {{"code", 409},
                                {"message", "Editor-only method is unavailable in a game session: " + method},
                                {"data", {{"code", "session_kind_rejected"},
                                          {"method", method},
                                          {"retryable", false}}}}}};
        }
        return GodotBridge::instance().execute(method, params, sessionKindName(m_sessionKind));
    }

    if (method == "vision.captureViewport") {
        // Isolation hides and restores nodes in the edited scene, which is an
        // editor concept. Capture itself is not: a game has a root viewport and
        // the frame is already in this process.
        if (m_sessionKind != runtime::SessionKind::editor &&
            params.value("node_isolation_path", "") != "") {
            return {{"error", {{"code", 409},
                                {"message", "Viewport node isolation is unavailable in a game session"},
                                {"data", {{"code", "session_kind_rejected"},
                                          {"method", method},
                                          {"retryable", false}}}}}};
        }
        return ViewportRenderer::instance().captureViewport(params, sessionKindName(m_sessionKind));
    }
    if (method == "vision.capturePasses") {
        // Replacement materials go on nodes in whichever scene is open, which
        // both kinds of session have. Only the editor has cameras to choose
        // between, and capturePasses refuses that argument in a game itself.
        return ViewportRenderer::instance().capturePasses(params, sessionKindName(m_sessionKind));
    }
    if (method == "vision.diffViewport") {
        if (m_sessionKind != runtime::SessionKind::editor &&
            params.value("node_isolation_path", "") != "") {
            return {{"error", {{"code", 409},
                                {"message", "Viewport node isolation is unavailable in a game session"},
                                {"data", {{"code", "session_kind_rejected"},
                                          {"method", method},
                                          {"retryable", false}}}}}};
        }
        return ViewportRenderer::instance().diffViewport(params, sessionKindName(m_sessionKind));
    }
    // Both log streams answer the same query shape, so they share one reader.
    // Validation living in a single place is what keeps the two tools from
    // drifting into accepting different cursors or levels.
    if (method == "runtime.getLogs" || method == "runtime.getOutput") {
        const bool engine_stream = (method == "runtime.getOutput");
        const char* subject = engine_stream ? "runtime output" : "runtime log";
        const auto bad_request = [&](const std::string& detail) {
            return json{{"error", {{"code", 400},
                                   {"message", std::string("Invalid ") + subject + " request: " + detail}}}};
        };
        uint64_t cursor = 0;
        size_t limit = 100;
        std::string minimum_level = "debug";
        if (!params.is_object()) {
            return bad_request("params must be an object");
        }
        if (params.contains("cursor")) {
            const auto& value = params["cursor"];
            if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
                (value.is_number_integer() && value.get<int64_t>() < 0)) {
                return bad_request("cursor must be a non-negative integer");
            }
            cursor = value.get<uint64_t>();
        }
        if (params.contains("limit")) {
            const auto& value = params["limit"];
            if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
                (value.is_number_integer() && value.get<int64_t>() < 1) ||
                value.get<uint64_t>() > 500) {
                return bad_request("limit must be an integer from 1 to 500");
            }
            limit = static_cast<size_t>(value.get<uint64_t>());
        }
        if (params.contains("minimum_level")) {
            if (!params["minimum_level"].is_string() ||
                !RuntimeLogRing::isValidLevel(params["minimum_level"].get<std::string>())) {
                return bad_request("minimum_level must be debug, info, warning, or error");
            }
            minimum_level = params["minimum_level"].get<std::string>();
        }
        const auto& ring = engine_stream ? *m_engineOutput : *m_runtimeLogs;
        const auto page_result = ring.read(cursor, limit, minimum_level);
        if (page_result.isErr()) {
            return {{"error", {{"code", page_result.error().code}, {"message", page_result.error().message}}}};
        }
        auto page = page_result.value();
        page["execution_mode"] = "live";
        if (engine_stream) page["stream"] = "engine";
        return page;
    }

    static const std::unordered_set<std::string> offline_only = {
        "asset.query", "script.diagnostics", "script.checkSyntax",
        "script.reflectClass", "script.patchSymbols", "resource.create",
        "resource.inspect", "vision.createVisualTestLab"
    };
    if (offline_only.count(method)) {
        return {{"error", {{"code", 409},
                           {"message", "Offline-only method must execute in the standalone MCP process: " + method},
                           {"data", {{"code", "offline_only_method"},
                                     {"method", method},
                                     {"retryable", false}}}}}};
    }

    static const std::unordered_set<std::string> registered_but_unimplemented = {
        "scene.mutate", "physics.simulateStep", "nav.bakeMesh", "asset.instantiate",
        "runtime.getCallStack"
    };
    if (registered_but_unimplemented.count(method)) {
        return {{"error", {{"code", 501},
                           {"message", "Method is registered for compatibility but has no trustworthy live implementation: " + method},
                           {"data", {{"code", "unimplemented_method"},
                                     {"method", method},
                                     {"retryable", false}}}}}};
    }
    return {{"error", {{"code", 404},
                       {"message", "Unknown method: " + method},
                       {"data", {{"code", "unknown_method"},
                                 {"method", method},
                                 {"retryable", false}}}}}};
}

RuntimeLogRing& EditorHook::runtimeLogs() {
    return *m_runtimeLogs;
}

RuntimeLogRing& EditorHook::engineOutput() {
    return *m_engineOutput;
}

json EditorHookTestAccess::executeOnMainThread(EditorHook& hook,
                                               const std::string& method,
                                               const json& params) {
    return hook.executeOnMainThread(method, params);
}

void EditorHookTestAccess::setSessionKind(
    EditorHook& hook, std::optional<runtime::SessionKind> session_kind) {
    hook.m_sessionKind = session_kind;
}

std::optional<runtime::SessionKind> EditorHookTestAccess::sessionKind(
    const EditorHook& hook) {
    return hook.m_sessionKind;
}

size_t EditorHookTestAccess::queueDepth(EditorHook& hook) {
    std::lock_guard<std::mutex> lock(hook.m_queueMutex);
    return hook.m_commandQueue.size();
}

CommandTicket EditorHookTestAccess::enqueue(EditorHook& hook,
                                            const std::string& method,
                                            const json& params) {
    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();
    auto control = std::make_shared<CommandControl>();
    {
        std::lock_guard<std::mutex> lock(hook.m_queueMutex);
        hook.m_commandQueue.push({method, params, promise, control});
    }
    return {std::move(future), std::move(promise), std::move(control)};
}

bool EditorHookTestAccess::runtimeStepActive(EditorHook& hook) {
    return hook.m_runtimeStepGate.active();
}

bool EditorHookTestAccess::hasPendingRuntimeStep(EditorHook& hook) {
    std::lock_guard<std::mutex> lock(hook.m_stepMutex);
    return hook.m_pendingRuntimeStep.has_value();
}

bool EditorHookTestAccess::hasPendingAssetReimport(EditorHook& hook) {
    std::lock_guard<std::recursive_mutex> lock(hook.m_reimportMutex);
    return hook.m_pendingAssetReimport.has_value();
}

bool EditorHookTestAccess::hasPendingProfilerRead(EditorHook& hook) {
    std::lock_guard<std::mutex> lock(hook.m_profilerMutex);
    return hook.m_pendingProfilerRead.has_value();
}

bool EditorHookTestAccess::pumping(const EditorHook& hook) {
    return hook.m_pumping;
}

void EditorHookTestAccess::setPumping(EditorHook& hook, bool pumping) {
    hook.m_pumping = pumping;
}

void EditorHookTestAccess::setImportPassOpen(EditorHook& hook, std::optional<bool> open) {
    hook.m_importPassOverride = open;
}

bool EditorHookTestAccess::hasPendingQuit(const EditorHook& hook) {
    return hook.m_pendingQuitExitCode.has_value();
}

} // namespace godot
} // namespace didi
