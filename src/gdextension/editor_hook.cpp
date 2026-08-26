#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/visual_test_lab.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/types.hpp"
#include <unordered_set>

namespace didi {
namespace godot {

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
    auto cancelled = std::make_shared<std::atomic<bool>>(false);

    EngineCommand cmd;
    cmd.method = method;
    cmd.params = params;
    cmd.response_promise = prom;
    cmd.cancelled = cancelled;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!GodotApi::instance().isLiveReady()) {
            prom->set_value({{"error", {{"code", 503}, {"message", "Godot main-loop bridge is not ready"}}}});
            return {std::move(fut), std::move(cancelled)};
        }
        m_commandQueue.push(std::move(cmd));
    }

    return {std::move(fut), std::move(cancelled)};
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
        if (cmd.cancelled && cmd.cancelled->load()) {
            if (cmd.response_promise) {
                cmd.response_promise->set_value({{"error", {{"code", 504}, {"message", "Command cancelled after timeout"}}}});
            }
            continue;
        }
        try {
            json result = executeOnMainThread(cmd.method, cmd.params);
            if (cmd.response_promise) {
                cmd.response_promise->set_value(result);
            }
        } catch (const std::exception& e) {
            DIDI_LOG_ERROR("EDITOR_HOOK", "Exception executing command '", cmd.method, "': ", e.what());
            if (cmd.response_promise) {
                cmd.response_promise->set_value({{"error", {{"code", 500}, {"message", e.what()}}}});
            }
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
        if (command.cancelled) command.cancelled->store(true);
        if (command.response_promise) {
            command.response_promise->set_value(
                {{"error", {{"code", 503}, {"message", reason}}}});
        }
    }
}

json EditorHook::executeOnMainThread(const std::string& method, const json& params) {
    DIDI_LOG_DEBUG("EDITOR_HOOK", "Executing command on Godot main thread: ", method);

    static const std::unordered_set<std::string> live_phase_one = {
        "editor.getState", "scene.getHierarchy", "scene.instantiateNode",
        "scene.removeNode", "scene.reparentNode", "scene.setProperty",
        "scene.getProperty", "scene.duplicateNode", "editor.undo", "editor.redo",
        "editor.saveScene", "editor.reloadProject"
    };
    if (live_phase_one.count(method)) {
        return GodotBridge::instance().execute(method, params);
    }

    if (method == "vision.captureViewport") {
        return ViewportRenderer::instance().captureViewport(params);
    }
    if (method == "asset.query") {
        offline::ResourceIndexer indexer;
        indexer.scan(".");
        auto matches = indexer.query(params.value("search_path", "res://"),
                                     params.value("type_filter", ""),
                                     params.value("fuzzy_query", ""));
        json resources = json::array();
        for (const auto& resource : matches) resources.push_back(resource.toJson());
        return {{"execution_mode", "offline_fallback"},
                {"resources", resources}, {"total_found", matches.size()}};
    }
    if (method == "script.diagnostics" || method == "script.checkSyntax") {
        json result = handleScriptDiagnostics(params);
        result["execution_mode"] = "offline_fallback";
        return result;
    }
    if (method == "script.reflectClass") {
        json result = offline::GDScriptDiagnostics::reflectClass(params.value("class_name", "Node"));
        result["execution_mode"] = "offline_fallback";
        return result;
    }
    if (method == "runtime.getLogs") {
        return {{"execution_mode", "live"}, {"logs", getRecentLogs()}};
    }
    if (method == "vision.createVisualTestLab") {
        json result = VisualTestLab::instance().createLab(params);
        result["execution_mode"] = "offline_fallback";
        return result;
    }

    static const std::unordered_set<std::string> registered_but_unimplemented = {
        "scene.mutate", "signal.listConnections", "signal.connect", "signal.disconnect",
        "signal.emit", "physics.raycast", "physics.simulateStep", "nav.bakeMesh",
        "nav.queryPath", "anim.listTracks", "anim.playTrack", "tilemap.setCells",
        "tilemap.getUsedRect", "gridmap.setCells", "asset.instantiate",
        "resource.create", "resource.inspect", "script.patchSymbols",
        "runtime.injectInput", "runtime.getCallStack", "runtime.readProfiler",
        "vision.setCameraTransform", "vision.toggleDebugDraw"
    };
    if (registered_but_unimplemented.count(method)) {
        return {{"error", {{"code", 501},
                           {"message", "Method is registered for compatibility but has no trustworthy live implementation: " + method}}}};
    }
    return {{"error", {{"code", 404}, {"message", "Unknown method: " + method}}}};
}
json EditorHook::handleScriptDiagnostics(const json& params) {
    std::string file_path = params.value("file_path", "");
    std::string source_text = params.value("source_text", "");

    auto diags = offline::GDScriptDiagnostics::analyze(file_path, source_text);
    json diag_arr = json::array();
    bool has_err = false;

    for (const auto& d : diags) {
        diag_arr.push_back(d.toJson());
        if (d.severity == "error") has_err = true;
    }

    return {
        {"file_path", file_path},
        {"diagnostics", diag_arr},
        {"diagnostics_count", diags.size()},
        {"has_errors", has_err}
    };
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
