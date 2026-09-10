#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <filesystem>
#include <string>

namespace didi::offline {

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
