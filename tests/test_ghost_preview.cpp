#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/ghost_preview.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::GhostKind;
using didi::runtime::parseGhostClearRequest;
using didi::runtime::parseGhostPreviewRequest;

json v2(double x, double y) { return {{"x", x}, {"y", y}}; }
json v3(double x, double y, double z) { return {{"x", x}, {"y", y}, {"z", z}}; }

json box(const json& position, const json& size) {
    return {{"position", position}, {"size", size}};
}

void test_ghost_preview_describes_a_shape_and_nothing_else() {
    auto request = parseGhostPreviewRequest({{"previews", json::array({box(v3(1, 2, 3), v3(2, 2, 2))})}});
    ASSERT_TRUE(request.isOk());
    ASSERT_EQ(request.value().shapes.size(), 1u);
    ASSERT_EQ(request.value().dimension(), 3);
    // A preview stands for one proposal, so a second call replaces the first
    // unless the caller says otherwise.
    ASSERT_TRUE(request.value().replace);
    const auto& shape = request.value().shapes[0];
    ASSERT_TRUE(shape.kind == GhostKind::addition);
    ASSERT_TRUE(!shape.color_given);
    ASSERT_TRUE(!shape.rotated);
    // Cyan for an addition, which is the colour the caller did not have to pick.
    ASSERT_EQ(shape.color.red, 0.0);
    ASSERT_EQ(shape.color.green, 1.0);
    ASSERT_EQ(shape.color.blue, 1.0);

    auto moved = parseGhostPreviewRequest(
        {{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                    {"kind", "translation"}}})}});
    ASSERT_TRUE(moved.isOk());
    ASSERT_EQ(moved.value().shapes[0].color.red, 1.0);
    ASSERT_EQ(moved.value().shapes[0].color.green, 1.0);
    ASSERT_EQ(moved.value().shapes[0].color.blue, 0.0);

    auto removed = parseGhostPreviewRequest(
        {{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                    {"kind", "deletion"}}})}});
    ASSERT_TRUE(removed.isOk());
    ASSERT_EQ(removed.value().shapes[0].color.red, 1.0);
    ASSERT_EQ(removed.value().shapes[0].color.green, 0.0);

    auto tinted = parseGhostPreviewRequest(
        {{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                    {"kind", "deletion"}, {"color", {{"r", 0.5}, {"g", 0.25}, {"b", 0.125}}},
                                    {"label", "north wall"}}})},
         {"replace", false}});
    ASSERT_TRUE(tinted.isOk());
    ASSERT_TRUE(!tinted.value().replace);
    ASSERT_TRUE(tinted.value().shapes[0].color_given);
    ASSERT_EQ(tinted.value().shapes[0].color.red, 0.5);
    ASSERT_EQ(tinted.value().shapes[0].label, std::string("north wall"));

    auto flat = parseGhostPreviewRequest({{"previews", json::array({box(v2(4, 5), v2(2, 3))})}});
    ASSERT_TRUE(flat.isOk());
    ASSERT_EQ(flat.value().dimension(), 2);

    auto turned = parseGhostPreviewRequest(
        {{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                    {"rotation_degrees", v3(0, 45, 0)}}})}});
    ASSERT_TRUE(turned.isOk());
    ASSERT_TRUE(turned.value().shapes[0].rotated);
    ASSERT_EQ(turned.value().shapes[0].rotation_degrees[1], 45.0);
}

void test_ghost_preview_rejects_what_it_cannot_draw() {
    const auto rejected = [](const json& params) {
        return parseGhostPreviewRequest(params).isErr();
    };
    ASSERT_TRUE(rejected(json::object()));
    ASSERT_TRUE(rejected({{"previews", json::array()}}));
    ASSERT_TRUE(rejected({{"previews", "not an array"}}));
    ASSERT_TRUE(rejected({{"previews", json::array({box(v3(0, 0, 0), v3(1, 1, 1))})}, {"unknown", 1}}));
    // A shape with no extent on an axis draws nothing there, and a person
    // looking at it would read the gap as a fault in the scene.
    ASSERT_TRUE(rejected({{"previews", json::array({box(v3(0, 0, 0), v3(1, 0, 1))})}}));
    ASSERT_TRUE(rejected({{"previews", json::array({box(v3(0, 0, 0), v3(1, -2, 1))})}}));
    // One call draws into one world.
    ASSERT_TRUE(rejected({{"previews", json::array({box(v3(0, 0, 0), v3(1, 1, 1)),
                                                    box(v2(0, 0), v2(1, 1))})}}));
    ASSERT_TRUE(rejected({{"previews", json::array({box(v3(0, 0, 0), v2(1, 1))})}}));
    // Rotation is a 3D idea; a 2D preview is an axis-aligned rectangle.
    ASSERT_TRUE(rejected({{"previews", json::array({{{"position", v2(0, 0)}, {"size", v2(1, 1)},
                                                     {"rotation_degrees", v3(0, 45, 0)}}})}}));
    ASSERT_TRUE(rejected({{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                                     {"kind", "resize"}}})}}));
    ASSERT_TRUE(rejected({{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                                     {"color", {{"r", 2}, {"g", 0}, {"b", 0}}}}})}}));
    ASSERT_TRUE(rejected({{"previews", json::array({{{"position", v3(0, 0, 0)}, {"size", v3(1, 1, 1)},
                                                     {"color", {{"r", 1}, {"g", 0}}}}})}}));
    ASSERT_TRUE(rejected({{"previews", json::array({box(v3(0, 0, 0), v3(1, 1, 1))})}, {"replace", "yes"}}));
    // The index of the bad entry is named, because a batch refused without
    // saying which shape is wrong is a batch the caller has to bisect.
    auto refused = parseGhostPreviewRequest(
        {{"previews", json::array({box(v3(0, 0, 0), v3(1, 1, 1)), box(v3(0, 0, 0), v3(1, 0, 1))})}});
    ASSERT_TRUE(refused.isErr());
    ASSERT_TRUE(refused.error().message.find("previews[1]") != std::string::npos);
}

void test_ghost_clear_takes_all_or_one() {
    auto everything = parseGhostClearRequest(json::object());
    ASSERT_TRUE(everything.isOk());
    ASSERT_TRUE(everything.value().preview_id.empty());
    auto one = parseGhostClearRequest({{"preview_id", "ghost_3"}});
    ASSERT_TRUE(one.isOk());
    ASSERT_EQ(one.value().preview_id, std::string("ghost_3"));
    ASSERT_TRUE(parseGhostClearRequest({{"preview_id", ""}}).isErr());
    ASSERT_TRUE(parseGhostClearRequest({{"preview_id", 3}}).isErr());
    ASSERT_TRUE(parseGhostClearRequest({{"unknown", 1}}).isErr());
}

void test_ghost_tools_are_registered_as_live_reads_that_change_nothing() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    for (const auto* name : {"editor_render_ghost_preview", "editor_clear_ghost_previews"}) {
        const auto* tool = registry.getTool(name);
        ASSERT_TRUE(tool != nullptr);
        ASSERT_TRUE(tool->capability.implemented);
        const auto description = tool->toJson();
        ASSERT_EQ(description["_meta"]["didi"]["executionModes"], json::array({"live"}));
        // Nothing here reaches the scene, so there is nothing to preview and
        // nothing to confirm: a dry_run field would be describing a mutation
        // that does not exist.
        ASSERT_TRUE(!description["inputSchema"]["properties"].contains("dry_run"));
        ASSERT_TRUE(description["description"].get<std::string>().rfind("UNIMPLEMENTED:", 0) != 0);
    }
}

struct RegisterGhostPreview {
    RegisterGhostPreview() {
        registerTest("ghost_preview.describes_a_shape",
                     test_ghost_preview_describes_a_shape_and_nothing_else);
        registerTest("ghost_preview.rejects_undrawable",
                     test_ghost_preview_rejects_what_it_cannot_draw);
        registerTest("ghost_preview.clear_all_or_one", test_ghost_clear_takes_all_or_one);
        registerTest("ghost_preview.registered_as_live_reads",
                     test_ghost_tools_are_registered_as_live_reads_that_change_nothing);
    }
} g_registerGhostPreview;

} // namespace
