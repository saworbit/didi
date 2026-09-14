#include "didi/common/logger.hpp"
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#define DIDI_STDERR_IS_TTY() (_isatty(_fileno(stderr)) != 0)
#else
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
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cerr << (color ? color_code : "") << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << "[" << level_str << "] [" << tag << "]" << (color ? "\033[0m" : "") << " "
              << message << std::endl;
}

void Logger::setSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

} // namespace didi
