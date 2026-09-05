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
using didi::runtime::parseClearanceRequest;
using didi::runtime::parseRaycastBatchRequest;
using didi::runtime::parseFrustumRequest;
using didi::runtime::parseRaycastRequest;

json v2(double x, double y) { return {{"x", x}, {"y", y}}; }
json v3(double x, double y, double z) { return {{"x", x}, {"y", y}, {"z", z}}; }

void test_registry_advertises_live_reads() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto* name : {"physics_raycast_query", "spatial_query_raycast_batch",
                             "spatial_query_clearance", "spatial_query_frustum",
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
        for (const auto* method : {"physics.raycast", "vision.frustumQuery", "nav.queryPath"}) {
            const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(
                hook, method, {{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)},
                               {"start_point", v3(0, 0, 0)}, {"end_point", v3(1, 0, 0)},
                               {"camera_node", "/root/Scene/Camera3D"}});
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

void test_clearance_describes_a_body_and_not_a_line() {
    // A ray answers whether a line is clear. This answers whether a body is,
    // which is the question a doorway asks, so the shape is the part that has
    // to be described exactly.
    auto box = parseClearanceRequest({
        {"shape", {{"kind", "box"}, {"size", v3(1, 2, 1)}}},
        {"from", v3(0, 0, 0)}, {"to", v3(4, 0, 0)}});
    ASSERT_TRUE(box.isOk());
    ASSERT_EQ(box.value().dimension(), 3);
    ASSERT_EQ(box.value().collision_mask, 1);

    auto capsule = parseClearanceRequest({
        {"shape", {{"kind", "capsule"}, {"radius", 0.4}, {"height", 1.8}}},
        {"from", v2(0, 0)}, {"to", v2(0, 5)}, {"collision_mask", 3}});
    ASSERT_TRUE(capsule.isOk());
    ASSERT_EQ(capsule.value().dimension(), 2);
    ASSERT_EQ(capsule.value().collision_mask, 3);

    // Asking whether a shape fits where it stands is a real question, unlike a
    // ray of no length, so a zero-length sweep is accepted here on purpose.
    ASSERT_TRUE(parseClearanceRequest({
        {"shape", {{"kind", "sphere"}, {"radius", 1}}},
        {"from", v3(2, 2, 2)}, {"to", v3(2, 2, 2)}}).isOk());

    const auto rejected = [](const json& params) {
        return parseClearanceRequest(params).isErr();
    };
    // A shape with no extent is not a body, and the engine would answer about
    // it rather than refuse.
    ASSERT_TRUE(rejected({{"shape", {{"kind", "sphere"}, {"radius", 0}}},
                          {"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}));
    ASSERT_TRUE(rejected({{"shape", {{"kind", "box"}, {"size", v3(1, 0, 1)}}},
                          {"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}));
    // Fields that belong to another shape are refused rather than ignored, so a
    // capsule request missing its height cannot pass as a sphere.
    ASSERT_TRUE(rejected({{"shape", {{"kind", "capsule"}, {"radius", 1}}},
                          {"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}));
    ASSERT_TRUE(rejected({{"shape", {{"kind", "sphere"}, {"radius", 1}, {"height", 2}}},
                          {"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}));
    // A 2D sweep of a 3D box is two different worlds in one request.
    ASSERT_TRUE(rejected({{"shape", {{"kind", "box"}, {"size", v3(1, 1, 1)}}},
                          {"from", v2(0, 0)}, {"to", v2(1, 0)}}));
    ASSERT_TRUE(rejected({{"shape", {{"kind", "box"}, {"size", v3(1, 1, 1)}}},
                          {"from", v3(0, 0, 0)}, {"to", v2(1, 0)}}));
    ASSERT_TRUE(rejected({{"shape", {{"kind", "pyramid"}, {"radius", 1}}},
                          {"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}));
    ASSERT_TRUE(rejected({{"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}}));
    ASSERT_TRUE(rejected({{"shape", {{"kind", "sphere"}, {"radius", 1}}},
                          {"from", v3(0, 0, 0)}, {"to", v3(1, 0, 0)}, {"unknown", 1}}));
}

void test_frustum_takes_one_camera_and_no_hidden_shape() {
    // A frustum can come from a camera in the scene or from parameters written
    // out by hand, and both must be a complete description. Two would be two
    // answers to one question, and neither would be none.
    auto from_node = parseFrustumRequest({{"camera_node", "/root/Scene/Camera3D"}});
    ASSERT_TRUE(from_node.isOk());
    ASSERT_TRUE(from_node.value().source == didi::runtime::FrustumSource::camera_node);
    ASSERT_EQ(from_node.value().collision_mask, 1);
    ASSERT_TRUE(!from_node.value().sightline);
    ASSERT_EQ(from_node.value().max_results, didi::runtime::kDefaultFrustumResults);

    const json camera = {{"position", v3(0, 2, 0)}, {"look_at", v3(0, 2, -10)},
                         {"fov_degrees", 70}, {"near", 0.1}, {"far", 50}, {"aspect", 1.777}};
    auto explicit_frustum = parseFrustumRequest({{"camera", camera}, {"sightline", true},
                                                 {"collision_mask", 3}, {"max_results", 10}});
    ASSERT_TRUE(explicit_frustum.isOk());
    ASSERT_TRUE(explicit_frustum.value().source == didi::runtime::FrustumSource::parameters);
    ASSERT_TRUE(explicit_frustum.value().sightline);
    ASSERT_EQ(explicit_frustum.value().collision_mask, 3);
    ASSERT_EQ(explicit_frustum.value().max_results, 10);
    // The one field with a default records that it was defaulted, so the
    // response can say which roll answered rather than leave it assumed.
    ASSERT_TRUE(!explicit_frustum.value().up_given);
    ASSERT_EQ(explicit_frustum.value().up.toJson(), v3(0, 1, 0));

    const auto with = [&](const char* key, const json& value) {
        json body = camera;
        body[key] = value;
        return json{{"camera", body}};
    };
    const auto without = [&](const char* key) {
        json body = camera;
        body.erase(key);
        return json{{"camera", body}};
    };
    // A quarter turn of roll, which is a different frustum and not a rejected one.
    ASSERT_TRUE(parseFrustumRequest(with("up", v3(1, 0, 0))).isOk());
    ASSERT_TRUE(parseFrustumRequest(with("up", v3(1, 0, 0))).value().up_given);

    const auto rejected = [](const json& params) { return parseFrustumRequest(params).isErr(); };
    // Neither form and both forms are the same mistake seen from two sides.
    ASSERT_TRUE(rejected(json::object()));
    ASSERT_TRUE(rejected({{"camera_node", "/root/Scene/Camera3D"}, {"camera", camera}}));
    // Every part of a hand written frustum changes which nodes fall inside it,
    // so none of them may be left out and filled in quietly.
    for (const auto* required : {"position", "look_at", "fov_degrees", "near", "far", "aspect"}) {
        ASSERT_TRUE(rejected(without(required)));
    }
    // A frustum is a 3D shape and has no 2D form to fall back to.
    ASSERT_TRUE(rejected(with("position", v2(0, 2))));
    ASSERT_TRUE(rejected(with("look_at", v2(0, 2))));
    // A camera that looks at itself has no direction, and an up along the view
    // direction leaves the roll undefined rather than merely odd.
    ASSERT_TRUE(rejected(with("look_at", v3(0, 2, 0))));
    ASSERT_TRUE(rejected(with("up", v3(0, 0, -1))));
    ASSERT_TRUE(rejected(with("up", v3(0, 0, 0))));
    // A volume needs a near in front of its far.
    ASSERT_TRUE(rejected(with("far", 0.05)));
    ASSERT_TRUE(rejected(with("near", 0)));
    ASSERT_TRUE(rejected(with("fov_degrees", 180)));
    ASSERT_TRUE(rejected(with("aspect", 0)));
    ASSERT_TRUE(rejected(with("elevation", 3)));
    ASSERT_TRUE(rejected({{"camera_node", ""}}));
    ASSERT_TRUE(rejected({{"camera_node", "/root/Scene/Camera3D"}, {"sightline", "yes"}}));
    ASSERT_TRUE(rejected({{"camera_node", "/root/Scene/Camera3D"}, {"max_results", 0}}));
    ASSERT_TRUE(rejected({{"camera_node", "/root/Scene/Camera3D"},
                          {"max_results", didi::runtime::kMaxFrustumResults + 1}}));
    ASSERT_TRUE(rejected({{"camera_node", "/root/Scene/Camera3D"}, {"collision_mask", 0}}));
    ASSERT_TRUE(rejected({{"camera_node", "/root/Scene/Camera3D"}, {"unknown", 1}}));
}

struct RegisterPhase7bSpatial {
    RegisterPhase7bSpatial() {
        registerTest("phase7b_spatial.registry_live_reads", test_registry_advertises_live_reads);
        registerTest("phase7b_spatial.raycast_parses", test_raycast_parses_both_dimensions_with_defaults);
        registerTest("phase7b_spatial.raycast_rejects", test_raycast_rejects_contract_violations);
        registerTest("phase7b_spatial.clearance_describes_a_body",
                     test_clearance_describes_a_body_and_not_a_line);
        registerTest("phase7b_spatial.raycast_batch_matches_single",
                     test_raycast_batch_is_exactly_the_single_ray_contract_repeated);
        registerTest("phase7b_spatial.frustum_takes_one_camera",
                     test_frustum_takes_one_camera_and_no_hidden_shape);
        registerTest("phase7b_spatial.nav_parses_and_rejects", test_nav_path_parses_and_rejects);
        registerTest("phase7b_spatial.hook_admits_both_kinds", test_hook_admits_both_session_kinds_by_policy);
    }
} g_registerPhase7bSpatial;

} // namespace
