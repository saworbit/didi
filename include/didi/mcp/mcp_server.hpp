#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <atomic>
#include <optional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
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

    // A background thread notices another process changing a board, so every
    // write to stdout has to be serialised: two interleaved writes are one
    // corrupt line, and a corrupt line ends the session.
    void writeLine(const std::string& payload);
    void startBoardWatcher();
    void stopBoardWatcher();
    void watchBoards();

    std::atomic<bool> m_running{false};
    bool m_initialized{false};
    bool m_skipConfirmations{false};
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_runtimeSessionClient;

    std::mutex m_writeMutex;
    mutable std::mutex m_subscriptionMutex;
    std::set<std::string> m_subscriptions;
    std::thread m_boardWatcher;
    std::atomic<bool> m_watching{false};

public:
    // Test seam. Subscription bookkeeping and the notification payload are the
    // parts worth asserting without standing up a process and a real clock.
    // The single writer, exposed because the interleaving test has to drive it
    // from a thread alongside the watcher. Nothing else should call it.
    void writeLineForTest(const std::string& payload) { writeLine(payload); }
    bool subscribeResource(const std::string& uri);
    bool unsubscribeResource(const std::string& uri);
    std::vector<std::string> subscribedResources() const;
};

} // namespace mcp
} // namespace didi
