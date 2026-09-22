#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <atomic>
#include <optional>
#include "types.hpp"
#include "protocol.hpp"

namespace didi {
namespace ipc {

inline constexpr int kWaitForDefinitiveResponse = -1;

using MessageHandler = std::function<json(const json& request)>;

struct TransportFailureState {
    bool request_started{false};
    bool outcome_unknown{false};
    bool timed_out{false};
    // Why the transport gave up, when it can tell.
    //
    // "the peer hung up" and "we ran out of time" used to share one message and
    // one false timed_out flag, which is how a live-harness failure could say
    // "Failed or timed out reading response length" alongside timed_out: false
    // and leave nobody able to say which had happened. They are different
    // diagnoses: a closed pipe means the other side decided to stop, and a
    // deadline means this side did.
    //
    // Empty when the cause is not established. Otherwise "peer_closed",
    // "deadline", "io_error", or "stopped".
    std::string reason;
    // How long this operation actually waited before failing, or -1 when that
    // was not measured. A read that dies at five seconds under a ten second
    // deadline is being ended by something other than its own deadline, and
    // the number is what shows that.
    int waited_ms{-1};
};

inline Error transportFailure(std::string message, TransportFailureState state) {
    if (!state.request_started) state.outcome_unknown = false;
    json transport = {{"request_started", state.request_started},
                      {"outcome_unknown", state.outcome_unknown},
                      {"timed_out", state.timed_out}};
    // Added rather than substituted: the three flags above are what existing
    // callers read, and these say why.
    if (!state.reason.empty()) transport["reason"] = state.reason;
    if (state.waited_ms >= 0) transport["waited_ms"] = state.waited_ms;
    return Error(state.timed_out ? 504 : 502, std::move(message),
                 {{"transport", std::move(transport)}});
}

// Records that a transport failure was already asked a second time, so a reader
// can tell an engine that answered nothing twice from one that was asked once.
// Does nothing when nothing was repeated or when the error carries no transport
// state, which is every failure the engine itself produced.
inline void markTransportRepeated(Error& error, bool repeated) {
    if (!repeated || !error.data.is_object()) return;
    if (!error.data.contains("transport") || !error.data["transport"].is_object()) return;
    error.data["transport"]["repeated"] = true;
}

inline std::optional<TransportFailureState> transportFailureState(const Error& error) {
    if (!error.data.is_object() || !error.data.contains("transport") ||
        !error.data["transport"].is_object()) {
        return std::nullopt;
    }
    const auto& state = error.data["transport"];
    if (!state.contains("request_started") || !state["request_started"].is_boolean() ||
        !state.contains("outcome_unknown") || !state["outcome_unknown"].is_boolean() ||
        !state.contains("timed_out") || !state["timed_out"].is_boolean()) {
        return std::nullopt;
    }
    TransportFailureState parsed{state["request_started"].get<bool>(),
                                 state["outcome_unknown"].get<bool>(),
                                 state["timed_out"].get<bool>()};
    // Optional, because a failure whose cause was not established says nothing
    // rather than guessing, and because an older peer emits neither.
    if (state.contains("reason") && state["reason"].is_string()) {
        parsed.reason = state["reason"].get<std::string>();
    }
    if (state.contains("waited_ms") && state["waited_ms"].is_number_integer()) {
        parsed.waited_ms = state["waited_ms"].get<int>();
    }
    return parsed;
}

class IIpcClient {
public:
    virtual ~IIpcClient() = default;
    virtual bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    // A negative timeout waits for the extension's definitive response. Live main-thread
    // operations use this so the extension owns pending-vs-running timeout semantics.
    virtual Result<json> sendRequest(const std::string& method, const json& params = json::object(), int timeout_ms = 10000) = 0;
};

using IpcClientFactory = std::function<std::unique_ptr<IIpcClient>()>;

class IIpcServer {
public:
    virtual ~IIpcServer() = default;
    virtual bool start(const std::string& pipe_name = kDefaultPipeName) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    // The handler is called from one thread at a time, whichever connection the
    // request arrived on. A server listens on serverConnectionSlots()
    // connections at once (#873), so the reading is concurrent; the calling is
    // not, and a handler does not have to be written for two callers.
    virtual void setHandler(MessageHandler handler) = 0;
};

std::unique_ptr<IIpcClient> createIpcClient();
std::unique_ptr<IIpcServer> createIpcServer();

// Why this transport cannot use this endpoint, before anything tries.
//
// On POSIX the endpoint is a filesystem path that has to fit in
// sockaddr_un::sun_path, which holds 104 bytes on macOS and 108 on Linux. The
// session endpoint is built under the temporary directory, and a stock
// macos-latest runner's is 48 bytes, so a session there has five bytes of
// headroom. Both ends answered the overflow with a bare `return false`: the
// plugin reported itself active, no descriptor was published, every live tool
// gave the ordinary "no editor is running" refusal, and nothing anywhere named
// a path length -- which is indistinguishable from the editor not running,
// the most common state in the world (#711).
//
// Empty on Windows, whose named pipes are not paths and have their own,
// separate limit.
std::optional<std::string> endpointPathRejection(const std::string& endpoint);

// How many connections one server listens on and serves at the same time.
//
// A connection arriving while a slot is free is read when it arrives. One
// arriving when every slot is held waits for a slot to recycle, which is what
// withAcceptAllowance below is sized for.
int serverConnectionSlots();

// What a deadline has to allow for besides the work, on a request that may be
// the first one on a connection the server has not accepted yet.
//
// A server used to serve one accepted connection at a time and accept the next
// only once the one it held had been idle for its recycle window. A client
// arriving in the meantime still connected, because the kernel takes it into
// the listen backlog and a named pipe hands out an instance when one comes
// free, and then waited with a request nothing had read. So a deadline chosen
// from how long the work should take refused a request that was always going
// to be answered, and the platform with the longer recycle window was the one
// it happened on.
//
// That is #782. The attach handshake allowed a flat 3000 ms, which is over the
// Windows window of 1000 and under the POSIX window of 5000, so the
// runtime_attach_session that runtime_launch --detach documents failed on
// macOS and Linux and passed on Windows.
//
// #873 gave the servers serverConnectionSlots() connections at once, so the
// ordinary case no longer pays this. Two things still can. Every slot can be
// held, by a client that walked away without closing its connections. And the
// server on the other end is the addon binary sitting in someone's Godot
// project, which is updated separately from this one, so it may predate the
// change entirely. Call sites say how long the work gets; this adds what being
// accepted can still cost, from the same place the window itself is set.
// kWaitForDefinitiveResponse is returned unchanged, because a call with no
// deadline has nothing to extend.
int withAcceptAllowance(int work_ms);

namespace testing {
// Drives the idle-recycle contract in milliseconds instead of seconds, so a
// test can sit on the boundary without taking seconds to do it, and can invert
// the margin on purpose to exercise the server's half of it on its own.
void setIdleRecycleOverridesForTesting(int server_recycle_ms, int client_reuse_ms);
void clearIdleRecycleOverridesForTesting();
} // namespace testing

#if defined(_WIN32)
namespace testing {
using PipeSecurityDescriptorFactory = std::function<void*()>;
std::unique_ptr<IIpcServer> createIpcServerWithSecurityDescriptorFactory(
    PipeSecurityDescriptorFactory factory);
} // namespace testing
#endif

} // namespace ipc
} // namespace didi
