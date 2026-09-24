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
    // Which of the seven causes set `malformed`, as a stable token, and a
    // sentence saying what was found. One refusal covered all six, and for two
    // of them the line was computed and thrown away, so "somewhere in this
    // file" was the whole search on a file where `[preset.0.options]` alone
    // runs to forty keys (#828). Empty when the file parsed.
    std::string reason;
    std::string detail;
    // The line the cause is on, where the cause has one. Zero for a cause that
    // is about the file as a whole or about a preset rather than a line.
    int line{0};
    // The first [preset.N] number the engine finds missing, which is where it
    // stops reading and where the next preset has to go. Zero for an empty
    // file; meaningless when malformed is set.
    int first_missing_index{0};
    // Every section header the file declares, [preset.N.options] and anything
    // else included, as the engine names them.
    std::vector<std::string> section_names;
};

ExportPresetsFile readExportPresets(const std::string& contents);

// The sentence and the payload for a presets file that is there and could not
// be parsed. Both refusal sites word it the same way, because it is one state
// with one code: project_list_export_presets and project_export used to give
// different answers to it, and project_export's sent the reader off to add a
// preset the file already declares.
std::string malformedPresetsMessage(const ExportPresetsFile& file);
json malformedPresetsData(const ExportPresetsFile& file);

// The export platforms Godot ships, spelled the way export_presets.cfg names
// them. The same seven on 4.5.1, 4.6.2 and 4.7.2, matched exactly. The editor
// skips a preset on any other name without a word, unless an editor plugin or
// a GDExtension registers a platform by exactly that name (#921).
const std::vector<std::string>& shippedExportPlatforms();

// The shipped platform a file most likely meant by `written`, when `written`
// is not one Godot reads: a difference of letter case, the OS name
// (`Windows`) or the Godot 3 name (`HTML5`). Empty when nothing is that close,
// and for `Linux/X11`, which the engine still reads as Linux.
std::optional<std::string> shippedPlatformFor(const std::string& written);

// The names Godot lists after "Invalid export preset name" when an export asks
// for a preset it did not detect. Empty when the output holds no such list.
std::vector<std::string> detectedPresetsInEngineOutput(const std::string& output);

// The name Godot says it could not find, from "Invalid export preset name: X.",
// or nothing when the output holds no such line. It is the name Godot read off
// its command line, which is not always the one it was handed.
std::optional<std::string> invalidPresetNameInEngineOutput(const std::string& output);

// A preset name as project_export hands it to Godot on its command line.
//
// Godot trims the whitespace from both ends of every argument, and then turns
// each %20 in it back into a space. So " Padded " arrived as "Padded", which it
// did not detect, and a name of spaces alone arrived empty, which started an
// editor that ran until the export timed out. A space sent as %20 survives both
// steps and comes back as the space. Measured on 4.5.1, 4.6.2 and 4.7.2 with
// tools/vibe/probes/export_preset_writer.py.
std::string presetNameForCommandLine(const std::string& name);

// Why Godot could never be asked for this preset on its command line, or
// nothing when it can. A literal %20 is turned into a space, and nothing on
// that command line escapes a percent sign.
std::optional<std::string> presetNameCommandLineProblem(const std::string& name);

// What Godot printed under "Cannot export project with preset ... due to
// configuration errors", one entry per problem. A missing export template is
// two lines in the output, the sentence and then the path, and is one entry
// here; its path is also in `missing_templates`. Empty when there is no such
// block.
struct ExportConfigurationErrors {
    std::vector<std::string> errors;
    std::vector<std::string> missing_templates;
};
ExportConfigurationErrors exportConfigurationErrors(const std::string& output);

// project_add_export_preset's refusal of a platform it does not write, with the
// spelling Godot uses when the one given is close to it. Nothing when
// `platform` is absent, not a string, or one of the seven. The registry asks
// this before the schema's own enum refusal, which can only list the seven.
std::optional<Error> exportPlatformRefusal(const json& args);

// The record for the preset project_export would hand to Godot, or the refusal
// it gives instead. Reads export_presets.cfg beneath the current project root.
//
// Exported so the confirmation preview asks the same question the call asks.
// Without it the preview returned the same clean answer in all five states the
// file can be in, and the call failed in every one, including for a preset name
// the sibling tool in the same process could prove does not exist (#652).
Result<json> findExportPreset(const std::string& preset);

// What project_add_export_preset appends, and the file it leaves.
struct ExportPresetAddition {
    int index{0};
    // Exactly the text appended, which is also what the dry run shows.
    std::string section_text;
    // The whole file after the append: the bytes that were there, then the
    // section. Nothing already in the file is rewritten.
    std::string contents;
    bool file_created{false};
    size_t presets_before{0};
};

// Plans one new export preset, or says why it cannot be added. `existing` is
// the file's text, or nothing when there is no file. `export_path` is already
// confined to the project and spelled relative to it, or empty.
//
// It writes the fewest keys that every supported engine loads without printing
// an ERROR, measured on 4.5.1, 4.6.2 and 4.7.2 and recorded in the
// project_add_export_preset amendment in docs/SURFACE_AMENDMENTS.md. The
// editor fills in the rest from its own defaults, which differ by line.
Result<ExportPresetAddition> planExportPresetAddition(const std::optional<std::string>& existing,
                                                      const std::string& name,
                                                      const std::string& platform,
                                                      const std::string& export_path);

// The same plan for the project in the current directory, from the arguments
// project_add_export_preset receives: it reads export_presets.cfg and confines
// export_path to the project. The call and its dry run both come through
// here, so a preview refuses what the call would.
Result<ExportPresetAddition> planExportPresetForProject(const json& args);

// The presets alone, empty when the file is malformed. Kept for callers that
// only need the list.
std::vector<json> parseExportPresets(const std::string& contents);
std::vector<std::string> isolatedGodotArguments(std::vector<std::string> arguments);

} // namespace didi::offline
