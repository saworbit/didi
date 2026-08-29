#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <memory>
#include <string_view>

namespace didi::mcp {

CallToolResult sendPhase7LiveRequest(const ResolvedToolBinding& binding,
                                     const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client);

} // namespace didi::mcp
