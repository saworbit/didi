#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/spatial_queries.hpp"

#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::parseNavPathRequest;
using didi::runtime::parseRaycastBatchRequest;
using didi::runtime::parseRaycastRequest;

json v2(double x, double y) { return {{"x", x}, {"y", y}}; }
json v3(double x, double y, double z) { return {{"x", x}, {"y", y}, {"z", z}}; }

void test_registry_advertises_live_reads() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto* name : {"physics_raycast_query", "spatial_query_raycast_batch",
                             "nav_query_path"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(tool->capability.implemented);
        const auto description = tool->toJson();
        ASSERT_EQ(description["_meta"]["didi"]["executionModes"], json::array({"live"}));
        ASSERT_TRUE(!description["inputSchema"]["properties"].contains("dry_run"));
        ASSERT_TRUE(description["description"].get<std::string>().rfind("UNIMPLEMENTED:", 0) != 0);
    }
    const auto result = registry.callTool(
        "physics_raycast_query", {{"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}});
    ASSERT_TRUE(result.isError);
    ASSERT_TRUE(result.content[0].text.find("no trustworthy execution path") == std::string::npos);
}

void test_raycast_parses_both_dimensions_with_defaults() {
    auto three = parseRaycastRequest({{"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}});
    ASSERT_TRUE(three.isOk());
    ASSERT_EQ(three.value().dimension(), 3);
    ASSERT_EQ(three.value().collision_mask, 1);
    ASSERT_EQ(three.value().to.x, 4.0);
    auto two = parseRaycastRequest({{"from", v2(0, 0)}, {"to", v2(0, 4)}, {"collision_mask", 5}});
    ASSERT_TRUE(two.isOk());
    ASSERT_EQ(two.value().dimension(), 2);
    ASSERT_EQ(two.value().collision_mask, 5);
    ASSERT_EQ(two.value().to.toJson(), v2(0, 4));
    ASSERT_EQ(three.value().from.toJson(), v3(0, 0, 0));
}

void test_raycast_rejects_contract_violations() {
    const double inf = std::numeric_limits<double>::infinity();
    const json bad[] = {
        {{"from", v3(0, 0, 0)}},
        {{"to", v3(0, 0, 0)}},
        {{"from", v3(0, 0, 0)}, {"to", v2(1, 0)}},
        {{"from", v3(0, 0, 0)}, {"to", v3(0, 0, 0)}},
        {{"from", v2(1, 1)}, {"to", v2(1, 1)}},
        {{"from", v3(0, 0, 0)}, {"to", v3(1000001, 0, 0)}},
        {{"from", v3(0, 0, 0)}, {"to", v3(inf, 0, 0)}},
        {{"from", v3(0, 0, 0)}, {"to", {{"x", 1}, {"y", 0}, {"z", 0}, {"w", 0}}}},
        {{"from", v3(0, 0, 0)}, {"to", {{"x", "1"}, {"y", 0}, {"z", 0}}}},
        {{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}, {"collision_mask", 0}},
        {{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}, {"collision_mask", 2147483648LL}},
        {{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}, {"collision_mask", 1.5}},
        {{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}, {"exclude", json::array()}},
        {{"from", json::array({0, 0, 0})}, {"to", v3(1, 0, 0)}},
    };
    for (const auto& params : bad) {
        auto parsed = parseRaycastRequest(params);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
    }
    ASSERT_TRUE(parseRaycastRequest(json::array()).isErr());
}

void test_nav_path_parses_and_rejects() {
    auto parsed = parseNavPathRequest({{"start_point", v3(-1, 0, 0)}, {"end_point", v3(1, 0, 0)}});
    ASSERT_TRUE(parsed.isOk());
    ASSERT_EQ(parsed.value().dimension(), 3);
    ASSERT_EQ(parsed.value().navigation_layers, 1);
    ASSERT_TRUE(parsed.value().optimize);
    auto explicit_options = parseNavPathRequest({{"start_point", v2(-1, 0)}, {"end_point", v2(1, 0)},
                                                 {"navigation_layers", 3}, {"optimize", false}});
    ASSERT_TRUE(explicit_options.isOk());
    ASSERT_EQ(explicit_options.value().dimension(), 2);
    ASSERT_EQ(explicit_options.value().navigation_layers, 3);
    ASSERT_TRUE(!explicit_options.value().optimize);
    // Start equal to end is a legal query for a path; only the ray needs a segment.
    ASSERT_TRUE(parseNavPathRequest({{"start_point", v2(0, 0)}, {"end_point", v2(0, 0)}}).isOk());

    const json bad[] = {
        {{"start_point", v3(0, 0, 0)}},
        {{"start_point", v3(0, 0, 0)}, {"end_point", v2(1, 0)}},
        {{"start_point", v3(0, 0, 0)}, {"end_point", v3(1, 0, 0)}, {"navigation_layers", 0}},
        {{"start_point", v3(0, 0, 0)}, {"end_point", v3(1, 0, 0)}, {"optimize", "yes"}},
        {{"start_point", v3(0, 0, 0)}, {"end_point", v3(1, 0, 0)}, {"map", "main"}},
        {{"start_point", v3(-1000001, 0, 0)}, {"end_point", v3(1, 0, 0)}},
    };
    for (const auto& params : bad) {
        auto result = parseNavPathRequest(params);
        ASSERT_TRUE(result.isErr());
        ASSERT_EQ(result.error().code, 400);
    }
}

void test_hook_admits_both_session_kinds_by_policy() {
    // The hook's game gate used to admit runtime.* only. Both spatial reads
    // are editor-or-game by policy, so a game session must not be refused
    // before the bridge; without an engine the bridge itself reports it is
    // not ready, which is the proof the gate was passed.
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    for (const auto kind : {didi::runtime::SessionKind::editor, didi::runtime::SessionKind::game}) {
        didi::godot::EditorHookTestAccess::setSessionKind(hook, kind);
        for (const auto* method : {"physics.raycast", "nav.queryPath"}) {
            const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(
                hook, method, {{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)},
                               {"start_point", v3(0, 0, 0)}, {"end_point", v3(1, 0, 0)}});
            ASSERT_TRUE(response.contains("error"));
            ASSERT_TRUE(response["error"]["message"] != "session_kind_rejected");
            ASSERT_TRUE(response["error"]["message"].get<std::string>().find("Editor-only method") ==
                        std::string::npos);
        }
    }
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
}

void test_raycast_batch_is_exactly_the_single_ray_contract_repeated() {
    // The batch exists so fifty sightlines cost one dispatch and one space
    // state lookup rather than fifty. What it must not become is a second
    // contract: an entry the single call would reject has to be rejected here,
    // and the other way round, or a caller learns two sets of rules.
    auto batch = parseRaycastBatchRequest({{"rays", json::array({
        {{"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}},
        {{"from", v3(0, 0, 0)}, {"to", v3(0, 9, 0)}, {"collision_mask", 5}}})}});
    ASSERT_TRUE(batch.isOk());
    ASSERT_EQ(batch.value().rays.size(), 2u);
    ASSERT_EQ(batch.value().dimension(), 3);
    // The default mask is the single call's default, not a second one.
    ASSERT_EQ(batch.value().rays[0].collision_mask, 1);
    ASSERT_EQ(batch.value().rays[1].collision_mask, 5);

    auto two_d = parseRaycastBatchRequest({{"rays", json::array({
        {{"from", v2(0, 0)}, {"to", v2(0, 4)}}})}});
    ASSERT_TRUE(two_d.isOk());
    ASSERT_EQ(two_d.value().dimension(), 2);

    // Everything the single call refuses, refused here, and the message says
    // which entry so the caller does not have to bisect the batch.
    const auto rejected = [](const json& params) { return parseRaycastBatchRequest(params); };
    auto zero_length = rejected({{"rays", json::array({
        {{"from", v3(1, 1, 1)}, {"to", v3(1, 1, 1)}}})}});
    ASSERT_TRUE(zero_length.isErr());
    ASSERT_TRUE(zero_length.error().message.find("rays[0]") != std::string::npos);

    auto bad_second = rejected({{"rays", json::array({
        {{"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}},
        {{"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}, {"collision_mask", 0}}})}});
    ASSERT_TRUE(bad_second.isErr());
    ASSERT_TRUE(bad_second.error().message.find("rays[1]") != std::string::npos);

    // A 2D and a 3D ray are answered by different space states, so a batch that
    // mixed them would be answering from two different worlds.
    auto mixed = rejected({{"rays", json::array({
        {{"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}},
        {{"from", v2(0, 0)}, {"to", v2(0, 4)}}})}});
    ASSERT_TRUE(mixed.isErr());
    ASSERT_TRUE(mixed.error().message.find("space state") != std::string::npos);

    // An empty batch is a question about nothing, and an unbounded one is a
    // response nobody sized.
    ASSERT_TRUE(rejected({{"rays", json::array()}}).isErr());
    json too_many = json::array();
    for (int index = 0; index < 65; ++index) {
        too_many.push_back({{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}});
    }
    ASSERT_TRUE(rejected({{"rays", too_many}}).isErr());
    json at_the_cap = json::array();
    for (int index = 0; index < 64; ++index) {
        at_the_cap.push_back({{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}});
    }
    ASSERT_TRUE(parseRaycastBatchRequest({{"rays", at_the_cap}}).isOk());

    ASSERT_TRUE(rejected({{"rays", json::array({{{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}})},
                          {"unknown", 1}}).isErr());
    ASSERT_TRUE(rejected({{"rays", "not an array"}}).isErr());
}

struct RegisterPhase7bSpatial {
    RegisterPhase7bSpatial() {
        registerTest("phase7b_spatial.registry_live_reads", test_registry_advertises_live_reads);
        registerTest("phase7b_spatial.raycast_parses", test_raycast_parses_both_dimensions_with_defaults);
        registerTest("phase7b_spatial.raycast_rejects", test_raycast_rejects_contract_violations);
        registerTest("phase7b_spatial.raycast_batch_matches_single",
                     test_raycast_batch_is_exactly_the_single_ray_contract_repeated);
        registerTest("phase7b_spatial.nav_parses_and_rejects", test_nav_path_parses_and_rejects);
        registerTest("phase7b_spatial.hook_admits_both_kinds", test_hook_admits_both_session_kinds_by_policy);
    }
} g_registerPhase7bSpatial;

} // namespace
