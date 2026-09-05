#pragma once

#include "didi/common/types.hpp"

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace didi::offline {

inline constexpr char kOfflineHelperEnvironment[] = "DIDI_OFFLINE_HELPER";

// Tells a Godot this process launches not to publish a runtime session.
//
// The extension starts an authenticated IPC session and writes a descriptor
// whenever it loads, which is right for an editor or a game someone is running
// and wrong for an engine Didi started to answer a question. A check or a
// sandbox run that published one would leave another session for the next
// discovery to find, and a run killed at its timeout would leave the descriptor
// behind for the tombstone reaper.
//
// The variable is inherited by the child, so it has to be set on this process.
// Scoped, so it is put back however the call ends, and serialised, because a
// process-wide variable set by one caller and cleared by another is a launch
// running without the isolation it asked for. Holding the lock for the whole
// scope means two isolated launches take turns rather than overlap.
class ScopedOfflineHelperEnvironment {
public:
    ScopedOfflineHelperEnvironment() : lock_(mutex()) {
        if (const auto* current = std::getenv(kOfflineHelperEnvironment)) previous_ = current;
#if defined(_WIN32)
        ready_ = _putenv_s(kOfflineHelperEnvironment, "1") == 0;
#else
        ready_ = setenv(kOfflineHelperEnvironment, "1", 1) == 0;
#endif
    }
    ScopedOfflineHelperEnvironment(const ScopedOfflineHelperEnvironment&) = delete;
    ScopedOfflineHelperEnvironment& operator=(const ScopedOfflineHelperEnvironment&) = delete;

    ~ScopedOfflineHelperEnvironment() {
        if (!ready_) return;
#if defined(_WIN32)
        (void)_putenv_s(kOfflineHelperEnvironment,
                        previous_.has_value() ? previous_->c_str() : "");
#else
        if (previous_.has_value()) (void)setenv(kOfflineHelperEnvironment, previous_->c_str(), 1);
        else (void)unsetenv(kOfflineHelperEnvironment);
#endif
    }

    bool ready() const { return ready_; }

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }

    std::unique_lock<std::mutex> lock_;
    std::optional<std::string> previous_;
    bool ready_{false};
};

struct DomainDiagnostic {
    std::string severity;
    std::string code;
    std::string message;
    std::string path;
    int line{0};
    int column{0};

    json toJson() const;
};

std::vector<DomainDiagnostic> parseMsBuildDiagnostics(const std::string& output);
std::vector<DomainDiagnostic> parseGodotDiagnostics(const std::string& output);
std::vector<json> parseExportPresets(const std::string& contents);
std::vector<std::string> isolatedGodotArguments(std::vector<std::string> arguments);

} // namespace didi::offline
