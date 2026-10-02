#include "didi/mcp/jobs.hpp"

#include "didi/common/cancellation.hpp"
#include "didi/common/secure_random.hpp"

#include <algorithm>
#include <ctime>
#include <exception>
#include <iomanip>
#include <sstream>

namespace didi {
namespace mcp {

const char* jobStateName(JobState state) {
    switch (state) {
        case JobState::Working: return "working";
        case JobState::Completed: return "completed";
        case JobState::Cancelled: return "cancelled";
        case JobState::Abandoned: return "abandoned";
    }
    return "working";
}

JobStore::JobStore(Limits limits, Clock wall_clock)
    : m_limits(limits), m_clock(std::move(wall_clock)) {}

JobStore::~JobStore() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutting_down = true;
        for (auto& [id, entry] : m_jobs) {
            entry->cancelled->store(true, std::memory_order_release);
            if (entry->thread.joinable()) threads.push_back(std::move(entry->thread));
        }
        for (auto& orphan : m_orphans) threads.push_back(std::move(orphan));
        m_orphans.clear();
    }
    // Outside the lock: a job still running takes it to record its answer.
    for (auto& thread : threads) thread.join();
}

int64_t JobStore::now() const {
    if (m_clock) return m_clock();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void JobStore::expireLocked(int64_t now_ms) {
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        auto& view = it->second->view;
        // A running job outlives its ttl rather than vanishing under the work
        // still writing to it; it expires once it has an answer to drop.
        if (view.state != JobState::Working && now_ms - view.created_at_ms >= view.ttl_ms) {
            if (it->second->thread.joinable()) m_orphans.push_back(std::move(it->second->thread));
            it = m_jobs.erase(it);
        } else {
            ++it;
        }
    }
}

void JobStore::evictLocked() {
    while (m_jobs.size() >= m_limits.max_retained) {
        auto oldest = m_jobs.end();
        for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
            if (it->second->view.state == JobState::Working) continue;
            if (oldest == m_jobs.end() ||
                it->second->view.created_at_ms < oldest->second->view.created_at_ms) {
                oldest = it;
            }
        }
        if (oldest == m_jobs.end()) return;
        if (oldest->second->thread.joinable()) m_orphans.push_back(std::move(oldest->second->thread));
        m_jobs.erase(oldest);
    }
}

size_t JobStore::workingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<size_t>(std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& entry) {
        return entry.second->view.state == JobState::Working;
    }));
}

Result<JobView> JobStore::start(std::string conversation, std::string tool, std::string request_id,
                                std::string fingerprint, Work work) {
    auto id = security::secureRandomHex(16);
    if (id.isErr()) return id.error();
    // Threads of jobs already dropped from the store. Each one had an answer
    // before it was dropped, so it is finishing or finished, and joining it
    // here, outside the lock, keeps a long session from collecting them.
    std::vector<std::thread> finished_threads;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        finished_threads.swap(m_orphans);
    }
    for (auto& thread : finished_threads) thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutting_down) {
        return Error(503, "The server is shutting down, so no new job was started.",
                     {{"code", "not_connected"}, {"retryable", false}});
    }
    const auto created = now();
    expireLocked(created);
    const auto working = static_cast<size_t>(std::count_if(
        m_jobs.begin(), m_jobs.end(),
        [](const auto& entry) { return entry.second->view.state == JobState::Working; }));
    if (working >= m_limits.max_working) {
        return Error(429,
                     std::to_string(working) + " jobs are already running, which is as many as "
                     "this server runs at once. Nothing was started; wait for one to finish.",
                     {{"code", "rate_limited"},
                      {"retryable", true},
                      {"working_jobs", working},
                      {"max_working_jobs", m_limits.max_working},
                      {"retry_after_ms", m_limits.poll_interval_ms}});
    }
    evictLocked();

    auto entry = std::make_unique<Entry>();
    entry->cancelled = std::make_shared<std::atomic<bool>>(false);
    auto& view = entry->view;
    view.id = id.value();
    view.conversation = std::move(conversation);
    view.tool = std::move(tool);
    view.request_id = std::move(request_id);
    view.fingerprint = std::move(fingerprint);
    view.created_at_ms = created;
    view.updated_at_ms = created;
    view.ttl_ms = m_limits.ttl_ms;
    view.poll_interval_ms = m_limits.poll_interval_ms;
    const JobView started = view;

    auto* raw = entry.get();
    auto cancelled = entry->cancelled;
    m_jobs.emplace(started.id, std::move(entry));
    raw->thread = std::thread([this, job_id = started.id, cancelled, work = std::move(work)]() {
        std::optional<json> result;
        std::string failure;
        try {
            const CancellationScope scope(cancelled.get());
            result = work(*cancelled);
        } catch (const std::exception& error) {
            failure = std::string("the work threw: ") + error.what();
        } catch (...) {
            failure = "the work threw something that is not an exception";
        }
        {
            std::lock_guard<std::mutex> finished(m_mutex);
            auto found = m_jobs.find(job_id);
            if (found != m_jobs.end()) {
                auto& done = found->second->view;
                done.updated_at_ms = now();
                if (!failure.empty()) {
                    done.state = JobState::Abandoned;
                    done.abandoned_reason = failure;
                } else if (cancelled->load(std::memory_order_acquire)) {
                    // Asked to stop, so its answer is not the work's answer:
                    // a build killed half way says nothing about the code.
                    done.state = JobState::Cancelled;
                } else {
                    done.state = JobState::Completed;
                    done.result = std::move(result);
                }
            }
        }
        m_changed.notify_all();
    });
    return started;
}

std::optional<JobView> JobStore::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_jobs.find(id);
    if (found == m_jobs.end()) return std::nullopt;
    const auto& view = found->second->view;
    if (view.state != JobState::Working && now() - view.created_at_ms >= view.ttl_ms) {
        return std::nullopt;
    }
    return view;
}

std::optional<JobView> JobStore::findByRequest(const std::string& conversation,
                                               const std::string& tool,
                                               const std::string& request_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto current = now();
    for (const auto& [id, entry] : m_jobs) {
        const auto& view = entry->view;
        if (view.request_id.empty() || view.request_id != request_id || view.tool != tool ||
            view.conversation != conversation) {
            continue;
        }
        if (view.state != JobState::Working && current - view.created_at_ms >= view.ttl_ms) {
            return std::nullopt;
        }
        return view;
    }
    return std::nullopt;
}

std::optional<JobView> JobStore::waitFor(const std::string& id,
                                         std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto found = m_jobs.find(id);
        if (found == m_jobs.end()) return std::nullopt;
        if (found->second->view.state != JobState::Working) return found->second->view;
        if (m_changed.wait_until(lock, deadline) == std::cv_status::timeout) {
            const auto again = m_jobs.find(id);
            if (again == m_jobs.end()) return std::nullopt;
            return again->second->view;
        }
    }
}

std::optional<JobView> JobStore::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_jobs.find(id);
    if (found == m_jobs.end()) return std::nullopt;
    auto& view = found->second->view;
    if (view.state == JobState::Working) {
        found->second->cancelled->store(true, std::memory_order_release);
        view.cancel_requested = true;
        view.updated_at_ms = now();
    }
    return view;
}

std::string jobFingerprint(const std::string& tool, const json& arguments) {
    json kept = arguments.is_object() ? arguments : json::object();
    for (const auto* attempt : {"request_id", "confirmation_token", "dry_run"}) kept.erase(attempt);
    // nlohmann's object is ordered by key, so the same arguments in any order
    // dump the same text.
    return tool + "\n" + kept.dump();
}

std::string isoTimestamp(int64_t epoch_ms) {
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    std::ostringstream text;
    text << std::put_time(&parts, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << (epoch_ms % 1000) << 'Z';
    return text.str();
}

}  // namespace mcp
}  // namespace didi
