#include "didi/tools/phase7_live_forward.hpp"

#include "didi/runtime/session_client.hpp"
#include "didi/mcp/mutation_safety.hpp"

#include <array>
#include <string>

namespace didi::mcp {
namespace {

CallToolResult phase7Error(const ResolvedToolBinding& binding, int code,
                           std::string_view message, json data = json::object()) {
    data["tool"] = binding.invoked_name;
    data["canonical_tool"] = binding.canonical_name;
    if (!data.contains("retryable")) data["retryable"] = false;
    return CallToolResult::error(
        json{{"error", {{"code", code}, {"message", message}, {"data", std::move(data)}}}}.dump());
}

bool isTask1Blocker(std::string_view canonical_name) {
    constexpr std::array<std::string_view, 3> blockers = {
        "physics_simulate_step", "nav_bake_mesh", "runtime_get_call_stack"};
    for (const auto blocker : blockers) {
        if (canonical_name == blocker) return true;
    }
    return false;
}

size_t responseLimit(std::string_view canonical_name) {
    if (canonical_name == "signal_list_connections") return 64u * 1024u;
    if (canonical_name == "tilemap_get_used_rect") return 16u * 1024u;
    if (canonical_name == "spatial_query_raycast_batch") return 64u * 1024u;
    return 256u * 1024u;
}

CallToolResult malformedResponse(const ResolvedToolBinding& binding,
                                 const std::shared_ptr<ipc::IIpcClient>& client,
                                 const runtime::RuntimeRouteLease& lease) {
    runtime::quarantineRuntimeRoute(client, lease);
    return phase7Error(binding, 500, "extension_protocol_error",
                       {{"route_quarantine", true}});
}

} // namespace

CallToolResult sendPhase7LiveRequest(const ResolvedToolBinding& binding,
                                     const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client) {
    if (isTask1Blocker(binding.canonical_name)) {
        return phase7Error(binding, 501, "phase7_tool_blocked");
    }
    if (!client || !client->isConnected()) {
        return phase7Error(binding, 503, "runtime_route_unavailable", {{"retryable", true}});
    }

    const bool managed_route =
        std::dynamic_pointer_cast<runtime::IRuntimeRouteLeaseProvider>(client) != nullptr;
    const auto lease = runtime::acquireRuntimeRouteLease(client);
    if (!lease.has_value() || !lease->client || !lease->client->isConnected()) {
        return phase7Error(binding, 503,
                           managed_route ? "runtime_route_lease_unavailable"
                                         : "runtime_route_unavailable",
                           {{"retryable", true}});
    }

    // Send through the lease, not through its raw client. The lease is what
    // attaches the session token the extension authenticates against; going
    // straight to the client produced a 401 on every request. No Phase 7 tool
    // had ever executed against a real session, so nothing caught it until the
    // first one shipped.
    const auto response = lease->sendRequest(
        std::string(binding.ipc_method), arguments, runtime::kMaxPublicLiveRequestMs);
    if (response.isErr()) {
        const auto& failure = response.error();
        const auto transport = ipc::transportFailureState(failure);
        const bool explicit_quarantine =
            failure.data.is_object() && failure.data.value("route_quarantine", false);
        // Quarantine is for a broken transport, not for an engine that
        // answered. Retiring the route on an ordinary rejection -- a bad node
        // path, a missing method, a validation failure -- leaves every later
        // live call in the session unable to dispatch, including tools that
        // have nothing to do with the one that failed.
        const bool quarantined = (transport.has_value() || explicit_quarantine)
                                     ? runtime::quarantineRuntimeRoute(client, *lease)
                                     : false;
        if (MutationSafety::isMutation(binding) && transport.has_value() &&
            transport->request_started && transport->outcome_unknown) {
            return phase7Error(binding, 504, "unknown_outcome",
                               {{"retryable", false}, {"outcome", "unknown_outcome"},
                                {"route_quarantine", quarantined}});
        }
        // Carry what the engine actually said. Collapsing every live failure
        // into a bare "route request failed" leaves a caller -- human or agent
        // -- with no way to tell a bad node path from a dead transport, which
        // is the difference between fixing the request and retrying forever.
        //
        // The status is part of what it said. A 503 reads as a transport or
        // routing problem, so a rejected request sent a caller off to
        // re-verify the session when the engine had already answered and the
        // fix was in the arguments. When the transport is intact and the
        // engine refused with a client error, that status and message are the
        // result; 503 stays for a route that could not deliver at all.
        if (!transport.has_value() && failure.code >= 400 && failure.code < 500) {
            json data = failure.data.is_object() ? failure.data : json::object();
            data["retryable"] = false;
            data["route_quarantine"] = quarantined;
            data["upstream_code"] = failure.code;
            data["upstream_message"] = failure.message;
            return phase7Error(binding, failure.code, failure.message, std::move(data));
        }
        return phase7Error(binding, 503, "runtime_route_request_failed",
                           {{"retryable", false},
                            {"route_quarantine", quarantined},
                            {"upstream_code", failure.code},
                            {"upstream_message", failure.message}});
    }

    json payload = response.value();
    if (!payload.is_object()) return malformedResponse(binding, client, *lease);

    const auto limit = responseLimit(binding.canonical_name);
    if (payload.dump().size() > limit) {
        runtime::quarantineRuntimeRoute(client, *lease);
        return phase7Error(binding, 413, "envelope_or_response_limit",
                           {{"limit_bytes", limit}, {"route_quarantine", true}});
    }

    if (payload.contains("error")) {
        const auto& error = payload["error"];
        if (!error.is_object() || !error.contains("code") ||
            !error["code"].is_number_integer() || !error.contains("message") ||
            !error["message"].is_string()) {
            return malformedResponse(binding, client, *lease);
        }
        json data = error.value("data", json::object());
        if (!data.is_object()) data = json::object();
        data["tool"] = binding.invoked_name;
        data["canonical_tool"] = binding.canonical_name;
        data["retryable"] = false;
        return CallToolResult::error(
            json{{"error", {{"code", error["code"]}, {"message", error["message"]},
                            {"data", std::move(data)}}}}.dump());
    }

    payload["tool"] = binding.invoked_name;
    payload["canonical_tool"] = binding.canonical_name;
    return CallToolResult::successJson(payload);
}

} // namespace didi::mcp
