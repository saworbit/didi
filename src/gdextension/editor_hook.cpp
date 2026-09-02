#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
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
    return json{{"error", {{"code", 409},
                            {"message", "session_kind_rejected"},
                            {"data", {{"method", method},
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
            fulfillCommand(prom, control, {{"error", {{"code", 503}, {"message", "Godot main-loop bridge is not ready"}}}});
            return {std::move(fut), std::move(prom), std::move(control)};
        }
        m_commandQueue.push(std::move(cmd));
    }

    return {std::move(fut), std::move(prom), std::move(control)};
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
    if (m_pumping) {
        processRuntimeStepFrame();
        processAssetReimportFrame();
        processProfilerFrame();
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
                           {{"error", {{"code", 504}, {"message", "Command cancelled before execution"}}}});
            continue;
        }
        try {
            if (cmd.method == "asset.reimport") {
                scheduleAssetReimport(cmd.params, cmd.response_promise, cmd.control);
                continue;
            }
            if (cmd.method == "runtime.readProfiler") {
                scheduleProfilerRead(cmd.params, cmd.response_promise, cmd.control);
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
            json result = executeOnMainThread(cmd.method, cmd.params);
            cmd.control->markCompleted();
            fulfillCommand(cmd.response_promise, cmd.control, std::move(result));
        } catch (const std::exception& e) {
            DIDI_LOG_ERROR("EDITOR_HOOK", "Exception executing command: ", cmd.method);
            cmd.control->markCompleted();
            fulfillCommand(cmd.response_promise, cmd.control,
                           {{"error", {{"code", 500}, {"message", e.what()}}}});
        }
    }
    processRuntimeStepFrame();
    processAssetReimportFrame();
    processProfilerFrame();
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
                                                    preflight.error().message}}}});
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
                                        {"data", {{"retryable", true}}}}}});
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
                                    {"message", "Asset reimport is available only in editor sessions"}}}});
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
                                    {"message", "An asset reimport request is already active"}}}});
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
        resolved.value(), ReimportProgress(now, std::chrono::milliseconds(timeout_ms)),
        promise, control
    });

    auto started = GodotBridge::instance().startAssetReimport(resolved.value());
    if (started.isErr()) {
        m_pendingAssetReimport.reset();
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", started.error().code},
                                    {"message", started.error().message}}}});
        return;
    }
    DIDI_LOG_INFO("EDITOR_HOOK", "Started bounded asset reimport for ", resolved.value().size(), " path(s)");
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
    {
        std::lock_guard<std::recursive_mutex> lock(m_reimportMutex);
        if (!m_pendingAssetReimport.has_value() ||
            m_pendingAssetReimport->control != observed_control) {
            return;
        }
        if (scanning.isErr()) {
            completed = std::move(m_pendingAssetReimport);
            m_pendingAssetReimport.reset();
            response = {{"error", {{"code", scanning.error().code},
                                    {"message", scanning.error().message}}}};
        } else {
            const auto state = m_pendingAssetReimport->progress.observe(scanning.value(), now);
            if (state == ReimportProgressState::Pending) return;
            const auto elapsed = m_pendingAssetReimport->progress.elapsedMs(now);
            completed = std::move(m_pendingAssetReimport);
            m_pendingAssetReimport.reset();
            if (state == ReimportProgressState::TimedOut) {
                response = {{"error", {{"code", 504},
                                        {"message", "Asset reimport did not reach editor idle before timeout"},
                                        {"data", {{"outcome", "unknown_outcome"},
                                                   {"route_quarantine", false}}}}}};
            } else {
                response = {{"paths", completed->paths},
                            {"accepted_count", completed->paths.size()},
                            {"elapsed_ms", elapsed}, {"idle", true},
                            {"execution_mode", "live"}, {"is_live_engine", true},
                            {"session_kind", "editor"}};
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
                                    {"message", "Frame stepping is available only for game sessions"}}}});
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
                                    {"message", "Frame stepping requires a paused game session"}}}});
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stepMutex);
        if (!m_runtimeStepGate.tryAcquire()) {
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 409},
                                        {"message", "A runtime frame step is already active"}}}});
            return;
        }
        if (m_pendingRuntimeStep.has_value()) {
            m_runtimeStepGate.release();
            control->markCompleted();
            fulfillCommand(promise, control,
                           {{"error", {{"code", 409},
                                        {"message", "A runtime frame step is already active"}}}});
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
                                  {"message", "Godot did not re-pause after the runtime frame step"}}}};
        }
        fulfillCommand(completed->response_promise, completed->control, std::move(paused));
        return;
    }

    completed->control->markCompleted();
    fulfillCommand(completed->response_promise, completed->control,
                   {{"status", "success"}, {"frames", completed->requested_frames},
                    {"paused", true}, {"execution_mode", "live"},
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
                           {{"error", {{"code", 503}, {"message", reason}}}});
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
                       {{"error", {{"code", 503}, {"message", reason}}}});
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
                       {{"error", {{"code", 503}, {"message", reason}}}});
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
                                    {"data", {{"outcome", active_profiler->collector.started()
                                                              ? "unknown_outcome"
                                                              : "not_started"},
                                              {"retryable", false}}}}}});
    }
}

json EditorHook::executeOnMainThread(const std::string& method, const json& params) {
    if (auto rejected = validateSessionKindForMethod(method, m_sessionKind);
        rejected.has_value()) {
        return std::move(*rejected);
    }
    DIDI_LOG_DEBUG("EDITOR_HOOK", "Executing command on Godot main thread: ", method);

    static const std::unordered_set<std::string> live_bridge_methods = {
        "editor.getState", "scene.getHierarchy", "scene.instantiateNode",
        "scene.removeNode", "scene.reparentNode", "scene.setProperty",
        "scene.getProperty", "scene.duplicateNode", "editor.undo", "editor.redo",
        "editor.saveScene", "editor.reloadProject", "script.attachToNode",
        "script.detachFromNode", "project.listAutoloads", "project.setAutoload",
        "project.removeAutoload", "project.listInputActions", "project.setInputAction",
        "project.removeInputAction", "project.getSetting", "project.setSetting",
        "scene.listGroups", "scene.addToGroup", "scene.removeFromGroup",
        "scene.getGroupMembers", "scene.create", "scene.open", "scene.close",
        "scene.packBranch", "runtime.getTree", "runtime.setPaused", "runtime.stop",
        "runtime.evalGdscript", "ui.hitTest", "audio.listBuses", "audio.configureBus",
        // Admission is deliberately separate from the failure-injection seams.
        // One macro previously controlled both, so the feature could not be
        // admitted to production without also compiling test seams into a
        // shipping binary. Only the seam configurator stays gated.
        "signal.listConnections", "signal.connect", "signal.disconnect", "signal.emit",
        "runtime.injectInput", "physics.raycast", "nav.queryPath",
        "anim.listTracks", "anim.playTrack", "vision.setCameraTransform",
        "vision.toggleDebugDraw", "tilemap.setCells", "tilemap.getUsedRect",
        "gridmap.setCells"
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
        , "phase7SignalTest.configure"
#endif
    };
    if (live_bridge_methods.count(method)) {
        // Spatial reads are editor-or-game by policy; everything else that is
        // not runtime.* is editor-only here.
        const bool game_admitted = method.rfind("runtime.", 0) == 0 ||
                                   method == "physics.raycast" || method == "nav.queryPath" ||
                                   method == "anim.listTracks" || method == "anim.playTrack";
        if (m_sessionKind == runtime::SessionKind::game && !game_admitted) {
            return {{"error", {{"code", 409},
                                {"message", "Editor-only method is unavailable in a game session: " + method}}}};
        }
        return GodotBridge::instance().execute(method, params, sessionKindName(m_sessionKind));
    }

    if (method == "vision.captureViewport") {
        if (m_sessionKind != runtime::SessionKind::editor &&
            params.value("node_isolation_path", "") != "") {
            return {{"error", {{"code", 409},
                                {"message", "Viewport node isolation is unavailable in a game session"}}}};
        }
        return ViewportRenderer::instance().captureViewport(params);
    }
    if (method == "vision.diffViewport") {
        if (m_sessionKind != runtime::SessionKind::editor) {
            return {{"error", {{"code", 409},
                                {"message", "Viewport diff capture is unavailable in a game session"}}}};
        }
        return ViewportRenderer::instance().diffViewport(params);
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
                           {"message", "Offline-only method must execute in the standalone MCP process: " + method}}}};
    }

    static const std::unordered_set<std::string> registered_but_unimplemented = {
        "scene.mutate", "physics.simulateStep", "nav.bakeMesh", "asset.instantiate",
        "runtime.getCallStack"
    };
    if (registered_but_unimplemented.count(method)) {
        return {{"error", {{"code", 501},
                           {"message", "Method is registered for compatibility but has no trustworthy live implementation: " + method}}}};
    }
    return {{"error", {{"code", 404}, {"message", "Unknown method: " + method}}}};
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

bool EditorHookTestAccess::hasPendingQuit(const EditorHook& hook) {
    return hook.m_pendingQuitExitCode.has_value();
}

} // namespace godot
} // namespace didi
