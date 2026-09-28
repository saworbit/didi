// Refusal remedy tests (Q6 in docs/BUILD_QUEUE.md, #1040).
//
// Every refusal names the argument or the call that fixes it. A site that knows
// says so; the error floor fills the rest from src/mcp/refusal_remedies.cpp by
// code and tool. What is asserted here is the floor's half: that it fills a
// remedy where the site gave none, leaves a site's own alone, and gives a fault
// none. The census of codes is tests/test_refusal_remedies.py.

#include "didi/mcp/error_data.hpp"
#include "didi/mcp/tool_registry.hpp"

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

struct RegisterRefusalRemedyTests {
    RegisterRefusalRemedyTests() {
        registerTest("RefusalRemedies.CodeCarriesItsRemedy",
                     test_a_refusal_with_no_remedy_gets_the_one_its_code_carries);
        registerTest("RefusalRemedies.RemedyFollowsTheTool", test_the_remedy_follows_the_tool);
        registerTest("RefusalRemedies.SiteRemedyLeftAlone", test_a_site_remedy_is_left_alone);
        registerTest("RefusalRemedies.FaultNamesNoFix",
                     test_a_fault_names_no_fix_and_a_dead_end_says_so);
        registerTest("RefusalRemedies.ManifestPublishesTheTable",
                     test_the_manifest_publishes_the_table);
    }
} g_register_refusal_remedy_tests;
