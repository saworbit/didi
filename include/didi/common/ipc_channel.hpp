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
};

inline Error transportFailure(std::string message, TransportFailureState state) {
    if (!state.request_started) state.outcome_unknown = false;
    return Error(
        state.timed_out ? 504 : 502, std::move(message),
        {{"transport", {{"request_started", state.request_started},
                         {"outcome_unknown", state.outcome_unknown},
                         {"timed_out", state.timed_out}}}});
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
    return TransportFailureState{state["request_started"].get<bool>(),
                                 state["outcome_unknown"].get<bool>(),
                                 state["timed_out"].get<bool>()};
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

} // namespace ipc
} // namespace didi
