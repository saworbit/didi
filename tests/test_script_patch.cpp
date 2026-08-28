#include "didi/offline/gdscript_diagnostics.hpp"

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

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

struct RegisterScriptPatchTests {
    RegisterScriptPatchTests() {
        registerTest("GDScript.DiagnosticsDeprecation", test_gdscript_diagnostics_deprecation);
        registerTest("GDScript.PatchFunction", test_gdscript_symbol_patch_function);
        registerTest("GDScript.PatchSignal", test_gdscript_symbol_patch_signal);
        registerTest("GDScript.PatchPreservesOrdinaryComments", test_gdscript_symbol_patch_preserves_ordinary_comments);
        registerTest("GDScript.PatchPreservesSiblingPreamble", test_gdscript_symbol_patch_preserves_next_sibling_preamble);
    }
} g_registerScriptPatchTests;
