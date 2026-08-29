#pragma once

#include "didi/common/ipc_channel.hpp"
#include "didi/mcp/mcp_protocol.hpp"

#include <memory>
#include <string_view>

namespace didi::mcp {

CallToolResult sendPhase7LiveRequest(std::string_view invoked_name,
                                     std::string_view canonical_name,
                                     std::string_view method,
                                     const json& arguments,
                                     const std::shared_ptr<ipc::IIpcClient>& client);

} // namespace didi::mcp
