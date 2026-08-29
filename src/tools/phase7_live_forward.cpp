#include "didi/tools/phase7_live_forward.hpp"

#include "didi/runtime/session_client.hpp"

namespace didi::mcp {
namespace {

CallToolResult phase7Error(std::string_view invoked_name, int code,
                           std::string message, json data = json::object()) {
    if (!data.is_object()) data = json::object();
    data["tool"] = invoked_name;
    data["retryable"] = false;
    auto result = CallToolResult::successJson(
        {{"error", {{"code", code}, {"message", std::move(message)},
                     {"data", std::move(data)}}}});
    result.isError = true;
    return result;
}

} // namespace

CallToolResult sendPhase7LiveRequest(std::string_view invoked_name,
                                     std::string_view canonical_name,
                                     std::string_view method,
                                     const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client) {
    constexpr int kPublicDeadlineMs = 17'000;
    if (!client || !client->isConnected()) {
        return phase7Error(invoked_name, 503, "route_or_main_loop_unavailable",
                           {{"outcome", "not_started"}});
    }

    const auto lease = runtime::acquireRuntimeRouteLease(client);
    auto response = client->sendRequest(std::string(method), arguments, kPublicDeadlineMs);
    if (response.isErr()) {
        auto data = response.error().data;
        if (!data.is_object()) data = json::object();
        if (lease.has_value() && data.value("route_quarantine", false)) {
            (void)runtime::quarantineRuntimeRoute(client, *lease);
        }
        return phase7Error(invoked_name, response.error().code,
                           response.error().message, std::move(data));
    }
    if (!response.value().is_object()) {
        if (lease.has_value()) (void)runtime::quarantineRuntimeRoute(client, *lease);
        return phase7Error(invoked_name, 500, "extension_protocol_error",
                           {{"reason", "malformed_response"},
                            {"route_quarantine", lease.has_value()}});
    }

    auto payload = response.value();
    const size_t response_limit = canonical_name == "signal_list_connections"
                                      ? 64u * 1024u
                                      : canonical_name == "tilemap_get_used_rect"
                                            ? 16u * 1024u
                                            : 256u * 1024u;
    const auto response_size = payload.dump().size();
    if (response_size > response_limit) {
        if (lease.has_value()) (void)runtime::quarantineRuntimeRoute(client, *lease);
        return phase7Error(invoked_name, 413, "envelope_or_response_limit",
                           {{"response_bytes", response_size},
                            {"limit_bytes", response_limit},
                            {"route_quarantine", lease.has_value()}});
    }
    if (payload.contains("error") && payload["error"].is_object()) {
        if (!payload["error"].contains("code") || !payload["error"]["code"].is_number_integer() ||
            !payload["error"].contains("message") || !payload["error"]["message"].is_string()) {
            if (lease.has_value()) (void)runtime::quarantineRuntimeRoute(client, *lease);
            return phase7Error(invoked_name, 500, "extension_protocol_error",
                               {{"reason", "malformed_error_envelope"},
                                {"route_quarantine", lease.has_value()}});
        }
        if (!payload["error"].contains("data") || !payload["error"]["data"].is_object()) {
            payload["error"]["data"] = json::object();
        }
        payload["error"]["data"]["tool"] = invoked_name;
        payload["error"]["data"]["retryable"] = false;
        if (lease.has_value() &&
            payload["error"]["data"].value("route_quarantine", false)) {
            (void)runtime::quarantineRuntimeRoute(client, *lease);
        }
        auto result = CallToolResult::successJson(payload);
        result.isError = true;
        return result;
    }
    return CallToolResult::successJson(payload);
}

} // namespace didi::mcp
