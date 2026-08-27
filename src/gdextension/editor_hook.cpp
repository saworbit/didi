#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
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
    // Queue is pumped by Godot's registered main-loop frame callback.
}

EditorHook::~EditorHook() = default;

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
            json result = executeOnMainThread(cmd.method, cmd.params);
            cmd.control->markCompleted();
            fulfillCommand(cmd.response_promise, cmd.control, std::move(result));
        } catch (const std::exception& e) {
            DIDI_LOG_ERROR("EDITOR_HOOK", "Exception executing command '", cmd.method, "': ", e.what());
            cmd.control->markCompleted();
            fulfillCommand(cmd.response_promise, cmd.control,
                           {{"error", {{"code", 500}, {"message", e.what()}}}});
        }
    }
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
        "scene.packBranch"
    };
    if (live_bridge_methods.count(method)) {
        return GodotBridge::instance().execute(method, params);
    }

    if (method == "vision.captureViewport") {
        return ViewportRenderer::instance().captureViewport(params);
    }
    if (method == "runtime.getLogs") {
        return {{"execution_mode", "live"}, {"logs", getRecentLogs()}};
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

void EditorHook::addLogMessage(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logBuffer.push_back({
        {"level", level},
        {"message", message},
        {"timestamp", ""}
    });
    if (m_logBuffer.size() > 500) {
        m_logBuffer.erase(m_logBuffer.begin());
    }
}

json EditorHook::getRecentLogs(size_t max_count) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    json logs = json::array();
    size_t start = (m_logBuffer.size() > max_count) ? (m_logBuffer.size() - max_count) : 0;
    for (size_t i = start; i < m_logBuffer.size(); ++i) {
        logs.push_back(m_logBuffer[i]);
    }
    return logs;
}

} // namespace godot
} // namespace didi
