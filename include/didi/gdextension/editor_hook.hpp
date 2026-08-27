#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include "didi/gdextension/runtime_log.hpp"
#include <queue>
#include <mutex>
#include <future>
#include <string>
#include <atomic>

namespace didi {
namespace godot {

enum class CommandState {
    Pending,
    Running,
    Completed,
    Cancelled
};

class CommandControl {
public:
    bool tryStart() {
        CommandState expected = CommandState::Pending;
        return m_state.compare_exchange_strong(expected, CommandState::Running);
    }

    bool tryCancelPending() {
        CommandState expected = CommandState::Pending;
        return m_state.compare_exchange_strong(expected, CommandState::Cancelled);
    }

    void markCompleted() { m_state.store(CommandState::Completed); }
    CommandState state() const { return m_state.load(); }

    bool tryClaimResponse() {
        bool expected = false;
        return m_responseClaimed.compare_exchange_strong(expected, true);
    }

private:
    std::atomic<CommandState> m_state{CommandState::Pending};
    std::atomic<bool> m_responseClaimed{false};
};

struct EngineCommand {
    std::string method;
    json params;
    std::shared_ptr<std::promise<json>> response_promise;
    std::shared_ptr<CommandControl> control;
};

struct CommandTicket {
    std::future<json> response;
    std::shared_ptr<std::promise<json>> response_promise;
    std::shared_ptr<CommandControl> control;
};

class EditorHook {
public:
    static EditorHook& instance();

    CommandTicket postCommand(const std::string& method, const json& params = json::object());

    // Pumping queue
    void processQueue();
    void cancelPendingCommands(const std::string& reason);

    RuntimeLogRing& runtimeLogs();

private:
    EditorHook();
    ~EditorHook();

    json executeOnMainThread(const std::string& method, const json& params);

    std::queue<EngineCommand> m_commandQueue;
    std::mutex m_queueMutex;

    std::shared_ptr<RuntimeLogRing> m_runtimeLogs{std::make_shared<RuntimeLogRing>()};
};

} // namespace godot
} // namespace didi
