#include "didi/offline/project_settings_file.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::offline::settingLiteral;
using didi::offline::writeProjectSetting;

class ProjectFixture {
public:
    explicit ProjectFixture(const std::string& suffix, const std::string& contents) {
        m_root = std::filesystem::temp_directory_path() /
                 ("didi-project-settings-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_root);
        std::ofstream(m_root / "project.godot", std::ios::binary) << contents;
    }

    ~ProjectFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    const std::filesystem::path& root() const { return m_root; }

    std::string read() const {
        std::ifstream input(m_root / "project.godot", std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

private:
    std::filesystem::path m_root;
};

const char* const kBare = "config_version=5\n\n[application]\n\nconfig/name=\"Bare\"\n";

// The bootstrap #382 reported as impossible: a project with no [editor_plugins]
// section at all gains one, and the value is the Array the live path builds
// from the same JSON rather than a string that looks like one.
void enables_an_addon_in_a_project_that_has_no_section_for_it() {
    ProjectFixture project("bootstrap", kBare);
    auto written = writeProjectSetting(project.root(), "editor_plugins/enabled",
                                       didi::json::array({"res://addons/didi/plugin.cfg"}), false);
    ASSERT_TRUE(written.isOk());
    ASSERT_TRUE(written.value().section_created);
    ASSERT_TRUE(!written.value().existed);
    ASSERT_EQ(written.value().section, std::string("editor_plugins"));
    ASSERT_EQ(written.value().key, std::string("enabled"));

    const auto contents = project.read();
    ASSERT_TRUE(contents.find("[editor_plugins]") != std::string::npos);
    ASSERT_TRUE(contents.find("enabled=[\"res://addons/didi/plugin.cfg\"]") != std::string::npos);
    // Everything that was already there is still there.
    ASSERT_TRUE(contents.find("config/name=\"Bare\"") != std::string::npos);
    ASSERT_TRUE(contents.find("config_version=5") != std::string::npos);
}

// Break caught: a new key appended to the end of the file instead of the end of
// its section lands under whichever section happens to be last, so the setting
// is written and the engine never reads it.
void puts_a_new_key_inside_its_existing_section() {
    ProjectFixture project("section", "config_version=5\n\n[application]\n\nconfig/name=\"X\"\n\n[rendering]\n\nrenderer/rendering_method=\"gl_compatibility\"\n");
    auto written = writeProjectSetting(project.root(), "application/run/main_scene",
                                       didi::json("res://main.tscn"), false);
    ASSERT_TRUE(written.isOk());
    ASSERT_TRUE(!written.value().section_created);

    const auto contents = project.read();
    const auto application = contents.find("[application]");
    const auto rendering = contents.find("[rendering]");
    const auto key = contents.find("run/main_scene=\"res://main.tscn\"");
    ASSERT_TRUE(application != std::string::npos);
    ASSERT_TRUE(rendering != std::string::npos);
    ASSERT_TRUE(key != std::string::npos);
    ASSERT_TRUE(key > application && key < rendering);
}

void replaces_a_value_in_place_and_reports_what_it_replaced() {
    ProjectFixture project("replace", "config_version=5\n\n[application]\n\nconfig/name=\"Old\"\n");
    auto written = writeProjectSetting(project.root(), "application/config/name",
                                       didi::json("New"), false);
    ASSERT_TRUE(written.isOk());
    ASSERT_TRUE(written.value().existed);
    ASSERT_EQ(written.value().previous_literal, std::string("\"Old\""));

    const auto contents = project.read();
    ASSERT_TRUE(contents.find("config/name=\"New\"") != std::string::npos);
    ASSERT_TRUE(contents.find("\"Old\"") == std::string::npos);
}

void removes_a_setting_and_refuses_to_remove_one_that_is_not_there() {
    ProjectFixture project("remove", "config_version=5\n\n[application]\n\nconfig/name=\"X\"\nrun/main_scene=\"res://main.tscn\"\n");
    auto removed = writeProjectSetting(project.root(), "application/run/main_scene",
                                       didi::json(), true);
    ASSERT_TRUE(removed.isOk());
    ASSERT_TRUE(removed.value().removed);
    ASSERT_EQ(removed.value().previous_literal, std::string("\"res://main.tscn\""));
    ASSERT_TRUE(project.read().find("run/main_scene") == std::string::npos);
    // The section and its other key survive the removal.
    ASSERT_TRUE(project.read().find("config/name=\"X\"") != std::string::npos);

    auto again = writeProjectSetting(project.root(), "application/run/main_scene",
                                     didi::json(), true);
    ASSERT_TRUE(again.isErr());
    ASSERT_EQ(again.error().code, 404);
}

// The typed tools own these namespaces live. An offline route that did not
// share the rule would be a way around it.
void refuses_the_namespaces_the_typed_tools_own() {
    ProjectFixture project("namespaces", kBare);
    ASSERT_TRUE(writeProjectSetting(project.root(), "autoload/GameState",
                                    didi::json("*res://a.gd"), false).isErr());
    ASSERT_TRUE(writeProjectSetting(project.root(), "input/jump",
                                    didi::json::object(), false).isErr());
    ASSERT_TRUE(writeProjectSetting(project.root(), "no_slash", didi::json(1), false).isErr());
    ASSERT_TRUE(writeProjectSetting(project.root(), "a//b", didi::json(1), false).isErr());
    // Nothing was written on any of those paths.
    ASSERT_EQ(project.read(), std::string(kBare));
}

// Godot writes LF. Rewriting every line ending of a CRLF file to change one
// setting is a diff nobody asked for.
void keeps_the_line_endings_the_file_arrived_with() {
    ProjectFixture project("crlf", "config_version=5\r\n\r\n[application]\r\n\r\nconfig/name=\"X\"\r\n");
    auto written = writeProjectSetting(project.root(), "application/config/name",
                                       didi::json("Y"), false);
    ASSERT_TRUE(written.isOk());
    const auto contents = project.read();
    ASSERT_TRUE(contents.find("config/name=\"Y\"\r\n") != std::string::npos);
    ASSERT_TRUE(contents.find("\n\n") == std::string::npos);
}

// The reach is the live converter's reach, so a value written offline is the
// same Variant as one written live.
void writes_the_literals_the_live_converter_builds() {
    ASSERT_EQ(settingLiteral(didi::json()).value(), std::string("null"));
    ASSERT_EQ(settingLiteral(didi::json(true)).value(), std::string("true"));
    ASSERT_EQ(settingLiteral(didi::json(7)).value(), std::string("7"));
    ASSERT_EQ(settingLiteral(didi::json(-7)).value(), std::string("-7"));
    // A real stays a real. Writing 1 for 1.0 would store a different Variant
    // type than the live path stores for the same request.
    ASSERT_EQ(settingLiteral(didi::json(1.0)).value(), std::string("1.0"));
    ASSERT_EQ(settingLiteral(didi::json(0.5)).value(), std::string("0.5"));
    ASSERT_EQ(settingLiteral(didi::json("a\"b\\c")).value(), std::string("\"a\\\"b\\\\c\""));
    ASSERT_EQ(settingLiteral(didi::json::array({1, "x"})).value(), std::string("[1, \"x\"]"));
    ASSERT_EQ(settingLiteral(didi::json::object({{"k", 2}})).value(), std::string("{\"k\": 2}"));

    ASSERT_TRUE(settingLiteral(didi::json(std::numeric_limits<double>::infinity())).isErr());

    didi::json deep = 1;
    for (int level = 0; level < 20; ++level) deep = didi::json::array({deep});
    ASSERT_TRUE(settingLiteral(deep).isErr());
}

struct Register {
    Register() {
        registerTest("project_settings_file.bootstrap_section",
                     enables_an_addon_in_a_project_that_has_no_section_for_it);
        registerTest("project_settings_file.new_key_stays_in_section",
                     puts_a_new_key_inside_its_existing_section);
        registerTest("project_settings_file.replace_in_place",
                     replaces_a_value_in_place_and_reports_what_it_replaced);
        registerTest("project_settings_file.remove", removes_a_setting_and_refuses_to_remove_one_that_is_not_there);
        registerTest("project_settings_file.reserved_namespaces", refuses_the_namespaces_the_typed_tools_own);
        registerTest("project_settings_file.line_endings", keeps_the_line_endings_the_file_arrived_with);
        registerTest("project_settings_file.literals", writes_the_literals_the_live_converter_builds);
    }
} registrar;

} // namespace
