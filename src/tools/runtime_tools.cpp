#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/runtime/session_client.hpp"

namespace didi {
namespace mcp {

namespace {

std::optional<std::string> validateRuntimeLogRequest(const json& args) {
    if (!args.is_object()) return "params must be an object";
    if (args.contains("cursor")) {
        const auto& cursor = args["cursor"];
        if ((!cursor.is_number_integer() && !cursor.is_number_unsigned()) ||
            (cursor.is_number_integer() && cursor.get<int64_t>() < 0)) {
            return "cursor must be a non-negative integer";
        }
    }
    if (args.contains("limit")) {
        const auto& limit = args["limit"];
        if ((!limit.is_number_integer() && !limit.is_number_unsigned()) ||
            (limit.is_number_integer() && limit.get<int64_t>() < 1) ||
            limit.get<uint64_t>() > 500) {
            return "limit must be an integer from 1 to 500";
        }
    }
    if (args.contains("minimum_level")) {
        if (!args["minimum_level"].is_string()) {
            return "minimum_level must be debug, info, warning, or error";
        }
        const auto level = args["minimum_level"].get<std::string>();
        if (level != "debug" && level != "info" && level != "warning" && level != "error") {
            return "minimum_level must be debug, info, warning, or error";
        }
    }
    return std::nullopt;
}

CallToolResult sessionError(const Error& error) {
    return CallToolResult::error(error.message);
}

CallToolResult localSessionSuccess(json payload) {
    payload["execution_mode"] = "local_session_management";
    return CallToolResult::successJson(payload);
}

CallToolResult forwardLiveRuntime(const std::string& method, const json& args,
                                  const std::shared_ptr<ipc::IIpcClient>& ipc) {
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error("No runtime session is attached.");
    }
    auto result = ipc->sendRequest(method, args, ipc::kWaitForDefinitiveResponse);
    if (result.isErr()) return sessionError(result.error());
    return CallToolResult::successJson(result.value());
}

} // namespace

CallToolResult handleRuntimeListSessions(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return CallToolResult::error("Runtime session management is unavailable.");
    std::optional<std::string> project_path;
    if (args.contains("project_path")) {
        if (!args["project_path"].is_string()) return CallToolResult::error("project_path must be a string.");
        project_path = args["project_path"].get<std::string>();
    }
    auto result = sessions->listSessions(project_path);
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error());
}

CallToolResult handleRuntimeAttachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return CallToolResult::error("Runtime session management is unavailable.");
    if (!args.contains("session_id") || !args["session_id"].is_string()) {
        return CallToolResult::error("session_id must be a string.");
    }
    const auto session_id = args["session_id"].get<std::string>();
    if (session_id.empty()) return CallToolResult::error("session_id is required.");
    auto result = sessions->attachSession(session_id);
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error());
}

CallToolResult handleRuntimeDetachSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return CallToolResult::error("Runtime session management is unavailable.");
    auto result = sessions->detachSession();
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error());
}

CallToolResult handleRuntimeGetSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return CallToolResult::error("Runtime session management is unavailable.");
    const auto active = sessions->activeSession();
    if (!active.has_value()) return CallToolResult::error("No runtime session is attached.");
    return localSessionSuccess({{"session", active->toJson()}, {"connected", sessions->isConnected()}});
}

CallToolResult handleRuntimeReadLogs(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (const auto error = validateRuntimeLogRequest(args); error.has_value()) {
        return CallToolResult::error("Invalid runtime log request: " + *error);
    }
    return forwardLiveRuntime("runtime.getLogs", args, ipc);
}

CallToolResult handleRuntimeSetPaused(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveRuntime("runtime.setPaused", args, ipc);
}

CallToolResult handleRuntimeStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveRuntime("runtime.step", args, ipc);
}

CallToolResult handleRuntimeStop(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveRuntime("runtime.stop", args, ipc);
}

CallToolResult handleRuntimeGetTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveRuntime("runtime.getTree", args, ipc);
}

CallToolResult handleEvalGdscript(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveRuntime("runtime.evalGdscript", args, ipc);
}

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string scene_path = args.value("scene_path", "");
    int timeout_sec = args.value("timeout_seconds", 10);
    bool headless = args.value("headless", true);
    bool break_on_error = args.value("break_on_error", true);

    std::vector<std::string> extra_args;
    if (args.contains("extra_args") && args["extra_args"].is_array()) {
        for (const auto& a : args["extra_args"]) {
            if (a.is_string()) {
                extra_args.push_back(a.get<std::string>());
            }
        }
    }

    offline::TestRunner runner;
    auto session_res = runner.runSession(scene_path, timeout_sec, headless, break_on_error, extra_args);
    return CallToolResult::successJson(session_res.toJson());
}

CallToolResult handleInjectInputEvent(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string event_type = args.value("event_type", "action");

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.injectInput", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to inject input event: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor/Game instance is offline. Launch Godot to inject interactive inputs.");
}

CallToolResult handleRuntimeGetCallStack(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.getCallStack", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to fetch debugger call stack: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot in debug mode to inspect live call stacks.");
}

CallToolResult handleRuntimeReadProfiler(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.readProfiler", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to read profiler telemetry from Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor / game instance is offline. Launch Godot to inspect live profiler metrics (FPS, frame time, draw calls).");
}

} // namespace mcp
} // namespace didi
