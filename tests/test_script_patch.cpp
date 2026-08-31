#include "didi/offline/gdscript_diagnostics.hpp"
#include "didi/offline/class_reference.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : m_name(std::move(name)) {
        if (const char* value = std::getenv(m_name.c_str())) m_original = value;
    }

    ~ScopedEnvironmentVariable() {
#if defined(_WIN32)
        _putenv_s(m_name.c_str(), m_original ? m_original->c_str() : "");
#else
        if (m_original) setenv(m_name.c_str(), m_original->c_str(), 1);
        else unsetenv(m_name.c_str());
#endif
    }

    void set(const std::string& value) const {
#if defined(_WIN32)
        _putenv_s(m_name.c_str(), value.c_str());
#else
        setenv(m_name.c_str(), value.c_str(), 1);
#endif
    }

private:
    std::string m_name;
    std::optional<std::string> m_original;
};

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(std::string prefix)
        : m_path(std::filesystem::temp_directory_path() /
                 (std::move(prefix) + std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_path);
    }
    ~ScopedTempDirectory() { std::filesystem::remove_all(m_path); }
    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

static void test_gdscript_diagnostics_deprecation() {
    std::string bad_script =
        "extends CharacterBody3D\n"
        "export(int) var speed = 10\n"
        "onready var sprite = $Sprite\n"
        "func test():\n"
        "\tyield(get_tree(), \"idle_frame\")\n";

    auto diags = didi::offline::GDScriptDiagnostics::analyze("", bad_script);
    ASSERT_TRUE(!diags.empty());

    bool found_export = false, found_onready = false, found_yield = false;
    for (const auto& d : diags) {
        if (d.rule == "deprecated_export") found_export = true;
        if (d.rule == "deprecated_onready") found_onready = true;
        if (d.rule == "deprecated_yield") found_yield = true;
    }

    ASSERT_TRUE(found_export);
    ASSERT_TRUE(found_onready);
    ASSERT_TRUE(found_yield);
}

static void test_gdscript_colon_rule_requires_else_as_a_complete_token() {
    const auto diagnostics = didi::offline::GDScriptDiagnostics::analyze(
        "", "elsewhere = 1\nelse_func()\nelse\nelse\t# comment\n");

    int missing_colon_count = 0;
    std::vector<int> missing_colon_lines;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.rule == "missing_colon") {
            ++missing_colon_count;
            missing_colon_lines.push_back(diagnostic.line);
        }
    }
    ASSERT_EQ(missing_colon_count, 2);
    ASSERT_EQ(missing_colon_lines[0], 3);
    ASSERT_EQ(missing_colon_lines[1], 4);
}

static void test_gdscript_diagnostics_ignore_brackets_in_strings_and_comments() {
    const auto diagnostics = didi::offline::GDScriptDiagnostics::analyze(
        "",
        "var prompt = \"Enter name (optional): [required] {value}\"\n"
        "var count = 1 # unmatched ((([[{{\n"
        "var doc = \"\"\"single-line ( [ { documentation\"\"\"\n"
        "export(int) var legacy = 1\n");

    bool found_export = false;
    for (const auto& diagnostic : diagnostics) {
        ASSERT_TRUE(diagnostic.rule != "unbalanced_parentheses");
        ASSERT_TRUE(diagnostic.rule != "unbalanced_brackets");
        ASSERT_TRUE(diagnostic.rule != "unbalanced_braces");
        if (diagnostic.rule == "deprecated_export") found_export = true;
    }
    ASSERT_TRUE(found_export);
}

static void test_gdscript_diagnostics_ignore_escaped_triple_delimiters() {
    const auto diagnostics = didi::offline::GDScriptDiagnostics::analyze(
        "",
        R"gd(var double_doc = """
escaped \""" export(int) ((( [[ {{
still in double doc
"""
var single_doc = '''
escaped \''' yield( ))) ]] }}
still in single doc
'''
export(int) var live = 1
)gd");

    int deprecated_export_count = 0;
    for (const auto& diagnostic : diagnostics) {
        ASSERT_TRUE(diagnostic.rule != "unbalanced_parentheses");
        ASSERT_TRUE(diagnostic.rule != "unbalanced_brackets");
        ASSERT_TRUE(diagnostic.rule != "unbalanced_braces");
        ASSERT_TRUE(diagnostic.rule != "deprecated_yield");
        if (diagnostic.rule == "deprecated_export") ++deprecated_export_count;
    }
    ASSERT_EQ(deprecated_export_count, 1);
}

static void test_godot_45_multiline_compiler_output_is_preserved() {
    ScopedTempDirectory temp("didi-godot-diagnostics-");
    const auto script = temp.path() / "player.gd";
    std::ofstream(script) << "func broken()\n\tpass\n";

#if defined(_WIN32)
    const auto wrapper = temp.path() / "fake-godot.cmd";
    std::ofstream(wrapper)
        << "@echo off\n"
        << "echo SCRIPT ERROR: Parse Error: Expected colon after function declaration.\n"
        << "echo    at: GDScript::reload (res://player.gd:3)\n"
        << "echo ERROR: Failed to load script res://player.gd with error Parse error.\n"
        << "exit /b 1\n";
#else
    const auto wrapper = temp.path() / "fake-godot";
    std::ofstream(wrapper)
        << "#!/bin/sh\n"
        << "echo 'SCRIPT ERROR: Parse Error: Expected colon after function declaration.' >&2\n"
        << "echo '   at: GDScript::reload (res://player.gd:3)' >&2\n"
        << "echo 'ERROR: Failed to load script res://player.gd with error Parse error.' >&2\n"
        << "exit 1\n";
    chmod(wrapper.string().c_str(), 0755);
#endif

    ScopedEnvironmentVariable godot_bin("GODOT_BIN");
    godot_bin.set(wrapper.string());
    const auto diagnostics = didi::offline::GDScriptDiagnostics::runGodotCompilerCheck(script.string());

    bool found_parse_error = false;
    bool found_load_error = false;
    for (const auto& diagnostic : diagnostics) {
        found_parse_error = found_parse_error ||
            (diagnostic.line == 3 && diagnostic.message.find("Expected colon") != std::string::npos);
        found_load_error = found_load_error ||
            diagnostic.message.find("Failed to load script") != std::string::npos;
    }
    ASSERT_TRUE(found_parse_error);
    ASSERT_TRUE(found_load_error);
}

static void test_gdscript_symbol_patch_function() {
    std::string original =
        "extends Node\n\n"
        "func calculate_damage(base: int) -> int:\n"
        "\treturn base * 2\n\n"
        "func other_func():\n"
        "\tpass\n";

    std::string new_func =
        "func calculate_damage(base: int) -> int:\n"
        "\tvar multiplier = 3\n"
        "\treturn base * multiplier";

    auto patch_res = didi::offline::GDScriptDiagnostics::patchSymbol(original, "calculate_damage", new_func, "function");
    ASSERT_TRUE(patch_res.isOk());

    std::string patched = patch_res.value();
    ASSERT_TRUE(patched.find("var multiplier = 3") != std::string::npos);
    ASSERT_TRUE(patched.find("func other_func():") != std::string::npos);
}

static void test_gdscript_symbol_patch_signal() {
    std::string original =
        "extends Node\n\n"
        "func _ready():\n"
        "\tpass\n";

    std::string new_signal = "signal health_changed(new_hp: int)";

    auto patch_res = didi::offline::GDScriptDiagnostics::patchSymbol(original, "health_changed", new_signal, "signal");
    ASSERT_TRUE(patch_res.isOk());

    std::string patched = patch_res.value();
    ASSERT_TRUE(patched.find("signal health_changed(new_hp: int)") != std::string::npos);
}

static void test_gdscript_symbol_patch_preserves_ordinary_comments() {
    std::string original =
        "# Copyright 2026 Example Project\n"
        "## Calculates damage dealt by the attacker.\n"
        "@warning_ignore(\"unused_parameter\")\n"
        "func calculate_damage(base: int) -> int:\n"
        "\treturn base * 2\n\n"
        "func other_func():\n"
        "\tpass\n";

    std::string new_func =
        "func calculate_damage(base: int) -> int:\n"
        "\tvar multiplier = 3\n"
        "\treturn base * multiplier";

    auto patch_res = didi::offline::GDScriptDiagnostics::patchSymbol(original, "calculate_damage", new_func, "function");
    ASSERT_TRUE(patch_res.isOk());

    std::string patched = patch_res.value();
    ASSERT_TRUE(patched.find("# Copyright 2026 Example Project\n") == 0);
    ASSERT_TRUE(patched.find("## Calculates damage dealt by the attacker.") == std::string::npos);
    ASSERT_TRUE(patched.find("@warning_ignore(\"unused_parameter\")") == std::string::npos);
    ASSERT_TRUE(patched.find("func calculate_damage(base: int) -> int:\n\tvar multiplier = 3\n\treturn base * multiplier") != std::string::npos);
}

static void test_gdscript_symbol_patch_preserves_next_sibling_preamble() {
    std::string original =
        "func calculate_damage(base: int) -> int:\n"
        "\t# target implementation comment\n"
        "\treturn base * 2\n"
        "# Sibling license comment\n"
        "## Sibling documentation.\n"
        "@warning_ignore(\"unused_parameter\")\n"
        "func other_func():\n"
        "\tpass\n";

    std::string new_func =
        "func calculate_damage(base: int) -> int:\n"
        "\treturn base * 3";

    auto patch_res = didi::offline::GDScriptDiagnostics::patchSymbol(original, "calculate_damage", new_func, "function");
    ASSERT_TRUE(patch_res.isOk());

    std::string expected =
        "func calculate_damage(base: int) -> int:\n"
        "\treturn base * 3\n"
        "# Sibling license comment\n"
        "## Sibling documentation.\n"
        "@warning_ignore(\"unused_parameter\")\n"
        "func other_func():\n"
        "\tpass\n";
    ASSERT_EQ(patch_res.value(), expected);
}

static void test_gdscript_symbol_patch_parameterized_annotation() {
    // Break caught: patchSymbol matched annotations with (@\w+\s+)* only, so a
    // parameterized @export_* declaration was missed and a duplicate var was
    // prepended at the top of the file.
    std::string original =
        "extends Node\n\n"
        "@export_range(0, 100, 1) var speed: float = 5.0\n\n"
        "func _ready():\n"
        "\tpass\n";

    std::string new_var = "@export_range(0, 250, 5) var speed: float = 20.0";

    auto patch_res = didi::offline::GDScriptDiagnostics::patchSymbol(original, "speed", new_var, "variable");
    ASSERT_TRUE(patch_res.isOk());

    std::string patched = patch_res.value();
    ASSERT_TRUE(patched.find("@export_range(0, 250, 5) var speed: float = 20.0") != std::string::npos);
    ASSERT_TRUE(patched.find("@export_range(0, 100, 1)") == std::string::npos);
    ASSERT_EQ(patched.find("var speed"), patched.rfind("var speed"));
}

static void test_gdscript_extract_symbols_constants_and_container_types() {
    // Break caught: const declarations were dropped and Array[String] was
    // truncated to Array.
    std::string source =
        "extends Node\n"
        "const MAX_PLAYERS: int = 4\n"
        "const TAGS = [\"a\", \"b\"]\n"
        "var names: Array[String] = []\n"
        "var lookup: Dictionary[int, Variant] = {}\n"
        "func collect() -> Array[String]:\n"
        "\treturn names\n";

    const auto symbols = didi::offline::GDScriptDiagnostics::extractSymbols(source);

    ASSERT_TRUE(symbols.contains("constants"));
    ASSERT_EQ(symbols["constants"].size(), 2u);
    ASSERT_EQ(symbols["constants"][0]["name"], "MAX_PLAYERS");
    ASSERT_EQ(symbols["constants"][0]["type"], "int");
    ASSERT_EQ(symbols["constants"][1]["name"], "TAGS");

    ASSERT_EQ(symbols["variables"].size(), 2u);
    ASSERT_EQ(symbols["variables"][0]["type"], "Array[String]");
    ASSERT_EQ(symbols["variables"][1]["type"], "Dictionary[int, Variant]");
    ASSERT_EQ(symbols["functions"][0]["return_type"], "Array[String]");
}

static void test_gdscript_colon_rule_allows_continuations_and_open_braces() {
    // Break caught: missing_colon fired on block headers continued with a
    // trailing backslash or still inside a dictionary literal.
    std::string source =
        "extends Node\n"
        "func check(a: bool, b: bool) -> void:\n"
        "\tif a \\\n"
        "\t\t\tand b:\n"
        "\t\tpass\n"
        "\tif \"x\" in {\n"
        "\t\t\t\"x\": 1\n"
        "\t\t}:\n"
        "\t\tpass\n";

    const auto diagnostics = didi::offline::GDScriptDiagnostics::analyze("", source);
    for (const auto& diagnostic : diagnostics) {
        ASSERT_TRUE(diagnostic.rule != "missing_colon");
    }
}

static void test_reflect_class_answers_from_the_shipped_api_reference() {
    // Break caught: reflection answered from a hand-written snapshot of about
    // eighteen classes and returned is_known_class false for the rest of the
    // engine, even though the repository pins Godot's own API dump.
    const auto& reference = didi::offline::ClassReference::instance();
    if (!reference.loaded()) {
        throw std::runtime_error(
            "The generated class reference was not found next to the test binary. "
            "Build the didi target so CMake generates and places it.");
    }
    ASSERT_TRUE(reference.size() > 800u);

    // The class the issue names, which the old snapshot did not have.
    const auto rich = didi::offline::GDScriptDiagnostics::reflectClass("RichTextLabel");
    ASSERT_EQ(rich["is_known_class"], true);
    ASSERT_EQ(rich["source"], "extension_api");
    ASSERT_EQ(rich["inherits"], "Control");
    ASSERT_TRUE(rich["properties"].contains("bbcode_enabled"));
    ASSERT_EQ(rich["properties"]["bbcode_enabled"]["type"], "bool");
    ASSERT_TRUE(rich["methods"].contains("append_text"));
    ASSERT_TRUE(rich["signals"].is_array() && !rich["signals"].empty());
    ASSERT_TRUE(rich.contains("api_version"));

    // Other names the issue calls out as previously unknown.
    for (const char* name : {"AudioStreamPlayer2D", "Path3D", "RigidBody3D", "CanvasLayer"}) {
        const auto reflected = didi::offline::GDScriptDiagnostics::reflectClass(name);
        ASSERT_EQ(reflected["is_known_class"], true);
        ASSERT_TRUE(!reflected["inherits"].get<std::string>().empty());
    }

    // Method signatures carry their arguments and return type, not just names.
    const auto node = didi::offline::GDScriptDiagnostics::reflectClass("Node");
    ASSERT_EQ(node["methods"]["add_child"]["returns"], "void");
    ASSERT_TRUE(node["methods"]["add_child"]["args"].size() >= 1u);
    ASSERT_TRUE(node["methods"]["get_child"]["const"] == true);

    // Enums come through, which the old snapshot had no room for.
    ASSERT_TRUE(node.contains("enums"));
    ASSERT_TRUE(node["enums"].contains("ProcessMode"));

    // A name that is not a class is reported as unknown, and says which source
    // decided that, so a caller can tell "no such class" from "no reference".
    const auto unknown = didi::offline::GDScriptDiagnostics::reflectClass("NotARealGodotClass");
    ASSERT_EQ(unknown["is_known_class"], false);
    ASSERT_EQ(unknown["source"], "extension_api");
    ASSERT_TRUE(unknown["properties"].empty());
}

struct RegisterScriptPatchTests {
    RegisterScriptPatchTests() {
        registerTest("GDScript.DiagnosticsDeprecation", test_gdscript_diagnostics_deprecation);
        registerTest("GDScript.ElseTokenColonRule", test_gdscript_colon_rule_requires_else_as_a_complete_token);
        registerTest("GDScript.StringAwareBalance", test_gdscript_diagnostics_ignore_brackets_in_strings_and_comments);
        registerTest("GDScript.EscapedTripleDelimiter", test_gdscript_diagnostics_ignore_escaped_triple_delimiters);
        registerTest("GDScript.Godot45CompilerOutput", test_godot_45_multiline_compiler_output_is_preserved);
        registerTest("GDScript.PatchFunction", test_gdscript_symbol_patch_function);
        registerTest("GDScript.PatchSignal", test_gdscript_symbol_patch_signal);
        registerTest("GDScript.PatchPreservesOrdinaryComments", test_gdscript_symbol_patch_preserves_ordinary_comments);
        registerTest("GDScript.PatchPreservesSiblingPreamble", test_gdscript_symbol_patch_preserves_next_sibling_preamble);
        registerTest("GDScript.PatchParameterizedAnnotation", test_gdscript_symbol_patch_parameterized_annotation);
        registerTest("GDScript.ExtractConstantsAndContainerTypes", test_gdscript_extract_symbols_constants_and_container_types);
        registerTest("GDScript.ColonRuleAllowsContinuationsAndBraces", test_gdscript_colon_rule_allows_continuations_and_open_braces);
        registerTest("GDScript.ReflectClassUsesShippedApiReference",
                     test_reflect_class_answers_from_the_shipped_api_reference);
    }
} g_registerScriptPatchTests;

} // namespace
