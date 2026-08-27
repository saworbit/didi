#pragma once

#include "didi/gdextension/editor_hook.hpp"
#include "didi/runtime/session_client.hpp"

#include <chrono>
#include <string>

namespace didi::godot {

json handleSessionHandshake(const json& params, const runtime::SessionDescriptor& session);

json awaitRuntimeCommand(CommandTicket ticket, const std::string& method,
                         const runtime::SessionDescriptor& session,
                         std::chrono::milliseconds deadline);

} // namespace didi::godot
