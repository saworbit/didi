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

struct ControlRoomInputs {
    std::string server_name;
    std::string server_version;
    std::vector<std::string> protocol_versions;

    std::string project_root;
    // False when the root resolved at startup is no longer readable. The server
    // refuses to start without one, so this can only become false later.
    bool project_readable{true};

    bool connected{false};
    std::optional<std::string> session_kind;
    bool managed_unavailable{false};
    // Whether the extension published any descriptor at all, which is what
    // separates "no editor running" from "editor running, not attached".
    bool descriptors_present{false};

    std::vector<runtime::SessionDescriptor> sessions;
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

// Gathers the inputs above from live process state and returns the model.
// Read-only: it stats, it reads the tool registry, and it asks the router
// whether a route is up. It writes nothing.
CallToolResult handleControlRoom(const json& args,
                                 const std::shared_ptr<ipc::IIpcClient>& ipc,
                                 const std::shared_ptr<runtime::IRuntimeSessionClient>& sessions,
                                 bool skip_confirmations, bool managed_recovery_armed);

}  // namespace mcp
}  // namespace didi
