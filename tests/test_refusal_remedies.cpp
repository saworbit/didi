// Refusal remedy tests (Q6 in docs/BUILD_QUEUE.md, #1040).
//
// Every refusal names the argument or the call that fixes it. A site that knows
// says so; the error floor fills the rest from src/mcp/refusal_remedies.cpp by
// code and tool. What is asserted here is the floor's half: that it fills a
// remedy where the site gave none, leaves a site's own alone, and gives a fault
// none. The census of codes is tests/test_refusal_remedies.py.
//
// It only works on an envelope. The second half asserts that a failure a caller
// meets is one, including at the sites that used to answer a bare sentence.

#include "didi/mcp/error_data.hpp"
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

json floored(int status, const std::string& message, const json& data, const std::string& tool) {
    json error = {{"code", status}, {"message", message}};
    if (!data.is_null()) error["data"] = data;
    didi::mcp::applyErrorDataFloor(error, tool, tool);
    return error["data"];
}

// The error a failed result carries, or a throw naming what it carried instead.
json envelopeOf(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(result.isError);
    ASSERT_EQ(result.content.size(), 1u);
    const auto parsed = json::parse(result.content[0].text, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("error") || !parsed["error"].is_object()) {
        throw std::runtime_error("A failure answered without an envelope: " + result.content[0].text);
    }
    return parsed["error"];
}

class ScopedProject final {
public:
    explicit ScopedProject(const std::string& suffix)
        : m_original(std::filesystem::current_path()),
          m_root(m_original / "build" / "test-projects" /
                 ("didi-refusal-" + suffix + "-" +
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

static void test_a_refusal_with_no_remedy_gets_the_one_its_code_carries() {
    // No code at all: the status names it, and the code names the fix.
    const auto offline = floored(503, "No runtime session is attached", nullptr, "scene_get_property");
    ASSERT_EQ(offline["code"], "not_connected");
    ASSERT_EQ(offline["next_call"]["tool"], "runtime_list_sessions");
    ASSERT_TRUE(offline["next_call"]["reason"].is_string());

    // A busy state: the same call, later.
    const auto busy = floored(409, "Another reimport is running", {{"code", "asset_reimport_active"}},
                              "asset_reimport");
    ASSERT_EQ(busy["retry_after_ms"], 1000);

    // The argument the schema check quoted.
    const auto missing = floored(400, "Missing required argument 'target_node'.", nullptr,
                                 "scene_get_property");
    ASSERT_EQ(missing["field"], "target_node");
}

// One code, different fixes, by the tool that refused.
static void test_the_remedy_follows_the_tool() {
    ASSERT_EQ(floored(404, "No such node", nullptr, "scene_get_property")["next_call"]["tool"],
              "scene_get_hierarchy");
    ASSERT_EQ(floored(404, "No such key", nullptr, "blackboard_read")["next_call"]["tool"],
              "blackboard_list_keys");
    ASSERT_EQ(floored(404, "No such preset", nullptr, "project_export")["next_call"]["tool"],
              "project_list_export_presets");
    ASSERT_EQ(floored(409, "exists", {{"code", "already_exists"}}, "scene_create")["retry_with"],
              json({{"overwrite", true}}));
    ASSERT_EQ(floored(409, "exists", {{"code", "already_exists"}}, "project_set_autoload")["retry_with"],
              json({{"replace", true}}));
    ASSERT_EQ(floored(409, "wrong kind", {{"code", "session_kind_rejected"}}, "runtime_step")
                  ["next_call"]["tool"],
              "runtime_launch");
}

// A missing thing that is not a node is found where that kind of thing is
// listed. Every not_found outside the tools with a list of their own used to
// name scene_get_hierarchy, so an evicted capture or a missing script file was
// sent to read the open scene's node paths (#1117).
static void test_a_not_found_names_the_list_that_holds_what_was_missing() {
    const auto next = [](const std::string& message, const std::string& tool) {
        const auto data = floored(404, message, nullptr, tool);
        return data.contains("next_call") ? data["next_call"]["tool"].get<std::string>() : std::string();
    };
    ASSERT_EQ(next("Baseline capture ID is missing or has been evicted", "viewport_diff_capture"),
              "viewport_capture_frame");
    ASSERT_EQ(next("Script resource not found: res://missing.gd", "script_attach_to_node"),
              "project_list_resources");
    ASSERT_EQ(next("PackedScene not found: res://missing.tscn", "scene_instantiate_node"),
              "project_list_resources");
    ASSERT_EQ(next("No resource at res://missing.tres. Write the library first.", "anim_add_library"),
              "project_list_resources");
    ASSERT_EQ(next("Autoload resource not found: res://missing.gd", "project_set_autoload"),
              "project_list_resources");
    ASSERT_EQ(next("Autoload not found: Missing", "project_remove_autoload"), "project_list_autoloads");
    ASSERT_EQ(next("AnimationPlayer has no animation named walk", "anim_play_track"), "anim_list_tracks");
    ASSERT_EQ(next("The shader declares no uniform named tint", "shader_set_uniform"),
              "shader_list_uniforms");
    // The project_file argument is the fix, and nothing to call first.
    const auto build = floored(404, "No .sln or .csproj exists at the project root", nullptr,
                               "csharp_check_build");
    ASSERT_EQ(build["field"], "project_file");
    ASSERT_FALSE(build.contains("next_call"));
    // A preview that is gone is gone, and clearing it again cannot help.
    const auto preview = floored(404, "No preview with id abc", nullptr, "editor_clear_ghost_previews");
    ASSERT_TRUE(preview["no_remedy"].is_string());
    ASSERT_FALSE(preview.contains("next_call"));
    // A node the same tools could not find is still read off the open scene.
    ASSERT_EQ(next("Target node not found: /root/Main/Missing", "script_attach_to_node"),
              "scene_get_hierarchy");
    ASSERT_EQ(next("Parent node not found: Missing", "scene_instantiate_node"), "scene_get_hierarchy");
}

// A site that already said what fixes it is left exactly as it said it.
static void test_a_site_remedy_is_left_alone() {
    const json own = {{"code", "already_exists"}, {"retry_with", {{"replace", true}}}};
    const auto kept = floored(409, "exists", own, "scene_create");
    ASSERT_EQ(kept["retry_with"], json({{"replace", true}}));
    ASSERT_FALSE(kept.contains("next_call"));
    // A site's own fix stands even where its code's rule would give a different
    // kind: not_connected carries a next_call, and a site that said retry_with
    // gets none added.
    const auto own_retry = floored(503, "offline", {{"retry_with", {{"session_id", "a"}}}},
                                   "scene_get_property");
    ASSERT_FALSE(own_retry.contains("next_call"));
    // Naming the argument counts, under any of the names the surface uses.
    for (const char* key : {"field", "argument", "parameter", "missing", "did_you_mean"}) {
        const auto named = floored(404, "gone", {{key, "asset_path"}}, "asset_configure_import");
        ASSERT_FALSE(named.contains("next_call"));
    }
}

// A fault is the server's, and nothing the caller sends fixes it.
static void test_a_fault_names_no_fix_and_a_dead_end_says_so() {
    const auto fault = floored(500, "boom", nullptr, "scene_get_property");
    ASSERT_EQ(fault["code"], "internal_error");
    ASSERT_FALSE(didi::mcp::hasRemedy(fault));
    // Even a code the table has a rule for is not remedied as a fault.
    ASSERT_FALSE(didi::mcp::hasRemedy(
        floored(500, "boom", {{"code", "not_found"}}, "scene_get_property")));
    // 503 and 504 are states the caller can act on, not faults.
    ASSERT_TRUE(didi::mcp::hasRemedy(floored(504, "late", nullptr, "scene_get_property")));
    // A refusal nothing can fix says why, so it is not retried.
    const auto empty = floored(409, "Nothing to undo", {{"code", "nothing_to_undo"}}, "editor_undo");
    ASSERT_TRUE(empty["no_remedy"].is_string());
    // And a code the table does not know gets nothing invented for it.
    const auto unknown = floored(409, "Something", {{"code", "not_a_real_code"}}, "scene_get_property");
    ASSERT_FALSE(didi::mcp::hasRemedy(unknown));
}

// The manifest publishes the table, which is what the census reads.
static void test_the_manifest_publishes_the_table() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto refusals = registry.buildManifest().toJson()["refusals"];
    ASSERT_TRUE(refusals["remedied"].is_array());
    ASSERT_TRUE(refusals["remedied"].size() > 60);
    ASSERT_TRUE(refusals["without_remedy"].contains("internal_error"));
    for (const auto& code : refusals["remedied"]) {
        ASSERT_FALSE(refusals["without_remedy"].contains(code.get<std::string>()));
    }
}

// The builders a tool answers a failure with. The one that made a bare sentence
// is private, so these are all there is.
static void test_every_failure_builder_makes_an_envelope() {
    using didi::mcp::CallToolResult;
    const auto offline = envelopeOf(CallToolResult::notConnected("Launch Godot."));
    ASSERT_EQ(offline["code"], 503);
    ASSERT_EQ(offline["message"], "Launch Godot.");
    // The shape the registry's own refusal of a live-only tool carries.
    ASSERT_EQ(offline["data"]["retryable"], true);
    ASSERT_EQ(offline["data"]["blocked_on"], "no_live_session");
    ASSERT_EQ(offline["data"]["needs_live_engine"], true);

    // A site's own fix and its own values stand beside those defaults.
    const auto scoped = envelopeOf(
        CallToolResult::notConnected("x", {{"field", "root_path"}, {"retryable", false}}));
    ASSERT_EQ(scoped["data"]["field"], "root_path");
    ASSERT_EQ(scoped["data"]["retryable"], false);
    ASSERT_EQ(scoped["data"]["blocked_on"], "no_live_session");

    // An upstream error keeps its status and its data behind the tool's
    // sentence. Prefixing its message by hand is how these lost both.
    const auto upstream = envelopeOf(CallToolResult::fromError(
        didi::Error(409, "busy", {{"code", "editor_import_busy"}}), "Failed to reimport assets: "));
    ASSERT_EQ(upstream["code"], 409);
    ASSERT_EQ(upstream["message"], "Failed to reimport assets: busy");
    ASSERT_EQ(upstream["data"]["code"], "editor_import_busy");
}

// Failures that answered a bare sentence, met the way a caller meets them:
// through the registry, with the floor applied, and no editor attached.
static void test_former_plain_text_failures_name_their_fix() {
    ScopedProject project("envelopes");
    // A project with no run/main_scene.
    std::ofstream("project.godot") << "config_version=5" << std::endl;
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // Offline this reads a .tscn, and a node path needs an editor.
    const auto node_path =
        envelopeOf(registry.callTool("scene_get_hierarchy", {{"root_path", "Player"}}));
    ASSERT_EQ(node_path["code"], 503);
    ASSERT_EQ(node_path["data"]["code"], "not_connected");
    ASSERT_EQ(node_path["data"]["field"], "root_path");
    ASSERT_EQ(node_path["data"]["tool"], "scene_get_hierarchy");

    // No root_path, and no main scene to stand in for one.
    const auto no_main = envelopeOf(registry.callTool("scene_get_hierarchy", json::object()));
    ASSERT_EQ(no_main["data"]["code"], "not_connected");
    ASSERT_EQ(no_main["data"]["field"], "root_path");

    // A rule the schema cannot state. The sentence names the argument, and the
    // floor reads it back out.
    const auto both = envelopeOf(registry.callTool(
        "project_set_setting",
        {{"setting", "application/config/name"}, {"value", "x"}, {"remove", true}}));
    ASSERT_EQ(both["code"], 400);
    ASSERT_EQ(both["data"]["code"], "invalid_arguments");
    ASSERT_EQ(both["data"]["field"], "value");

    // A name no registration carries, which only an in-process caller reaches.
    const auto unknown = envelopeOf(registry.callTool("no_such_tool", json::object()));
    ASSERT_EQ(unknown["code"], 404);
    ASSERT_EQ(unknown["data"]["code"], "not_found");
    ASSERT_TRUE(unknown["data"]["no_remedy"].is_string());
}

// The arguments as a whole name no argument of their own. The floor names the
// fix for the two ways they are wrong.
static void test_the_arguments_as_a_whole_have_a_fix() {
    ASSERT_EQ(floored(400, "Invalid audio request: this tool takes no arguments", nullptr,
                      "audio_list_buses")["retry_with"],
              json::object());
    ASSERT_EQ(floored(400, "Export preset list arguments must be an empty object", nullptr,
                      "project_list_export_presets")["retry_with"],
              json::object());
    ASSERT_EQ(floored(400, "arguments must be an object", nullptr, "blackboard_write")["field"],
              "arguments");
    // A sentence that names a real argument is still about that argument.
    ASSERT_EQ(floored(400, "path is required", nullptr, "blackboard_write")["field"], "path");
}

struct RegisterRefusalRemedyTests {
    RegisterRefusalRemedyTests() {
        registerTest("RefusalRemedies.CodeCarriesItsRemedy",
                     test_a_refusal_with_no_remedy_gets_the_one_its_code_carries);
        registerTest("RefusalRemedies.RemedyFollowsTheTool", test_the_remedy_follows_the_tool);
        registerTest("RefusalRemedies.NotFoundNamesWhereItIsListed",
                     test_a_not_found_names_the_list_that_holds_what_was_missing);
        registerTest("RefusalRemedies.SiteRemedyLeftAlone", test_a_site_remedy_is_left_alone);
        registerTest("RefusalRemedies.FaultNamesNoFix",
                     test_a_fault_names_no_fix_and_a_dead_end_says_so);
        registerTest("RefusalRemedies.ManifestPublishesTheTable",
                     test_the_manifest_publishes_the_table);
        registerTest("RefusalRemedies.EveryBuilderMakesAnEnvelope",
                     test_every_failure_builder_makes_an_envelope);
        registerTest("RefusalRemedies.FormerPlainTextNamesItsFix",
                     test_former_plain_text_failures_name_their_fix);
        registerTest("RefusalRemedies.ArgumentsAsAWholeHaveAFix",
                     test_the_arguments_as_a_whole_have_a_fix);
    }
} g_register_refusal_remedy_tests;
