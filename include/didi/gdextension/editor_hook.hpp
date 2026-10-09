#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/runtime_log.hpp"
#include "didi/runtime/session_kind_policy.hpp"
#include "didi/runtime/profiler_collector.hpp"
#include "didi/runtime/performance_verdict.hpp"
#include "didi/runtime/invariant_watch.hpp"
#include "didi/runtime/scene_exploration.hpp"
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

    // The deadline alone, for a frame that may not observe idle.
    bool expired(std::chrono::steady_clock::time_point now) const { return now >= m_deadline; }

private:
    std::chrono::steady_clock::time_point m_startedAt;
    std::chrono::steady_clock::time_point m_deadline;
    int m_consecutiveIdle{0};
};

// Reimports that answered their command at once, for asset_reimport run as a
// job (Q8 in docs/BUILD_QUEUE.md, #996).
//
// A reimport that asks for a scan is not over until the editor has applied it,
// and applying a scan of many new scripts draws frames for each one: half a
// minute on a software-rendered editor for 25 scripts, past the fifteen seconds
// any one command is waited for. A detached reimport answers its command with
// an id instead, and its answer is kept here for the server's job to read with
// asset.reimportStatus.
//
// That read is answered on the IPC thread, never the main one. The editor holds
// every queued command while its progress dialog is open, and the dialog is
// open for the whole of the work a detached reimport exists to wait out, so a
// read through the queue would have waited for the very thing it asks about.
//
// editor_reload_project waits for the same thing, a scan applied, and keeps
// its detached answers in a second store read with editor.reloadStatus
// (#1157). `id_key` is what each store calls its ids in a read.
class DetachedReimports {
public:
    using Clock = std::chrono::steady_clock;

    explicit DetachedReimports(std::string id_key = "reimport_id") : m_idKey(std::move(id_key)) {}

    // How long a working reimport is waited for with nobody reading it. A
    // server that exits stops reading, and the wait it left would hold the one
    // reimport slot until the editor finished or its deadline passed, which can
    // be fifteen minutes. The work itself is the editor's and carries on.
    static constexpr std::chrono::seconds kReaderLease{60};
    // Answers kept. Only one reimport runs at a time, so these are the last few
    // a reader may not have read yet.
    static constexpr size_t kKept = 8;

    // A new working entry under `id`, read as just read.
    void begin(const std::string& id, Clock::time_point now);
    // Its answer, once the reimport has one.
    void finish(const std::string& id, json answer);
    // What asset.reimportStatus answers for `id`, renewing the lease, or
    // nothing when no such reimport is kept.
    std::optional<json> read(const std::string& id, Clock::time_point now);
    // Whether `id` is still working and nobody has read it for the lease.
    bool unread(const std::string& id, Clock::time_point now) const;

private:
    struct Entry {
        std::string id;
        Clock::time_point started;
        Clock::time_point last_read;
        std::optional<json> answer;
    };
    // Its own lock, never the reimport mutex: the main thread holds that one
    // across reimport_files, which runs frames.
    std::string m_idKey;
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
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

    // Where the engine's output stood when this command started, so the answer
    // can carry what the engine printed while it ran. Zero until it starts.
    void noteStart(std::string method, uint64_t engine_cursor) {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_method = std::move(method);
        m_engineCursor = engine_cursor;
    }
    std::pair<std::string, uint64_t> started() const {
        std::lock_guard<std::mutex> lock(m_startMutex);
        return {m_method, m_engineCursor};
    }

private:
    mutable std::mutex m_startMutex;
    std::string m_method;
    uint64_t m_engineCursor{0};
    std::atomic<CommandState> m_state{CommandState::Pending};
    std::atomic<bool> m_everStarted{false};
    std::atomic<bool> m_responseClaimed{false};
};

struct EngineCommand {
    std::string method;
    json params;
    std::shared_ptr<std::promise<json>> response_promise;
    std::shared_ptr<CommandControl> control;
    // When it was queued. The server stops waiting kMaxPublicLiveRequestMs
    // after it sent the request, so work that waits counts from here.
    std::chrono::steady_clock::time_point queued_at{std::chrono::steady_clock::now()};
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
    // Which kind of process this extension is loaded into, where it knows.
    // Used to be reachable only through the test seam, so a refusal built
    // inside the engine could not tell an editor from a game and called every
    // headless process an editor (#777).
    std::optional<runtime::SessionKind> sessionKind() const;

    void scheduleRuntimeStep(int frames,
                             const std::shared_ptr<std::promise<json>>& promise,
                             const std::shared_ptr<CommandControl>& control);
    void scheduleAssetReimport(const json& params,
                               const std::shared_ptr<std::promise<json>>& promise,
                               const std::shared_ptr<CommandControl>& control);
    // asset.reimportStatus, answered on the calling thread: see
    // DetachedReimports for why it never waits for the main one.
    json readDetachedReimport(const json& params);
    // editor.reloadStatus, the same way (#1157).
    json readDetachedReload(const json& params);
    // Starts a callback-driven Performance sample window. The command returns
    // on the callback that collects the last sample; nothing blocks the main
    // thread, and only one collector runs per session.
    void scheduleProfilerRead(const json& params,
                              const std::shared_ptr<std::promise<json>>& promise,
                              const std::shared_ptr<CommandControl>& control);
    // Starts a callback-driven invariant watch. Same shape as the profiler
    // above and for the same reason: the window is measured in engine frames,
    // and nothing blocks the main thread while it runs.
    void scheduleInvariantWatch(const json& params,
                                const std::shared_ptr<std::promise<json>>& promise,
                                const std::shared_ptr<CommandControl>& control);

    // Starts a callback-driven exploration run. Same shape again, and for one
    // more reason on top of the others: this one also presses buttons, and an
    // action has to go down on one frame and come up on another for the game's
    // own input handling to see a press at all.
    void scheduleSceneExploration(const json& params,
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
    // Whether this frame is inside an import pass, the editor's or Didi's own.
    // See ImportPassObservation.
    bool editorImportPassOpen();
    // Whether the editor's modal progress dialog has a task open. The dialog
    // pumps the main loop on every step, so this frame is then inside that
    // work, whoever started it (#995).
    bool editorProgressTaskOpen();
    // Whether the editor has yet to open its startup scenes. It opens them
    // once its first scan is applied and makes one current, so a scene opened
    // before then is not the edited scene for long (#1069).
    bool editorStarting();
    // Whether the editor filesystem is scanning, or has yet to apply a scan
    // this hook saw running. A file written then is not indexed: update_file
    // does nothing during a scan, and applying one replaces the index the file
    // was added to (#1004).
    bool editorFilesystemSettling();
    // Answers each parked scene_create whose uid is now indexed, or whose
    // deadline has passed. Indexing is tried only once the scan is applied.
    void processParkedSceneCreates(bool filesystem_settled);
    // Asks for the scan each parked resource.refreshCached waits for once the
    // editor is free to start one, and answers it once that scan is applied
    // or its deadline has passed.
    void processParkedRefreshes(bool filesystem_settled);
    // Asks for each pending editor_reload_project's scan once the editor is
    // free to start one, and answers it once the scan is applied or its
    // deadline has passed. Only from a frame that is not nested, because the
    // work that applies a scan runs frames of its own.
    void processProjectScanFrame();
    // Runs a scene_call_method, and parks it when the method is a coroutine.
    // Returns false when the request is not one of these, so the caller runs
    // the ordinary synchronous path.
    bool scheduleScriptCall(const json& params,
                            const std::shared_ptr<std::promise<json>>& promise,
                            const std::shared_ptr<CommandControl>& control);
    void processScriptCallFrame();
    // Selects the main screen a capture needs, then answers it a frame later.
    // Returns false when the request does not need this, so the caller runs the
    // ordinary synchronous path.
    bool scheduleMainScreenCapture(const std::string& method, const json& params,
                                   const std::shared_ptr<std::promise<json>>& promise,
                                   const std::shared_ptr<CommandControl>& control);
    void processMainScreenCaptureFrame();
    void processProfilerFrame();
    // Times the frame that just ended for a profiler read's verdict. Called
    // first in every frame callback, before any command runs in it, so Didi's
    // own work is not counted in the frame it ended.
    void processFrameTimerFrame();
    void processInvariantWatchFrame();
    void processSceneExplorationFrame();
    // Releases whatever the exploration is holding. Called when the run ends
    // for any reason, including the ones that are not success, because an
    // action left down outlives the run and keeps driving the game.
    void releaseExplorationAction(size_t action_index);
    void processPendingQuitFrame();

    struct PendingRuntimeStep {
        int requested_frames{0};
        int remaining_frames{0};
        bool awaiting_next_callback{true};
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
        // Input held during the pause and handed to Input when this step
        // resumed the tree, so the caller knows the stepped frame carried it.
        int64_t released_input_events{0};
        // The engine's physics frame count as the step resumed the tree.
        std::optional<int64_t> resumed_physics_frames;
    };

    // A capture that had to change the editor's main screen first.
    //
    // Selecting a main screen resizes the viewport through the control layout,
    // which happens on the next process frame; RenderingServer.force_draw does
    // not do it, measured on 4.7.2. So the capture cannot be answered in the
    // frame that asked for it (#381).
    struct PendingMainScreenCapture {
        // Which vision method to answer with once the screen is showing. All
        // three take a frame off an editor viewport, and a viewport that is not
        // on screen has no size, so all three need this (#568).
        std::string method;
        json params;
        std::string selected_screen;
        // Empty when the screen that was showing is one this cannot name, which
        // is any main screen an addon contributes.
        std::string previous_screen;
        int remaining_frames{1};
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    // A scene_call_method whose method turned out to be a coroutine.
    //
    // The value arrives on the completed signal rather than from the call, so
    // the request is answered from the frame loop when it lands, or when the
    // caller's timeout runs out (#389).
    struct PendingScriptCallRequest {
        uint64_t await_id{0};
        std::string target_node;
        std::string method_name;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    struct PendingAssetReimport {
        std::vector<std::string> paths;
        std::vector<std::string> reimported;
        std::vector<std::string> refreshed;
        ReimportProgress progress;
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
        // Set when a path had no .import sidecar, which is when a full scan was
        // asked for. The editor's scanning flag clears before the importer has
        // finished writing sidecars, so the first answer after idle reported a
        // brand-new asset as not imported while it was imported a moment later
        // (#731). The wait then asks the editor whether its work on each path
        // is finished rather than watching the flag.
        bool needs_scan{false};
        // Set when reimport_files could not be called yet, because the editor
        // was scanning, had not applied a scan's results, was inside a
        // progress task, or did not list one of the assets. A reimport while a
        // scan runs cannot find the file, skips it and was reported as done.
        // The frame loop starts it once all of that is over.
        bool reimport_deferred{false};
        // Frames in a row the editor has not been scanning while the reimport
        // was held. Two, the same rule that says a reimport has finished.
        int deferred_idle_frames{0};
        // Scan work to see finished first: a scan Didi started, or the
        // editor's own one that held the reimport. The scanning flag clears
        // before the editor applies what the scan found, in frames of its
        // own, so neither the reimport nor the answer goes by the flag alone.
        std::optional<ScanSettle> settle;
        // Set when the command was answered at once with this id, and the
        // answer goes to m_detachedReimports instead (Q8).
        std::string detached_id;
    };

    // Answers a finished reimport, to its command or to the detached store.
    void answerAssetReimport(PendingAssetReimport& completed, json response);

    // An editor_reload_project waiting for the editor to apply a scan it
    // started after the call arrived (#1114).
    struct PendingProjectScan {
        ProjectScanWait wait;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
        // Set when the command was answered at once with this id, and the
        // answer goes to m_detachedReloads instead (#1157).
        std::string detached_id;
    };

    // A scene_create whose uid the engine could not index yet. Its answer
    // waits for the index, up to a deadline, so a caller can create a scene
    // and then launch something that references it (#1004).
    struct ParkedSceneCreate {
        json response;
        std::string scene_path;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    // A resource.refreshCached for a file written into a folder the editor
    // does not list (#1177). Its answer waits for a full scan, which is what
    // lists a new folder, so a class_name the file declares is known to the
    // caller's next check.
    struct ParkedRefresh {
        json response;
        ProjectScanWait scan;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    struct PendingProfilerRead {
        runtime::ProfilerCollector collector;
        std::chrono::steady_clock::time_point started_at;
        bool awaiting_next_callback{true};
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
        // The frames the read covers, for its verdict. timing is false when
        // the frame timer could not start, and timing_unavailable says why.
        bool timing{false};
        std::string timing_unavailable;
        runtime::FrameBudget budget;
        runtime::FrameTimingLog frames;
        int64_t previous_end_usec{-1};
        // The self-check runs after the samples. Their answer and verdict wait
        // here while the stalled frames are timed.
        std::optional<json> samples;
        json baseline;
        double stall_ms{0.0};
        std::chrono::steady_clock::time_point self_check_started;
    };
    // Stops the frame timer and answers the read, with its verdict and, when
    // asked for, its self-check.
    void finishProfilerRead(PendingProfilerRead completed, json failure);

    struct PendingInvariantWatch {
        runtime::InvariantWatch watch;
        std::chrono::steady_clock::time_point started_at;
        bool awaiting_next_callback{true};
        // The engine output sequence the watch began at, so an error counted is
        // an error this run produced and not one already on the ring.
        uint64_t error_cursor{0};
        std::shared_ptr<std::promise<json>> response_promise;
        std::shared_ptr<CommandControl> control;
    };

    struct PendingSceneExploration {
        runtime::SceneExploration exploration;
        std::chrono::steady_clock::time_point started_at;
        bool awaiting_next_callback{true};
        uint64_t error_cursor{0};
        // Which action is currently down, and the slot it went down for. No
        // action is held before the first frame, which is what the empty
        // optional means rather than "action zero".
        std::optional<size_t> held_action;
        int64_t held_slot{-1};
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
    std::optional<PendingScriptCallRequest> m_pendingScriptCall;
    std::optional<PendingMainScreenCapture> m_pendingMainScreenCapture;
    std::optional<PendingAssetReimport> m_pendingAssetReimport;
    DetachedReimports m_detachedReimports;
    DetachedReimports m_detachedReloads{"reload_id"};
    std::mutex m_profilerMutex;
    std::optional<PendingProfilerRead> m_pendingProfilerRead;
    std::mutex m_invariantMutex;
    std::optional<PendingInvariantWatch> m_pendingInvariantWatch;
    std::mutex m_explorationMutex;
    std::optional<PendingSceneExploration> m_pendingSceneExploration;
    std::optional<runtime::SessionKind> m_sessionKind;
    // Main-thread only. Set while processQueue is dequeuing, so a nested pump
    // triggered from inside a command observes progress without starting work.
    bool m_pumping{false};
    // Test seam for editorImportPassOpen, which otherwise asks the engine.
    std::optional<bool> m_importPassOverride;
    // Test seam for editorProgressTaskOpen, which otherwise asks the engine.
    std::optional<bool> m_progressTaskOverride;
    // Test seam for editorStarting, which otherwise asks the engine.
    std::optional<bool> m_editorStartingOverride;
    // The scan editorFilesystemSettling saw running, until it is applied.
    std::optional<ScanSettle> m_filesystemSettle;
    std::mutex m_parkedSceneCreateMutex;
    std::vector<ParkedSceneCreate> m_parkedSceneCreates;
    std::mutex m_parkedRefreshMutex;
    std::vector<ParkedRefresh> m_parkedRefreshes;
    std::mutex m_projectScanMutex;
    std::vector<PendingProjectScan> m_pendingProjectScans;
    // Test seam for editorFilesystemSettling, which otherwise asks the engine.
    std::optional<bool> m_filesystemSettlingOverride;
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
    // Runs one reimport frame, so a test can drive the lease without an engine.
    static void processAssetReimportFrame(EditorHook& hook);
    static DetachedReimports& detachedReimports(EditorHook& hook);
    static DetachedReimports& detachedReloads(EditorHook& hook);
    // A detached reimport as the frame loop would find it, begun at `started`.
    static void plantDetachedReimport(EditorHook& hook, const std::string& id,
                                      std::chrono::steady_clock::time_point started);
    static bool hasPendingProfilerRead(EditorHook& hook);
    static bool pumping(const EditorHook& hook);
    static void setPumping(EditorHook& hook, bool pumping);
    static void setImportPassOpen(EditorHook& hook, std::optional<bool> open);
    static void setProgressTaskOpen(EditorHook& hook, std::optional<bool> open);
    static void setEditorStarting(EditorHook& hook, std::optional<bool> starting);
    static void setFilesystemSettling(EditorHook& hook, std::optional<bool> settling);
    static CommandTicket parkSceneCreate(EditorHook& hook, const json& response,
                                         std::chrono::steady_clock::time_point deadline);
    static CommandTicket parkRefresh(EditorHook& hook, const json& response,
                                     std::chrono::steady_clock::time_point deadline);
    static bool hasPendingQuit(const EditorHook& hook);
};

} // namespace godot
} // namespace didi
