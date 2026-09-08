#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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
    virtual std::optional<RuntimeRouteLease> acquireRouteLease() = 0;
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
    virtual Result<json> attachSession(const std::string& session_id) = 0;
    virtual Result<json> detachSession() = 0;
    virtual Result<json> refreshSession() {
        return Error::notConnected("Fresh runtime session state is unavailable");
    }
    virtual std::optional<SessionDescriptor> activeSession() const = 0;
    std::optional<RuntimeRouteLease> acquireRouteLease() override { return std::nullopt; }
    bool quarantineRoute(const RuntimeRouteLease&) override { return false; }
};

std::optional<RuntimeRouteLease> acquireRuntimeRouteLease(
    const std::shared_ptr<ipc::IIpcClient>& router);
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
