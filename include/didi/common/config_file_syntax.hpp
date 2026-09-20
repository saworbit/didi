#pragma once

#include "didi/common/types.hpp"

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
//
// Section headers come from here too. The name is the text between the
// brackets, trimmed: Godot reads `[ application ]` and a tab-padded header as
// `application` and merges a second spelling into the same section, so a reader
// that compared the untrimmed line saw a section nothing was under (#809). A
// `[...]` line is only a header where a key could start, which is why this is
// the only place that decides it.
Scan scan(std::string_view text);

} // namespace didi::config_file
