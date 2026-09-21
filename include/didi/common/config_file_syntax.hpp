#pragma once

#include "didi/common/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The line rules every Godot ConfigFile shares.
//
// project.godot, export_presets.cfg and every .import file are ConfigFiles, and
// Didi reads all of them offline. Each reader carried its own idea of what a
// comment is, what a section header is and what a key is, and each idea drifted
// from the engine in its own direction: #810 was a `#` line skipped as a comment
// when the engine registers it as a setting, #809 was `[ application ]` read as
// a section nobody names, #813 was the key itself read as the text before the
// first `=` on one line when the engine builds it from a run of tokens that can
// cross lines.
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

// One key the engine registers, and where it came from.
struct Entry {
    std::string section;      // the section in force; empty before the first header
    std::string key;          // the name the engine registers, not the text on the line
    // The value the engine reads, as text: every line it spans, trimmed and
    // joined with a newline, with the `;` comment on each line dropped. Not
    // the rest of the line after the `=`, which is one character for a
    // dictionary and carries the note in `name="a" ; note`.
    std::string value_text;
    // What the line holding the `=` contributed to the key on its own, which is
    // the whole key unless an earlier line joined into it. It is the name the
    // file looks like it declares where `key` is the one the engine registers.
    std::string key_on_line;
    int key_line{0};          // 1-based line the key's first token is on
    int line{0};              // 1-based line holding the `=`
    int value_end_line{0};    // 1-based last line of the value
    bool joined{false};       // the key was built from more than one line
};

// One header the engine honours.
struct Header {
    std::string name;
    int line{0};              // 1-based
};

struct Scan {
    std::vector<Entry> entries;
    std::vector<Header> headers;
    // False when the file ends inside a value. Godot answers ERR_PARSE_ERROR
    // for that file and loads none of it, so nothing above is what the project
    // runs on.
    bool complete{true};
    // True when the file ends part-way through a key, with no `=` after it. The
    // engine drops that text and still returns OK, so this is not a broken
    // file on its own: a `# note` under the last setting is the ordinary case.
    // It is the only trace left of text the engine ignored, which is what tells
    // a file with a trailing note apart from a file that is not a ConfigFile.
    bool trailing_key{false};
};

// Walks one ConfigFile's text the way Godot's parser walks it.
//
// Three rules a line-at-a-time reader does not have:
//
//   * Whitespace inside a key is not part of the key. `config / name`,
//     `con fig/name` and `config/name` are one setting, and the last spelling
//     in the file wins. A quoted key keeps its spaces: `"my action"` is the
//     action `my action`, which is how Godot writes an InputMap name that needs
//     them.
//   * A line with no `=` does not end the key. It joins forward into the next
//     line that has one, and it swallows any `[section]` header in between. A
//     `# disabled for now` note above an [autoload] entry loads that script
//     under the name `#disabledfornowGood`, so every reference to `Good` in the
//     project breaks and `autoload/Good` does not exist (#813). A note with no
//     `=` anywhere after it is harmless: the engine drops it and returns OK.
//   * A value can span lines. Every [input] action Godot writes is a dictionary
//     across four or more of them, and the lines inside it are value text, not
//     keys. A reader that joins those would build a key out of `"deadzone": 0.5`
//     and lose the action below it.
//   * A value ending does not end the line. The engine carries on reading from
//     where the value stopped, so `a=1 b=2` is two settings, `name="a" [t]`
//     opens the section `t`, and `name="a" # note` joins `#note` forward into
//     the key below exactly as a `# note` line of its own would (#816). A `;`
//     is the one thing that does comment out the rest of a line, at any
//     bracket depth, which is what the banner Godot writes at the top of every
//     project.godot documents.
//   * An identifier does not end a value. It is a value on its own only for
//     the seven names below; otherwise it names a constructor or a typed
//     container, and the parser skips whitespace -- including a line break --
//     to find the `(` or `[` that follows. `Vector2 (1, 2)`, `Vector2` with
//     `(1, 2)` on the line below, and `Array[int]([1, 2])` all load, so a
//     reader that ended the value at the space or at the `]` built a key out
//     of the rest and reported a setting the engine does not have.
//
// Section headers come from here too. The name is the text between the
// brackets, trimmed: Godot reads `[ application ]` and a tab-padded header as
// `application` and merges a second spelling into the same section, so a reader
// that compared the untrimmed line saw a section nothing was under (#809). A
// `[...]` line is only a header where a key could start, which is why this is
// the only place that decides it.
Scan scan(std::string_view text);

// Why Godot's parser refuses this value text, or empty when nothing about it
// proves that it does.
//
// `Scan::complete` counts brackets, because that is what the walk it falls out
// of can count. Godot parses a value, so a file that is bracket-balanced and
// still ERR_PARSE_ERROR was read, previewed and written as though it loaded
// (#820). `a=)`, `a=nonsense`, `a={1 2}` and `a=[1,,2]` are all err 43 on
// 4.5.1, 4.6.2 and 4.7.2, and all four are balanced.
//
// This is not a VariantParser and does not try to be one. It reports only what
// it can prove, and accepts everything else:
//
//   * A value that is empty, begins with a closer or a separator, or begins
//     with `(`. None of those is the start of any Variant.
//   * A bare identifier that is not `true`, `false`, `null`, `nil`, `nan`,
//     `inf` or `inf_neg` and is not followed by a `(` or a `[`. The match is
//     exact: `True`, `NAN` and `Vector2.ZERO` are all err 43.
//   * `&` or `@` not immediately followed by a quote. `&"a"` is a StringName
//     and `& "a"` is err 43.
//   * An array or dictionary whose elements are not separated the way the
//     parser requires. A trailing comma is allowed, `[1,,2]` and `{1 2}` are
//     not.
//
// A constructor with the wrong arity, one the engine does not know, and a
// `Resource(...)` whose file is missing are all err 43 and none is reported
// here, because deciding them needs the engine's own tables and a reader that
// guesses at those refuses files that load. Everything inside a constructor's
// argument list is skipped whole for the same reason:
// `Object(Resource,"resource_local_to_scene":false)` is a bare identifier
// followed by named arguments, which is a shape no other value has.
//
// So an empty answer is not a promise that Godot will load the value. A
// non-empty one is a promise that it will not.
std::string valueProblem(std::string_view value_text);

// Why Godot refuses to load this whole file, or nothing when the scan proves
// no such thing.
//
// `complete` and `valueProblem` are the two halves of one question -- will the
// engine load this file -- and every reader of one has to ask the other. Four
// of them grew their own copy of the pair and three of those asked neither, so
// project_analyze_impact reported an [autoload] as a live dependency out of a
// manifest the audit in the same session called unloadable (#826). The question
// is asked once here.
//
// Measured on 4.5.1 and 4.7.2 with `Good="*res://good.gd"` above a broken
// value and below it: the project does not open either way. `--headless
// --path` falls through to the project manager, nothing runs, and the
// singleton never enters the tree. `ProjectSettings.has_setting` answers true
// for an entry above the break under `--script`, which is a session with no
// project open rather than a project that registered it.
//
// The first failure, not all of them. A caller that means to list every one --
// project_audit_assets is the only one -- walks the scan itself, because the
// remedy is per line and the audit is where a user goes for the list.
struct LoadFailure {
    // The file ends part-way through a value. `section`, `key` and `line` name
    // the last key read, which is the one whose value never closed, and are
    // empty when the file holds no key at all.
    bool unterminated{false};
    std::string section;
    std::string key;
    int line{0};
    // valueProblem's sentence for the value on `line`. Empty when the file is
    // unterminated, which is the other way a file fails to load.
    std::string value_reason;
};

std::optional<LoadFailure> loadFailure(const Scan& scanned);

// What a value means where the engine wants a bool.
//
// Godot does not require the words `true` and `false`. It parses the value into
// a Variant and lets it convert, and that conversion is `booleanize()`, which is
// `!is_zero()`. So a number decides on being zero, `null` is false, and
// everything else the parser accepts is true.
//
// Measured on 4.5.1, 4.6.2 and 4.7.2, twice over. Against
// `application/config/use_hidden_project_data_directory`: `false`, `0` and
// `null` move the project data directory and `1` and `"false"` do not. Against
// `runnable` in an `export_presets.cfg` loaded through `ConfigFile`: `true` and
// `1` come back true, `false`, `0` and `0.0` come back false.
//
// A string was not measurable from GDScript, where `bool("true")` is not a call
// the language allows, so the claim for it rests on `is_zero()` having no case
// for STRING rather than on a reading. That is the same footing as
// `valueProblem` above: this reports what it can prove and treats everything
// else the way the engine's own rule says, rather than refusing a value that
// loads.
//
// The caller is expected to have checked `valueProblem` first. A value the
// parser will not start never reaches the engine's conversion at all, because
// the whole file is ERR_PARSE_ERROR.
bool booleanize(std::string_view value_text);

} // namespace didi::config_file
