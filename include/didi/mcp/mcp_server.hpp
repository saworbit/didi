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

// Which protocol revision a single request belongs to. Didi is dual-era: a
// legacy client negotiates once in `initialize` and the process remembers what
// it declared, while a modern client carries version and capabilities in
// `_meta` on every request and is entitled to have nothing inferred from
// earlier traffic on the same process. Requests of both kinds can be
// interleaved on one stdio process, so this is a property of the request and
// never of the server.
enum class ProtocolEra { Legacy, Modern };

class McpServer {
public:
    McpServer();
    ~McpServer();

    void initializeRegistries();
    void runStdio();
    void stop();

    // Signal safe. A handler may call this and nothing else on the server: it
    // stores two flags and returns. Every part of shutdown that touches a
    // mutex, the heap, a thread join or the IPC route happens on the normal
    // path when runStdio leaves its loop. Sticky, so a signal that arrives
    // before the loop starts still ends the session.
    void requestStop();

    // True while stdin still has a read outstanding after the loop ends: a
    // signal, or a frame the server refused to keep reading after. The reader
    // is detached and still inside that read, so whoever owns the process
    // should leave without running static destruction, which would take
    // std::cin away underneath it.
    bool stdinReaderStillParked() const { return m_readerParked->load(); }

    JsonRpcResponse handleRequest(const JsonRpcRequest& req);

    void setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client);
    std::shared_ptr<ipc::IIpcClient> getIpcClient() const;

    // Turns off the confirmation requirement for destructive tools. Set only
    // from the launch arguments, by the person starting the process. It is
    // deliberately unreachable from a tool call: an agent that can authorise
    // its own bypass makes the confirmation system decorative.
    void setConfirmationsSkipped(bool skipped);
    bool confirmationsSkipped() const { return m_skipConfirmations; }

    // Whether the MCP Apps surface -- the ui:// resource and the _meta.ui link
    // on didi_control_room -- is advertised.
    //
    // `auto` follows the extension's bilateral rule: advertise to a client that
    // declared io.modelcontextprotocol/ui, and to no one else, so an unaware
    // host cannot read a page of markup into a model's context. `always` is for
    // a host whose declaration this server does not recognise, and `off`
    // withdraws the surface, leaving didi_control_room a plain read-only tool.
    enum class UiAppMode { Auto, Always, Off };
    void setUiAppMode(UiAppMode mode) { m_uiAppMode = mode; }
    UiAppMode uiAppMode() const { return m_uiAppMode; }
    static std::optional<UiAppMode> parseUiAppMode(const std::string& value);

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
    std::atomic<bool> m_stopRequested{false};
    // Shared because the reader can finish after the server is destroyed.
    std::shared_ptr<std::atomic<bool>> m_readerParked{std::make_shared<std::atomic<bool>>(false)};
    bool m_initialized{false};
    bool m_skipConfirmations{false};
    UiAppMode m_uiAppMode{UiAppMode::Auto};
    // Sticky from a 2024-11-05 handshake, and read only for legacy requests.
    // A modern request declares its own capabilities and is answered from
    // those alone, so one client's extension choice cannot reach another's
    // request on a process serving both eras.
    bool m_clientDeclaredUiExtension{false};
    bool uiSurfaceVisible(ProtocolEra era, const json& params) const;
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
