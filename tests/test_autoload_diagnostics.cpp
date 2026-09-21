#include "didi/offline/gdscript_diagnostics.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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

// projectAutoloadNames reads the file from the project root, so a test for it
// needs a project root to be in.
class ScopedProject final {
public:
    explicit ScopedProject(const std::string& contents)
        : m_original(std::filesystem::current_path()),
          m_root(m_original / "build" / "test-projects" /
                 ("didi-autoload-names-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
        std::ofstream(m_root / "project.godot", std::ios::binary) << contents;
        std::filesystem::current_path(m_root);
    }

    ~ScopedProject() {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
        std::filesystem::remove_all(m_root, error);
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_root;
};

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

// Break caught: the section was matched as the whole line, so `[ autoload ]`
// registered no names and every singleton in the project came back as an
// undefined identifier -- the demotion above is the whole reason this list is
// read. Godot reads a spaced or tab-padded header as the same section, checked
// on 4.5.1, 4.6.2 and 4.7.2 (#809).
void reads_the_autoload_names_under_a_header_that_is_spaced() {
    {
        ScopedProject project("config_version=5\n"
                              "\n"
                              "[ autoload ]\n"
                              "\n"
                              "GameState=\"*res://scripts/game_state.gd\"\n");
        const auto names = GDScriptDiagnostics::projectAutoloadNames();
        ASSERT_EQ(names.size(), size_t(1));
        ASSERT_EQ(names[0], std::string("GameState"));
    }
    {
        // A different section is still a different section, or every settings
        // key in the file would be read as a singleton.
        ScopedProject project("config_version=5\n"
                              "\n"
                              "[ application ]\n"
                              "\n"
                              "config/name=\"Probe\"\n");
        ASSERT_TRUE(GDScriptDiagnostics::projectAutoloadNames().empty());
    }
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

// A manifest Godot will not load registers nothing, so the demotion has
// nothing to stand on and the diagnostic the user needs has to survive.
//
// Measured on 4.5.1 and 4.7.2 with the entry above the broken value and below
// it. Neither runs: the project does not open, `--headless --path` falls
// through to the project manager, and the singleton never enters the tree
// (#826).
void reads_no_autoloads_out_of_a_manifest_that_does_not_load() {
    {
        // Balanced and still ERR_PARSE_ERROR. The entry sits above the break,
        // which is the case where ProjectSettings.has_setting says true in a
        // session with no project open.
        ScopedProject project("config_version=5\n"
                              "\n"
                              "[autoload]\n"
                              "\n"
                              "GameState=\"*res://scripts/game_state.gd\"\n"
                              "\n"
                              "[application]\n"
                              "\n"
                              "config/broken=)\n");
        ASSERT_TRUE(GDScriptDiagnostics::projectAutoloadNames().empty());
    }
    {
        // A file that ends part-way through a value, which is the other half.
        ScopedProject project("config_version=5\n"
                              "\n"
                              "[autoload]\n"
                              "\n"
                              "GameState=\"*res://scripts/game_state.gd\"\n"
                              "\n"
                              "[input]\n"
                              "\n"
                              "jump={\"deadzone\": 0.5\n");
        ASSERT_TRUE(GDScriptDiagnostics::projectAutoloadNames().empty());
    }
    {
        // The control: the same entry in a file that loads is still read, or
        // this would be a way to lose every demotion rather than the wrong
        // ones.
        ScopedProject project("config_version=5\n"
                              "\n"
                              "[autoload]\n"
                              "\n"
                              "GameState=\"*res://scripts/game_state.gd\"\n"
                              "\n"
                              "[application]\n"
                              "\n"
                              "config/name=\"Probe\"\n");
        const auto names = GDScriptDiagnostics::projectAutoloadNames();
        ASSERT_EQ(names.size(), size_t(1));
        ASSERT_EQ(names[0], std::string("GameState"));
        ASSERT_TRUE(GDScriptDiagnostics::projectManifestLoadProblem().empty());
    }
}

// The error is right and it lands badly on its own: the script is fine and the
// identifier is unresolved because the project one file away does not open.
void names_the_manifest_beside_an_identifier_it_could_not_register() {
    ScopedProject project("config_version=5\n"
                          "\n"
                          "[autoload]\n"
                          "\n"
                          "GameState=\"*res://scripts/game_state.gd\"\n"
                          "\n"
                          "[application]\n"
                          "\n"
                          "config/broken=)\n");
    const auto problem = GDScriptDiagnostics::projectManifestLoadProblem();
    ASSERT_TRUE(!problem.empty());
    ASSERT_TRUE(problem.find("application/config/broken") != std::string::npos);

    std::vector<ScriptDiagnostic> diags{
        compilerError(10, "Compile Error: Identifier not found: GameState"),
        compilerError(12, "Compile Error: Some other fault")};
    GDScriptDiagnostics::noteUnloadableManifest(diags, problem);

    // Still an error, because it is one: nothing resolves that name at run time
    // while the project refuses to open.
    ASSERT_EQ(diags[0].severity, std::string("error"));
    ASSERT_TRUE(diags[0].note.find("application/config/broken") != std::string::npos);
    ASSERT_TRUE(diags[0].note.find("does not open") != std::string::npos);
    // Only the unresolved identifiers. A fault that has nothing to do with a
    // singleton does not get a note about project.godot.
    ASSERT_TRUE(diags[1].note.empty());
}

struct Register {
    Register() {
        registerTest("autoload_diagnostics.demotes_registered_autoload",
                     demotes_an_identifier_that_is_a_registered_autoload);
        registerTest("autoload_diagnostics.spaced_section_header",
                     reads_the_autoload_names_under_a_header_that_is_spaced);
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
        registerTest("autoload_diagnostics.unloadable_manifest_registers_nothing",
                     reads_no_autoloads_out_of_a_manifest_that_does_not_load);
        registerTest("autoload_diagnostics.unloadable_manifest_is_named",
                     names_the_manifest_beside_an_identifier_it_could_not_register);
    }
} registrar;

} // namespace
