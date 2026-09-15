#include "didi/common/logger.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#define DIDI_STDERR_IS_TTY() (_isatty(_fileno(stderr)) != 0)
#else
#include <cerrno>
#include <unistd.h>
#define DIDI_STDERR_IS_TTY() (isatty(STDERR_FILENO) != 0)
#endif

namespace didi {

namespace {
thread_local bool g_dispatchingSink = false;

class SinkDispatchGuard {
public:
    SinkDispatchGuard() { g_dispatchingSink = true; }
    ~SinkDispatchGuard() { g_dispatchingSink = false; }
};

// Bounds on what is held for a reader that is not reading. Four thousand lines
// is more than the whole DEBUG startup log, so an ordinary slow reader loses
// nothing; the byte cap is what stops one enormous message from standing for
// the rest. Past either, the oldest line goes, because the reason anyone is
// reading this log is to find out what just happened.
constexpr size_t kMaxQueuedLines = 4096;
constexpr size_t kMaxQueuedBytes = 4u * 1024u * 1024u;

// The writer thread's way out. Through the file descriptor rather than
// std::cerr, because that thread is never joined: iostreams are taken apart by
// static destruction while it may still be inside a write, and a descriptor
// stays valid until the process does. The inline path in Logger::log keeps
// using std::cerr, where there is no such hazard.
void writeConsole(const char* data, size_t size) {
#if defined(_WIN32)
    const int fd = _fileno(stderr);
    if (fd < 0) return;
#else
    const int fd = STDERR_FILENO;
#endif
    while (size > 0) {
#if defined(_WIN32)
        const int written = _write(fd, data, static_cast<unsigned int>(std::min<size_t>(size, 1u << 20)));
#else
        const auto written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
#endif
        if (written <= 0) return;
        data += written;
        size -= static_cast<size_t>(written);
    }
}

// The queue a log line lands in once the console is written off-thread.
//
// Deliberately never destroyed and never joined: joining means waiting on the
// very write that is stuck, which is the thing this exists to avoid. Leaking
// the state is what makes that safe -- a thread parked in write() at process
// exit cannot reach anything that has already been taken apart.
class ConsoleWriter {
public:
    static ConsoleWriter& instance() {
        static auto* writer = new ConsoleWriter();
        return *writer;
    }

    void push(std::string line) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_queue.empty() &&
                   (m_queue.size() >= kMaxQueuedLines || m_bytes + line.size() > kMaxQueuedBytes)) {
                m_bytes -= m_queue.front().size();
                m_queue.pop_front();
                --m_pending;
                ++m_dropped;
            }
            m_bytes += line.size();
            m_queue.push_back(std::move(line));
            ++m_pending;
        }
        m_arrived.notify_one();
    }

    bool drain(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_written.wait_for(lock, timeout, [this] { return m_pending == 0; });
    }

private:
    ConsoleWriter() { std::thread(&ConsoleWriter::run, this).detach(); }

    [[noreturn]] void run() {
        for (;;) {
            std::deque<std::string> batch;
            size_t dropped = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_arrived.wait(lock, [this] { return !m_queue.empty(); });
                batch.swap(m_queue);
                m_bytes = 0;
                dropped = m_dropped;
                m_dropped = 0;
            }
            if (dropped > 0) {
                // Said once per catch-up rather than per line, and said at all
                // because a log with a hole in it and no note is worse than a
                // shorter log.
                const auto notice = "[LOGGER] " + std::to_string(dropped) +
                                    " log lines were dropped: standard error was not being read\n";
                writeConsole(notice.data(), notice.size());
            }
            for (const auto& line : batch) writeConsole(line.data(), line.size());
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pending -= batch.size();
            }
            m_written.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_arrived;
    std::condition_variable m_written;
    std::deque<std::string> m_queue;
    size_t m_bytes{0};
    size_t m_pending{0};
    size_t m_dropped{0};
};

std::atomic<bool> g_consoleOffThread{false};
} // namespace

Logger& Logger::instance() {
    static Logger s_instance;
    return s_instance;
}

Logger::Logger() {
    if (const char* env_level = std::getenv("DIDI_LOG_LEVEL")) {
        if (auto level = parseLevel(env_level)) m_level = *level;
    }
    m_color = DIDI_STDERR_IS_TTY();
}

std::optional<LogLevel> Logger::parseLevel(std::string_view text) {
    if (text == "DEBUG" || text == "debug" || text == "0") return LogLevel::Debug;
    if (text == "INFO" || text == "info" || text == "1") return LogLevel::Info;
    if (text == "WARN" || text == "warn" || text == "2") return LogLevel::Warn;
    if (text == "ERROR" || text == "error" || text == "3") return LogLevel::Error;
    if (text == "NONE" || text == "none" || text == "4") return LogLevel::None;
    return std::nullopt;
}

bool Logger::levelSetByEnvironment() {
    const char* env_level = std::getenv("DIDI_LOG_LEVEL");
    return env_level && parseLevel(env_level).has_value();
}

void Logger::setColorEnabled(bool enabled) {
    m_color.store(enabled, std::memory_order_relaxed);
}

bool Logger::colorEnabled() const {
    return m_color.load(std::memory_order_relaxed);
}

void Logger::setLevel(LogLevel level) {
    m_level.store(level, std::memory_order_relaxed);
}

LogLevel Logger::getLevel() const {
    return m_level.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level, std::string_view tag, std::string_view message) {
    LogSink sink;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sink = m_sink;
    }
    if (sink && !g_dispatchingSink) {
        SinkDispatchGuard guard;
        sink(level, tag, message);
    }

    if (level < m_level.load(std::memory_order_relaxed)) return;

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm bt{};
#if defined(_WIN32)
    localtime_s(&bt, &in_time_t);
#else
    localtime_r(&in_time_t, &bt);
#endif

    const char* level_str = "INFO";
    const char* color_code = "\033[32m"; // Green
    switch (level) {
        case LogLevel::Debug: level_str = "DEBUG"; color_code = "\033[36m"; break; // Cyan
        case LogLevel::Info:  level_str = "INFO "; color_code = "\033[32m"; break; // Green
        case LogLevel::Warn:  level_str = "WARN "; color_code = "\033[33m"; break; // Yellow
        case LogLevel::Error: level_str = "ERROR"; color_code = "\033[31m"; break; // Red
        default: break;
    }

    const bool color = m_color.load(std::memory_order_relaxed);
    std::ostringstream line;
    line << (color ? color_code : "") << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S")
         << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
         << "[" << level_str << "] [" << tag << "]" << (color ? "\033[0m" : "") << " "
         << message << "\n";

    if (g_consoleOffThread.load(std::memory_order_relaxed)) {
        ConsoleWriter::instance().push(line.str());
        return;
    }
    // One line, one write, under the lock, so two threads cannot interleave
    // halves of a record. Still through std::cerr rather than the descriptor:
    // there is no teardown hazard on this path, and a caller that redirects
    // the stream to read what was written keeps working.
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cerr << line.str() << std::flush;
}

void Logger::useBackgroundConsoleWriter() {
    // Built before the flag is set, so the first line to arrive does not race
    // the thread that has to carry it.
    ConsoleWriter::instance();
    g_consoleOffThread.store(true, std::memory_order_relaxed);
}

bool Logger::flushConsole(std::chrono::milliseconds timeout) {
    if (!g_consoleOffThread.load(std::memory_order_relaxed)) return true;
    return ConsoleWriter::instance().drain(timeout);
}

void Logger::setSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

} // namespace didi
