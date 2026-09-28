// Follow-up tests (Q6 in docs/BUILD_QUEUE.md, #1040).
//
// A successful mutation that leaves work undone names it under follow_up, and
// every step comes from a fact the answer already carries. What is asserted
// here is each rule and each thing that must not produce a step. Which tools
// can leave which work is tests/follow_ups.json, and the live answers are
// checked by the harness on every engine line.

#include "didi/mcp/follow_ups.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::mcp::followUpsFor;

class ScopedProject final {
public:
    explicit ScopedProject(const std::string& suffix)
        : m_original(std::filesystem::current_path()),
          m_root(m_original / "build" / "test-projects" /
                 ("didi-follow-up-" + suffix + "-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(m_root);
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

}  // namespace

// A live edit of the open scene is saved by editor_save_scene.
static void test_an_unsaved_scene_names_the_save() {
    const auto steps = followUpsFor({{"status", "success"}, {"execution_mode", "live"},
                                     {"scene_saved", false}, {"undo_redo_registered", true}});
    ASSERT_EQ(steps.size(), 1u);
    ASSERT_EQ(steps[0]["work"], "save");
    ASSERT_EQ(steps[0]["tool"], "editor_save_scene");
    ASSERT_TRUE(steps[0]["reason"].is_string());
    // Only the fact, never its absence or its opposite.
    ASSERT_TRUE(followUpsFor({{"execution_mode", "live"}, {"scene_saved", true}}).empty());
    ASSERT_TRUE(followUpsFor({{"execution_mode", "live"}, {"undo_redo_registered", true}}).empty());
    ASSERT_TRUE(followUpsFor({{"scene_saved", "false"}}).empty());
}

// Managed recovery saves after a protected mutation, and says so in its receipt,
// so the bridge's scene_saved: false is stale by the time a caller reads it.
static void test_a_scene_recovery_saved_needs_no_save() {
    json saved = {{"scene_saved", false},
                  {"recovery", {{"operation", {{"outcome", "completed_saved_files_checkpointed"}}}}}};
    ASSERT_TRUE(followUpsFor(saved).empty());
    // A protection that failed saved nothing, so the save still stands.
    json failed = {{"scene_saved", false},
                   {"recovery", {{"operation", {{"outcome", "applied_persistence_failed"}}}}}};
    ASSERT_EQ(followUpsFor(failed).size(), 1u);
}

// Restarts: a change the attached editor keeps its old state for, and project
// files written with no editor to read them.
static void test_a_change_the_editor_cannot_take_names_the_restart() {
    const auto autoload = followUpsFor({{"execution_mode", "live"}, {"persisted", true},
                                        {"requires_editor_restart", true}});
    ASSERT_EQ(autoload.size(), 1u);
    ASSERT_EQ(autoload[0]["work"], "restart");
    // Nothing Didi sends restarts an editor, so the step names no tool.
    ASSERT_FALSE(autoload[0].contains("tool"));

    const auto offline_setting = followUpsFor({{"execution_mode", "offline_fallback"},
                                               {"written_to", "res://project.godot"},
                                               {"persisted", true}});
    ASSERT_EQ(offline_setting.size(), 1u);
    ASSERT_EQ(offline_setting[0]["work"], "restart");
    // Through an attached editor the editor saves the file itself.
    ASSERT_TRUE(followUpsFor({{"execution_mode", "live"}, {"written_to", "res://project.godot"}})
                    .empty());

    const auto preset = followUpsFor({{"written_to", "res://export_presets.cfg"},
                                      {"editor_reloaded", false}});
    ASSERT_EQ(preset.size(), 1u);
    ASSERT_EQ(preset[0]["work"], "restart");
    ASSERT_TRUE(followUpsFor({{"written_to", "res://export_presets.cfg"}, {"editor_reloaded", true}})
                    .empty());
}

// A site that named its own follow-up is left exactly as it said it, and an
// answer that is not an object has nothing to add to.
static void test_an_answer_that_names_its_own_is_left_alone() {
    json own = {{"scene_saved", false}, {"follow_up", json::array({{{"work", "restart"}}})}};
    ASSERT_FALSE(didi::mcp::applyFollowUps(own));
    ASSERT_EQ(own["follow_up"].size(), 1u);
    ASSERT_EQ(own["follow_up"][0]["work"], "restart");

    json plain = {{"status", "success"}};
    ASSERT_FALSE(didi::mcp::applyFollowUps(plain));
    ASSERT_FALSE(plain.contains("follow_up"));

    json list = json::array({1, 2});
    ASSERT_FALSE(didi::mcp::applyFollowUps(list));
}

// Met the way a caller meets it: an offline setting write through the registry
// answers with its restart, in the text and in structuredContent alike.
static void test_an_offline_setting_write_names_its_restart() {
    ScopedProject project("offline-setting");
    std::ofstream("project.godot") << "config_version=5" << std::endl;
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    const auto result = registry.callTool(
        "project_set_setting", {{"setting", "application/config/name"}, {"value", "Follow"}});
    ASSERT_FALSE(result.isError);
    const auto text = json::parse(result.content[0].text);
    ASSERT_EQ(text["execution_mode"], "offline_fallback");
    ASSERT_EQ(text["follow_up"].size(), 1u);
    ASSERT_EQ(text["follow_up"][0]["work"], "restart");
    ASSERT_TRUE(result.structuredContent.has_value());
    ASSERT_EQ((*result.structuredContent)["follow_up"], text["follow_up"]);

    // A read of the same file leaves nothing undone.
    const auto read = registry.callTool("project_get_setting", {{"setting", "application/config/name"}});
    ASSERT_FALSE(read.isError);
    ASSERT_FALSE(json::parse(read.content[0].text).contains("follow_up"));
}

struct RegisterFollowUpTests {
    RegisterFollowUpTests() {
        registerTest("FollowUps.UnsavedSceneNamesTheSave", test_an_unsaved_scene_names_the_save);
        registerTest("FollowUps.RecoverySavedNeedsNoSave", test_a_scene_recovery_saved_needs_no_save);
        registerTest("FollowUps.EditorKeepsOldStateNamesTheRestart",
                     test_a_change_the_editor_cannot_take_names_the_restart);
        registerTest("FollowUps.OwnFollowUpLeftAlone", test_an_answer_that_names_its_own_is_left_alone);
        registerTest("FollowUps.OfflineSettingWriteNamesItsRestart",
                     test_an_offline_setting_write_names_its_restart);
    }
} g_register_follow_up_tests;
