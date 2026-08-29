// Tool manifest tests.
//
// These assert structural invariants of the registered surface rather than
// magic counts. The counts themselves live in exactly one place -- the
// registry -- and documentation is checked against the generated manifest by
// tools/validate_documentation.py.

#include "didi/mcp/tool_registry.hpp"
#include "didi/mcp/mcp_protocol.hpp"

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
    }
} g_register_tool_manifest_tests;
