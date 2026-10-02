#pragma once

#include "didi/common/types.hpp"
#include "didi/setup/build_identity.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace didi::setup {

inline constexpr const char* kPluginConfigPath = "res://addons/didi/plugin.cfg";

// The library the engine loads on this platform, by the name didi.gdextension
// gives it.
const char* extensionLibraryName();

// What one addon folder holds, judged from the filesystem alone.
struct AddonInfo {
    std::filesystem::path directory;
    // didi.gdextension is there. The folder alone is not an addon: a folder
    // someone half-copied is not an installed addon (control room, #388).
    bool present{false};
    bool library_present{false};
    std::optional<BuildId> build;
    // Why there is no build when the library is there.
    std::string build_problem;
};

AddonInfo inspectAddon(const std::filesystem::path& directory);

// The first file `source` holds that `target` lacks or holds different bytes
// for, or nothing when every one matches. Files only the target has are left out
// of the question: the engine writes some of its own beside the addon's (the `~`
// copy of a reloadable library, .uid sidecars it mints).
std::optional<std::string> addonDifference(const std::filesystem::path& source,
                                           const std::filesystem::path& target);

// Where the addon that came with this binary is.
//
// Beside it in a build tree (build/addons/didi), one level up in a release
// archive (bin/didi beside addons/didi), and under the library directory of a
// `cmake --install` prefix. The repository's own addons/didi is never one: it is
// the manifest the build assembles from, and its library is whatever someone
// last dropped there (#325).
struct BundledAddonSearch {
    std::optional<std::filesystem::path> found;
    std::vector<std::filesystem::path> searched;
};
BundledAddonSearch findBundledAddon(const std::filesystem::path& executable_directory);

enum class AddonAction { Installed, Unchanged, Replaced, Refused };

struct AddonInstallReport {
    AddonAction action{AddonAction::Refused};
    std::string detail;
    std::optional<BuildId> previous;
};

// Puts `source` at <project>/addons/didi.
//
// A folder that holds the same build, byte for byte, is left alone. One from an
// older build, or one that is incomplete, is replaced. One from a newer build,
// or a build that cannot be ordered, is replaced only with `replace_newer`, and
// the refusal names both builds either way: an upgrade that silently became a
// downgrade is the failure this exists to prevent.
//
// The new folder is copied beside the old one under a hidden name and swapped
// in by rename, so a copy that fails part way leaves the installed addon as it
// was. On Windows a running editor holds the library, the rename of the old
// folder fails, and the answer says to close the editor.
Result<AddonInstallReport> installAddon(const std::filesystem::path& project_root,
                                        const AddonInfo& source, bool replace_newer);

// The plugin list project.godot holds, or nothing when the value is not a list
// of strings this can rewrite without losing anything.
std::optional<std::vector<std::string>> parsePluginList(const std::string& literal);
std::string pluginListLiteral(const std::vector<std::string>& plugins);

struct PluginEnableReport {
    bool changed{false};
    std::string previous_literal;
    std::string literal;
};

// Adds res://addons/didi/plugin.cfg to editor_plugins/enabled, keeping every
// plugin already there, in the PackedStringArray form the editor writes.
Result<PluginEnableReport> enablePlugin(const std::filesystem::path& project_root);

// Whether project.godot lists the plugin, read the same way.
bool pluginEnabled(const std::filesystem::path& project_root);

}  // namespace didi::setup
