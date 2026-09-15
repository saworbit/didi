#pragma once

#include <string>
#include <string_view>
#include <iostream>
#include <mutex>
#include <atomic>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <functional>
#include <optional>

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
    using LogSink = std::function<void(LogLevel, std::string_view, std::string_view)>;

    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel getLevel() const;

    // The level a DIDI_LOG_LEVEL value or a log_level setting names, or
    // nothing for text that names none.
    static std::optional<LogLevel> parseLevel(std::string_view text);

    // Whether DIDI_LOG_LEVEL was set for this process. The environment wins
    // over any default or setting, because it was set for this one launch.
    static bool levelSetByEnvironment();

    // ANSI colour on the console lines. Defaults to whether stderr is a
    // terminal: the escape codes are for a person reading a terminal, and
    // written into a redirected stream they are noise in every file and
    // Output dock that stream lands in (#601).
    void setColorEnabled(bool enabled);
    bool colorEnabled() const;

    void log(LogLevel level, std::string_view tag, std::string_view message);
    void setSink(LogSink sink);

    // Move console writes onto a writer thread with a bounded queue, so a
    // client that never reads this process's standard error cannot stop it.
    // The MCP stdio transport says a server MAY write logs to stderr and a
    // client MAY ignore them, so a server that blocks when nobody reads is
    // depending on something it was not promised: at DEBUG the startup log is
    // several hundred lines, the pipe buffer filled, and the write blocked on
    // the thread that would have answered `initialize` (#689).
    //
    // One way, and only the standalone server turns it on. The extension keeps
    // writing inline, because a thread that is never joined inside a library
    // the engine can unload is a worse problem than the one this solves.
    void useBackgroundConsoleWriter();

    // Wait up to `timeout` for queued lines to reach stderr. True when the
    // queue drained, false when it did not -- which is the case this exists
    // for, so a caller on the way out does not wait on a reader that is gone.
    // A no-op returning true when the writer thread was never started.
    bool flushConsole(std::chrono::milliseconds timeout);

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
        std::ostringstream ss;
        (ss << ... << args);
        log(level, tag, ss.str());
    }

    std::atomic<LogLevel> m_level{LogLevel::Info};
    std::atomic<bool> m_color{false};
    mutable std::mutex m_mutex;
    LogSink m_sink;
};

#define DIDI_LOG_DEBUG(tag, ...) ::didi::Logger::instance().debug(tag, __VA_ARGS__)
#define DIDI_LOG_INFO(tag, ...)  ::didi::Logger::instance().info(tag, __VA_ARGS__)
#define DIDI_LOG_WARN(tag, ...)  ::didi::Logger::instance().warn(tag, __VA_ARGS__)
#define DIDI_LOG_ERROR(tag, ...) ::didi::Logger::instance().error(tag, __VA_ARGS__)

} // namespace didi
