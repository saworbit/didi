#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"

namespace didi {
namespace mcp {

class ToolRegistry {
public:
    static ToolRegistry& instance();

    void registerTool(ToolDefinition tool);
    const ToolDefinition* getTool(const std::string& name) const;
    std::vector<ToolDefinition> listTools() const;
    CallToolResult callTool(const std::string& name, const json& arguments);

    void setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client);
    std::shared_ptr<ipc::IIpcClient> getIpcClient() const;
    void setRuntimeSessionClient(std::shared_ptr<runtime::IRuntimeSessionClient> session_client);
    std::shared_ptr<runtime::IRuntimeSessionClient> getRuntimeSessionClient() const;

    void registerAllDefaultTools();

private:
    ToolRegistry() = default;
    std::unordered_map<std::string, ToolDefinition> m_tools;
    std::shared_ptr<ipc::IIpcClient> m_sourceIpcClient;
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_runtimeSessionClient;
};

} // namespace mcp
} // namespace didi
