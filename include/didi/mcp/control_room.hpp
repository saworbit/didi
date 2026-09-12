#pragma once

#include "didi/common/logger.hpp"
#include "didi/common/types.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"

#include <cstddef>
#include <memory>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace mcp {

// The dashboard behind ui://didi/control-room. See docs/CONTROL_ROOM_DESIGN.md.
//
// Everything here is assembled from state that already exists. The one new
// measurement is the log ring, and it exists because Didi's own diagnostics go
// to standard error, which a client that launched this server over stdio
// usually discards -- no tool can read them today.

// ---------------------------------------------------------------------------
// Bounds. A project is allowed to be enormous; a dashboard is not.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kControlRoomMaxToolRows = 400;
inline constexpr std::size_t kControlRoomMaxSessions = 50;
inline constexpr std::size_t kControlRoomMaxLogRecords = 500;
// What a caller gets without asking for more. The tool's result is visible
// to the model as well as to the page, so the default is a glance rather
// than the whole ring: five hundred records of four hundred characters is
// two hundred kilobytes of context spent on a log nobody asked to read.
inline constexpr std::size_t kControlRoomDefaultLogRecords = 120;
inline constexpr std::size_t kControlRoomMaxMessageChars = 400;
inline constexpr std::size_t kControlRoomMaxFactChars = 400;

struct ControlRoomLogRecord {
    std::string time;
    std::string level;
    std::string tag;
    std::string message;
};

// A bounded, mutex-guarded tail of this process's own log, installed behind the
// existing Logger::setSink seam.
//
// The logger is called from the stdio loop, the board watcher and the IPC
// threads, so every operation takes the lock. Reentrancy is already handled by
// the logger, which will not dispatch to a sink from inside a sink.
//
// The sink runs *before* the logger applies its own level filter, so the ring
// applies one itself. It uses the logger's configured level, so the page shows
// what Didi actually logged rather than a second, differently filtered view of
// it: raise verbosity and the page follows.
class LogRing {
public:
    explicit LogRing(std::size_t capacity = kControlRoomMaxLogRecords);

    void record(LogLevel level, std::string_view tag, std::string_view message);
    std::vector<ControlRoomLogRecord> snapshot() const;
    std::size_t dropped() const;

    // Overrides the logger's level for this ring. Used by tests; the installed
    // ring follows the process level.
    void setMinimumLevel(std::optional<LogLevel> level);

    // Installs this ring as the process log sink.
    void install();

private:
    bool admits(LogLevel level) const;

    mutable std::mutex m_mutex;
    std::deque<ControlRoomLogRecord> m_records;
    std::size_t m_capacity;
    std::size_t m_dropped{0};
    std::optional<LogLevel> m_minimumLevel;
};

// The ring this process publishes on the dashboard. Installed once, by the
// standalone server; a test that wants its own builds a LogRing directly.
LogRing& controlRoomLogRing();
void installControlRoomLogRing();

// One published session, as the dashboard shows it.
//
// The descriptor is carried whole so the model builder's allowlist stays the
// single place that decides what leaves this process, and liveness rides
// alongside because it is not a descriptor field: it is what the session
// scanner learned about the process behind one.
struct ControlRoomSession {
    runtime::SessionDescriptor descriptor;
    // Whether the process behind the descriptor is running. Unknown is a real
    // answer and is reported as one rather than guessed either way.
    std::optional<bool> alive;
    // The scanner judged the descriptor no longer trustworthy.
    bool stale{false};
};

struct ControlRoomInputs {
    std::string server_name;
    std::string server_version;
    // This binary's build identity, and the one the attached bridge published.
    //
    // They are different files. A user copies the addon into a project once and
    // rebuilds the server many times, so the pair drifts apart quietly, and a
    // stale bridge answers every call with the tool contract of whatever build
    // it came from. Nothing else in this dashboard can tell: the version is the
    // server's own, and the protocol version does not move on a contract change.
    std::string server_build_id;
    // Absent when nothing is attached. Present and empty means the bridge is
    // from a build that predates the field, which is itself a mismatch.
    std::optional<std::string> bridge_build_id;
    std::vector<std::string> protocol_versions;

    std::string project_root;
    // False when the root resolved at startup is no longer readable. The server
    // refuses to start without one, so this can only become false later.
    bool project_readable{true};

    bool connected{false};
    std::optional<std::string> session_kind;
    bool managed_unavailable{false};
    // Why there is no route, when something is known. "Route: detached" is also
    // what this said thirty seconds after the editor crashed, and what a second
    // server on a held editor was told, so the one tool whose job is to say what
    // state the bridge is in could not tell those from a session that was never
    // started (#527, #536).
    std::optional<runtime::RouteObstruction> route_obstruction;
    // Whether the extension published any descriptor at all, which is what
    // separates "no editor running" from "editor running, not attached".
    bool descriptors_present{false};
    // Whether any of those descriptors belongs to this server's project root.
    //
    // Separate from descriptors_present because they answer different
    // questions, and conflating them made the dashboard tell a project with no
    // addon to "attach one" whenever an unrelated project happened to have an
    // editor open (#388). Following that instruction is what produces a
    // cross-project attach.
    bool descriptors_for_this_project{false};

    // The filesystem preflight for the addon this project needs. Both are plain
    // stats and neither needs a live route, so they answer even in the state
    // where nothing else can.
    bool addon_present{false};
    bool addon_enabled{false};

    std::vector<ControlRoomSession> sessions;
    std::optional<std::string> selected_session_id;

    bool skip_confirmations{false};
    bool managed_recovery_armed{false};

    // Whether a blackboard file exists, from a plain stat.
    //
    // Deliberately not task counts: every board-reading entry point in
    // offline::blackboard sweeps expired state and reclaims lapsed leases,
    // and then saves the board. That is a write, and this tool declares
    // readOnlyHint. Counts are available by calling blackboard_task_list,
    // whose own classification covers the write it performs.
    bool board_present{false};
    std::vector<ControlRoomLogRecord> log;
    std::size_t log_dropped{0};
    // How many of the newest records to return, capped at the ring size.
    std::size_t log_limit{kControlRoomDefaultLogRecords};

    std::string captured_at;
};

// The payload the tool returns and the page renders.
//
// Field selection is an allowlist, deliberately. Nothing here serialises a
// session descriptor wholesale, so a field added to a descriptor later cannot
// reach a client by accident. See requirement A1 in the design.
json buildControlRoomModel(const ControlRoomInputs& inputs,
                           const std::vector<ToolDefinition>& tools);

// ISO-8601 UTC, to the second.
std::string controlRoomTimestamp();

// Reads the public payload runtime_list_sessions returns.
//
// Exposed because the obvious implementation is wrong in a way no unit test of
// the model would notice: SessionDescriptor::fromJson requires the session
// token, which this payload deliberately never carries, so parsing through it
// silently drops every session.
std::vector<ControlRoomSession> parseListedSessions(const json& payload);

// Gathers the inputs above from live process state and returns the model.
// Read-only: it stats, it reads the tool registry, and it asks the router
// whether a route is up. It writes nothing.
CallToolResult handleControlRoom(const json& args,
                                 const std::shared_ptr<ipc::IIpcClient>& ipc,
                                 const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                                 bool skip_confirmations, bool managed_recovery_armed);

}  // namespace mcp
}  // namespace didi
