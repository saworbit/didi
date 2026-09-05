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
    virtual void setHandler(MessageHandler handler) = 0;
};

std::unique_ptr<IIpcClient> createIpcClient();
std::unique_ptr<IIpcServer> createIpcServer();

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
