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
    //
    // The repeat has to happen before the quarantine below, which retires the
    // route and leaves nothing to ask on.
    auto sent = runtime::sendLiveRouteRequest(
        *lease, std::string(binding.ipc_method), arguments, runtime::kMaxPublicLiveRequestMs,
        liveCallIsRepeatable(binding, arguments));
    auto response = std::move(sent.response);
    const bool repeated = sent.repeat_attempted;
    const int transport_repeats = sent.repeat_answered ? 1 : 0;
    if (response.isErr()) {
        auto failure = response.error();
        const auto transport = ipc::transportFailureState(failure);
        ipc::markTransportRepeated(failure, repeated);
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
            // Why the outcome is unknown is the useful half. A mutation whose
            // engine crashed mid-call and one whose engine is still running
            // and merely slow are the same sentence here, and they are not the
            // same problem (#854).
            Error unknown(504, failure.message, failure.data);
            runtime::annotateLiveRouteFailure(unknown, lease->descriptor, quarantined);
            json data = unknown.data.is_object() ? unknown.data : json::object();
            data["retryable"] = false;
            data["outcome"] = "unknown_outcome";
            return phase7Error(binding, 504, "unknown_outcome", std::move(data));
        }
        // Carry what the engine actually said. Collapsing every live failure
        // into a bare "route request failed" leaves a caller -- human or agent
        // -- with no way to tell a bad node path from a dead transport, which
        // is the difference between fixing the request and retrying forever.
        //
        // The status is part of what it said. A 503 reads as a transport or
        // routing problem, so a rejected request sent a caller off to
        // re-verify the session when the engine had already answered and the
        // fix was in the arguments. When the transport is intact the engine
        // answered, and that status and message are the result; 503 stays for
        // a route that could not deliver at all.
        //
        // The clause that used to stand here scoped this to 4xx, which threw
        // the argument away for the one class it matters most in: an engine
        // that refused with a 5xx was reported as a 503 with
        // `data.code: "not_connected"`, on a session whose very next call
        // succeeded. `not_connected` was accidentally true of the signal and
        // false of the session, and the field means the session, so an agent
        // branching on it detached and re-attached over a per-call refusal
        // (#625). A 5xx is still not the caller's to fix, so it becomes a 502
        // rather than passing through, which keeps it distinguishable from
        // both a caller error and a route that could not deliver.
        if (!transport.has_value()) {
            json data = failure.data.is_object() ? failure.data : json::object();
            data["retryable"] = false;
            data["route_quarantine"] = quarantined;
            data["upstream_code"] = failure.code;
            data["upstream_message"] = failure.message;
            // An engine-side failure is still not the caller's to fix, so it
            // does not pass through as the engine's own 5xx and does not look
            // like a request the caller can correct. It is reported as 502:
            // the route delivered and the engine failed, which is a different
            // fact from the route not delivering, and neither of them is
            // "this session is not connected".
            if (failure.code >= 500) {
                // A game this caller asked to stop answers here, because the
                // extension reports a stopped main loop as an engine failure
                // while the process still answers. That is a fact about the
                // session and it used to be attached on the 503 path this
                // class fell through to, so it is asked here before the
                // per-call classification takes over (#595).
                //
                // Through the same funnel as everything else, so an engine
                // that asked for a quarantine on its way out carries the
                // engine state here that it carries on every other route
                // (#854). With no quarantine asked for, this is the requested
                // stop and nothing more, exactly as before.
                Error stopped(503, failure.message, data);
                runtime::annotateLiveRouteFailure(stopped, lease->descriptor, quarantined);
                data = stopped.data.is_object() ? stopped.data : data;
                if (data.value("incident", std::string{}) == "game_stopped") {
                    return phase7Error(binding, 503, "runtime_route_request_failed",
                                       std::move(data));
                }
                if (!data.contains("code")) data["code"] = "engine_refused";
                return phase7Error(binding, 502, failure.message, std::move(data));
            }
            return phase7Error(binding, failure.code, failure.message, std::move(data));
        }
        // Everything a live failure carries on every other route, and then
        // what belongs to this envelope. Until #854 this path annotated only
        // the requested stop, so a crashed engine came back as
        // runtime_route_request_failed with no engine state, no crash report
        // and no incident, and the caller's next move was to call again.
        Error live(503, failure.message, failure.data);
        runtime::annotateLiveRouteFailure(live, lease->descriptor, quarantined);
        json data = live.data.is_object() ? live.data : json::object();
        // Forced, not inherited. A route that could not deliver is not the
        // caller's to retry whatever the failure underneath said about itself,
        // and that has been this envelope's contract since it shipped.
        data["retryable"] = false;
        data["route_quarantine"] = quarantined;
        data["upstream_code"] = failure.code;
        data["upstream_message"] = failure.message;
        return phase7Error(binding, 503, "runtime_route_request_failed", std::move(data));
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
    // Only when it happened, so an ordinary result is unchanged and a result
    // that survived a lost connection says so.
    if (transport_repeats > 0 && !payload.contains("transport")) {
        payload["transport"] = {{"repeats", transport_repeats}};
    }
    return CallToolResult::successJson(payload);
}

} // namespace didi::mcp
