#include "didi/common/logger.hpp"
#include <cstdlib>
#include <cstring>

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
    const char* env_level = std::getenv("DIDI_LOG_LEVEL");
    if (env_level) {
        std::string lvl(env_level);
        if (lvl == "DEBUG" || lvl == "debug" || lvl == "0") m_level = LogLevel::Debug;
        else if (lvl == "INFO" || lvl == "info" || lvl == "1") m_level = LogLevel::Info;
        else if (lvl == "WARN" || lvl == "warn" || lvl == "2") m_level = LogLevel::Warn;
        else if (lvl == "ERROR" || lvl == "error" || lvl == "3") m_level = LogLevel::Error;
        else if (lvl == "NONE" || lvl == "none" || lvl == "4") m_level = LogLevel::None;
    }
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

    std::lock_guard<std::mutex> lock(m_mutex);
    std::cerr << color_code << "[" << std::put_time(&bt, "%Y-%m-%d %H:%M:%S")
              << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
              << "[" << level_str << "] [" << tag << "]\033[0m "
              << message << std::endl;
}

void Logger::setSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sink = std::move(sink);
}

} // namespace didi
