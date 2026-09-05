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

// The same three states as the word a transport failure reports.
const char* processInstanceStateName(ProcessInstanceState state);

struct SessionDescriptor;

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
