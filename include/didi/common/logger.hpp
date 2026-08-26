#pragma once

#include <string>
#include <string_view>
#include <iostream>
#include <mutex>
#include <atomic>
#include <sstream>
#include <chrono>
#include <iomanip>

namespace didi {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    None = 4
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel getLevel() const;

    void log(LogLevel level, std::string_view tag, std::string_view message);

    template <typename... Args>
    void debug(std::string_view tag, Args&&... args) {
        logFormat(LogLevel::Debug, tag, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::string_view tag, Args&&... args) {
        logFormat(LogLevel::Info, tag, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::string_view tag, Args&&... args) {
        logFormat(LogLevel::Warn, tag, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::string_view tag, Args&&... args) {
        logFormat(LogLevel::Error, tag, std::forward<Args>(args)...);
    }

private:
    Logger();
    ~Logger() = default;

    template <typename... Args>
    void logFormat(LogLevel level, std::string_view tag, Args&&... args) {
        if (level < m_level.load(std::memory_order_relaxed)) return;
        std::ostringstream ss;
        (ss << ... << args);
        log(level, tag, ss.str());
    }

    std::atomic<LogLevel> m_level{LogLevel::Info};
    mutable std::mutex m_mutex;
};

#define DIDI_LOG_DEBUG(tag, ...) ::didi::Logger::instance().debug(tag, __VA_ARGS__)
#define DIDI_LOG_INFO(tag, ...)  ::didi::Logger::instance().info(tag, __VA_ARGS__)
#define DIDI_LOG_WARN(tag, ...)  ::didi::Logger::instance().warn(tag, __VA_ARGS__)
#define DIDI_LOG_ERROR(tag, ...) ::didi::Logger::instance().error(tag, __VA_ARGS__)

} // namespace didi
