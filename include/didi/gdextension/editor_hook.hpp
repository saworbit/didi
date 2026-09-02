#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include "didi/gdextension/runtime_log.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/runtime/profiler_collector.hpp"
#include <queue>
#include <mutex>
#include <future>
#include <string>
#include <atomic>
#include <chrono>

namespace didi {
namespace godot {

class EditorHook;
class EditorHookTestAccess;

std::optional<json> validateSessionKindForMethod(
    std::string_view method, std::optional<runtime::SessionKind> session_kind);

enum class CommandState {
    Pending,
    Running,
    Completed,
    Cancelled
};

enum class ReimportProgressState { Pending, Idle, TimedOut };

class ReimportProgress {
public:
    ReimportProgress(std::chrono::steady_clock::time_point started_at,
                     std::chrono::milliseconds timeout)
        : m_startedAt(started_at), m_deadline(started_at + timeout) {}

    ReimportProgressState observe(bool scanning, std::chrono::steady_clock::time_point now) {
        if (now >= m_deadline) return ReimportProgressState::TimedOut;
        if (scanning) {
            m_consecutiveIdle = 0;
            return ReimportProgressState::Pending;
        }
        ++m_consecutiveIdle;
        return m_consecutiveIdle >= 2 ? ReimportProgressState::Idle
                                      : ReimportProgressState::Pending;
    }

    int64_t elapsedMs(std::chrono::steady_clock::time_point now) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startedAt).count();
    }

private:
    std::chrono::steady_clock::time_point m_startedAt;
    std::chrono::steady_clock::time_point m_deadline;
    int m_consecutiveIdle{0};
};

class CommandControl {
public:
    bool tryStart() {
        CommandState expected = CommandState::Pending;
        if (!m_state.compare_exchange_strong(expected, CommandState::Running)) return false;
        m_everStarted.store(true);
        return true;
    }

    bool tryCancelPending() {
        CommandState expected = CommandState::Pending;
        return m_state.compare_exchange_strong(expected, CommandState::Cancelled);
    }

    bool tryCancelRunning() {
        CommandState expected = CommandState::Running;
        return m_state.compare_exchange_strong(expected, CommandState::Cancelled);
    }

    void markCompleted() { m_state.store(CommandState::Completed); }
    CommandState state() const { return m_state.load(); }
    bool hasEverStarted() const { return m_everStarted.load(); }

    bool tryClaimResponse() {
        bool expected = false;
        return m_responseClaimed.compare_exchange_strong(expected, true);
    }

private:
    std::atomic<CommandState> m_state{CommandState::Pending};
    std::atomic<bool> m_everStarted{false};
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

class RuntimeStepGate {
public:
    bool tryAcquire() {
        bool expected = false;
        return m_active.compare_exchange_strong(expected, true);
    }

    void release() { m_active.store(false); }
    bool active() const { return m_active.load(); }

private:
    std::atomic<bool> m_active{false};
};

class EditorHook {
public:
    static EditorHook& instance();

    CommandTicket postCommand(const std::string& method, const json& params = json::object());
    void setSessionKind(const std::string& session_kind);

    void scheduleRuntimeStep(int frames,
                             const std::shared_ptr<std::promise<json>>& promise,
                             const std::shared_ptr<CommandControl>& control);
    void scheduleAssetReimport(const json& params,
                               const std::shared_ptr<std::promise<json>>& promise,
                               const std::shared_ptr<CommandControl>& control);
    // Starts a callback-driven Performance sample window. The command returns
    // on the callback that collects the last sample; nothing blocks the main
    // thread, and only one collector runs per session.
    void scheduleProfilerRead(const json& params,
                              const std::shared_ptr<std::promise<json>>& promise,
                              const std::shared_ptr<CommandControl>& control);

    // Asks for SceneTree.quit on a later frame instead of right now. runtime.stop
    // has to return its response over IPC before the main loop is allowed to
    // exit, or the client sees a broken pipe rather than the exit code.
    void requestSceneTreeQuit(int64_t exit_code);

    // Pumping queue
    void processQueue();
    void cancelPendingCommands(const std::string& reason);

    RuntimeLogRing& runtimeLogs();

    // Output the *engine* produced -- `print()` from a running game, script
    // errors with their file and line. Kept deliberately separate from
    // runtimeLogs(), which holds Didi's own diagnostics: they answer different
    // questions, and merging them would make each one harder to read.
    RuntimeLogRing& engineOutput();

private:
    friend class EditorHookTestAccess;
    EditorHook();
    ~EditorHook();

    json executeOnMainThread(const std::string& method, const json& params);
    void processRuntimeStepFrame();
    void processAssetReimportFrame();
    void processProfilerFrame();
    void processPendingQuitFrame();

    struct PendingRuntimeStep {
        int requested_frames{0};
        int remaining_frames{0};
        bool awaiting_next_callback{true};
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    struct PendingAssetReimport {
        std::vector<std::string> paths;
        ReimportProgress progress;
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    struct PendingProfilerRead {
        runtime::ProfilerCollector collector;
        std::chrono::steady_clock::time_point started_at;
        bool awaiting_next_callback{true};
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    std::queue<EngineCommand> m_commandQueue;
    std::mutex m_queueMutex;
    std::mutex m_stepMutex;
    // EditorFileSystem.reimport_files can synchronously re-enter the main-loop callback.
    // Recursive ownership keeps that same-thread observation from deadlocking while the
    // pending request is established; cross-thread shutdown still serializes normally.
    std::recursive_mutex m_reimportMutex;
    RuntimeStepGate m_runtimeStepGate;
    std::optional<PendingRuntimeStep> m_pendingRuntimeStep;
    std::optional<PendingAssetReimport> m_pendingAssetReimport;
    std::mutex m_profilerMutex;
    std::optional<PendingProfilerRead> m_pendingProfilerRead;
    std::optional<runtime::SessionKind> m_sessionKind;
    // Main-thread only. Set while processQueue is dequeuing, so a nested pump
    // triggered from inside a command observes progress without starting work.
    bool m_pumping{false};
    std::optional<int64_t> m_pendingQuitExitCode;
    int m_pendingQuitFrames{0};

    std::shared_ptr<RuntimeLogRing> m_runtimeLogs{std::make_shared<RuntimeLogRing>()};
    std::shared_ptr<RuntimeLogRing> m_engineOutput{std::make_shared<RuntimeLogRing>()};
};

class EditorHookTestAccess {
public:
    static json executeOnMainThread(EditorHook& hook, const std::string& method,
                                    const json& params);
    static void setSessionKind(EditorHook& hook,
                               std::optional<runtime::SessionKind> session_kind);
    static std::optional<runtime::SessionKind> sessionKind(const EditorHook& hook);
    static size_t queueDepth(EditorHook& hook);
    static CommandTicket enqueue(EditorHook& hook, const std::string& method,
                                 const json& params = json::object());
    static bool runtimeStepActive(EditorHook& hook);
    static bool hasPendingRuntimeStep(EditorHook& hook);
    static bool hasPendingAssetReimport(EditorHook& hook);
    static bool hasPendingProfilerRead(EditorHook& hook);
    static bool pumping(const EditorHook& hook);
    static void setPumping(EditorHook& hook, bool pumping);
    static bool hasPendingQuit(const EditorHook& hook);
};

} // namespace godot
} // namespace didi
