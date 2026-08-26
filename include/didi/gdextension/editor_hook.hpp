#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include <queue>
#include <mutex>
#include <future>
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace didi {
namespace godot {

struct EngineCommand {
    std::string method;
    json params;
    std::shared_ptr<std::promise<json>> response_promise;
};

class EditorHook {
public:
    static EditorHook& instance();

    void enqueueCommand(EngineCommand cmd);
    std::future<json> postCommand(const std::string& method, const json& params = json::object());

    // Pumping queue
    void processQueue();
    void startAutoPump();
    void stopAutoPump();

    // Log interceptor ring buffer
    void addLogMessage(const std::string& level, const std::string& message);
    json getRecentLogs(size_t max_count = 100);

private:
    EditorHook();
    ~EditorHook();

    json executeOnMainThread(const std::string& method, const json& params);

    // Domain handlers
    json handleGetState(const json& params);
    json handleGetHierarchy(const json& params);
    json handleMutateScene(const json& params);
    json handleInstantiateAsset(const json& params);
    json handleScriptDiagnostics(const json& params);
    json handleInjectInput(const json& params);

    // Internal scene tree helpers
    json parseTscnHierarchy(const std::string& scene_file_path, int max_depth, bool include_props);

    std::queue<EngineCommand> m_commandQueue;
    std::mutex m_queueMutex;

    std::atomic<bool> m_autoPumpRunning{false};
    std::thread m_autoPumpThread;

    std::vector<json> m_logBuffer;
    std::mutex m_logMutex;
};

} // namespace godot
} // namespace didi
