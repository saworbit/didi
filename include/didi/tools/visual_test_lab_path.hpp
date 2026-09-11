#pragma once

#include <string_view>

namespace didi::tools {

// The visual test lab is written to one fixed path rather than to a path the
// caller names, so the writer and the mutation gate have to agree on it. They
// were separate string literals in separate files until the gate started
// reading the target to decide whether an overwrite destroys anything (#425),
// at which point a drift between them would have meant gating the wrong file.
inline constexpr std::string_view kVisualTestLabScenePath =
    "res://addons/didi/test_lab_sandbox.tscn";
inline constexpr std::string_view kVisualTestLabDiskPath =
    "addons/didi/test_lab_sandbox.tscn";

} // namespace didi::tools
