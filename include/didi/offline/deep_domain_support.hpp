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

// How many projects MSBuild says it produced an assembly for.
//
// dotnet exits 0 for a solution that builds nothing -- one whose project paths
// do not resolve, and one MSBuild's solution parser skips because it carries no
// configuration mapping -- so the tool reported success: true for code it never
// looked at (#706). MSBuild prints one "Name -> path" line per project it
// builds, on a fresh build and on an up-to-date one alike, and the line is not
// localised. Zero of them beside exit code 0 is the state that has no other
// name.
int parseMsBuildProjectOutputCount(const std::string& output);
std::vector<DomainDiagnostic> parseGodotDiagnostics(const std::string& output);
// What export_presets.cfg holds, with the states a caller has to tell apart
// kept apart.
//
// parseExportPresets returns an empty list for a file with no preset sections
// and for one it could not parse, and the tools above it merged those with
// "the file is not there" into one sentence with an "or" in it: a project that
// has never configured an export was an error, and so was a file whose bytes
// are unreadable, and project_export answered both with "Export preset not
// found", sending a caller to add a preset that is already there (#651).
struct ExportPresetsFile {
    std::vector<json> presets;
    // A line, a value or a name this could not make sense of. The presets
    // vector is empty when this is set, because a partially understood export
    // configuration is not one to act on.
    bool malformed{false};
    // How many [preset.N] sections the file declared, whether or not they
    // parsed. Zero with malformed false is a project with no export presets,
    // which is the same fact as having no file at all.
    size_t section_count{0};
};

ExportPresetsFile readExportPresets(const std::string& contents);

// The refusal project_export gives for this preset, or nothing when the file
// declares it. Reads export_presets.cfg beneath the current project root.
//
// Exported so the confirmation preview asks the same question the call asks.
// Without it the preview returned the same clean answer in all five states the
// file can be in, and the call failed in every one, including for a preset name
// the sibling tool in the same process could prove does not exist (#652).
std::optional<Error> checkExportPreset(const std::string& preset);

// The presets alone, empty when the file is malformed. Kept for callers that
// only need the list.
std::vector<json> parseExportPresets(const std::string& contents);
std::vector<std::string> isolatedGodotArguments(std::vector<std::string> arguments);

} // namespace didi::offline
