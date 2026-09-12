#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "didi/common/ipc_channel.hpp"

namespace didi::runtime {

inline constexpr int kMaxPublicLiveRequestMs = 17000;

struct ProcessIdentity {
    int64_t started_at_ms{0};
    int64_t resolution_ms{1};
};

Result<ProcessIdentity> queryProcessIdentity(uint64_t pid);

// Whether the process behind a session is still the process that opened it.
//
// Three states rather than two on purpose. A process that cannot be queried is
// not a process that has gone, and reporting one as the other would be
// inventing the fact a caller most wants. proven_stale means the pid is gone or
// belongs to something started at a different time; unverifiable means the
// question could not be answered.
enum class ProcessInstanceState { alive, proven_stale, unverifiable };

ProcessInstanceState processInstanceState(uint64_t pid, int64_t started_at_ms);

// The same answer, with the reason behind an unverifiable one.
//
// "unknown" is honest and, on its own, useless. A transport failure that
// reports it leaves a reader exactly where they were: the engine may have
// crashed, or the query may simply have been refused. The two are different
// problems and the payload could not tell them apart, which is how #227 spent
// several rounds on inference.
//
// `reason` is empty for a plain alive or gone, which need no explaining, and
// otherwise one of:
//
//   open_denied               the process could not be opened, and not because
//                             there is no such process
//   running_but_unidentified  something with that pid is running, and it could
//                             not be confirmed as the one the session opened
struct ProcessInstanceReport {
    ProcessInstanceState state{ProcessInstanceState::unverifiable};
    std::string reason;
    // The operating system's own error, when there was one. Zero otherwise.
    unsigned long os_error{0};
};

ProcessInstanceReport describeProcessInstance(uint64_t pid, int64_t started_at_ms);

// The same three states as the word a transport failure reports.
const char* processInstanceStateName(ProcessInstanceState state);

struct SessionDescriptor;

// What the engine left behind when it died. The capture writes this from
// inside the engine process, so it is the only account of a fault that kills
// the editor before it can report anything itself.
struct EngineCrashReport {
    bool found{false};
    // Whether a frame of ours is on the faulting stack. False means the engine
    // faulted in its own code, which is a different conversation from a fault
    // in a tool call.
    bool in_extension{false};
    bool on_main_thread{false};
    std::string exception;
    std::string path;
};

// Looks where the engine would have written one: DIDI_CRASH_CAPTURE_DIR when
// it is set, otherwise the project's .didi/crash.
EngineCrashReport findEngineCrashReport(const std::string& project_path, uint64_t pid);

// What happened to the engine, and what a caller can do about it.
//
// A transport failure already carries facts. This is the reading of them: one
// classification site, so the routes that report a failure cannot disagree
// about what kind of failure it was.
enum class EngineIncidentKind { none, crashed, unreachable, hung, session_lost };

struct EngineIncident {
    EngineIncidentKind kind{EngineIncidentKind::none};
    std::string cause;
    // One imperative sentence. The agent on the other end has to be able to act
    // on it without reading anything else.
    std::string recovery;
    bool recoverable_without_human{false};
};

const char* engineIncidentKindName(EngineIncidentKind kind);

// Pure, so the table of cases can be tested without a live engine. Takes
// whether the failure was a transport failure, because an engine that is alive
// and did not answer is a different incident from one that is merely alive.
EngineIncident classifyEngineIncident(ProcessInstanceState state, const EngineCrashReport& crash,
                                      bool transport_failed);

// Records whether the engine behind a session is still there, on a transport
// failure that is about to be reported.
//
// A failure saying the peer closed the pipe does not say why it went, and that
// is the difference between an engine that crashed and one that is alive and
// merely stopped answering. Every route that classifies a transport failure
// calls this, so the four of them cannot answer the question differently.
//
// Silent when there is no session or no pid: an absent fact is reported by
// being absent, not by a default.
void annotateEngineState(Error& error, const std::optional<SessionDescriptor>& session);

// Why this server has no live route, remembered past the call that found out.
//
// An engine crash is reported perfectly once, to whichever call happened to be
// next, and then forgotten: every later call answered the generic 503 and
// didi_control_room -- the one tool whose job is to say what state the bridge
// is in -- said "detached", which is also what it says when no editor was ever
// started (#536). A bridge held by a second MCP server is worse, because the
// answer a second agent gets is byte for byte the answer it would get with no
// Godot running at all, and the sensible next move from there is to edit files
// underneath the agent that does hold it (#527).
//
// One record, process wide, because there is one bridge. It is set when a route
// is refused or lost and cleared when one is opened, so it always describes the
// current absence rather than an old one.
struct RouteObstruction {
    // engine_crashed, engine_unreachable, engine_hung, session_lost or
    // bridge_held. The first four are EngineIncidentKind names, so a caller
    // that already branches on `incident` reads the same vocabulary.
    std::string kind;
    std::string cause;
    std::string recovery;
    uint64_t pid{0};
    std::string session_id;
    int64_t at_ms{0};

    json toJson() const;
};

void recordRouteObstruction(RouteObstruction obstruction);
void clearRouteObstruction();
std::optional<RouteObstruction> lastRouteObstruction();

// Merges the remembered obstruction into an error that is about to say only
// that nothing is attached. Does nothing when there is none, so a server that
// has simply never attached still answers exactly as it did.
void annotateRouteObstruction(Error& error);
Result<std::filesystem::path> resolveSessionDescriptorDirectory();

using DescriptorOpenedHook = std::function<void(const std::filesystem::path&)>;

struct SessionDescriptor {
    int schema_version{1};
    std::string session_id;
    std::string token;
    uint64_t pid{0};
    std::string kind;
    std::string project_path;
    std::string endpoint;
    int64_t started_at_ms{0};
    std::string protocol_version;
    // Which build of Didi published this session, which is the extension's
    // build and not the server's. Optional, and empty when the extension that
    // published the descriptor predates the field. Empty is itself an answer:
    // it means the bridge is older than this server, so it is reported rather
    // than passed over.
    std::string build_id;
    // Which Godot the extension is running in, as the engine reports it, for
    // example "Godot v4.5.1.stable.official". Optional and empty for the same
    // reason build_id is: an extension built before the field existed publishes
    // a descriptor without it. Empty means unknown, never a match.
    std::string engine_version;

    json toJson(bool include_token = false) const;
    // Which session something happened on, without the address to reach it.
    //
    // A failure has to say which route it failed on, and session_id, kind,
    // pid and project_path all say that. The endpoint says something else:
    // it is the named pipe or socket to connect to. Session discovery
    // legitimately hands that out, because picking a session is what
    // runtime_list_sessions is for. An error does not, because the caller
    // is already attached to the session it is being told about.
    json toProvenanceJson() const;
    static Result<SessionDescriptor> fromJson(const json& value);
};

struct RuntimeRouteLease {
    std::shared_ptr<ipc::IIpcClient> client;
    std::optional<SessionDescriptor> descriptor;
    uint64_t generation{0};

    Result<json> sendRequest(const std::string& method, const json& params,
                             int timeout_ms = 5000) const;
};

class IRuntimeRouteLeaseProvider {
public:
    virtual ~IRuntimeRouteLeaseProvider() = default;
    // The process selection: whichever session `runtime_attach_session` last
    // pointed at. This is the legacy lifecycle and stays exactly that.
    virtual std::optional<RuntimeRouteLease> acquireRouteLease() = 0;

    // A lease on one named session, whether or not it is the process
    // selection. This is what lets two tasks interleave requests against two
    // editors on one stdio process without either seeing the other's.
    //
    // The default is the single-route behaviour every implementation had
    // before there could be more than one: the selected route, and only when
    // it happens to be the session that was asked for.
    virtual std::optional<RuntimeRouteLease> acquireRouteLeaseFor(const std::string& session_id) {
        auto lease = acquireRouteLease();
        if (lease.has_value() && lease->descriptor.has_value() &&
            lease->descriptor->session_id == session_id) {
            return lease;
        }
        return std::nullopt;
    }
    virtual bool quarantineRoute(const RuntimeRouteLease& lease) = 0;
};

enum class DescriptorRetirementOutcome {
    deleted,
    retained_collision_or_race,
    retained_unavailable,
};

// Retirement is move-then-delete. An owner that dies between the two steps
// leaves a `<id>.json.didi-retired-<id>-<nonce>` tombstone that nothing will
// ever finish removing. Reaping is deliberately conservative: a tombstone is
// removed only when its contents parse as a descriptor, the session id in the
// filename matches the session id inside it, and the owning process is provably
// gone. Anything less is retained.
enum class TombstoneReapOutcome {
    reaped,
    // The owner is alive, or its state could not be proven either way.
    retained_owner_not_proven_gone,
    // Unreadable, unparseable, or the name and contents disagree.
    retained_unverifiable,
    // POSIX only: no portable unlink primitive is bound to a verified open
    // file, so removing by name would leave a substitution window. Retained for
    // the same reason retirement retains its own tombstones.
    retained_unavailable,
    // Not one of our tombstones. The entry is left untouched.
    not_a_tombstone,
};

TombstoneReapOutcome reapOrphanedDescriptorTombstone(
    const std::filesystem::path& directory,
    const std::filesystem::path& path);

DescriptorRetirementOutcome retireOwnedSessionDescriptor(
    const std::filesystem::path& path,
    const SessionDescriptor& descriptor,
    const std::function<void(const std::filesystem::path&)>& before_move = {},
    const std::function<void(const std::filesystem::path&)>& after_verification = {},
    const std::function<void(const std::filesystem::path&)>& before_final_delete = {});

class IRuntimeSessionClient : public ipc::IIpcClient, public IRuntimeRouteLeaseProvider {
public:
    virtual Result<json> listSessions(const std::optional<std::string>& project_path) = 0;
    // Selects a session for the process. Sticky, and what the legacy
    // `runtime_attach_session` tool has always done.
    virtual Result<json> attachSession(const std::string& session_id) = 0;

    // Attaching a session that belongs to a different project than the one this
    // server was started on.
    //
    // Auto-selection has always required the project paths to match, but naming
    // a session explicitly skipped that check, so a server pointed at project B
    // would happily serve project A's scene tree and route mutations into it
    // (#387). Refusing is the default; the flag is the caller saying they mean
    // it. Implementations that do not route by project ignore it.
    virtual Result<json> attachSession(const std::string& session_id,
                                       bool allow_foreign_project) {
        (void)allow_foreign_project;
        return attachSession(session_id);
    }
    virtual Result<json> detachSession() = 0;

    // Holds a route to one session without changing the process selection.
    //
    // A modern request names the session it means, and opening that route must
    // not move the selection out from under a legacy client on the same
    // process. The default routes it through attach, which is the best a
    // single-route implementation can do and is what they all did before.
    virtual Result<json> openSessionRoute(const std::string& session_id) {
        return attachSession(session_id);
    }
    // Releases one named route. Releasing the selected one also clears the
    // selection, so this is a superset of detach rather than a second way to
    // do it.
    virtual Result<json> closeSessionRoute(const std::string&) {
        return detachSession();
    }
    // Every session this process is currently holding a route to, selected or
    // not. Shutdown reads this: a route that is not released takes its
    // ownership lock with it, and every other Didi process is refused that
    // session until this one exits.
    virtual std::vector<SessionDescriptor> heldSessions() const {
        const auto selected = activeSession();
        if (!selected.has_value()) return {};
        return {*selected};
    }
    virtual Result<json> refreshSession() {
        return Error::notConnected("Fresh runtime session state is unavailable");
    }
    virtual std::optional<SessionDescriptor> activeSession() const = 0;
    std::optional<RuntimeRouteLease> acquireRouteLease() override { return std::nullopt; }
    bool quarantineRoute(const RuntimeRouteLease&) override { return false; }
};

std::optional<RuntimeRouteLease> acquireRuntimeRouteLease(
    const std::shared_ptr<ipc::IIpcClient>& router);

// A lease on one named session. Unlike the selection-based form above this
// does not require the process selection to be connected, because the whole
// point is to reach a session that is not the selected one.
std::optional<RuntimeRouteLease> acquireRuntimeRouteLeaseFor(
    const std::shared_ptr<ipc::IIpcClient>& router, const std::string& session_id);
bool quarantineRuntimeRoute(const std::shared_ptr<ipc::IIpcClient>& router,
                            const RuntimeRouteLease& lease);

// How long a repeat attempt gets to open a new connection to the same session.
// The endpoint is a local pipe or socket that either accepts immediately or is
// not there, so this is a bound on a stall rather than a budget to spend.
inline constexpr int kRouteReconnectMs = 2000;

// Opens a new connection for a lease whose old one a transport failure closed,
// so a call that is safe to repeat can be repeated on the same session. The
// session token travels in every request, so a new connection needs no second
// handshake. False means the endpoint would not take a connection, which is
// the answer when the engine has gone.
bool reconnectRuntimeRoute(const RuntimeRouteLease& lease,
                           int timeout_ms = kRouteReconnectMs);

// What a live request came back with, and whether it took two attempts.
struct RouteRequestResult {
    Result<json> response;
    // A repeat was made. Present on a failure so a reader can tell an engine
    // that answered nothing twice from one that was asked once.
    bool repeat_attempted{false};
    // The repeat is the attempt that answered.
    bool repeat_answered{false};
};

// Sends one live request, and asks again once when the transport fails and
// repeating the call cannot change anything.
//
// A transport failure leaves the caller unable to say whether the engine ran
// the request. For a mutation that ambiguity has to be reported, because
// applying it twice is worse than not knowing. For a call that changes nothing,
// asking again is what settles it, and it costs one reconnect: the failure took
// the old connection with it, so the repeat opens a new one on the same
// session.
//
// One repeat, not a loop. The point is to survive a connection that went away,
// not to keep knocking on an engine that has.
RouteRequestResult sendLiveRouteRequest(const RuntimeRouteLease& lease,
                                        const std::string& method, const json& params,
                                        int timeout_ms, bool repeatable);

std::shared_ptr<IRuntimeSessionClient> createRuntimeSessionClient(
    const std::string& project_root,
    ipc::IpcClientFactory ipc_client_factory = ipc::createIpcClient,
    DescriptorOpenedHook descriptor_opened_hook = {});

} // namespace didi::runtime
