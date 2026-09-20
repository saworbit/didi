#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <filesystem>
#include <string>

namespace didi::offline {

// Whether one project.godot line assigns the named key, whatever spacing it
// uses. Godot's ConfigFile writer emits `key=value`, but `key = value` and a
// tabbed form register exactly the same setting -- checked on 4.5.1, 4.6.2 and
// 4.7.2 -- so a hand-edited file is still a file Didi has to read and update
// rather than treat as a file with no such key (#802).
//
// Shared rather than copied. project_analyze_impact had its own prefix match
// and answered impact_count: 0 for a spaced [autoload] entry, which is the
// answer that tool uses to mean safe.
bool assignsKey(const std::string& line, const std::string& key);

// What one offline write did to project.godot.
//
// The caller reports these rather than only "persisted", because an offline
// write has no engine to confirm it against. The literal that went into the
// file, and the one it replaced, are the evidence.
struct ProjectSettingWrite {
    std::string setting;
    std::string section;
    std::string key;
    std::string literal;           // what was written; empty for a removal
    std::string previous_literal;  // what was there; empty when nothing was
    bool existed{false};
    bool removed{false};
    bool section_created{false};
};

// Renders one JSON value as the Godot literal that ProjectSettings.save()
// writes for the Variant the live path builds from the same JSON.
//
// The reach is deliberately the reach of the live converter and no wider:
// null, booleans, integers, finite reals, strings, arrays and string-keyed
// dictionaries, nested at most 16 levels. A caller that gets a value into
// project.godot offline gets the same bytes it would have got live, so nothing
// depends on which mode ran.
Result<std::string> settingLiteral(const json& value, int depth = 0);

// What project.godot holds for one setting right now, without writing.
//
// writeProjectSetting reports previous_literal, but only by writing. A dry-run
// preview has to answer "what is there now" without changing anything, which is
// the difference between a preview that shows the change and one that echoes
// the arguments back (#417). An absent setting is not an error: not being there
// is a real answer, reported as existed = false.
struct ProjectSettingRead {
    std::string setting;
    std::string literal;
    bool existed{false};
};

Result<ProjectSettingRead> readProjectSetting(const std::filesystem::path& project_root,
                                              const std::string& setting);

// Persists or removes one setting in project.godot with no engine running.
//
// project.godot is a Godot ConfigFile: the first segment of a ProjectSettings
// name is the section header and the rest is the key, so
// editor_plugins/enabled is `enabled` under `[editor_plugins]`. Everything the
// file already holds is preserved byte for byte apart from the one line this
// touches.
Result<ProjectSettingWrite> writeProjectSetting(const std::filesystem::path& project_root,
                                                const std::string& setting,
                                                const json& value,
                                                bool remove);

} // namespace didi::offline
