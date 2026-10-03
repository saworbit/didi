#pragma once

#include "didi/gdextension/editor_hook.hpp"
#include "didi/runtime/session_client.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace didi::godot {

json handleSessionHandshake(const json& params, const runtime::SessionDescriptor& session);

std::optional<json> rejectDisallowedSessionMethod(
    const std::string& method, const runtime::SessionDescriptor& session);

// The answer to a method this thread answers itself instead of queueing it for
// Godot's main thread, or nothing for any other method. asset.reimportStatus is
// the one: the editor holds every queued command while it applies a scan, and a
// read of a detached reimport has to answer during exactly that (Q8).
std::optional<json> answerOffMainThread(const std::string& method, const json& params,
                                        const runtime::SessionDescriptor& session);

json awaitRuntimeCommand(CommandTicket ticket, const std::string& method,
                         const runtime::SessionDescriptor& session,
                         std::chrono::milliseconds deadline);

} // namespace didi::godot
