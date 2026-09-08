#include "didi/mcp/tool_availability.hpp"

#include "didi/runtime/session_client.hpp"
#include "didi/runtime/session_kind_policy.hpp"

#include <algorithm>

namespace didi {
namespace mcp {

bool liveAllowedFor(const std::string& identifier, bool resource,
                    const std::string& session_kind) {
    if (resource) {
        if (identifier == "godot://runtime/logs") {
            return session_kind == "editor" || session_kind == "game";
        }
        return session_kind == "editor";
    }
    return runtime::allowsSessionKind(runtime::livePolicyForTool(identifier), session_kind);
}

bool managedRouteUnavailable(const std::shared_ptr<ipc::IIpcClient>& client, bool connected) {
    if (connected ||
        !std::dynamic_pointer_cast<runtime::IRuntimeRouteLeaseProvider>(client)) {
        return false;
    }
    const auto sessions = std::dynamic_pointer_cast<runtime::IRuntimeSessionClient>(client);
    if (!sessions) return true;
    // A real session manager with nothing selected is the normal offline state. Once a route is
    // selected, failure to produce its authenticated lease is authoritative unavailability.
    return sessions->activeSession().has_value();
}

std::string currentModeFor(const ExecutionCapability& capability, const std::string& identifier,
                           bool resource, bool connected,
                           const std::optional<std::string>& session_kind,
                           bool managed_unavailable) {
    const auto has_mode = [&](const std::string& mode) {
        return std::find(capability.modes.begin(), capability.modes.end(), mode) !=
               capability.modes.end();
    };
    const auto effective_kind = session_kind.value_or(connected ? "editor" : "");
    const bool live_available =
        connected && has_mode("live") && liveAllowedFor(identifier, resource, effective_kind);

    if (!capability.implemented) return "unimplemented";
    if (live_available) return "live";
    // A connected route of the wrong kind is an authoritative live selection, not an invitation
    // to silently run an offline fallback. This applies equally to tools and resources.
    if ((connected || managed_unavailable) && has_mode("live")) return "unavailable";
    if (has_mode("offline_fallback")) return "offline_fallback";
    return "unavailable";
}

void addCurrentAvailability(json& definition, const ExecutionCapability& capability,
                            bool connected, const std::optional<std::string>& session_kind,
                            bool resource, bool managed_unavailable) {
    const auto has_mode = [&](const std::string& mode) {
        return std::find(capability.modes.begin(), capability.modes.end(), mode) !=
               capability.modes.end();
    };
    const auto identifier = definition.value(resource ? "uri" : "name", "");
    const auto effective_kind = session_kind.value_or(connected ? "editor" : "");
    const bool live_available =
        connected && has_mode("live") && liveAllowedFor(identifier, resource, effective_kind);

    definition["_meta"]["didi"]["currentMode"] = currentModeFor(
        capability, identifier, resource, connected, session_kind, managed_unavailable);
    definition["_meta"]["didi"]["liveAvailable"] = live_available;
    definition["_meta"]["didi"]["editorConnected"] = connected && effective_kind == "editor";
    if (!effective_kind.empty()) definition["_meta"]["didi"]["sessionKind"] = effective_kind;
}

}  // namespace mcp
}  // namespace didi
