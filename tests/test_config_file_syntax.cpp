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

    registerTest("config_file_syntax.booleanize_reads_a_value_the_way_the_engine_does", [] {
        // Two readers had to reproduce this rule and only one of them had it
        // right. The engine parses the value and lets the Variant convert,
        // which is !is_zero(): a number decides on being zero, null is false,
        // and everything else the parser accepts is true.
        //
        // Measured on 4.5.1, 4.6.2 and 4.7.2, twice. Against
        // use_hidden_project_data_directory, where false, 0 and null move the
        // project data directory and 1 and "false" do not. Against runnable in
        // an export_presets.cfg loaded through ConfigFile, where true and 1
        // come back true and false, 0 and 0.0 come back false.
        struct Case { const char* value; bool expected; };
        const Case cases[] = {
            {"true", true},   {"false", false},
            {"1", true},      {"0", false},
            {"2", true},      {"0.0", false},
            {"-0.0", false},  {"-1", true},
            {"null", false},  {"nil", false},
            // A string is not zero, which is why the word false in quotes is
            // true. This is the case that catches a reader comparing text.
            {"\"false\"", true}, {"\"\"", true},
            // Whitespace around the value is not part of it.
            {"  false  ", false}, {"  1  ", true},
            // Containers and constructors are not zero either.
            {"[]", true}, {"{}", true}, {"Vector2(0, 0)", true},
        };
        for (const auto& item : cases) {
            if (didi::config_file::booleanize(item.value) != item.expected) {
                throw std::runtime_error(std::string("booleanize disagreed on: ") + item.value);
            }
        }
    });

    registerTest("config_file_syntax.booleanize_expects_the_parser_to_have_spoken_first", [] {
        // A value the parser will not start never reaches the engine's
        // conversion, because the whole file is ERR_PARSE_ERROR. So this does
        // not try to decide one, and the caller checks valueProblem first.
        // Asserted so the order stays deliberate: `maybe` is refused there, not
        // read as true here.
        if (didi::config_file::valueProblem("maybe").empty()) {
            throw std::runtime_error("a bare identifier stopped being a parse problem");
        }
        if (!didi::config_file::booleanize("maybe")) {
            throw std::runtime_error("booleanize started deciding values the parser refuses");
        }
    });

    registerTest("config_file_syntax.load_failure_names_the_line_to_repair", [] {
        // The pair every reader of a manifest has to ask about, asked once.
        // Three readers asked neither half and described a project the engine
        // will not open (#826).
        const auto clean = didi::config_file::scan(
            "config_version=5\n\n[application]\n\nconfig/name=\"Fine\"\n");
        if (didi::config_file::loadFailure(clean)) {
            throw std::runtime_error("a loadable manifest was called a failure");
        }

        // Balanced and still err 43. This is the half a reader that stops at
        // `complete` misses.
        const auto refused = didi::config_file::scan(
            "config_version=5\n\n[application]\n\nconfig/name=\"Fine\"\nconfig/broken=)\n");
        const auto value_failure = didi::config_file::loadFailure(refused);
        if (!value_failure) throw std::runtime_error("config/broken=) read as loadable");
        if (value_failure->unterminated) {
            throw std::runtime_error("a refused value was reported as an unterminated file");
        }
        if (value_failure->section != "application" || value_failure->key != "config/broken") {
            throw std::runtime_error("the failure named the wrong setting");
        }
        if (value_failure->line != 6) throw std::runtime_error("the failure named the wrong line");
        if (value_failure->value_reason.empty()) {
            throw std::runtime_error("the failure carried no reason");
        }

        // A file that ends part-way through a value, which is the other half.
        const auto unterminated =
            didi::config_file::scan("config_version=5\n\n[input]\n\njump={\"deadzone\": 0.5\n");
        const auto open_failure = didi::config_file::loadFailure(unterminated);
        if (!open_failure) throw std::runtime_error("an unterminated value read as loadable");
        if (!open_failure->unterminated) {
            throw std::runtime_error("an unterminated file was reported as a refused value");
        }
        if (open_failure->key != "jump" || open_failure->line != 5) {
            throw std::runtime_error("the unterminated failure named the wrong line");
        }

        // A file ending part-way through a *key* is the ordinary trailing note.
        // The engine drops the text and load() returns OK, so this is not a
        // failure and reporting it would refuse files that load.
        const auto trailing = didi::config_file::scan(
            "config_version=5\n\n[application]\n\nconfig/name=\"Fine\"\n# a note\n");
        if (!trailing.trailing_key) {
            throw std::runtime_error("the fixture stopped exercising a trailing key");
        }
        if (didi::config_file::loadFailure(trailing)) {
            throw std::runtime_error("a trailing note was called a load failure");
        }
    });

    registerTest("config_file_syntax.string_value_reads_escapes_the_way_the_engine_does", [] {
        // Every row was read through ConfigFile.load and through a .tres loaded
        // into AudioServer on 4.5.1, 4.6.2 and 4.7.2, and the two agreed
        // (tools/vibe/probes/config_string_escapes.py). Readers took the text
        // between the quotes, so each of these escapes came back as two
        // characters and a bus name no tool accepts (#934).
        struct Case { const char* value; const char* expected; };
        const Case cases[] = {
            {R"x(&"a\tb")x", "a\tb"},
            {R"x(&"a\nb")x", "a\nb"},
            {R"x(&"a\rb")x", "a\rb"},
            {R"x(&"a\bb")x", "a\bb"},
            {R"x(&"a\fb")x", "a\fb"},
            {R"x(&"a\"b")x", "a\"b"},
            {R"x(&"a\\b")x", "a\\b"},
            {R"x(&"a\'b")x", "a'b"},
            {R"x(&"a\/b")x", "a/b"},
            // Any other character after a backslash is itself.
            {R"x(&"a\qb")x", "aqb"},
            {R"x(&"a\0b")x", "a0b"},
            {R"x(&"a\ab")x", "aab"},
            {R"x(&"a\vb")x", "avb"},
            {R"x(&"a\x41b")x", "ax41b"},
            // An escape is a byte of what is decoded as UTF-8 when the string
            // closes, so two escapes make one character and one alone is not.
            {R"x(&"\u0041")x", "A"},
            {R"x(&"\u00c3\u00a9")x", "\xC3\xA9"},
            {R"x(&"\u00f0\u009f\u0098\u0080")x", "\xF0\x9F\x98\x80"},
            {R"x(&"\u007f")x", "\x7F"},
            {R"x(&"\u00e9")x", "\xEF\xBF\xBD"},
            {R"x(&"\u00E9")x", "\xEF\xBF\xBD"},
            {R"x(&"\u00ff")x", "\xEF\xBF\xBD"},
            // Above a byte is a space, and so is zero.
            {R"x(&"\u0100")x", " "},
            {R"x(&"\u97f3")x", " "},
            {R"x(&"\U01F600")x", " "},
            {R"x(&"\U110000")x", " "},
            {R"x(&"\uD83D\uDE00")x", " "},
            {R"x(&"a\u0000b")x", "a b"},
            // One replacement character for each byte that does not begin a
            // well-formed sequence, carrying on from the byte after it.
            {R"x(&"\u00e9\u00e9")x", "\xEF\xBF\xBD\xEF\xBF\xBD"},
            {R"x(&"\u00e9x")x", "\xEF\xBF\xBDx"},
            {R"x(&"\u00c3")x", "\xEF\xBF\xBD"},
            {R"x(&"\u00c3x")x", "\xEF\xBF\xBDx"},
            {R"x(&"a\u0080b")x", "a\xEF\xBF\xBD" "b"},
            {R"x(&"\u00c0\u0080")x", "\xEF\xBF\xBD\xEF\xBF\xBD"},
            {R"x(&"\u00ed\u00a0\u0080")x", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},
            {R"x(&"\u00f0\u009f\u0098")x", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},
            // A byte order mark is dropped where it leads the string only.
            {R"x(&"\u00ef\u00bb\u00bfX")x", "X"},
            {R"x(&"X\u00ef\u00bb\u00bfY")x", "X\xEF\xBB\xBFY"},
            // Raw text is read as written, a tab, a line break and UTF-8 too.
            {"&\"a\tb\"", "a\tb"},
            {"&\"a\nb\"", "a\nb"},
            {"&\"\xC3\x9Cn\xC3\xAF\"", "\xC3\x9Cn\xC3\xAF"},
            // A String, a StringName and a NodePath are read alike, and the
            // text around the value is not part of it.
            {R"x("a\tb")x", "a\tb"},
            {R"x(@"a\tb")x", "a\tb"},
            {R"x(  &"a\\"  )x", "a\\"},
        };
        for (const auto& item : cases) {
            const auto read = didi::config_file::stringValue(item.value);
            if (!read) throw std::runtime_error(std::string("not read as a string: ") + item.value);
            if (!read->problem.empty()) {
                throw std::runtime_error(std::string("refused a string the engine reads: ") +
                                         item.value + " -- " + read->problem);
            }
            if (read->text != item.expected) {
                throw std::runtime_error(std::string("read ") + item.value + " as " + read->text);
            }
            if (!didi::config_file::valueProblem(item.value).empty()) {
                throw std::runtime_error(std::string("valueProblem refused a string that loads: ") +
                                         item.value);
            }
        }

        // Not one string, so not this function's to answer.
        const char* others[] = {"1", "Music", "&Music", R"x("a" "b")x", R"x("a)x", R"x("a"x)x"};
        for (const auto* value : others) {
            if (didi::config_file::stringValue(value)) {
                throw std::runtime_error(std::string("read a value that is not one string: ") + value);
            }
        }
    });

    registerTest("config_file_syntax.string_escape_the_parser_cannot_read_fails_the_file", [] {
        // Each of these is err 43 for ConfigFile.load and a .tres that does not
        // load, on 4.5.1, 4.6.2 and 4.7.2. The tokenizer reads a string
        // wherever it is, so an array and a constructor's arguments fail the
        // file the same way, and an array with nothing wrong in it loads.
        const char* refused[] = {
            R"x(&"\uZZZZ")x", R"x(&"a\u00e")x", R"x(&"\U1F600")x",
            R"x(&"\uD83D")x", R"x(&"\uDE00")x", R"x(&"\uD83Dx")x",
        };
        for (const auto* value : refused) {
            const auto read = didi::config_file::stringValue(value);
            if (!read || read->problem.empty()) {
                throw std::runtime_error(std::string("read a string the engine refuses: ") + value);
            }
            if (didi::config_file::valueProblem(value).empty()) {
                throw std::runtime_error(std::string("valueProblem accepted: ") + value);
            }
        }
        if (didi::config_file::valueProblem(R"x(["ok", "\uZZZZ"])x").empty()) {
            throw std::runtime_error("a refused escape inside an array was accepted");
        }
        if (didi::config_file::valueProblem(R"x(PackedStringArray("\uD83D"))x").empty()) {
            throw std::runtime_error("a refused escape inside a constructor was accepted");
        }
        if (!didi::config_file::valueProblem(R"x(["ok", "fine"])x").empty()) {
            throw std::runtime_error("an array that loads was refused");
        }

        // The whole file, through the question every reader asks.
        const auto scanned = didi::config_file::scan("[s]\n\nk=&\"\\uZZZZ\"\n");
        const auto failure = didi::config_file::loadFailure(scanned);
        if (!failure || failure->key != "k" || failure->line != 3 || failure->value_reason.empty()) {
            throw std::runtime_error("loadFailure did not name the string the parser refuses");
        }
    });
    }
} registrar;

} // namespace
