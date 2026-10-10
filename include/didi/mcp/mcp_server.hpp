#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <optional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
#include "didi/mcp/jsonrpc.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/resource_registry.hpp"
#include "didi/mcp/prompt_registry.hpp"
#include "didi/mcp/response_economy.hpp"
#include "didi/mcp/repeated_failures.hpp"
#include "didi/mcp/jobs.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/offline/blackboard.hpp"

namespace didi {
namespace mcp {

// ProtocolEra and RequestScope live in mcp_protocol.hpp, reached through
// tool_registry.hpp above: dispatch needs them too, and this header cannot be
// the one that defines them without the registry including the server.

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

    // Which tools tools/list shows and tools/call accepts. Set from --tools at
    // startup and never changed afterwards; see ToolProfile.
    void setToolProfile(ToolProfile profile) { m_toolProfile = profile; }
    ToolProfile toolProfile() const { return m_toolProfile; }

    // Whether every live answer carries the whole session descriptor, or only
    // the first on a route. Set from --session-descriptor at startup and never
    // changed afterwards; see SessionDescriptorMode.
    void setSessionDescriptorMode(SessionDescriptorMode mode) { m_sessionDescriptorMode = mode; }
    SessionDescriptorMode sessionDescriptorMode() const { return m_sessionDescriptorMode; }
    // Whether tool answers repeat structuredContent as text. Set from
    // --text-copy at startup and never changed after (#1238).
    void setTextCopyMode(TextCopyMode mode) { m_textCopyMode = mode; }
    TextCopyMode textCopyMode() const { return m_textCopyMode; }

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

    // The bridge facts every tools/list and resources/list entry carries.
    //
    // `_meta.didi`'s currentMode, liveAvailable, editorConnected, sessionKind
    // and confirmationsSkipped are all state, and tool_availability.cpp derives
    // every one of them from these few inputs. Cheap to read -- the selected
    // descriptor and a connection flag, no route lease and no IPC -- so it can
    // be taken after every request.
    std::string listingFingerprint() const;
    // Sends the two list_changed notifications when that string has moved.
    // Primes silently on its first call: announcing at the moment a client
    // connects would be telling it something changed that did not.
    void announceListingsIfChanged();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    // Shared because the reader can finish after the server is destroyed.
    std::shared_ptr<std::atomic<bool>> m_readerParked{std::make_shared<std::atomic<bool>>(false)};
    bool m_initialized{false};
    bool m_skipConfirmations{false};
    UiAppMode m_uiAppMode{UiAppMode::Auto};
    ToolProfile m_toolProfile{ToolProfile::Full};
    SessionDescriptorMode m_sessionDescriptorMode{SessionDescriptorMode::Every};
    TextCopyMode m_textCopyMode{TextCopyMode::Always};
    // Sticky from a 2024-11-05 handshake, and read only for legacy requests.
    // A modern request declares its own capabilities and is answered from
    // those alone, so one client's extension choice cannot reach another's
    // request on a process serving both eras.
    bool m_clientDeclaredUiExtension{false};
    bool uiSurfaceVisible(ProtocolEra era, const json& params) const;
    // The same rule for didi/responseEconomy: a legacy request has what the
    // handshake declared plus what it declares itself, a modern request only
    // the latter. The ledger is the legacy conversation's, and a modern
    // request never reads or writes it.
    ResponseEconomy m_clientDeclaredEconomy;
    SessionDescriptorLedger m_descriptorLedger;
    // The same call failing the same way again, per conversation (Q6).
    RepeatedFailures m_repeatedFailures;
    ResponseEconomy responseEconomyFor(ProtocolEra era, const json& params) const;
    // Whether the client behind a request already holds a session descriptor.
    DescriptorHeld descriptorHolderFor(const RequestScope& scope);
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_runtimeSessionClient;

    std::mutex m_writeMutex;
    mutable std::mutex m_subscriptionMutex;
    std::set<std::string> m_subscriptions;
    // Each subscribed board's file as it last stood, taken when the board is
    // first subscribed and dropped when its last URI goes. An absent value is
    // a board with no file yet, which is a state and not an unknown.
    std::unordered_map<std::string, std::optional<offline::BlackboardFileStamp>> m_boardBaselines;
    std::thread m_boardWatcher;
    std::optional<std::string> m_listingFingerprint;
    std::atomic<bool> m_watching{false};

    // Long work as jobs (Q8). A job runs a tool call on its own thread and
    // keeps the answer; only this thread, the stdio loop, reads it out to a
    // client. Declared last so it is destroyed first, cancelling and joining
    // any job still running before the rest of the server goes.
    //
    // How long a call that asked for a job by request_id waits for it before
    // answering that it is still working, and how long a task-extension call
    // waits before answering with a task. Both stay under the route deadlines.
    std::chrono::milliseconds m_jobWait{10000};
    std::chrono::milliseconds m_taskGrace{250};
    // The confirmation a job's call went through, for the answer read later.
    // The label, and whether the route stamps it on an error too.
    std::unordered_map<std::string, std::pair<std::string, bool>> m_jobProvenance;
    json answerJob(const JobView& view, bool as_task, const ResponseEconomy& economy,
                   const DescriptorHeld& client_holds) const;
    JobStore m_jobs;

public:
    // Test seam: the job waits, shortened so a test of a working job does not
    // spend ten seconds, and the store, so a test can see what is running.
    void setJobWaitsForTesting(std::chrono::milliseconds wait, std::chrono::milliseconds grace) {
        m_jobWait = wait;
        m_taskGrace = grace;
    }
    const JobStore& jobsForTesting() const { return m_jobs; }

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
