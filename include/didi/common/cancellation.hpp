#pragma once

// Cooperative cancellation for work running as a job (Q8 in
// docs/BUILD_QUEUE.md).
//
// A job runs a tool call on its own thread, through the same handlers a
// synchronous call takes, and those handlers have no cancellation parameter.
// The job's flag is published for the thread instead, and the code that can
// actually stop, the process runner's wait loop, reads it. Work that never
// checks is not interrupted; the job still reports that cancellation was asked
// for.

#include <atomic>

namespace didi {

namespace detail {
inline thread_local const std::atomic<bool>* t_cancellation = nullptr;
}  // namespace detail

// Publishes `flag` as this thread's cancellation flag for the scope's lifetime,
// and restores whatever was published before.
class CancellationScope {
public:
    explicit CancellationScope(const std::atomic<bool>* flag) : previous_(detail::t_cancellation) {
        detail::t_cancellation = flag;
    }
    ~CancellationScope() { detail::t_cancellation = previous_; }
    CancellationScope(const CancellationScope&) = delete;
    CancellationScope& operator=(const CancellationScope&) = delete;

private:
    const std::atomic<bool>* previous_;
};

// Whether the job running on this thread has been asked to stop. False on any
// thread that is not running a job.
inline bool cancellationRequested() {
    const auto* flag = detail::t_cancellation;
    return flag != nullptr && flag->load(std::memory_order_acquire);
}

// Whether this thread is running a job at all. A handler whose wait is bounded
// by a deadline the stdio loop cannot outlive may wait longer here, because a
// job's caller is not waiting on that loop.
inline bool runningAsJob() { return detail::t_cancellation != nullptr; }

}  // namespace didi
