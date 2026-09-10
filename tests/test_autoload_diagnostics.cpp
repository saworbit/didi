#include "didi/offline/gdscript_diagnostics.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::offline::GDScriptDiagnostics;
using didi::offline::ScriptDiagnostic;

ScriptDiagnostic compilerError(int line, const std::string& message) {
    ScriptDiagnostic diagnostic;
    diagnostic.line = line;
    diagnostic.severity = "error";
    diagnostic.message = message;
    diagnostic.rule = "godot_compiler";
    return diagnostic;
}

bool anyError(const std::vector<ScriptDiagnostic>& diags) {
    for (const auto& diagnostic : diags) {
        if (diagnostic.severity == "error") return true;
    }
    return false;
}

// The report in #383: a script whose only fault is naming an autoload comes
// back with has_errors true forever, because the compiler check runs in a
// process that never registers autoloads.
void demotes_an_identifier_that_is_a_registered_autoload() {
    std::vector<ScriptDiagnostic> diags{
        compilerError(10, "Compile Error: Identifier not found: GameState"),
        compilerError(1, "Failed to load script \"res://scripts/hud.gd\" with error \"Compilation failed\".")
    };
    GDScriptDiagnostics::demoteAutoloadDiagnostics(diags, {"GameState", "Audio"});

    ASSERT_EQ(diags.size(), size_t(2));
    ASSERT_EQ(diags[0].severity, std::string("warning"));
    ASSERT_TRUE(!diags[0].note.empty());
    // The cascade the compiler prints once it has given up goes with it, or
    // has_errors stays true and nothing has been fixed.
    ASSERT_EQ(diags[1].severity, std::string("warning"));
    ASSERT_TRUE(!anyError(diags));
    // Nothing is thrown away. An autoload whose own script is broken is still
    // worth seeing.
    ASSERT_TRUE(diags[0].message.find("GameState") != std::string::npos);
}

// The demotion has to be narrow or it becomes a way to hide real faults.
void leaves_an_identifier_that_is_not_an_autoload_alone() {
    std::vector<ScriptDiagnostic> diags{
        compilerError(10, "Compile Error: Identifier not found: Typo"),
        compilerError(1, "Failed to load script \"res://scripts/hud.gd\" with error \"Compilation failed\".")
    };
    GDScriptDiagnostics::demoteAutoloadDiagnostics(diags, {"GameState"});

    ASSERT_EQ(diags[0].severity, std::string("error"));
    ASSERT_EQ(diags[1].severity, std::string("error"));
    ASSERT_TRUE(anyError(diags));
}

// Break caught: demoting the cascade whenever anything was demoted would report
// a script with a genuine fault as clean.
void keeps_the_verdict_when_a_real_error_sits_beside_the_autoload() {
    std::vector<ScriptDiagnostic> diags{
        compilerError(4, "Compile Error: Identifier not found: GameState"),
        compilerError(5, "Parse Error: Cannot assign a value of type \"String\" as \"int\"."),
        compilerError(1, "Failed to load script \"res://scripts/hud.gd\" with error \"Parse error\".")
    };
    GDScriptDiagnostics::demoteAutoloadDiagnostics(diags, {"GameState"});

    ASSERT_EQ(diags[0].severity, std::string("warning"));
    ASSERT_EQ(diags[1].severity, std::string("error"));
    ASSERT_EQ(diags[2].severity, std::string("error"));
    ASSERT_TRUE(anyError(diags));
}

// A project with no autoloads must behave exactly as it did before.
void changes_nothing_when_the_project_registers_no_autoloads() {
    std::vector<ScriptDiagnostic> diags{
        compilerError(10, "Compile Error: Identifier not found: GameState")
    };
    GDScriptDiagnostics::demoteAutoloadDiagnostics(diags, {});
    ASSERT_EQ(diags[0].severity, std::string("error"));
    ASSERT_TRUE(diags[0].note.empty());
}

void demotes_every_autoload_the_script_names() {
    std::vector<ScriptDiagnostic> diags{
        compilerError(4, "Compile Error: Identifier not found: GameState"),
        compilerError(6, "Compile Error: Identifier not found: Audio"),
        compilerError(1, "Failed to load script \"res://a.gd\" with error \"Compilation failed\".")
    };
    GDScriptDiagnostics::demoteAutoloadDiagnostics(diags, {"GameState", "Audio"});
    ASSERT_TRUE(!anyError(diags));
    ASSERT_TRUE(!diags[1].note.empty());
}

// A warning the check already emitted is not the demotion's business.
void does_not_touch_diagnostics_that_are_already_warnings() {
    ScriptDiagnostic warning;
    warning.line = 3;
    warning.severity = "warning";
    warning.message = "Compile Error: Identifier not found: GameState";
    warning.rule = "godot_compiler";
    std::vector<ScriptDiagnostic> diags{warning};
    GDScriptDiagnostics::demoteAutoloadDiagnostics(diags, {"GameState"});
    ASSERT_TRUE(diags[0].note.empty());
}

struct Register {
    Register() {
        registerTest("autoload_diagnostics.demotes_registered_autoload",
                     demotes_an_identifier_that_is_a_registered_autoload);
        registerTest("autoload_diagnostics.leaves_other_identifiers",
                     leaves_an_identifier_that_is_not_an_autoload_alone);
        registerTest("autoload_diagnostics.keeps_real_errors",
                     keeps_the_verdict_when_a_real_error_sits_beside_the_autoload);
        registerTest("autoload_diagnostics.no_autoloads_no_change",
                     changes_nothing_when_the_project_registers_no_autoloads);
        registerTest("autoload_diagnostics.every_autoload",
                     demotes_every_autoload_the_script_names);
        registerTest("autoload_diagnostics.leaves_warnings",
                     does_not_touch_diagnostics_that_are_already_warnings);
    }
} registrar;

} // namespace
