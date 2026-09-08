#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/common/types.hpp"
#include "didi/mcp/mcp_protocol.hpp"

#include <memory>
#include <optional>
#include <string>

namespace didi {
namespace mcp {

// What a registration can do *right now*, as opposed to what it supports in
// principle. `tools/list`, `resources/list` and the Control Room all answer
// this question, and a dashboard that disagreed with discovery would be worse
// than no dashboard, so they answer it from here.

// Whether a live route of this kind is allowed to serve this exact tool or
// resource. Logs, tree inspection, evaluation and viewport capture take either
// kind; pause, step and stop take only a game; the rest are editor-only.
bool liveAllowedFor(const std::string& identifier, bool resource,
                    const std::string& session_kind);

// Whether an unconnected client is authoritatively unavailable rather than
// merely detached. A real session manager with nothing selected is the normal
// offline state; once a route is selected, failing to produce its authenticated
// lease is a fact about that route.
bool managedRouteUnavailable(const std::shared_ptr<ipc::IIpcClient>& client,
                             bool connected);

// `live`, `offline_fallback`, `unavailable` or `unimplemented`.
std::string currentModeFor(const ExecutionCapability& capability,
                           const std::string& identifier, bool resource, bool connected,
                           const std::optional<std::string>& session_kind,
                           bool managed_unavailable);

// Writes currentMode, liveAvailable, editorConnected and any sessionKind into
// `definition["_meta"]["didi"]`.
void addCurrentAvailability(json& definition, const ExecutionCapability& capability,
                            bool connected, const std::optional<std::string>& session_kind,
                            bool resource = false, bool managed_unavailable = false);

}  // namespace mcp
}  // namespace didi
