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

void fulfillCommand(const std::shared_ptr<std::promise<json>>& promise,
                    const std::shared_ptr<CommandControl>& control,
                    json response) {
    if (promise && control && control->tryClaimResponse()) {
        promise->set_value(std::move(response));
    }
}

} // namespace

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
    m_sessionKind = session_kind;
}

void EditorHook::processQueue() {
    std::vector<EngineCommand> commands;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        constexpr size_t kMaxCommandsPerFrame = 64;
        while (!m_commandQueue.empty() && commands.size() < kMaxCommandsPerFrame) {
            commands.push_back(std::move(m_commandQueue.front()));
            m_commandQueue.pop();
        }
    }

    for (auto& cmd : commands) {
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
}

void EditorHook::scheduleAssetReimport(
    const json& params,
    const std::shared_ptr<std::promise<json>>& promise,
    const std::shared_ptr<CommandControl>& control) {
    if (m_sessionKind != "editor") {
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
    auto started = GodotBridge::instance().beginAssetReimport(paths);
    if (started.isErr()) {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", started.error().code},
                                    {"message", started.error().message}}}});
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    m_pendingAssetReimport.emplace(PendingAssetReimport{
        started.value(), ReimportProgress(now, std::chrono::milliseconds(timeout_ms)),
        promise, control
    });
    DIDI_LOG_INFO("EDITOR_HOOK", "Started bounded asset reimport for ", started.value().size(), " path(s)");
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
    if (m_sessionKind != "game") {
        control->markCompleted();
        fulfillCommand(promise, control,
                       {{"error", {{"code", 409},
                                    {"message", "Frame stepping is available only for game sessions"}}}});
        return;
    }

    auto state = executeRuntimeBridge("runtime.getTree",
                                      {{"root_path", "/root"}, {"max_depth", 0}},
                                      m_sessionKind);
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

    auto resumed = executeRuntimeBridge("runtime.setPaused", {{"paused", false}}, m_sessionKind);
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

    auto paused = executeRuntimeBridge("runtime.setPaused", {{"paused", true}}, m_sessionKind);
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
                    {"is_live_engine", true}, {"session_kind", m_sessionKind}});
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
}

json EditorHook::executeOnMainThread(const std::string& method, const json& params) {
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
        "runtime.evalGdscript", "ui.hitTest"
    };
    if (live_bridge_methods.count(method)) {
        if (m_sessionKind == "game" && method.rfind("runtime.", 0) != 0) {
            return {{"error", {{"code", 409},
                                {"message", "Editor-only method is unavailable in a game session: " + method}}}};
        }
        return GodotBridge::instance().execute(method, params, m_sessionKind);
    }

    if (method == "vision.captureViewport") {
        if (m_sessionKind != "editor" && params.value("node_isolation_path", "") != "") {
            return {{"error", {{"code", 409},
                                {"message", "Viewport node isolation is unavailable in a game session"}}}};
        }
        return ViewportRenderer::instance().captureViewport(params);
    }
    if (method == "vision.diffViewport") {
        if (m_sessionKind != "editor") {
            return {{"error", {{"code", 409},
                                {"message", "Viewport diff capture is unavailable in a game session"}}}};
        }
        return ViewportRenderer::instance().diffViewport(params);
    }
    if (method == "runtime.getLogs") {
        uint64_t cursor = 0;
        size_t limit = 100;
        std::string minimum_level = "debug";
        if (!params.is_object()) {
            return {{"error", {{"code", 400}, {"message", "Invalid runtime log request: params must be an object"}}}};
        }
        if (params.contains("cursor")) {
            const auto& value = params["cursor"];
            if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
                (value.is_number_integer() && value.get<int64_t>() < 0)) {
                return {{"error", {{"code", 400}, {"message", "Invalid runtime log request: cursor must be a non-negative integer"}}}};
            }
            cursor = value.get<uint64_t>();
        }
        if (params.contains("limit")) {
            const auto& value = params["limit"];
            if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
                (value.is_number_integer() && value.get<int64_t>() < 1) ||
                value.get<uint64_t>() > 500) {
                return {{"error", {{"code", 400}, {"message", "Invalid runtime log request: limit must be an integer from 1 to 500"}}}};
            }
            limit = static_cast<size_t>(value.get<uint64_t>());
        }
        if (params.contains("minimum_level")) {
            if (!params["minimum_level"].is_string() ||
                !RuntimeLogRing::isValidLevel(params["minimum_level"].get<std::string>())) {
                return {{"error", {{"code", 400}, {"message", "Invalid runtime log request: minimum_level must be debug, info, warning, or error"}}}};
            }
            minimum_level = params["minimum_level"].get<std::string>();
        }
        const auto page_result = m_runtimeLogs->read(cursor, limit, minimum_level);
        if (page_result.isErr()) {
            return {{"error", {{"code", page_result.error().code}, {"message", page_result.error().message}}}};
        }
        auto page = page_result.value();
        page["execution_mode"] = "live";
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
        "scene.mutate", "signal.listConnections", "signal.connect", "signal.disconnect",
        "signal.emit", "physics.raycast", "physics.simulateStep", "nav.bakeMesh",
        "nav.queryPath", "anim.listTracks", "anim.playTrack", "tilemap.setCells",
        "tilemap.getUsedRect", "gridmap.setCells", "asset.instantiate",
        "runtime.injectInput", "runtime.getCallStack", "runtime.readProfiler",
        "vision.setCameraTransform", "vision.toggleDebugDraw"
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

} // namespace godot
} // namespace didi
