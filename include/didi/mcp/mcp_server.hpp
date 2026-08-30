#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <atomic>
#include <optional>
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

    // Turns off the confirmation requirement for destructive tools. Set only
    // from the launch arguments, by the person starting the process. It is
    // deliberately unreachable from a tool call: an agent that can authorise
    // its own bypass makes the confirmation system decorative.
    void setConfirmationsSkipped(bool skipped) { m_skipConfirmations = skipped; }
    bool confirmationsSkipped() const { return m_skipConfirmations; }

private:
    std::optional<JsonRpcResponse> dispatchPayload(const json& payload);
    void sendResponse(const JsonRpcResponse& resp);
    void sendBatchResponse(const json& responses);
    void sendNotification(const std::string& method, const json& params);
    void releaseRuntimeSession();

    std::atomic<bool> m_running{false};
    bool m_initialized{false};
    bool m_skipConfirmations{false};
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_runtimeSessionClient;
};

} // namespace mcp
} // namespace didi
