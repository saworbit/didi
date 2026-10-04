#pragma once

// Long work as jobs (Q8 in docs/BUILD_QUEUE.md, principle P7).
//
// The server answers one request at a time, and an offline helper such as
// project_export ran on that thread until it finished, for up to its
// timeout_seconds. A job runs the same tool call on a thread of its own, keeps
// the answer, and lets the caller read it later, by the job's id or by the
// request id the caller chose. The answer stored is the one the call would
// have given synchronously, because the job runs the same pipeline.
//
// This store owns the threads and the answers. It knows nothing about MCP; the
// server decides which calls become jobs and how a job is shown on the wire.

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace didi {
namespace mcp {

enum class JobState {
    // The work is running.
    Working,
    // The work returned, and `result` is what it answered, an error included.
    Completed,
    // The caller asked for it to stop before it returned. No result is kept.
    Cancelled,
    // The server stopped it without being asked: the work threw, or the server
    // shut down while it ran. `abandoned_reason` says which.
    Abandoned,
};

const char* jobStateName(JobState state);

// What a reader of the store is shown about one job.
struct JobView {
    std::string id;
    std::string conversation;
    std::string tool;
    std::string request_id;
    std::string fingerprint;
    JobState state{JobState::Working};
    bool cancel_requested{false};
    std::string abandoned_reason;
    // Wall-clock, in milliseconds since the epoch, for the ISO timestamps the
    // tasks extension asks for.
    int64_t created_at_ms{0};
    int64_t updated_at_ms{0};
    int64_t ttl_ms{0};
    int64_t poll_interval_ms{0};
    // The work's answer, as CallToolResult::toJson() gave it. Present only in
    // the Completed state.
    std::optional<json> result;
};

class JobStore {
public:
    struct Limits {
        // Jobs running at once. A start past this is refused, not queued: a
        // queue would be a second place for work to wait unseen.
        size_t max_working{4};
        // Jobs kept, finished ones included. The oldest finished job goes
        // first; a running job is never evicted.
        size_t max_retained{64};
        // How long a job is kept from its creation.
        int64_t ttl_ms{60 * 60 * 1000};
        // How often a reader is asked to look again.
        int64_t poll_interval_ms{2000};
    };

    // The work a job runs. It is handed the job's cancellation flag, which the
    // runner also publishes for the thread (didi/common/cancellation.hpp).
    using Work = std::function<json(const std::atomic<bool>& cancelled)>;
    using Clock = std::function<int64_t()>;

    // Two constructors rather than a defaulted Limits: GCC and Clang refuse
    // a default argument that needs a nested struct's member initialisers
    // inside the class that declares it.
    JobStore();
    explicit JobStore(Limits limits, Clock wall_clock = {});
    ~JobStore();
    JobStore(const JobStore&) = delete;
    JobStore& operator=(const JobStore&) = delete;

    // Starts `work` on a thread of its own. Refused with 429 when max_working
    // jobs are running, with the count in the error's data.
    Result<JobView> start(std::string conversation, std::string tool, std::string request_id,
                          std::string fingerprint, Work work);

    std::optional<JobView> find(const std::string& id) const;
    // The job a conversation started with a request id for this tool, if it is
    // still kept.
    std::optional<JobView> findByRequest(const std::string& conversation, const std::string& tool,
                                         const std::string& request_id) const;

    // Waits until the job leaves Working or `timeout` passes, and returns what
    // it is then. Nothing when no such job is kept.
    std::optional<JobView> waitFor(const std::string& id, std::chrono::milliseconds timeout) const;

    // Asks the job to stop. A finished job is left as it is, and the answer
    // says so; the caller decides whether that is an error.
    std::optional<JobView> cancel(const std::string& id);

    size_t workingCount() const;
    const Limits& limits() const { return m_limits; }

    // Cancels every running job, joins its thread, and refuses new ones. The
    // destructor does the same; this is for a server about to leave by a way
    // that runs no destructor (#1169). A second call finds nothing to join.
    void shutdown();

private:
    struct Entry {
        JobView view;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::thread thread;
    };

    void expireLocked(int64_t now);
    void evictLocked();
    int64_t now() const;

    Limits m_limits;
    Clock m_clock;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::map<std::string, std::unique_ptr<Entry>> m_jobs;
    // Threads of jobs that left the map while running cannot be joined under
    // the lock; they are joined here, outside it, when the store goes.
    std::vector<std::thread> m_orphans;
    bool m_shutting_down{false};
};

// A fingerprint of a call's arguments for deciding whether a repeated request
// id asks for the same work. The arguments that are about this one attempt,
// not about the work, are left out.
std::string jobFingerprint(const std::string& tool, const json& arguments);

// An ISO 8601 UTC timestamp, to the millisecond.
std::string isoTimestamp(int64_t epoch_ms);

}  // namespace mcp
}  // namespace didi
