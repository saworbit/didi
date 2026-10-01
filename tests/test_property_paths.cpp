// The typed object layer's engine-independent rules (Q7 in docs/BUILD_QUEUE.md).
//
// scene_get_property and scene_set_property reach a property by path and take a
// batch. What is asserted here is everything that can be decided from strings:
// how a path is spelled, which file keeps a change inside a sub-resource, which
// writes are refused, and which two writes cannot share one undo step. The
// engine half -- that a batch undoes as one step, and what a save keeps -- is
// asserted by the live harness on every engine line.

#include "didi/gdextension/property_paths.hpp"
#include "didi/mcp/tool_registry.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using namespace didi::godot;

void test_a_path_is_split_at_colons_and_a_slash_belongs_to_a_name() {
    const auto plain = parsePropertyPath("position");
    ASSERT_TRUE(plain.problem.empty());
    ASSERT_EQ(plain.steps, std::vector<std::string>({"position"}));

    const auto theme = parsePropertyPath("theme_override_styles/panel:bg_color");
    ASSERT_TRUE(theme.problem.empty());
    ASSERT_EQ(theme.steps, std::vector<std::string>({"theme_override_styles/panel", "bg_color"}));

    const auto shader = parsePropertyPath("material:shader_parameter/tint");
    ASSERT_EQ(shader.steps, std::vector<std::string>({"material", "shader_parameter/tint"}));

    const auto deep = parsePropertyPath("material_override:next_pass:albedo_color");
    ASSERT_EQ(deep.steps.size(), 3u);
}

void test_a_path_with_an_empty_step_is_refused_not_tidied() {
    // Each of these means something different in a NodePath, and none of them
    // is a property.
    for (const auto* path : {"", ":position", "position:", "material::albedo_color"}) {
        const auto parsed = parsePropertyPath(path);
        ASSERT_FALSE(parsed.problem.empty());
        ASSERT_TRUE(parsed.steps.empty());
    }
    // A name the NodePath would change on the way through is refused too, so a
    // step is never checked under one name and written under another.
    for (const auto* path : {"/position", "theme_override_styles/:bg_color", "a//b"}) {
        const auto parsed = parsePropertyPath(path);
        ASSERT_FALSE(parsed.problem.empty());
        ASSERT_TRUE(parsed.problem.find("slash") != std::string::npos);
    }
}

void test_a_path_is_capped_in_depth_and_length() {
    ASSERT_TRUE(parsePropertyPath("a:b:c:d:e:f:g:h").problem.empty());
    const auto nine = parsePropertyPath("a:b:c:d:e:f:g:h:i");
    ASSERT_FALSE(nine.problem.empty());
    ASSERT_TRUE(nine.problem.find("8 steps") != std::string::npos);
    const auto long_path = parsePropertyPath(std::string(kMaxPropertyPathLength + 1, 'a'));
    ASSERT_FALSE(long_path.problem.empty());
}

// The table measured on 4.5.1, 4.6.2 and 4.7.2, which the header records.
void test_the_file_that_keeps_a_resource_is_the_part_before_the_separator() {
    const std::string edited = "res://main.tscn";
    ASSERT_TRUE(resourceHome("", edited).home == ResourceHome::EditedScene);
    ASSERT_TRUE(resourceHome("res://main.tscn::StyleBoxFlat_a", edited).home == ResourceHome::EditedScene);
    ASSERT_TRUE(resourceHome("res://main.tscn::StyleBoxFlat_a", edited).file.empty());

    const auto instanced = resourceHome("res://child.tscn::StyleBoxFlat_c", edited);
    ASSERT_TRUE(instanced.home == ResourceHome::OtherScene);
    ASSERT_EQ(instanced.file, "res://child.tscn");
    ASSERT_TRUE(resourceHome("res://level.scn::Material_1", edited).home == ResourceHome::OtherScene);

    const auto external = resourceHome("res://shared_box.tres", edited);
    ASSERT_TRUE(external.home == ResourceHome::ResourceFile);
    ASSERT_EQ(external.file, "res://shared_box.tres");
    const auto nested = resourceHome("res://outer.tres::StandardMaterial3D_inner", edited);
    ASSERT_TRUE(nested.home == ResourceHome::ResourceFile);
    ASSERT_EQ(nested.file, "res://outer.tres");
    ASSERT_TRUE(resourceHome("res://Box.TRES", edited).home == ResourceHome::ResourceFile);
    ASSERT_TRUE(resourceHome("res://data.res", edited).home == ResourceHome::ResourceFile);

    // An imported asset, a shader file and a script are not saved from a scene.
    for (const auto* path : {"res://model.glb::Material_x", "res://icon.png", "res://glow.gdshader",
                             "res://player.gd"}) {
        ASSERT_TRUE(resourceHome(path, edited).home == ResourceHome::NotSaved);
    }
}

void test_a_scene_not_yet_saved_owns_only_its_unsaved_resources() {
    // A new scene has no path, so nothing with one can be its own.
    ASSERT_TRUE(resourceHome("", "").home == ResourceHome::EditedScene);
    ASSERT_TRUE(resourceHome("res://main.tscn::X", "").home == ResourceHome::OtherScene);
}

void test_the_excluded_writes_are_a_short_list_with_reasons() {
    for (const auto* property : {"owner", "scene_file_path"}) {
        const auto reason = excludedPropertyWrite(property, true);
        ASSERT_TRUE(reason.has_value());
        ASSERT_FALSE(reason->empty());
        // Only a node has these, so a resource property of the same name is
        // not the same thing.
        ASSERT_FALSE(excludedPropertyWrite(property, false).has_value());
    }
    ASSERT_TRUE(excludedPropertyWrite("resource_path", false).has_value());
    ASSERT_FALSE(excludedPropertyWrite("resource_path", true).has_value());
    for (const auto* property : {"position", "name", "bg_color", "shader_parameter/tint", "script"}) {
        ASSERT_FALSE(excludedPropertyWrite(property, true).has_value());
        ASSERT_FALSE(excludedPropertyWrite(property, false).has_value());
    }
    ASSERT_EQ(toolForExcludedWrite("owner"), "scene_reparent_node");
    ASSERT_EQ(toolForExcludedWrite("scene_file_path"), "scene_instantiate_node");
    ASSERT_TRUE(toolForExcludedWrite("resource_path").empty());
}

void test_two_writes_to_one_property_or_one_inside_another_overlap() {
    const auto overlap = overlappingWrites({
        {"1", {"position"}},
        {"2", {"theme_override_styles/panel"}},
        {"2", {"theme_override_styles/panel", "bg_color"}},
    });
    ASSERT_TRUE(overlap.has_value());
    ASSERT_EQ(overlap->first, 1u);
    ASSERT_EQ(overlap->second, 2u);

    const auto same = overlappingWrites({{"1", {"modulate"}}, {"1", {"modulate"}}});
    ASSERT_TRUE(same.has_value());
    ASSERT_EQ(same->first, 0u);
    ASSERT_EQ(same->second, 1u);
}

void test_siblings_and_other_nodes_do_not_overlap() {
    ASSERT_FALSE(overlappingWrites({
        {"1", {"theme_override_styles/panel", "bg_color"}},
        {"1", {"theme_override_styles/panel", "border_color"}},
        {"2", {"theme_override_styles/panel", "bg_color"}},
        {"1", {"material", "shader_parameter/tint"}},
    }).has_value());
    ASSERT_FALSE(overlappingWrites({}).has_value());
    ASSERT_FALSE(overlappingWrites({{"1", {"position"}}}).has_value());
}

void test_a_declared_constraint_is_relayed_in_the_engines_own_words() {
    const auto range = declaredConstraint(1, "0,100,1,or_greater");
    ASSERT_TRUE(range.has_value());
    ASSERT_EQ((*range)["kind"], "range");
    ASSERT_EQ((*range)["hint_string"], "0,100,1,or_greater");
    ASSERT_EQ((*declaredConstraint(2, "Master,Music"))["kind"], "enum");
    ASSERT_EQ((*declaredConstraint(3, "a,b"))["kind"], "enum_suggestion");
    ASSERT_EQ((*declaredConstraint(17, "StyleBox"))["kind"], "resource_type");
    // An editor affordance says nothing about which values fit.
    ASSERT_FALSE(declaredConstraint(0, "").has_value());
    ASSERT_FALSE(declaredConstraint(18, "multiline").has_value());
    // A hint with nothing in it constrains nothing.
    ASSERT_FALSE(declaredConstraint(1, "").has_value());
}

void test_candidates_put_the_likely_name_first() {
    const std::vector<std::string> names = {"content_margin_left", "bg_color", "border_color",
                                            "border_width_left", "corner_radius_top_left",
                                            "draw_center", "skew"};
    const auto exact = propertyNameCandidates("BG_Color", names, 3);
    ASSERT_EQ(exact.front(), "bg_color");
    ASSERT_EQ(exact.size(), 3u);

    const auto contained = propertyNameCandidates("color", names, 10);
    ASSERT_EQ(contained[0], "bg_color");
    ASSERT_EQ(contained[1], "border_color");

    // Sharing a first word ranks above an unrelated name.
    const auto word = propertyNameCandidates("border_size", names, 10);
    ASSERT_EQ(word[0], "border_color");
    ASSERT_EQ(word[1], "border_width_left");
    ASSERT_EQ(word.size(), names.size());

    ASSERT_TRUE(propertyNameCandidates("x", names, 0).empty());
}

const json* propertyOf(const json& schema, const char* name) {
    if (!schema.contains("properties") || !schema["properties"].contains(name)) return nullptr;
    return &schema["properties"][name];
}

void test_both_tools_publish_a_closed_typed_batch() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* set = registry.getTool("scene_set_property");
    const auto* get = registry.getTool("scene_get_property");
    ASSERT_TRUE(set != nullptr && get != nullptr);

    // Either the single form or the batch, so neither is required by schema.
    ASSERT_FALSE(set->inputSchema.contains("required"));
    ASSERT_FALSE(get->inputSchema.contains("required"));

    const auto* writes = propertyOf(set->inputSchema, "writes");
    ASSERT_TRUE(writes != nullptr);
    ASSERT_EQ((*writes)["type"], "array");
    ASSERT_EQ((*writes)["maxItems"], 64);
    const auto& item = (*writes)["items"];
    ASSERT_EQ(item["additionalProperties"], false);
    ASSERT_EQ(item["required"], json::array({"target_node", "property_name", "value"}));
    // A batch item takes exactly the value spellings a single write does.
    ASSERT_EQ(item["properties"]["value"]["type"], (*propertyOf(set->inputSchema, "value"))["type"]);

    const auto* reads = propertyOf(get->inputSchema, "reads");
    ASSERT_TRUE(reads != nullptr);
    ASSERT_EQ((*reads)["items"]["required"], json::array({"target_node", "property_name"}));
    ASSERT_EQ((*reads)["items"]["additionalProperties"], false);

    // The single form keeps its bounds now that it is not required: a path
    // takes as much room as a node path.
    for (const auto* tool : {set, get}) {
        ASSERT_EQ((*propertyOf(tool->inputSchema, "property_name"))["minLength"], 1);
        ASSERT_EQ((*propertyOf(tool->inputSchema, "property_name"))["maxLength"], 1024);
        ASSERT_EQ((*propertyOf(tool->inputSchema, "target_node"))["minLength"], 1);
    }
}

std::string errorMessage(const didi::mcp::CallToolResult& result) {
    return json::parse(result.content[0].text)["error"]["message"].get<std::string>();
}

json errorData(const didi::mcp::CallToolResult& result) {
    return json::parse(result.content[0].text)["error"]["data"];
}

void test_a_batch_is_refused_whole_before_any_engine_is_asked() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);

    // Both forms at once: which one was meant is not guessed.
    const auto both = registry.callTool(
        "scene_set_property",
        {{"target_node", "Player"}, {"property_name", "position"}, {"value", json{{"x", 1}, {"y", 2}}},
         {"writes", json::array({{{"target_node", "Player"}, {"property_name", "visible"}, {"value", false}}})}});
    ASSERT_TRUE(both.isError);
    ASSERT_TRUE(errorMessage(both).find("not both") != std::string::npos);
    ASSERT_EQ(errorData(both)["field"], "writes");

    const auto empty_name = registry.callTool(
        "scene_set_property",
        {{"writes", json::array({{{"target_node", "Player"}, {"property_name", ""}, {"value", 1}}})}});
    ASSERT_TRUE(empty_name.isError);
    ASSERT_EQ(errorData(empty_name)["index"], 0);
    ASSERT_EQ(errorData(empty_name)["field"], "writes");

    // The item schema is closed, and the validator reads it.
    const auto extra = registry.callTool(
        "scene_set_property",
        {{"writes", json::array({{{"target_node", "Player"}, {"property_name", "visible"},
                                  {"value", false}, {"dry_run", true}}})}});
    ASSERT_TRUE(extra.isError);
    ASSERT_TRUE(errorMessage(extra).find("dry_run") != std::string::npos);

    const auto missing_value = registry.callTool(
        "scene_set_property",
        {{"writes", json::array({{{"target_node", "Player"}, {"property_name", "visible"}}})}});
    ASSERT_TRUE(missing_value.isError);
    ASSERT_TRUE(errorMessage(missing_value).find("value") != std::string::npos);

    json too_many = json::array();
    for (int index = 0; index < 65; ++index) {
        too_many.push_back({{"target_node", "Player"}, {"property_name", "visible"}, {"value", true}});
    }
    const auto over = registry.callTool("scene_set_property", {{"writes", too_many}});
    ASSERT_TRUE(over.isError);
    ASSERT_TRUE(errorMessage(over).find("64") != std::string::npos);

    const auto reads_and_single = registry.callTool(
        "scene_get_property",
        {{"target_node", "Player"},
         {"reads", json::array({{{"target_node", "Player"}, {"property_name", "visible"}}})}});
    ASSERT_TRUE(reads_and_single.isError);
    ASSERT_EQ(errorData(reads_and_single)["field"], "reads");

    // Nothing to send at all names both forms.
    const auto nothing = registry.callTool("scene_set_property", json::object());
    ASSERT_TRUE(nothing.isError);
    ASSERT_TRUE(errorMessage(nothing).find("'writes'") != std::string::npos);
}

void test_a_well_formed_batch_reaches_the_engine_check() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    registry.setIpcClient(nullptr);
    // With no editor the batch passes every argument check and stops where a
    // single write does: there is nothing to write to.
    const auto offline = registry.callTool(
        "scene_set_property",
        {{"writes", json::array({{{"target_node", "HUD/Panel"},
                                  {"property_name", "theme_override_styles/panel:bg_color"},
                                  {"value", "#203040"}},
                                 {{"target_node", "Player"}, {"property_name", "visible"},
                                  {"value", false}}})}});
    ASSERT_TRUE(offline.isError);
    ASSERT_EQ(errorData(offline)["code"], "not_connected");

    const auto read = registry.callTool(
        "scene_get_property",
        {{"reads", json::array({{{"target_node", "Player"}, {"property_name", "position"}}})}});
    ASSERT_TRUE(read.isError);
    ASSERT_EQ(errorData(read)["code"], "not_connected");
}

struct RegisterPropertyPathTests {
    RegisterPropertyPathTests() {
        registerTest("PropertyPaths.SplitAtColonsSlashBelongsToName",
                     test_a_path_is_split_at_colons_and_a_slash_belongs_to_a_name);
        registerTest("PropertyPaths.EmptyStepRefusedNotTidied",
                     test_a_path_with_an_empty_step_is_refused_not_tidied);
        registerTest("PropertyPaths.CappedInDepthAndLength", test_a_path_is_capped_in_depth_and_length);
        registerTest("PropertyPaths.FileThatKeepsAResource",
                     test_the_file_that_keeps_a_resource_is_the_part_before_the_separator);
        registerTest("PropertyPaths.UnsavedSceneOwnsOnlyUnsavedResources",
                     test_a_scene_not_yet_saved_owns_only_its_unsaved_resources);
        registerTest("PropertyPaths.ExcludedWritesHaveReasons",
                     test_the_excluded_writes_are_a_short_list_with_reasons);
        registerTest("PropertyPaths.OverlappingWrites",
                     test_two_writes_to_one_property_or_one_inside_another_overlap);
        registerTest("PropertyPaths.SiblingsDoNotOverlap", test_siblings_and_other_nodes_do_not_overlap);
        registerTest("PropertyPaths.DeclaredConstraint",
                     test_a_declared_constraint_is_relayed_in_the_engines_own_words);
        registerTest("PropertyPaths.CandidatesLikelyFirst", test_candidates_put_the_likely_name_first);
        registerTest("PropertyPaths.ClosedTypedBatchSchemas", test_both_tools_publish_a_closed_typed_batch);
        registerTest("PropertyPaths.BatchRefusedWholeBeforeEngine",
                     test_a_batch_is_refused_whole_before_any_engine_is_asked);
        registerTest("PropertyPaths.WellFormedBatchReachesEngineCheck",
                     test_a_well_formed_batch_reaches_the_engine_check);
    }
} g_register_property_path_tests;

}  // namespace
