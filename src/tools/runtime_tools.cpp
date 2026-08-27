#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/gdextension/expression_sandbox.hpp"
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

bool integerInRange(const json& value, int64_t minimum, int64_t maximum) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) return false;
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        return number >= minimum && number <= maximum;
    }
    const auto number = value.get<uint64_t>();
    return number >= static_cast<uint64_t>(minimum) &&
           number <= static_cast<uint64_t>(maximum);
}

std::optional<std::string> validateRuntimePath(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.find('\0') != std::string::npos) {
        return "root_path must be a non-empty UTF-8 path of at most 1024 bytes";
    }
    if (path != "/root" && path.rfind("/root/", 0) != 0) {
        return "root_path must be a canonical absolute path beneath /root";
    }
    if (path.back() == '/' || path.find("//") != std::string::npos ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return "root_path must be a canonical absolute NodePath";
    }
    size_t start = 1;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == ".." || segment.front() == '%') {
            return "root_path may not contain empty, '.', '..', or unique-name alias segments";
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> validateExpressionContextPath(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.find('\0') != std::string::npos) {
        return "context_node must be a non-empty path of at most 1024 bytes";
    }
    if (path != "/root" && path.rfind("/root/", 0) != 0) {
        return "context_node must be a canonical absolute path beneath /root";
    }
    if (path.back() == '/' || path.find("//") != std::string::npos ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return "context_node must be a canonical absolute NodePath";
    }
    size_t start = 1;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string::npos
                                                   ? std::string::npos
                                                   : end - start);
        if (segment.empty() || segment == "." || segment == ".." || segment.front() == '%') {
            return "context_node may not contain aliases or relative segments";
        }
        if (end == std::string::npos) break;
        start = end + 1;
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
    if (!args.is_object() || !args.contains("paused") || !args["paused"].is_boolean()) {
        return CallToolResult::error("Invalid runtime pause request: paused must be a boolean.");
    }
    return forwardLiveRuntime("runtime.setPaused", args, ipc);
}

CallToolResult handleRuntimeStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() ||
        (args.contains("frames") && !integerInRange(args["frames"], 1, 60))) {
        return CallToolResult::error("Invalid runtime step request: frames must be an integer from 1 to 60.");
    }
    return forwardLiveRuntime("runtime.step", args, ipc);
}

CallToolResult handleRuntimeStop(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() ||
        (args.contains("exit_code") && !integerInRange(args["exit_code"], 0, 255))) {
        return CallToolResult::error("Invalid runtime stop request: exit_code must be an integer from 0 to 255.");
    }
    return forwardLiveRuntime("runtime.stop", args, ipc);
}

CallToolResult handleRuntimeGetTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("Invalid runtime tree request: params must be an object.");
    }
    if (args.contains("root_path")) {
        if (!args["root_path"].is_string()) {
            return CallToolResult::error("Invalid runtime tree request: root_path must be a string.");
        }
        if (const auto error = validateRuntimePath(args["root_path"].get<std::string>()); error.has_value()) {
            return CallToolResult::error("Invalid runtime tree request: " + *error + ".");
        }
    }
    if (args.contains("max_depth") && !integerInRange(args["max_depth"], 0, 16)) {
        return CallToolResult::error("Invalid runtime tree request: max_depth must be an integer from 0 to 16.");
    }
    return forwardLiveRuntime("runtime.getTree", args, ipc);
}

CallToolResult handleEvalGdscript(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("expression") || !args["expression"].is_string()) {
        return CallToolResult::error("Invalid expression request: expression is required and must be a string.");
    }
    const auto policy = godot::ExpressionPolicy::validate(args["expression"].get<std::string>());
    if (policy.isErr()) {
        return CallToolResult::error("Invalid expression request: " + policy.error().message + ".");
    }
    if (args.contains("context_node")) {
        if (!args["context_node"].is_string()) {
            return CallToolResult::error("Invalid expression request: context_node must be a string.");
        }
        if (const auto error = validateExpressionContextPath(
                args["context_node"].get<std::string>()); error.has_value()) {
            return CallToolResult::error("Invalid expression request: " + *error + ".");
        }
    }
    if (args.contains("timeout_ms") && !integerInRange(args["timeout_ms"], 1, 5000)) {
        return CallToolResult::error(
            "Invalid expression request: timeout_ms must be an integer from 1 to 5000.");
    }
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
