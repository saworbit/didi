#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <atomic>
#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/prompt_registry.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"

namespace didi {
namespace mcp {

class McpServer {
public:
    McpServer();
    ~McpServer();

    void initializeRegistries();
    void runStdio();
    void stop();

    JsonRpcResponse handleRequest(const JsonRpcRequest& req);

    void setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client);
    std::shared_ptr<ipc::IIpcClient> getIpcClient() const;

private:
    void sendResponse(const JsonRpcResponse& resp);
    void sendNotification(const std::string& method, const json& params);

    std::atomic<bool> m_running{false};
    bool m_initialized{false};
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_runtimeSessionClient;
};

} // namespace mcp
} // namespace didi
