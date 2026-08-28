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

CallToolResult sessionError(const Error& error,
                            const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions) {
    const auto active = sessions ? sessions->activeSession()
                                 : std::optional<runtime::SessionDescriptor>{};
    json data = error.data.is_object() ? error.data : json::object();
    if (!error.data.is_null() && !error.data.is_object()) data["details"] = error.data;
    json envelope = {{"execution_mode", "local_session_management"},
                     {"session", active.has_value() ? active->toJson() : json(nullptr)},
                     {"error", {{"code", error.code}, {"message", error.message},
                                {"data", std::move(data)}}}};
    auto result = CallToolResult::successJson(envelope);
    result.isError = true;
    return result;
}

CallToolResult liveError(const Error& error,
                         const std::optional<runtime::SessionDescriptor>& session) {
    json data = error.data.is_object() ? error.data : json::object();
    if (!error.data.is_null() && !error.data.is_object()) data["details"] = error.data;
    json envelope = {
        {"execution_mode", "live"},
        {"session", session.has_value() ? session->toJson() : json(nullptr)},
        {"error", {{"code", error.code}, {"message", error.message}, {"data", std::move(data)}}}
    };
    auto result = CallToolResult::successJson(envelope);
    result.isError = true;
    return result;
}

std::optional<runtime::SessionDescriptor> activeSessionFor(
    const std::shared_ptr<ipc::IIpcClient>& ipc) {
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    return sessions ? sessions->activeSession() : std::optional<runtime::SessionDescriptor>{};
}

CallToolResult liveValidationError(const std::string& message,
                                   const std::shared_ptr<ipc::IIpcClient>& ipc) {
    return liveError(Error::invalidArgument(message), activeSessionFor(ipc));
}

CallToolResult localSessionSuccess(json payload) {
    payload["execution_mode"] = "local_session_management";
    return CallToolResult::successJson(payload);
}

CallToolResult forwardLiveRuntime(const std::string& method, const json& args,
                                  const std::shared_ptr<ipc::IIpcClient>& ipc) {
    const auto lease = runtime::acquireRuntimeRouteLease(ipc);
    if (!lease.has_value()) {
        return liveError(Error::notConnected("No runtime session is attached"),
                         activeSessionFor(ipc));
    }
    const auto session = lease->descriptor;
    if ((method == "runtime.setPaused" || method == "runtime.step" || method == "runtime.stop") &&
        (!session.has_value() || session->kind != "game")) {
        return liveError(Error(409, "Runtime control is available only for game sessions",
                               {{"allowed_session_kinds", json::array({"game"})}}), session);
    }
    constexpr int kEndToEndLiveDeadlineMs = 17000;
    auto result = lease->sendRequest(method, args, kEndToEndLiveDeadlineMs);
    if (result.isErr()) {
        auto error = result.error();
        const bool explicit_quarantine = error.data.is_object() &&
            error.data.value("route_quarantine", false);
        const auto transport = ipc::transportFailureState(error);
        const bool known_transport_timeout = error.code == 500 &&
            (error.message.rfind("Timeout waiting for response", 0) == 0 ||
             error.message.rfind("Failed or timed out reading response", 0) == 0 ||
             error.message.rfind("Failed or timed out writing to", 0) == 0);
        const bool transport_deadline =
            (error.code == 504 || known_transport_timeout) &&
            (!error.data.is_object() || !error.data.contains("outcome"));
        if (explicit_quarantine || transport.has_value() || transport_deadline) {
            if (transport_deadline && !transport.has_value()) error.code = 504;
            if (!error.data.is_object()) error.data = json::object();
            if (transport.has_value()) {
                error.data["outcome"] = transport->outcome_unknown
                                              ? "unknown_outcome"
                                              : "not_started";
            } else {
                error.data["outcome"] = "unknown_outcome";
            }
            error.data["route_quarantine"] = true;
            (void)runtime::quarantineRuntimeRoute(ipc, *lease);
        }
        return liveError(error, session);
    }
    json response = result.value().is_object()
                        ? result.value()
                        : json{{"result", result.value()}};
    response["execution_mode"] = "live";
    response["session"] = session.has_value() ? session->toJson() : json(nullptr);
    return CallToolResult::successJson(response);
}

} // namespace

CallToolResult handleRuntimeListSessions(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return sessionError(Error::notConnected("Runtime session management is unavailable"), sessions);
    std::optional<std::string> project_path;
    if (args.contains("project_path")) {
        if (!args["project_path"].is_string()) {
            return sessionError(Error::invalidArgument("project_path must be a string"), sessions);
        }
        project_path = args["project_path"].get<std::string>();
    }
    auto result = sessions->listSessions(project_path);
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error(), sessions);
}

CallToolResult handleRuntimeAttachSession(const json& args, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return sessionError(Error::notConnected("Runtime session management is unavailable"), sessions);
    if (!args.contains("session_id") || !args["session_id"].is_string()) {
        return sessionError(Error::invalidArgument("session_id must be a string"), sessions);
    }
    const auto session_id = args["session_id"].get<std::string>();
    if (session_id.empty()) return sessionError(Error::invalidArgument("session_id is required"), sessions);
    auto result = sessions->attachSession(session_id);
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error(), sessions);
}

CallToolResult handleRuntimeDetachSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return sessionError(Error::notConnected("Runtime session management is unavailable"), sessions);
    auto result = sessions->detachSession();
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error(), sessions);
}

CallToolResult handleRuntimeGetSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return sessionError(Error::notConnected("Runtime session management is unavailable"), sessions);
    auto result = sessions->refreshSession();
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error(), sessions);
}

CallToolResult handleRuntimeReadLogs(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (const auto error = validateRuntimeLogRequest(args); error.has_value()) {
        return liveValidationError("Invalid runtime log request: " + *error, ipc);
    }
    return forwardLiveRuntime("runtime.getLogs", args, ipc);
}

CallToolResult handleRuntimeSetPaused(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("paused") || !args["paused"].is_boolean()) {
        return liveValidationError("Invalid runtime pause request: paused must be a boolean", ipc);
    }
    return forwardLiveRuntime("runtime.setPaused", args, ipc);
}

CallToolResult handleRuntimeStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() ||
        (args.contains("frames") && !integerInRange(args["frames"], 1, 60))) {
        return liveValidationError(
            "Invalid runtime step request: frames must be an integer from 1 to 60", ipc);
    }
    return forwardLiveRuntime("runtime.step", args, ipc);
}

CallToolResult handleRuntimeStop(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() ||
        (args.contains("exit_code") && !integerInRange(args["exit_code"], 0, 255))) {
        return liveValidationError(
            "Invalid runtime stop request: exit_code must be an integer from 0 to 255", ipc);
    }
    return forwardLiveRuntime("runtime.stop", args, ipc);
}

CallToolResult handleRuntimeGetTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return liveValidationError("Invalid runtime tree request: params must be an object", ipc);
    }
    if (args.contains("root_path")) {
        if (!args["root_path"].is_string()) {
            return liveValidationError(
                "Invalid runtime tree request: root_path must be a string", ipc);
        }
        if (const auto error = validateRuntimePath(args["root_path"].get<std::string>()); error.has_value()) {
            return liveValidationError("Invalid runtime tree request: " + *error, ipc);
        }
    }
    if (args.contains("max_depth") && !integerInRange(args["max_depth"], 0, 16)) {
        return liveValidationError(
            "Invalid runtime tree request: max_depth must be an integer from 0 to 16", ipc);
    }
    return forwardLiveRuntime("runtime.getTree", args, ipc);
}

CallToolResult handleEvalGdscript(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("expression") || !args["expression"].is_string()) {
        return liveValidationError(
            "Invalid expression request: expression is required and must be a string", ipc);
    }
    const auto policy = godot::ExpressionPolicy::validate(args["expression"].get<std::string>());
    if (policy.isErr()) {
        return liveValidationError("Invalid expression request: " + policy.error().message, ipc);
    }
    if (args.contains("context_node")) {
        if (!args["context_node"].is_string()) {
            return liveValidationError(
                "Invalid expression request: context_node must be a string", ipc);
        }
        if (const auto error = validateExpressionContextPath(
                args["context_node"].get<std::string>()); error.has_value()) {
            return liveValidationError("Invalid expression request: " + *error, ipc);
        }
    }
    if (args.contains("timeout_ms") && !integerInRange(args["timeout_ms"], 1, 5000)) {
        return liveValidationError(
            "Invalid expression request: timeout_ms must be an integer from 1 to 5000", ipc);
    }
    return forwardLiveRuntime("runtime.evalGdscript", args, ipc);
}

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("runtime_launch arguments must be an object");
    }
    if (args.contains("timeout_seconds") &&
        !integerInRange(args["timeout_seconds"], 1, 120)) {
        return CallToolResult::error("timeout_seconds must be an integer from 1 to 120");
    }
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
