#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
#include <atomic>
#include "types.hpp"
#include "protocol.hpp"

namespace didi {
namespace ipc {

using MessageHandler = std::function<json(const json& request)>;

class IIpcClient {
public:
    virtual ~IIpcClient() = default;
    virtual bool connect(const std::string& pipe_name = kDefaultPipeName, int timeout_ms = 2000) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual Result<json> sendRequest(const std::string& method, const json& params = json::object(), int timeout_ms = 10000) = 0;
};

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
