#include "didi/common/config_file_syntax.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::config_file::scan;

// Every expectation below is what Godot's own ConfigFile.load answers for the
// same text, checked on 4.5.1, 4.6.2 and 4.7.2.
std::string keyAt(const std::string& text, size_t index) {
    const auto scanned = scan(text);
    if (index >= scanned.entries.size()) return "<missing>";
    return scanned.entries[index].section + "/" + scanned.entries[index].key;
}

struct Register {
    Register() {
    registerTest("config_file_syntax.key_drops_inner_whitespace", [] {
        ASSERT_EQ(keyAt("[application]\ncon fig/name=\"x\"\n", 0), "application/config/name");
        ASSERT_EQ(keyAt("[application]\nconfig / name=\"x\"\n", 0), "application/config/name");
        ASSERT_EQ(keyAt("[application]\nconfig\tname=\"x\"\n", 0), "application/configname");
    });

    registerTest("config_file_syntax.quoted_key_keeps_its_spaces", [] {
        // Godot writes an InputMap action that needs spaces in quotes, and the
        // quotes are not part of the name.
        ASSERT_EQ(keyAt("[input]\n\"my action\"={}\n", 0), "input/my action");
        ASSERT_EQ(keyAt("[input]\n\"a=b\"=1\n", 0), "input/a=b");
    });

    registerTest("config_file_syntax.bare_note_joins_into_the_key_below", [] {
        // The whole point of #813: the script still loads, under a name no
        // reference in the project uses.
        const auto scanned = scan("[autoload]\n\n# disabled for now\nGood=\"*res://good.gd\"\n");
        ASSERT_EQ(scanned.entries.size(), 1u);
        ASSERT_EQ(scanned.entries[0].key, "#disabledfornowGood");
        ASSERT_EQ(scanned.entries[0].section, "autoload");
        ASSERT_TRUE(scanned.entries[0].joined);
        ASSERT_EQ(scanned.entries[0].key_on_line, "Good");
        ASSERT_EQ(scanned.entries[0].key_line, 3);
        ASSERT_EQ(scanned.entries[0].line, 4);
    });

    registerTest("config_file_syntax.join_swallows_the_header_under_it", [] {
        const auto scanned = scan("# just a note\n[preset.0]\nname=\"a\"\n");
        ASSERT_EQ(scanned.entries.size(), 1u);
        ASSERT_EQ(scanned.entries[0].key, "#justanote[preset.0]name");
        ASSERT_TRUE(scanned.entries[0].section.empty());
        ASSERT_TRUE(scanned.headers.empty());
    });

    registerTest("config_file_syntax.trailing_note_is_harmless", [] {
        // No `=` anywhere after it, so the engine drops it and load() is OK.
        // Reporting this file as broken is the mistake #651 made.
        const auto scanned = scan("[preset.0]\nname=\"a\"\n# just a note\n");
        ASSERT_EQ(scanned.entries.size(), 1u);
        ASSERT_EQ(scanned.entries[0].key, "name");
        ASSERT_TRUE(scanned.complete);
    });

    registerTest("config_file_syntax.hash_with_an_equals_is_a_key", [] {
        const auto scanned = scan("[preset.0]\n# name=\"a\"\nplatform=\"W\"\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].key, "#name");
        ASSERT_EQ(scanned.entries[1].key, "platform");
        ASSERT_TRUE(!scanned.entries[1].joined);
    });

    registerTest("config_file_syntax.semicolon_is_still_a_comment", [] {
        const auto scanned = scan("[preset.0]\n# note\n; real comment\nname=\"a\"\n");
        ASSERT_EQ(scanned.entries.size(), 1u);
        ASSERT_EQ(scanned.entries[0].key, "#notename");
    });

    registerTest("config_file_syntax.quoted_token_replaces_the_key_so_far", [] {
        const auto scanned = scan("[preset.0]\n# note \"a=b\"\nname=\"a\"\n");
        ASSERT_EQ(scanned.entries.size(), 1u);
        ASSERT_EQ(scanned.entries[0].key, "a=bname");
    });

    registerTest("config_file_syntax.spaced_header_is_the_same_section", [] {
        ASSERT_EQ(keyAt("[ preset.0 ]\nname=\"a\"\n", 0), "preset.0/name");
        ASSERT_EQ(keyAt("[ preset.0.options ]\nfoo=1\n", 0), "preset.0.options/foo");
        ASSERT_EQ(keyAt("[ remap ]\npath=\"a\"\n", 0), "remap/path");
    });

    registerTest("config_file_syntax.multiline_value_is_not_a_key", [] {
        // Every [input] action Godot writes looks like this. Joining the lines
        // inside the dictionary would build a key out of the deadzone and lose
        // the action below it.
        const auto scanned = scan(
            "[input]\nmove_left={\n\"deadzone\": 0.5,\n\"events\": [Object(InputEventKey,"
            "\"keycode\":0)]\n}\nmove_right={\n\"deadzone\": 0.5\n}\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].key, "move_left");
        ASSERT_EQ(scanned.entries[0].line, 2);
        ASSERT_EQ(scanned.entries[0].value_end_line, 5);
        ASSERT_EQ(scanned.entries[1].key, "move_right");
        ASSERT_TRUE(scanned.complete);
    });

    registerTest("config_file_syntax.header_after_a_multiline_value", [] {
        const auto scanned = scan("[input]\nui_x={\n\"deadzone\": 0.5\n}\n[other]\nk=1\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[1].section, "other");
        ASSERT_EQ(scanned.headers.size(), 2u);
        ASSERT_EQ(scanned.headers[1].line, 5);
    });

    registerTest("config_file_syntax.value_left_open_does_not_load", [] {
        // Godot answers ERR_PARSE_ERROR and loads none of it.
        const auto scanned = scan("[input]\nui_x={\n\"deadzone\": 0.5\n[other]\nk=1\n");
        ASSERT_TRUE(!scanned.complete);
    });

    registerTest("config_file_syntax.value_can_start_on_the_next_line", [] {
        const auto scanned = scan("[preset.0]\nname=\n\"a\"\nplatform=\"W\"\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].key, "name");
        ASSERT_EQ(scanned.entries[0].value_end_line, 3);
        ASSERT_EQ(scanned.entries[1].key, "platform");
    });

    registerTest("config_file_syntax.string_and_brackets_inside_a_value", [] {
        const auto scanned = scan("[preset.0]\nname=\"{\"\nplatform=\"a\\\"b\"\nrunnable=true\n");
        ASSERT_EQ(scanned.entries.size(), 3u);
        ASSERT_EQ(scanned.entries[2].key, "runnable");
        ASSERT_TRUE(scanned.complete);
    });

    registerTest("config_file_syntax.last_spelling_of_a_key_wins", [] {
        const auto scanned = scan("[application]\nconfig / name=\"first\"\nconfig/name=\"second\"\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].key, "config/name");
        ASSERT_EQ(scanned.entries[1].key, "config/name");
        ASSERT_EQ(scanned.entries[1].value_text, "\"second\"");
    });

    registerTest("config_file_syntax.carriage_returns_are_not_key_text", [] {
        const auto scanned = scan("[preset.0]\r\n# note\r\nname=\"a\"\r\n");
        ASSERT_EQ(scanned.entries.size(), 1u);
        ASSERT_EQ(scanned.entries[0].key, "#notename");
    });

    registerTest("config_file_syntax.trailing_semicolon_is_not_value_text", [] {
        const auto scanned = scan("[preset.0]\nname=\"a\" ; note\nplatform=\"W\"\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[1].key, "platform");
        // The banner Godot writes at the top of every project.godot documents
        // `param=value ; comment`, and ConfigFile.load gives name the string a.
        // Carrying the note into the value made the preset name unquotable, so
        // project_list_export_presets published it with the quotes still on.
        ASSERT_EQ(scanned.entries[0].value_text, "\"a\"");
    });

    registerTest("config_file_syntax.value_text_spans_every_line_it_covers", [] {
        // ProjectSettings.save_custom writes a dictionary over four lines. The
        // first character of it is what project_set_setting's dry run used to
        // show as the value being replaced (#816).
        const auto scanned =
            scan("[shader_globals]\ntint={\n\"type\": \"color\",\n\"value\": Color(1, 1, 1, 1)"
                 "\n}\nother=1\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].value_text,
                  "{\n\"type\": \"color\",\n\"value\": Color(1, 1, 1, 1)\n}");
        ASSERT_EQ(scanned.entries[0].value_end_line, 5);
        ASSERT_EQ(scanned.entries[1].value_text, "1");
    });

    registerTest("config_file_syntax.semicolon_is_a_comment_inside_a_value", [] {
        // Checked on all three lines: the dictionary loads as {"x": 1}, so the
        // note is not part of it even though the value has not closed yet.
        const auto scanned = scan("[s]\nd={\n\"x\": 1 ; note\n}\nb=3\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].value_text, "{\n\"x\": 1\n}");
        ASSERT_EQ(scanned.entries[1].key, "b");
    });

    registerTest("config_file_syntax.value_ending_does_not_end_the_line", [] {
        // `name="a" # note` registers `#noteplatform` for the line below, the
        // same forward join a `# note` line of its own makes. A `#` comment is
        // the habitual way to write one, so this is the likeliest way a project
        // ends up with a setting nobody can name.
        const auto joined = scan("[preset.0]\nname=\"a\" # note\nplatform=\"W\"\n");
        ASSERT_EQ(joined.entries.size(), 2u);
        ASSERT_EQ(joined.entries[1].key, "#noteplatform");
        ASSERT_EQ(joined.entries[1].key_on_line, "platform");
        ASSERT_TRUE(joined.entries[1].joined);
        ASSERT_EQ(joined.entries[1].key_line, 2);

        // Two settings on one line, and a header opened on one.
        const auto pair = scan("[s]\na=1 b=2\nc=3\n");
        ASSERT_EQ(pair.entries.size(), 3u);
        ASSERT_EQ(pair.entries[1].key, "b");
        ASSERT_EQ(pair.entries[1].value_text, "2");

        const auto header = scan("[s]\nname=\"a\" [t]\nk=1\n");
        ASSERT_EQ(header.headers.size(), 2u);
        ASSERT_EQ(header.entries.size(), 2u);
        ASSERT_EQ(header.entries[1].section, "t");
    });

    registerTest("config_file_syntax.a_string_value_keeps_its_own_semicolon", [] {
        const auto scanned = scan("[s]\nname=\"a;b\"\nb=3\n");
        ASSERT_EQ(scanned.entries.size(), 2u);
        ASSERT_EQ(scanned.entries[0].value_text, "\"a;b\"");
    });

    registerTest("config_file_syntax.an_identifier_reaches_its_call", [] {
        // `Vector2 (1, 2)` is one Vector2 on 4.5.1, 4.6.2 and 4.7.2, so a
        // reader that ended the value at the space built the key `(1,2)b` out
        // of what followed and reported a setting the engine does not have.
        const auto spaced = scan("[s]\na=Vector2 (1, 2)\nb=3\n");
        ASSERT_EQ(spaced.entries.size(), 2u);
        ASSERT_EQ(spaced.entries[0].value_text, "Vector2 (1, 2)");
        ASSERT_EQ(spaced.entries[1].key, "b");

        // The engine skips a line break between the two just as readily.
        const auto broken = scan("[s]\na=Vector2\n(1, 2)\nb=3\n");
        ASSERT_EQ(broken.entries.size(), 2u);
        ASSERT_EQ(broken.entries[0].value_text, "Vector2\n(1, 2)");
        ASSERT_EQ(broken.entries[0].value_end_line, 3);
        ASSERT_EQ(broken.entries[1].key, "b");
        ASSERT_EQ(broken.entries[1].line, 4);

        // The `[int]` of a typed container is its type, not an array, so the
        // value carries on to the call after the `]`.
        const auto typed = scan("[s]\na=Array[int]([1, 2])\nb=3\n");
        ASSERT_EQ(typed.entries.size(), 2u);
        ASSERT_EQ(typed.entries[0].value_text, "Array[int]([1, 2])");
        ASSERT_EQ(typed.entries[1].key, "b");

        // A keyword is a value on its own, and the key after it is its own
        // key. This is the case the lookahead must not swallow.
        const auto keyword = scan("[s]\na=true b=3\n");
        ASSERT_EQ(keyword.entries.size(), 2u);
        ASSERT_EQ(keyword.entries[0].value_text, "true");
        ASSERT_EQ(keyword.entries[1].key, "b");
        ASSERT_EQ(keyword.entries[1].value_text, "3");

        // An identifier is the last thing in the file. Nothing follows it, so
        // it was the whole value and the file is complete.
        const auto last = scan("[s]\na=true\n");
        ASSERT_TRUE(last.complete);
        ASSERT_EQ(last.entries.size(), 1u);
        ASSERT_EQ(last.entries[0].value_text, "true");
        ASSERT_EQ(last.entries[0].value_end_line, 2);

        // A keyword is a whole value, so the parser returns on it and never
        // looks for a call. The section below it is a section, and the blank
        // line between them changes nothing. Godot's own writer ends a section
        // with a bool often enough that missing this loses the rest of the
        // file: `[application]` here came back as part of the value above it.
        const auto section = scan("[a]\nflag=true\n\n[application]\n\nconfig/name=\"x\"\n");
        ASSERT_EQ(section.headers.size(), 2u);
        ASSERT_EQ(section.entries.size(), 2u);
        ASSERT_EQ(section.entries[0].value_text, "true");
        ASSERT_EQ(section.entries[1].section, "application");
        ASSERT_EQ(section.entries[1].key, "config/name");
        ASSERT_EQ(section.entries[1].line, 6);
    });

    registerTest("config_file_syntax.value_problem_reports_what_the_engine_refuses", [] {
        // Every string here was put to ConfigFile.load on 4.5.1, 4.6.2 and
        // 4.7.2 as `a=<value>`, and all three answered err 43 for it.
        const char* refused[] = {
            "",        ")",        "]",           "}",        ",1",
            ":",       "([1])",    "nonsense",    "True",     "False",
            "Null",    "NAN",      "Vector2.ZERO", "&sn",     "@foo",
            "[,]",     "{,}",      "[,1]",        "[1,,]",    "[1,,2]",
            "{\"a\": 1,, \"b\": 2}",              "[1 2]",    "{1 2}",
            "{\"a\" 1}",           "{\"a\":}",    "{\"a\":1 \"b\":2}",
        };
        for (const auto* value : refused) {
            if (didi::config_file::valueProblem(value).empty()) {
                throw std::runtime_error(std::string("not reported as broken: ") + value);
            }
        }
    });

    registerTest("config_file_syntax.value_problem_passes_what_the_engine_loads", [] {
        // The same measurement the other way: all three lines answered err 0
        // for every one of these, so reporting any of them would be refusing a
        // file that loads.
        const char* accepted[] = {
            "1", "-1", "1.5", "1.0e10", "-1e-3", "1e3", "-0", "0x1F",
            "\"\"", "\"str\"", "&\"sn\"", "@\"foo\"", "#ff0000", "#zzz",
            "true", "false", "null", "nil", "nan", "inf", "inf_neg", "-inf",
            "[]", "{}", "{ }", "[1, 2]", "[1,]", "[\"a\", \"b\",]",
            "{\"a\": 1}", "{\"a\":1,}", "{1: 2}", "{Vector2(0, 0): 1}",
            "{\"a\": {\"b\": 1}}", "[[1], [2]]", "[true, null, nan]",
            "Vector2(1, 2)", "Vector2 (1, 2)", "Vector2\n(1, 2)", "Vector2i(1, 2)",
            "Color(1, 1, 1, 1)", "Rect2(0, 0, 1, 1)", "Transform2D(1, 0, 0, 1, 0, 0)",
            "NodePath(\"a/b\")", "PackedStringArray(\"4.5\")", "PackedInt32Array(1, 2)",
            "PackedVector2Array(0, 0, 512, 0)",
            "Object(Resource,\"resource_local_to_scene\":false)",
            "[Object(Resource,\"resource_local_to_scene\":false)]",
            "Array[int]([1, 2])", "Array [int]([1, 2])",
            "Dictionary[String, int]({\"a\": 1})",
            "{\"deadzone\": 0.5, \"events\": [Object(InputEventKey,\"resource_local_to_scene\":false)]}",
        };
        for (const auto* value : accepted) {
            const auto problem = didi::config_file::valueProblem(value);
            if (!problem.empty()) {
                throw std::runtime_error(std::string("refused a value that loads: ") + value +
                                         " -- " + problem);
            }
        }
    });

    registerTest("config_file_syntax.value_problem_stays_inside_what_it_can_prove", [] {
        // These are err 43 too and none of them is reported, because deciding
        // them needs the engine's own constructor tables, number grammar and
        // resource loader. The boundary is written down here so a later reader
        // knows it was chosen rather than missed: an empty answer is not a
        // promise that the file loads.
        const char* unproven[] = {
            "Nonsense(1)", "Vector2(1)", "Vector2(1,)", "Vector2(1,2,3)",
            "+1", ".5", "-.5", "^\"np\"", "Resource(\"res://missing.tres\")",
        };
        for (const auto* value : unproven) {
            const auto problem = didi::config_file::valueProblem(value);
            if (!problem.empty()) {
                throw std::runtime_error(std::string("claimed more than it can prove: ") + value +
                                         " -- " + problem);
            }
        }
    });
    }
} registrar;

} // namespace
