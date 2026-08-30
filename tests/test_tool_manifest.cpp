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
    // Didi's world is one local project. No tool reaches the network.
    ASSERT_EQ(annotations["openWorldHint"].get<bool>(), false);
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
    }
} g_register_tool_manifest_tests;
