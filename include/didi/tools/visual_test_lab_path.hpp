#pragma once

#include <string_view>

namespace didi::tools {

// The visual test lab is written to one fixed path rather than to a path the
// caller names, so the writer and the mutation gate have to agree on it. They
// were separate string literals in separate files until the gate started
// reading the target to decide whether an overwrite destroys anything (#425),
// at which point a drift between them would have meant gating the wrong file.
//
// At the project root, not under addons/didi. The addon folder is the one a
// user's installed plugin lives in, a project without the addon had the folder
// invented for it, and project_audit_assets excludes res://addons/ from orphan
// checks, so a generated scene put there was a generated scene nothing would
// ever report as unused (#564).
inline constexpr std::string_view kVisualTestLabScenePath = "res://didi_test_lab.tscn";
inline constexpr std::string_view kVisualTestLabDiskPath = "didi_test_lab.tscn";

} // namespace didi::tools
