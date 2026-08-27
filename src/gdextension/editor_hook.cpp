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
            DIDI_LOG_ERROR("EDITOR_HOOK", "Exception executing command: ", cmd.method);
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
        auto page = m_runtimeLogs->read(cursor, limit, minimum_level);
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
