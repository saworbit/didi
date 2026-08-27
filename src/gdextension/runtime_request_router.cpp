#include "didi/gdextension/runtime_request_router.hpp"

#include "didi/common/logger.hpp"
#include "didi/runtime/session_kind_policy.hpp"

namespace didi::godot {
namespace {

json publicSessionEnvelope(const runtime::SessionDescriptor& session) {
    return {{"execution_mode", "live"}, {"session", session.toJson()}};
}

json decorateRuntimeResponse(json response, const runtime::SessionDescriptor& session) {
    const auto envelope = publicSessionEnvelope(session);
    if (!response.is_object()) response = {{"result", std::move(response)}};
    if (response.contains("error")) {
        auto& error = response["error"];
        if (!error.is_object()) {
            error = {{"code", 500}, {"message", "Malformed live engine error"}};
        }
        json data = error.value("data", json::object());
        if (!data.is_object()) data = {{"details", std::move(data)}};
        data["execution_mode"] = envelope["execution_mode"];
        data["session"] = envelope["session"];
        error["data"] = std::move(data);
        return response;
    }
    response["execution_mode"] = envelope["execution_mode"];
    response["session"] = envelope["session"];
    return response;
}

json timeoutResponse(const std::string& method, const runtime::SessionDescriptor& session,
                     const char* outcome, bool quarantine, const char* message) {
    return decorateRuntimeResponse(
        {{"error", {{"code", 504}, {"message", message},
                    {"data", {{"outcome", outcome}, {"route_quarantine", quarantine},
                              {"method", method}}}}}},
        session);
}

} // namespace

json handleSessionHandshake(const json& params, const runtime::SessionDescriptor& session) {
    if (!params.is_object() || !params.contains("protocol_version") ||
        !params["protocol_version"].is_string() || params["protocol_version"] != "1.3") {
        return {{"error", {{"code", 409},
                           {"message", "Runtime session protocol 1.3 is required"},
                           {"data", {{"required_protocol_version", "1.3"}}}}}};
    }
    auto response = session.toJson();
    response["status"] = "ok";
    return response;
}

std::optional<json> rejectDisallowedSessionMethod(
    const std::string& method, const runtime::SessionDescriptor& session) {
    const auto policy = runtime::livePolicyForMethod(method);
    if (runtime::allowsSessionKind(policy, session.kind)) return std::nullopt;
    json allowed = policy == runtime::LiveSessionKindPolicy::editor_only
                       ? json::array({"editor"})
                       : json::array({"game"});
    return decorateRuntimeResponse(
        {{"error", {{"code", 409},
                    {"message", "Live method is unavailable for the selected session kind"},
                    {"data", {{"method", method},
                              {"selected_session_kind", session.kind},
                              {"allowed_session_kinds", std::move(allowed)}}}}}},
        session);
}

json awaitRuntimeCommand(CommandTicket ticket, const std::string& method,
                         const runtime::SessionDescriptor& session,
                         std::chrono::milliseconds deadline) {
    if (ticket.response.wait_for(deadline) == std::future_status::ready) {
        return decorateRuntimeResponse(ticket.response.get(), session);
    }

    if (ticket.control && ticket.control->tryCancelPending()) {
        const auto timeout = timeoutResponse(method, session, "not_started", false,
                                             "Main-thread command timed out before execution");
        if (ticket.response_promise && ticket.control->tryClaimResponse()) {
            ticket.response_promise->set_value(timeout);
        }
        DIDI_LOG_ERROR("GDEXT_IPC", "Command timed out before main-thread execution: ", method);
        return ticket.response.get();
    }

    const auto unknown = timeoutResponse(
        method, session, "unknown_outcome", true,
        "Main-thread command exceeded its deadline after execution started; outcome is unknown");
    if (ticket.control) ticket.control->tryCancelRunning();
    if (ticket.response_promise && ticket.control && ticket.control->tryClaimResponse()) {
        ticket.response_promise->set_value(unknown);
        DIDI_LOG_WARN("GDEXT_IPC", "Quarantining route after unresolved main-thread command: ", method);
        return ticket.response.get();
    }

    // Completion and promise publication are separate atomic operations. Give an already-claimed
    // producer one final bounded opportunity, then return the truthful unknown outcome.
    if (ticket.response.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {
        return decorateRuntimeResponse(ticket.response.get(), session);
    }
    DIDI_LOG_WARN("GDEXT_IPC", "Main-thread command outcome remained unresolved at deadline: ", method);
    return unknown;
}

} // namespace didi::godot
