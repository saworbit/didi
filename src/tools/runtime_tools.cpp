#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/engine_version.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/version.hpp"
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
    // Same reading of the same facts the live routes get. A session error that
    // said less than a tool error would send a caller looking in two places.
    Error annotated = error;
    runtime::annotateEngineState(annotated, active);
    // With no session left to classify, annotateEngineState says nothing. The
    // remembered obstruction is the only thing that can, and runtime_get_session
    // is the tool a caller reaches for once the engine has gone (#527, #536).
    runtime::annotateRouteObstruction(annotated);
    json data = annotated.data.is_object() ? annotated.data : json::object();
    if (!annotated.data.is_null() && !annotated.data.is_object()) data["details"] = annotated.data;
    json envelope = {{"execution_mode", "local_session_management"},
                     {"session", active.has_value() ? active->toJson() : json(nullptr)},
                     {"error", {{"code", annotated.code}, {"message", annotated.message},
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

// Says whether the session in this payload was published by the same build as
// this server. The two binaries are copied around separately, so a bridge from
// another build answers every call with the tool contract it was built with
// while nothing else in the response looks unusual. Absent build_id means the
// extension predates the field, which is a mismatch and reported as one.
void noteBridgeBuild(json& payload) {
    // Either field. A detach answer names the session it disconnected
    // `detached_session` (#506), and looking only for `session` silently
    // dropped server_build_id from the one answer that reports on a bridge the
    // caller has just stopped talking to.
    auto session = payload.find("session");
    if (session == payload.end() || !session->is_object()) {
        session = payload.find("detached_session");
    }
    if (session == payload.end() || !session->is_object()) return;
    const std::string bridge = session->value("build_id", std::string());
    payload["server_build_id"] = kBuildId;
    if (bridge == kBuildId) return;
    payload["bridge_build_matches"] = false;
    payload["bridge_build_note"] =
        "The attached GDExtension is from a different build than this server. Tool schemas come "
        "from the server and the answers come from the bridge, so a call can be refused for a "
        "reason the schema says is supported. Copy build/addons/didi into the project again and "
        "restart the editor.";
}

CallToolResult localSessionSuccess(json payload) {
    payload["execution_mode"] = "local_session_management";
    noteBridgeBuild(payload);
    return CallToolResult::successJson(payload);
}

CallToolResult forwardLiveRuntime(const char* tool, const std::string& method, const json& args,
                                  const std::shared_ptr<ipc::IIpcClient>& ipc) {
    const auto lease = runtime::acquireRuntimeRouteLease(ipc);
    if (!lease.has_value()) {
        auto error = Error::notConnected("No runtime session is attached");
        runtime::annotateRouteObstruction(error);
        return liveError(error, activeSessionFor(ipc));
    }
    const auto session = lease->descriptor;
    if ((method == "runtime.setPaused" || method == "runtime.step" || method == "runtime.stop") &&
        (!session.has_value() || session->kind != "game")) {
        return liveError(Error(409, "Runtime control is available only for game sessions",
                               {{"allowed_session_kinds", json::array({"game"})}}), session);
    }
    constexpr int kEndToEndLiveDeadlineMs = 17000;
    // The repeat has to happen before the quarantine below, which retires the
    // route and leaves nothing to ask on.
    auto sent = runtime::sendLiveRouteRequest(
        *lease, method, args, kEndToEndLiveDeadlineMs,
        liveCallIsRepeatable(resolveAliasBinding(tool, args), args));
    auto result = std::move(sent.response);
    if (result.isErr()) {
        auto error = result.error();
        ipc::markTransportRepeated(error, sent.repeat_attempted);
        const bool explicit_quarantine = error.data.is_object() &&
            error.data.value("route_quarantine", false);
        const auto transport = ipc::transportFailureState(error);
        const bool known_transport_timeout = error.code == 500 &&
            (error.message.rfind("Timeout waiting for response", 0) == 0 ||
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
            // A refusal from the extension of a game this caller asked to stop,
            // such as its main loop having gone while the process still
            // answers, is the requested exit and not a route to retry (#595).
            if (error.code == 503 || error.code == 504) {
                runtime::annotateRequestedStop(error, session);
            }
                error.data["outcome"] = "unknown_outcome";
            }
            runtime::annotateEngineState(error, session);
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
    // Only when it happened, so an ordinary result is unchanged and one that
    // survived a lost connection says so.
    if (sent.repeat_answered) response["transport"] = {{"repeats", 1}};
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
    if (args.contains("allow_foreign_project") && !args["allow_foreign_project"].is_boolean()) {
        return sessionError(Error::invalidArgument("allow_foreign_project must be a boolean"), sessions);
    }
    const bool allow_foreign_project = args.value("allow_foreign_project", false);
    auto result = sessions->attachSession(session_id, allow_foreign_project);
    return result.isOk() ? localSessionSuccess(result.value()) : sessionError(result.error(), sessions);
}

CallToolResult handleRuntimeDetachSession(const json&, std::shared_ptr<runtime::IRuntimeSessionClient> sessions) {
    if (!sessions) return sessionError(Error::notConnected("Runtime session management is unavailable"), sessions);
    auto result = sessions->detachSession();
    if (result.isErr()) return sessionError(result.error(), sessions);
    // Say what happened. The descriptor coming back is the editor this call has
    // just disconnected, and it used to sit in the same `session` field a
    // connected answer uses, with no `connected` key at all -- so a detached
    // answer and an attached one were told apart by an absence, which is not a
    // statement (#506). Named for what it is, and the state stated outright.
    json payload = result.value();
    if (payload.contains("session")) {
        payload["detached_session"] = payload["session"];
        payload.erase("session");
    }
    // Whether this call is the one that released something, or found the work
    // already done. Both are the cleanup succeeding (#537).
    if (!payload.contains("detached")) payload["detached"] = false;
    payload["connected"] = false;
    return localSessionSuccess(std::move(payload));
}

CallToolResult handleRuntimeGetSession(const json&,
                                      std::shared_ptr<runtime::IRuntimeSessionClient> sessions,
                                      std::vector<std::string> available_without_engine) {
    if (!sessions) return sessionError(Error::notConnected("Runtime session management is unavailable"), sessions);
    auto result = sessions->refreshSession();
    if (result.isErr()) {
        auto error = result.error();
        // The one tool a caller reaches for when the engine has gone is this
        // one, so it has to answer even when the handshake cannot. Saying what
        // still works is the whole point of being asked.
        if (!error.data.is_object()) error.data = json::object();
        error.data["available_without_engine"] = available_without_engine;
        return sessionError(error, sessions);
    }
    auto value = result.value();
    // A handshake can still succeed while a game this caller asked to stop is
    // tearing down: its IPC thread answers after its main loop has gone. The
    // answer is true, and it says what the caller did (#595).
    if (value.is_object() && value.contains("session") && value["session"].is_object()) {
        const auto& session = value["session"];
        if (const auto stop = runtime::requestedStopFor(session.value("pid", uint64_t{0}),
                                                        session.value("session_id", std::string()))) {
            value["stop_requested"] = {
                {"exit_code", stop->exit_code},
                {"requested_by", "runtime_stop"},
                {"note", "This session was asked to exit and is still answering while it shuts "
                         "down; it will not be there for the next call."}};
        }
    }
    auto payload = localSessionSuccess(value);
    return payload;
}

CallToolResult handleRuntimeReadLogs(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (const auto error = validateRuntimeLogRequest(args); error.has_value()) {
        return liveValidationError("Invalid runtime log request: " + *error, ipc);
    }
    return forwardLiveRuntime("runtime_read_logs", "runtime.getLogs", args, ipc);
}

// Engine output shares the log query shape, so it shares the validator. What
// differs is only which stream the editor hook reads.
CallToolResult handleRuntimeReadOutput(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (const auto error = validateRuntimeLogRequest(args); error.has_value()) {
        return liveValidationError("Invalid runtime output request: " + *error, ipc);
    }
    return forwardLiveRuntime("runtime_read_output", "runtime.getOutput", args, ipc);
}

CallToolResult handleRuntimeSetPaused(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() || !args.contains("paused") || !args["paused"].is_boolean()) {
        return liveValidationError("Invalid runtime pause request: paused must be a boolean", ipc);
    }
    return forwardLiveRuntime("runtime_set_paused", "runtime.setPaused", args, ipc);
}

CallToolResult handleRuntimeStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() ||
        (args.contains("frames") && !integerInRange(args["frames"], 1, 60))) {
        return liveValidationError(
            "Invalid runtime step request: frames must be an integer from 1 to 60", ipc);
    }
    return forwardLiveRuntime("runtime_step", "runtime.step", args, ipc);
}

CallToolResult handleRuntimeStop(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object() ||
        (args.contains("exit_code") && !integerInRange(args["exit_code"], 0, 255))) {
        return liveValidationError(
            "Invalid runtime stop request: exit_code must be an integer from 0 to 255", ipc);
    }
    // Which game is being asked to go, read before the request so the answer
    // to every later call can say the exit was this caller's doing (#595).
    const auto lease = runtime::acquireRuntimeRouteLease(ipc);
    auto result = forwardLiveRuntime("runtime_stop", "runtime.stop", args, ipc);
    if (!result.isError && lease.has_value() && lease->descriptor.has_value()) {
        runtime::recordRequestedStop({lease->descriptor->pid, lease->descriptor->session_id,
                                      args.value("exit_code", int64_t{0}), 0});
    }
    return result;
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
    return forwardLiveRuntime("runtime_get_tree", "runtime.getTree", args, ipc);
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
    return forwardLiveRuntime("eval_gdscript", "runtime.evalGdscript", args, ipc);
}

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::error("runtime_launch arguments must be an object");
    }
    if (args.contains("timeout_seconds") &&
        !integerInRange(args["timeout_seconds"], 1, 120)) {
        return CallToolResult::error("timeout_seconds must be an integer from 1 to 120");
    }
    if (args.contains("detach") && !args["detach"].is_boolean()) {
        return CallToolResult::error("detach must be a boolean");
    }
    std::string scene_path = args.value("scene_path", "");
    int timeout_sec = args.value("timeout_seconds", 10);
    bool headless = args.value("headless", true);
    bool break_on_error = args.value("break_on_error", true);
    const bool detach = args.value("detach", false);

    std::vector<std::string> extra_args;
    if (args.contains("extra_args") && args["extra_args"].is_array()) {
        for (const auto& a : args["extra_args"]) {
            if (a.is_string()) {
                extra_args.push_back(a.get<std::string>());
            }
        }
    }

    // Taken before the spawn, so a session published by the game we are about
    // to start can be told from one that was already there.
    const int64_t launched_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    offline::TestRunner runner;
    auto session_res =
        runner.runSession(scene_path, timeout_sec, headless, break_on_error, extra_args, detach);
    json result = session_res.toJson();

    // A detached game is only useful once it has published a session, because
    // that is what every runtime tool routes through. Returning the moment the
    // process exists would hand the caller a pid and a race; this waits for the
    // descriptor, bounded by the same timeout the blocking mode uses, and says
    // plainly if it never appeared.
    const auto sessions_for_detach =
        detach ? std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc) : nullptr;
    if (detach && session_res.pid != 0) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        json published;
        while (std::chrono::steady_clock::now() < deadline) {
            if (sessions_for_detach) {
                auto listed = sessions_for_detach->listSessions(std::nullopt);
                if (listed.isOk() && listed.value().is_object() &&
                    listed.value()["sessions"].is_array()) {
                    for (const auto& entry : listed.value()["sessions"]) {
                        if (entry.value("kind", std::string()) != "game") continue;
                        // The pid when it is ours, which it is for a direct
                        // launch. It is not for a Godot that launches the
                        // engine and waits -- a godot.cmd wrapper, or Godot's
                        // own Windows console build -- where the game is a
                        // grandchild with a pid of its own. A game session on
                        // this project that started after this call did is the
                        // one this call started.
                        if (entry.value("pid", uint64_t{0}) == session_res.pid ||
                            entry.value("started_at_ms", int64_t{0}) >= launched_at_ms) {
                            published = entry;
                            break;
                        }
                    }
                }
            }
            if (!published.is_null()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!published.is_null()) {
            // The pid that matters is the game's, not the launcher's.
            result["pid"] = published["pid"];
            // And it is the session the calls after this one mean. Publishing
            // it without selecting it left them going wherever the process was
            // already pointed, which is the editor this game was launched
            // from, so runtime_set_paused came back 409 "unavailable for the
            // selected session kind" on a game that was running and reachable.
            const auto session_id = published.value("session_id", std::string());
            if (sessions_for_detach && !session_id.empty()) {
                sessions_for_detach->attachSession(session_id);
            }
        }
        result["session_published"] = !published.is_null();
        result["game_session"] = published.is_null() ? json(nullptr) : published;
        result["success"] = !published.is_null();
        result["limitation"] =
            "This game is running and this call is not watching it. Nothing was captured, so "
            "logs, errors and exit_code are empty: read a running game with runtime_read_output, "
            "drive it with runtime_attach_session and the other runtime tools, and end it with "
            "runtime_stop.";
        if (published.is_null()) {
            result["summary"] =
                "The game was started as process " + std::to_string(session_res.pid) +
                " and published no session within " + std::to_string(timeout_sec) +
                " seconds. It may still be starting, or the Didi addon may not be enabled in "
                "this project. It is still running; stop it yourself if it should not be.";
        } else {
            result["summary"] = "The game is running as process " +
                                std::to_string(session_res.pid) +
                                " and has published a session to attach to.";
        }
    } else if (detach) {
        result["session_published"] = false;
        result["game_session"] = nullptr;
    }
    // The same comparison the two tools that shell out to a discovered Godot
    // have made since #617. This one had no engine fields at all, so a project
    // run by 4.7 while its editor is 4.5 could only be spotted by reading the
    // banner out of the captured logs (#687).
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(ipc);
    const auto attached = sessions ? sessions->observableSession()
                                   : std::optional<runtime::SessionDescriptor>{};
    versions::annotateCheckEngine(result, session_res.engine_version,
                                  session_res.engine_executable,
                                  attached.has_value() ? attached->engine_version : std::string());
    const auto configured = offline::resolveGodotExecutableDetailed();
    versions::annotateConfiguredEngine(result, configured.configured,
                                       configured.configured_rejected);
    return CallToolResult::successJson(result);
}

CallToolResult handleInjectInputEvent(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleRuntimeGetCallStack(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleRuntimeReadProfiler(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleRuntimeWatchInvariants(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleRuntimeExploreScene(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

} // namespace mcp
} // namespace didi
