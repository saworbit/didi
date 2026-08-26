#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include <queue>
#include <mutex>
#include <future>
#include <string>
#include <atomic>

namespace didi {
namespace godot {

struct EngineCommand {
    std::string method;
    json params;
    std::shared_ptr<std::promise<json>> response_promise;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

struct CommandTicket {
    std::future<json> response;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

class EditorHook {
public:
    static EditorHook& instance();

    CommandTicket postCommand(const std::string& method, const json& params = json::object());

    // Pumping queue
    void processQueue();
    void cancelPendingCommands(const std::string& reason);

    // Log interceptor ring buffer
    void addLogMessage(const std::string& level, const std::string& message);
    json getRecentLogs(size_t max_count = 100);

private:
    EditorHook();
    ~EditorHook();

    json executeOnMainThread(const std::string& method, const json& params);

    // Domain handlers
    json handleScriptDiagnostics(const json& params);

    std::queue<EngineCommand> m_commandQueue;
    std::mutex m_queueMutex;

    std::vector<json> m_logBuffer;
    std::mutex m_logMutex;
};

} // namespace godot
} // namespace didi
