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

using didi::offline::readProjectSetting;
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

// Break caught: the section name was the bracket text untrimmed, so
// `[ application ]` matched no section any caller names. The dry run reported a
// setting that is in the file as absent, with an empty previous_literal, and the
// write appended a second [application] section rather than updating the line
// that was already there (#809). Godot reads `[ application ]`, `[application ]`
// and a tab-padded header as the same section -- checked on 4.5.1, 4.6.2 and
// 4.7.2 -- and merges a second spelling into it.
void reads_and_updates_a_section_whose_header_is_spaced() {
    ProjectFixture project("spaced-header",
                           "config_version=5\n\n[ application ]\n\nconfig/name=\"Probe\"\n");

    // The preview reader first. exists: false is what a caller decides on, and
    // it is what this said about a setting that is right there.
    auto preview = readProjectSetting(project.root(), "application/config/name");
    ASSERT_TRUE(preview.isOk());
    ASSERT_TRUE(preview.value().existed);
    ASSERT_EQ(preview.value().literal, std::string("\"Probe\""));

    // A new key lands under the header that is there rather than under a second
    // copy of it, which is a file Godot's own writer would never produce.
    auto added = writeProjectSetting(project.root(), "application/config/description",
                                     didi::json("vibe"), false);
    ASSERT_TRUE(added.isOk());
    ASSERT_TRUE(!added.value().section_created);
    auto contents = project.read();
    ASSERT_TRUE(contents.find("[application]") == std::string::npos);
    ASSERT_TRUE(contents.find("[ application ]") != std::string::npos);
    ASSERT_TRUE(contents.find("config/description=\"vibe\"") != std::string::npos);

    // And an existing key is replaced in place, with what it replaced reported.
    auto replaced = writeProjectSetting(project.root(), "application/config/name",
                                        didi::json("Renamed"), false);
    ASSERT_TRUE(replaced.isOk());
    ASSERT_TRUE(replaced.value().existed);
    ASSERT_EQ(replaced.value().previous_literal, std::string("\"Probe\""));
    contents = project.read();
    ASSERT_TRUE(contents.find("config/name=\"Renamed\"") != std::string::npos);
    ASSERT_TRUE(contents.find("\"Probe\"") == std::string::npos);

    // A tab-padded header is the same section to the engine, so it is the same
    // section here.
    ProjectFixture tabbed("tabbed-header",
                          "config_version=5\n\n[\tapplication\t]\n\nconfig/name=\"Probe\"\n");
    auto tabbed_write = writeProjectSetting(tabbed.root(), "application/config/name",
                                            didi::json("Renamed"), false);
    ASSERT_TRUE(tabbed_write.isOk());
    ASSERT_TRUE(tabbed_write.value().existed);

    // A different section is still a different section: trimming the name must
    // not make every header match.
    ProjectFixture other("other-header",
                         "config_version=5\n\n[ rendering ]\n\nconfig/name=\"Probe\"\n");
    auto absent = readProjectSetting(other.root(), "application/config/name");
    ASSERT_TRUE(absent.isOk());
    ASSERT_TRUE(!absent.value().existed);
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

void reads_and_rewrites_a_key_whose_spelling_carries_spaces() {
    // Godot drops the whitespace inside a key, so `config / name` is
    // application/config/name and the last spelling in the file wins. Matching
    // the key as a prefix of the line missed it, previewed exists: false, and
    // appended a duplicate key Godot's own writer would never produce (#813).
    ProjectFixture project("spaced-key",
                           "[application]\n"
                           "config / name=\"before\"\n");
    const auto read = readProjectSetting(project.root(), "application/config/name");
    ASSERT_TRUE(read.isOk());
    ASSERT_TRUE(read.value().existed);
    ASSERT_EQ(read.value().literal, "\"before\"");

    const auto write = writeProjectSetting(project.root(), "application/config/name", "after", false);
    ASSERT_TRUE(write.isOk());
    ASSERT_TRUE(write.value().existed);
    const auto text = project.read();
    ASSERT_TRUE(text.find("config/name=\"after\"") != std::string::npos);
    ASSERT_TRUE(text.find("config / name") == std::string::npos);
}

void treats_a_key_a_bare_note_swallowed_as_a_key_that_is_not_there() {
    // A line with no `=` joins forward, so this file registers
    // application/#anoteconfig/name and has no config/name of its own. Reading
    // it as present, and rewriting that line, leaves a setting that still does
    // not register (#813).
    ProjectFixture project("swallowed-key",
                           "[application]\n"
                           "# a note\n"
                           "config/name=\"invisible\"\n");
    const auto read = readProjectSetting(project.root(), "application/config/name");
    ASSERT_TRUE(read.isOk());
    ASSERT_TRUE(!read.value().existed);

    // The write lands as a new line at the end of the section, after the key
    // the note ate, so the engine registers it and the last one wins.
    const auto write = writeProjectSetting(project.root(), "application/config/name", "landed", false);
    ASSERT_TRUE(write.isOk());
    ASSERT_TRUE(!write.value().existed);
    const auto text = project.read();
    ASSERT_TRUE(text.find("# a note") != std::string::npos);
    ASSERT_TRUE(text.find("config/name=\"invisible\"") != std::string::npos);
    ASSERT_TRUE(text.find("config/name=\"landed\"") != std::string::npos);
    ASSERT_TRUE(text.find("config/name=\"invisible\"") < text.find("config/name=\"landed\""));
}

void replaces_a_value_that_spans_lines_without_leaving_its_tail_behind() {
    // Godot writes a dictionary over several lines. Replacing only the line
    // holding the `=` left `}` behind, and a line with no `=` joins forward
    // into the next key, which would have destroyed the setting below it.
    ProjectFixture project("multiline-value",
                           "[shader_globals]\n"
                           "tint={\n"
                           "\"type\": \"color\",\n"
                           "\"value\": Color(1, 1, 1, 1)\n"
                           "}\n"
                           "scale=2.0\n");
    const auto write = writeProjectSetting(project.root(), "shader_globals/tint", 4, false);
    ASSERT_TRUE(write.isOk());
    ASSERT_TRUE(write.value().existed);
    const auto text = project.read();
    ASSERT_TRUE(text.find("tint=4") != std::string::npos);
    ASSERT_TRUE(text.find("\"type\"") == std::string::npos);
    ASSERT_TRUE(text.find("}") == std::string::npos);
    ASSERT_TRUE(text.find("scale=2.0") != std::string::npos);

    const auto scale = readProjectSetting(project.root(), "shader_globals/scale");
    ASSERT_TRUE(scale.isOk());
    ASSERT_TRUE(scale.value().existed);
}

struct Register {
    Register() {
        registerTest("project_settings_file.bootstrap_section",
                     enables_an_addon_in_a_project_that_has_no_section_for_it);
        registerTest("project_settings_file.new_key_stays_in_section",
                     puts_a_new_key_inside_its_existing_section);
        registerTest("project_settings_file.spaced_section_header",
                     reads_and_updates_a_section_whose_header_is_spaced);
        registerTest("project_settings_file.replace_in_place",
                     replaces_a_value_in_place_and_reports_what_it_replaced);
        registerTest("project_settings_file.remove", removes_a_setting_and_refuses_to_remove_one_that_is_not_there);
        registerTest("project_settings_file.reserved_namespaces", refuses_the_namespaces_the_typed_tools_own);
        registerTest("project_settings_file.line_endings", keeps_the_line_endings_the_file_arrived_with);
        registerTest("project_settings_file.literals", writes_the_literals_the_live_converter_builds);
        registerTest("project_settings_file.spaced_key",
                     reads_and_rewrites_a_key_whose_spelling_carries_spaces);
        registerTest("project_settings_file.key_a_note_swallowed",
                     treats_a_key_a_bare_note_swallowed_as_a_key_that_is_not_there);
        registerTest("project_settings_file.multiline_value",
                     replaces_a_value_that_spans_lines_without_leaving_its_tail_behind);
    }
} registrar;

} // namespace
