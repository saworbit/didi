#pragma once

#include "didi/common/types.hpp"

#include <optional>
#include <string>
#include <string_view>

// The two line rules every Godot ConfigFile shares.
//
// project.godot, export_presets.cfg and every .import file are ConfigFiles, and
// Didi reads all of them offline. Each reader carried its own idea of what a
// comment is and what a section header is, and each idea drifted from the engine
// in its own direction: #810 was a `#` line skipped as a comment when the engine
// registers it as a setting, #809 was `[ application ]` read as a section nobody
// names. Both are the same mistake as #802, which is why assignsKey is shared
// next to writeProjectSetting.
//
// Every claim below was checked against Godot 4.5.1, 4.6.2 and 4.7.2.
namespace didi::config_file {

// Whether one line is a comment.
//
// `;` starts one. `#` does not. Under [autoload], `# Hash="*res://a.gd"`
// registers the setting `autoload/#Hash`, and the script it names enters the
// tree on every run -- confirmed on all three lines. `#` is GDScript's comment
// character, and a ConfigFile is not GDScript, so a user who disables an
// autoload the habitual way has not disabled it (#810).
inline bool isComment(std::string_view line) {
    const auto text = strings::trim(line);
    return !text.empty() && text.front() == ';';
}

// The section one `[...]` header declares, or nothing when the line is not a
// header.
//
// The name is the text between the brackets, trimmed. Godot reads
// `[ application ]`, `[application ]` and a tab-padded header as `application`,
// and merges a second spelling into the same section -- confirmed on all three
// lines. A reader that compares the untrimmed text sees a section nothing is
// under, which is a setting reported as absent and a duplicate section appended
// below it (#809).
inline std::optional<std::string> sectionName(std::string_view line) {
    const auto text = strings::trim(line);
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') return std::nullopt;
    return strings::trim(std::string_view(text).substr(1, text.size() - 2));
}

} // namespace didi::config_file
