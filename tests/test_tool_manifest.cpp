// Tool manifest tests.
//
// These assert structural invariants of the registered surface rather than
// magic counts. The counts themselves live in exactly one place -- the
// registry -- and documentation is checked against the generated manifest by
// tools/validate_documentation.py.

#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/mcp/mutation_safety.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

didi::mcp::ToolManifest freshManifest() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    return registry.buildManifest();
}

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

} // namespace

// Every declared legacy name is actually registered and actually marked legacy.
// Without this, a legacy name could be silently dropped or silently promoted.
static void test_manifest_declared_legacy_names_are_registered_and_marked() {
    const auto manifest = freshManifest();
    for (const char* name : didi::mcp::kLegacyToolNames) {
        ASSERT_TRUE(contains(manifest.legacy, std::string(name)));
        ASSERT_TRUE(!contains(manifest.canonical, std::string(name)));
    }
    ASSERT_EQ(manifest.legacy.size(), didi::mcp::kLegacyToolNames.size());
}

// Nothing outside the declared list may claim legacy status.
static void test_manifest_no_undeclared_legacy_tools() {
    const auto manifest = freshManifest();
    std::unordered_set<std::string> declared;
    for (const char* name : didi::mcp::kLegacyToolNames) declared.insert(name);
    for (const auto& name : manifest.legacy) {
        ASSERT_TRUE(declared.count(name) == 1);
    }
}

// canonical and legacy partition the registered surface: no overlap, no gap.
static void test_manifest_partitions_the_surface() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto manifest = registry.buildManifest();
    const auto all = registry.listTools();

    ASSERT_EQ(manifest.canonical.size() + manifest.legacy.size(), all.size());

    std::unordered_set<std::string> seen;
    for (const auto& n : manifest.canonical) ASSERT_TRUE(seen.insert(n).second);
    for (const auto& n : manifest.legacy)    ASSERT_TRUE(seen.insert(n).second);
    ASSERT_EQ(seen.size(), all.size());
}

// implemented and unimplemented partition the canonical surface.
static void test_manifest_implemented_partitions_canonical() {
    const auto manifest = freshManifest();
    ASSERT_EQ(manifest.implemented.size() + manifest.unimplemented.size(),
              manifest.canonical.size());
    for (const auto& n : manifest.implemented) {
        ASSERT_TRUE(!contains(manifest.unimplemented, n));
    }
}

// A registered-but-unimplemented tool must never claim to be implemented.
// This is the Phase 1 honesty rule, asserted structurally.
static void test_manifest_unimplemented_tools_are_not_implemented() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto manifest = registry.buildManifest();
    for (const auto& name : manifest.unimplemented) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(!tool->capability.implemented);
    }
    for (const auto& name : manifest.implemented) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(tool->capability.implemented);
    }
}

// The JSON form carries every count the documentation contract needs, and the
// counts agree with the name arrays they summarise.
static void test_manifest_json_counts_agree_with_names() {
    const auto manifest = freshManifest();
    const auto doc = manifest.toJson();

    ASSERT_EQ(doc["counts"]["canonical"].get<size_t>(), manifest.canonical.size());
    ASSERT_EQ(doc["counts"]["legacy"].get<size_t>(), manifest.legacy.size());
    ASSERT_EQ(doc["counts"]["implemented"].get<size_t>(), manifest.implemented.size());
    ASSERT_EQ(doc["counts"]["unimplemented"].get<size_t>(), manifest.unimplemented.size());
    ASSERT_EQ(doc["counts"]["total"].get<size_t>(),
              manifest.canonical.size() + manifest.legacy.size());

    ASSERT_EQ(doc["names"]["canonical"].size(), manifest.canonical.size());
    ASSERT_EQ(doc["names"]["legacy"].size(), manifest.legacy.size());
}

// Manifest name lists are sorted, so the generated artifact is stable across
// runs and platforms and can be diffed in CI.
static void test_manifest_names_are_sorted() {
    const auto manifest = freshManifest();
    ASSERT_TRUE(std::is_sorted(manifest.canonical.begin(), manifest.canonical.end()));
    ASSERT_TRUE(std::is_sorted(manifest.legacy.begin(), manifest.legacy.end()));
    ASSERT_TRUE(std::is_sorted(manifest.implemented.begin(), manifest.implemented.end()));
    ASSERT_TRUE(std::is_sorted(manifest.unimplemented.begin(), manifest.unimplemented.end()));
}

// --- Tool annotations and structured results -------------------------------
//
// Annotations are the specification's own way to tell a client which tools are
// safe to auto-approve. They are derived from the existing mutation
// classification rather than a second hand-maintained list, so they cannot
// drift from the dry-run and confirmation contracts.

static void test_read_only_tools_are_annotated_read_only() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("scene_get_hierarchy");
    ASSERT_TRUE(tool != nullptr);
    const auto annotations = tool->toJson()["annotations"];
    ASSERT_EQ(annotations["readOnlyHint"].get<bool>(), true);
    ASSERT_EQ(annotations["destructiveHint"].get<bool>(), false);
    ASSERT_EQ(annotations["idempotentHint"].get<bool>(), true);
    // Reading a scene never starts a subprocess, so this one really is closed.
    ASSERT_EQ(annotations["openWorldHint"].get<bool>(), false);
}

// Break caught: every tool advertised openWorldHint false, including the ones
// that start Godot or dotnet against the project. A client that uses the hint
// to decide what needs review would auto-approve a call that runs project code.
static void test_tools_that_run_project_code_are_open_world() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    for (const char* name : {"csharp_check_build", "shader_check_compile", "project_export",
                             "gridmap_export_mesh_library", "runtime_launch",
                             "script_check_syntax"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_EQ(tool->toJson()["annotations"]["openWorldHint"].get<bool>(), true);
    }

    // An alias promises what the canonical tool promises.
    for (const auto& pair : {std::make_pair("execute_test_session", "runtime_launch"),
                             std::make_pair("analyze_script_diagnostics", "script_check_syntax")}) {
        const auto* alias = registry.getTool(pair.first);
        ASSERT_TRUE(alias != nullptr);
        ASSERT_EQ(alias->toJson()["annotations"]["openWorldHint"].get<bool>(), true);
    }

    // Tools whose domain really is closed stay closed.
    for (const char* name : {"scene_get_hierarchy", "project_list_resources",
                             "script_get_symbols", "resource_create"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_EQ(tool->toJson()["annotations"]["openWorldHint"].get<bool>(), false);
    }
}

static void test_mutating_tools_are_not_annotated_read_only() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("scene_remove_node");
    ASSERT_TRUE(tool != nullptr);
    const auto annotations = tool->toJson()["annotations"];
    ASSERT_EQ(annotations["readOnlyHint"].get<bool>(), false);
    ASSERT_EQ(annotations["destructiveHint"].get<bool>(), true);
}

static void test_every_registered_tool_carries_annotations() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto& tool : registry.listTools()) {
        const auto definition = tool.toJson();
        ASSERT_TRUE(definition.contains("annotations"));
        for (const char* hint :
             {"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"}) {
            ASSERT_TRUE(definition["annotations"].contains(hint));
            ASSERT_TRUE(definition["annotations"][hint].is_boolean());
        }
    }
}

// The safety-critical invariant: a tool that can change the project must never
// be advertised as read-only, because clients use that hint to auto-approve.
static void test_no_mutation_is_ever_advertised_as_read_only() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto& tool : registry.listTools()) {
        // Classify through the resolved binding, the same way registerTool
        // does, so an alias is judged as the canonical tool it resolves to.
        const auto binding = didi::mcp::resolveAliasBinding(tool.name, didi::json::object());
        const bool is_mutation = didi::mcp::MutationSafety::isMutation(binding);
        const bool claims_read_only = tool.toJson()["annotations"]["readOnlyHint"].get<bool>();
        ASSERT_TRUE(!(is_mutation && claims_read_only));
        ASSERT_EQ(claims_read_only, !is_mutation);
    }
}

// A dry-run capable tool is by definition a mutation, so the two contracts must
// agree. This catches either one drifting from the other.
static void test_annotations_agree_with_the_dry_run_contract() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    size_t checked = 0;
    for (const auto& tool : registry.listTools()) {
        const auto definition = tool.toJson();
        const auto& schema = definition.at("inputSchema");
        if (!schema.is_object() || !schema.contains("properties")) continue;
        if (!schema.at("properties").contains("dry_run")) continue;
        ASSERT_EQ(definition["annotations"]["readOnlyHint"].get<bool>(), false);
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

static void test_tool_results_carry_structured_content() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("script_reflect_class", {{"class_name", "CharacterBody3D"}});
    ASSERT_TRUE(!result.isError);
    const auto encoded = result.toJson();
    ASSERT_TRUE(encoded.contains("structuredContent"));
    // Backwards compatibility: the same JSON must still appear as text for
    // clients that do not read structuredContent.
    ASSERT_EQ(encoded["content"][0]["type"].get<std::string>(), std::string("text"));
    ASSERT_EQ(didi::json::parse(encoded["content"][0]["text"].get<std::string>()),
              encoded["structuredContent"]);
}

static void test_error_results_do_not_carry_structured_content() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto result = registry.callTool("signal_connect", didi::json::object());
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(!result.toJson().contains("structuredContent"));
}

// --- Tool output schemas ---------------------------------------------------
//
// A declared outputSchema is a promise about the shape of structuredContent, so
// it is declared only for tools whose real output has been observed. Inferring
// a schema for a tool that cannot be exercised would assert a shape nobody has
// seen, which is the failure mode the capability metadata exists to prevent.

static void test_declared_output_schemas_are_well_formed() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    size_t declared = 0;
    for (const auto& tool : registry.listTools()) {
        const auto definition = tool.toJson();
        if (!definition.contains("outputSchema")) continue;
        ++declared;
        const auto& schema = definition.at("outputSchema");
        ASSERT_TRUE(schema.is_object());
        ASSERT_EQ(schema.at("type").get<std::string>(), std::string("object"));
        ASSERT_TRUE(schema.contains("properties"));
        ASSERT_TRUE(schema.at("properties").is_object());
        // Every Didi result identifies how it executed, so every schema must
        // require it. This is the field a client uses to tell a live result
        // from an offline fallback.
        ASSERT_TRUE(schema.contains("required"));
        bool requires_execution_mode = false;
        for (const auto& entry : schema.at("required")) {
            if (entry.get<std::string>() == "execution_mode") requires_execution_mode = true;
        }
        ASSERT_TRUE(requires_execution_mode);
        ASSERT_TRUE(schema.at("properties").contains("execution_mode"));
    }
    ASSERT_TRUE(declared > 0);
}

static void test_output_schemas_are_declared_for_the_observed_tools() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const char* name : {"script_check_syntax", "project_search_text",
                             "project_search_symbols", "project_list_resources",
                             "runtime_list_sessions", "viewport_capture_frame",
                             "scene_get_hierarchy"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(tool->toJson().contains("outputSchema"));
    }
}

// The set of JSON types a scalar Godot property will accept is fixed by
// validateJsonForPropertyType in src/gdextension/godot_bridge.cpp: null for
// NIL, boolean for BOOL, integer for INT, any number for FLOAT, string for
// STRING/STRING_NAME/NODE_PATH. Arrays and objects are refused outright.
static const std::vector<std::string> kScalarPropertyJsonTypes = {
    "null", "boolean", "integer", "number", "string"};

// The schema is the only description of a parameter a client ever sees. An
// untyped `value` leaves it guessing between 1.0 and "1.0", and a client that
// guesses does not guess consistently -- field trial 02 sent both for the same
// float property one turn apart and misread the rejection as a Didi bug.
static void test_scalar_property_value_declares_its_accepted_json_types() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("scene_set_property");
    ASSERT_TRUE(tool != nullptr);
    const auto& value = tool->inputSchema.at("properties").at("value");
    ASSERT_TRUE(value.contains("type"));
    ASSERT_TRUE(value.at("type").is_array());
    auto declared = value.at("type").get<std::vector<std::string>>();
    std::sort(declared.begin(), declared.end());
    auto accepted = kScalarPropertyJsonTypes;
    std::sort(accepted.begin(), accepted.end());
    ASSERT_TRUE(declared == accepted);
    // A type list alone still does not say that a float property wants 1.0 and
    // not "1.0". An example carries the rest of the meaning.
    ASSERT_TRUE(value.contains("examples"));
    ASSERT_TRUE(value.at("examples").is_array());
    ASSERT_TRUE(!value.at("examples").empty());
}

// scene_instantiate_node's initial properties reach the same validator one
// level down, so its values need the same description.
static void test_instantiate_node_property_values_are_described() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("scene_instantiate_node");
    ASSERT_TRUE(tool != nullptr);
    const auto& properties = tool->inputSchema.at("properties").at("properties");
    ASSERT_EQ(properties.at("type"), "object");
    ASSERT_TRUE(properties.contains("additionalProperties"));
    const auto& values = properties.at("additionalProperties");
    ASSERT_TRUE(values.is_object());
    ASSERT_TRUE(values.contains("type"));
    auto declared = values.at("type").get<std::vector<std::string>>();
    std::sort(declared.begin(), declared.end());
    auto accepted = kScalarPropertyJsonTypes;
    std::sort(accepted.begin(), accepted.end());
    ASSERT_TRUE(declared == accepted);
}

// The converse guard. A genuinely free-form value -- the memory store keeps
// whatever JSON it is handed, verbatim -- is correctly untyped, and narrowing
// it to the scalar property set would be a regression, not a fix.
static void test_free_form_values_stay_untyped() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* tool = registry.getTool("blackboard_write");
    ASSERT_TRUE(tool != nullptr);
    const auto& value = tool->inputSchema.at("properties").at("value");
    ASSERT_TRUE(!value.contains("type"));
}

// A tool that cannot execute has no observed shape, so it must not promise one.
static void test_unimplemented_tools_declare_no_output_schema() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto& tool : registry.listTools()) {
        if (tool.capability.implemented) continue;
        ASSERT_TRUE(!tool.toJson().contains("outputSchema"));
    }
}

struct RegisterToolManifestTests {
    RegisterToolManifestTests() {
        registerTest("tool_manifest.declared_legacy_marked",
                     test_manifest_declared_legacy_names_are_registered_and_marked);
        registerTest("tool_manifest.no_undeclared_legacy",
                     test_manifest_no_undeclared_legacy_tools);
        registerTest("tool_manifest.partitions_surface",
                     test_manifest_partitions_the_surface);
        registerTest("tool_manifest.implemented_partitions_canonical",
                     test_manifest_implemented_partitions_canonical);
        registerTest("tool_manifest.unimplemented_not_implemented",
                     test_manifest_unimplemented_tools_are_not_implemented);
        registerTest("tool_manifest.json_counts_agree",
                     test_manifest_json_counts_agree_with_names);
        registerTest("tool_manifest.names_sorted",
                     test_manifest_names_are_sorted);
        registerTest("tool_annotations.read_only_tools",
                     test_read_only_tools_are_annotated_read_only);
        registerTest("tool_annotations.project_code_is_open_world",
                     test_tools_that_run_project_code_are_open_world);
        registerTest("tool_annotations.mutations_not_read_only",
                     test_mutating_tools_are_not_annotated_read_only);
        registerTest("tool_annotations.every_tool_annotated",
                     test_every_registered_tool_carries_annotations);
        registerTest("tool_annotations.no_mutation_claims_read_only",
                     test_no_mutation_is_ever_advertised_as_read_only);
        registerTest("tool_annotations.agree_with_dry_run",
                     test_annotations_agree_with_the_dry_run_contract);
        registerTest("tool_annotations.structured_content",
                     test_tool_results_carry_structured_content);
        registerTest("tool_annotations.errors_have_no_structured_content",
                     test_error_results_do_not_carry_structured_content);
        registerTest("tool_output_schema.well_formed",
                     test_declared_output_schemas_are_well_formed);
        registerTest("tool_output_schema.declared_for_observed_tools",
                     test_output_schemas_are_declared_for_the_observed_tools);
        registerTest("tool_output_schema.none_for_unimplemented",
                     test_unimplemented_tools_declare_no_output_schema);
        registerTest("tool_input_schema.scalar_property_value_typed",
                     test_scalar_property_value_declares_its_accepted_json_types);
        registerTest("tool_input_schema.instantiate_property_values_typed",
                     test_instantiate_node_property_values_are_described);
        registerTest("tool_input_schema.free_form_values_untyped",
                     test_free_form_values_stay_untyped);
    }
} g_register_tool_manifest_tests;
