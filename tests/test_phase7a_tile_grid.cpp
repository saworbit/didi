#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7TileGrid fail-closed contract", "[phase7][tile-grid][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"tilemap_set_cells", "tilemap_get_used_rect", "gridmap_set_cells"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool && tool->capability.implemented);
        ASSERT_TRUE(registry.callTool(name, didi::json::object()).isError);
    }
}
struct RegisterPhase7TileGrid { RegisterPhase7TileGrid() { registerTest("Phase7TileGrid fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7TileGrid;

// TASK 4 TILE GRID BEHAVIOR BEGIN
#include "didi/common/ipc_channel.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <memory>
#include <limits>
#include <string>
#include <vector>

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

namespace didi::mcp {
CallToolResult handleTilemapSetCells(const ResolvedToolBinding&, const json&,
                                     std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleTilemapGetUsedRect(const ResolvedToolBinding&, const json&,
                                        std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleGridmapSetCells(const ResolvedToolBinding&, const json&,
                                     std::shared_ptr<ipc::IIpcClient>);
}

namespace {

class TileGridRecordingClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json& params,
                                         int timeout_ms) override {
        ++requests;
        last_method = method;
        last_params = params;
        last_timeout_ms = timeout_ms;
        return didi::json{{"native_observed", true}};
    }

    bool connected{true};
    int requests{0};
    int last_timeout_ms{0};
    std::string last_method;
    didi::json last_params;
};

didi::json tileGridPayload(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(!result.content.empty());
    return didi::json::parse(result.content.front().text);
}

void assertTileGridRejected(const didi::mcp::CallToolResult& result,
                            const std::shared_ptr<TileGridRecordingClient>& client) {
    ASSERT_TRUE(result.isError);
    ASSERT_EQ(tileGridPayload(result)["error"]["code"], 400);
    ASSERT_EQ(client->requests, 0);
}

didi::json tileSetRecord(int x, int y) {
    return {{"coords", {x, y}}, {"source_id", 0}, {"atlas_coords", {0, 0}}};
}

didi::json gridSetRecord(int x, int y, int z) {
    return {{"position", {x, y, z}}, {"item", 0}};
}

void test_phase7_tile_grid_handlers_reject_invalid_batches_before_dispatch() {
    using namespace didi::mcp;
    const auto unsigned_overflow = std::numeric_limits<uint64_t>::max();
    auto tile_client = std::make_shared<TileGridRecordingClient>();
    auto tile = resolveAliasBinding("tilemap_set_cells");
    didi::json too_many_tiles = didi::json::array();
    for (int i = 0; i < 257; ++i) too_many_tiles.push_back(tileSetRecord(i, 0));
    for (const auto& invalid : std::vector<didi::json>{
             nullptr, didi::json::object(), {{"tilemap_path", ""}, {"cells", didi::json::array({tileSetRecord(0, 0)})}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", didi::json::array()}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", too_many_tiles}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {0, 0}}, {"erase", false}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {0, 0}}, {"erase", true}, {"source_id", 0}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {0, 0}}, {"source_id", -1}, {"atlas_coords", {0, 0}}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {unsigned_overflow, 0}}, {"erase", true}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {0, 0}}, {"source_id", unsigned_overflow}, {"atlas_coords", {0, 0}}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {0, 0}}, {"source_id", 0}, {"atlas_coords", {unsigned_overflow, 0}}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {{{"coords", {0, 0}}, {"source_id", 0}, {"atlas_coords", {0, 0}}, {"alternative_tile", unsigned_overflow}}}}},
             {{"tilemap_path", "/root/Tiles"}, {"cells", {tileSetRecord(0, 0), {{"coords", {1048577, 0}}, {"erase", true}}}}}}) {
        assertTileGridRejected(handleTilemapSetCells(tile, invalid, tile_client), tile_client);
    }

    auto rect_client = std::make_shared<TileGridRecordingClient>();
    for (const auto& invalid : std::vector<didi::json>{nullptr, didi::json::object(),
             {{"tilemap_path", ""}}, {{"tilemap_path", "/root/Tiles"}, {"extra", true}}}) {
        assertTileGridRejected(handleTilemapGetUsedRect(
            resolveAliasBinding("tilemap_get_used_rect"), invalid, rect_client), rect_client);
    }

    auto grid_client = std::make_shared<TileGridRecordingClient>();
    auto grid = resolveAliasBinding("gridmap_set_cells");
    didi::json too_many_grid = didi::json::array();
    for (int i = 0; i < 257; ++i) too_many_grid.push_back(gridSetRecord(i, 0, 0));
    for (const auto& invalid : std::vector<didi::json>{
             nullptr, didi::json::object(), {{"gridmap_path", "/root/Grid"}, {"cells", didi::json::array()}},
             {{"gridmap_path", "/root/Grid"}, {"cells", too_many_grid}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {{{"position", {0, 0}}, {"item", 0}}}}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {{{"position", {0, 0, 0}}, {"item", -1}, {"orientation", 1}}}}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {{{"position", {0, 0, 0}}, {"item", 0}, {"orientation", 24}}}}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {{{"position", {unsigned_overflow, 0, 0}}, {"item", 0}}}}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {{{"position", {0, 0, 0}}, {"item", unsigned_overflow}}}}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {{{"position", {0, 0, 0}}, {"item", 0}, {"orientation", unsigned_overflow}}}}},
             {{"gridmap_path", "/root/Grid"}, {"cells", {gridSetRecord(0, 0, 0), {{"position", {0, 0, 1048577}}, {"item", 0}}}}}}) {
        assertTileGridRejected(handleGridmapSetCells(grid, invalid, grid_client), grid_client);
    }

    auto duplicate_tile = handleTilemapSetCells(tile,
        {{"tilemap_path", "/root/Tiles"}, {"cells", {tileSetRecord(0, 0), tileSetRecord(0, 0)}}}, tile_client);
    ASSERT_EQ(tileGridPayload(duplicate_tile)["error"]["code"], 409);
    auto duplicate_grid = handleGridmapSetCells(grid,
        {{"gridmap_path", "/root/Grid"}, {"cells", {gridSetRecord(0, 0, 0), gridSetRecord(0, 0, 0)}}}, grid_client);
    ASSERT_EQ(tileGridPayload(duplicate_grid)["error"]["code"], 409);
    ASSERT_EQ(tile_client->requests, 0);
    ASSERT_EQ(grid_client->requests, 0);
}

void test_phase7_tile_grid_handlers_forward_exact_requests_once() {
    using namespace didi::mcp;
    struct Case { const char* tool; const char* method; didi::json args; };
    const std::vector<Case> cases = {
        {"tilemap_set_cells", "tilemap.setCells", {{"tilemap_path", "/root/Tiles"}, {"cells", {tileSetRecord(1, 2), {{"coords", {3, 4}}, {"erase", true}}}}}},
        {"tilemap_get_used_rect", "tilemap.getUsedRect", {{"tilemap_path", "/root/Tiles"}}},
        {"gridmap_set_cells", "gridmap.setCells", {{"gridmap_path", "/root/Grid"}, {"cells", {gridSetRecord(1, 2, 3), {{"position", {4, 5, 6}}, {"item", -1}}}}}}
    };
    for (const auto& item : cases) {
        auto client = std::make_shared<TileGridRecordingClient>();
        didi::mcp::CallToolResult result;
        const auto binding = resolveAliasBinding(item.tool);
        if (binding.canonical_name == "tilemap_set_cells") result = handleTilemapSetCells(binding, item.args, client);
        else if (binding.canonical_name == "tilemap_get_used_rect") result = handleTilemapGetUsedRect(binding, item.args, client);
        else result = handleGridmapSetCells(binding, item.args, client);
        ASSERT_TRUE(!result.isError);
        ASSERT_EQ(client->requests, 1);
        ASSERT_EQ(client->last_method, item.method);
        ASSERT_EQ(client->last_params, item.args);
        ASSERT_EQ(client->last_timeout_ms, 17000);
    }
}

void test_phase7_tile_grid_methods_are_editor_only_but_reach_the_bridge() {
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("tile grid test reset");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    for (const auto* method : {"tilemap.setCells", "tilemap.getUsedRect", "gridmap.setCells"}) {
        const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, method, didi::json::object());
        ASSERT_TRUE(response.contains("error"));
        ASSERT_TRUE(response["error"]["message"].get<std::string>().find("no trustworthy live implementation") == std::string::npos);
    }
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::game);
    for (const auto* method : {"tilemap.setCells", "tilemap.getUsedRect", "gridmap.setCells"}) {
        const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(hook, method, didi::json::object());
        ASSERT_EQ(response["error"]["code"], 409);
    }
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
}

// Break caught: tilemap_set_cells publishes `coords` as a two-element array
// through $defs/vector2i, and the schema layer resolved no $ref, read no
// prefixItems and read no oneOf, so none of that was enforced. The handler was
// the only check and answered a wrong-shaped argument with the tool name
// wrapped in a token. gridmap_set_cells, whose schema inlines the same shape,
// named the field all along (#442).
static void test_the_published_cell_shape_is_enforced_by_the_schema() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();

    const auto message = [&](const char* tool, const didi::json& arguments) {
        const auto result = registry.callTool(tool, arguments);
        ASSERT_TRUE(result.isError);
        return result.content.empty() ? std::string() : result.content[0].text;
    };

    // The object form most of the rest of the surface takes is a violation
    // here, and it is now named rather than tokenised.
    const auto tilemap = message("tilemap_set_cells",
        didi::json{{"tilemap_path", "/root/Main/Branch"},
                   {"cells", didi::json::array({{{"coords", {{"x", 0}, {"y", 0}}},
                                                 {"source_id", 0},
                                                 {"atlas_coords", {{"x", 0}, {"y", 0}}}}})}});
    ASSERT_TRUE(tilemap.find("must be an array, not an object") != std::string::npos);
    ASSERT_TRUE(tilemap.find("\"code\":400") != std::string::npos);
    ASSERT_TRUE(tilemap.find("invalid_tilemap_set_cells_request") == std::string::npos);

    // Which is what the same mistake has always got from gridmap_set_cells.
    const auto gridmap = message("gridmap_set_cells",
        didi::json{{"gridmap_path", "/root/Main/Branch"},
                   {"cells", didi::json::array({{{"position", {{"x", 0}, {"y", 0}, {"z", 0}}},
                                                 {"item", 0}}})}});
    ASSERT_TRUE(gridmap.find("must be an array, not an object") != std::string::npos);

    // prefixItems is what says a coordinate is a pair, and const is what tells
    // the erase shape from the placement shape.
    ASSERT_TRUE(message("tilemap_set_cells",
        didi::json{{"tilemap_path", "/root/Main/Branch"},
                   {"cells", didi::json::array({{{"coords", didi::json::array({0, 0, 0})},
                                                 {"source_id", 0},
                                                 {"atlas_coords", didi::json::array({0, 0})}}})}})
        .find("at most 2 entries") != std::string::npos);
    ASSERT_TRUE(message("tilemap_set_cells",
        didi::json{{"tilemap_path", "/root/Main/Branch"},
                   {"cells", didi::json::array({{{"coords", didi::json::array({0, 0})},
                                                 {"erase", false}}})}})
        .find("must be true") != std::string::npos);

    // Both published shapes still reach dispatch. Without an editor that is a
    // 503 about the route, which is a different failure from a rejected
    // argument and is the proof the schema did not eat a legal call.
    for (const didi::json& cell : {didi::json{{"coords", didi::json::array({0, 0})},
                                              {"source_id", 0},
                                              {"atlas_coords", didi::json::array({0, 0})}},
                                   didi::json{{"coords", didi::json::array({0, 0})},
                                              {"erase", true}}}) {
        const auto accepted = registry.callTool(
            "tilemap_set_cells", didi::json{{"tilemap_path", "/root/Main/Branch"},
                                            {"cells", didi::json::array({cell})}});
        const auto text = accepted.content.empty() ? std::string() : accepted.content[0].text;
        ASSERT_TRUE(text.find("invalid_tilemap_set_cells_request") == std::string::npos);
        ASSERT_TRUE(text.find("does not match any accepted shape") == std::string::npos);
        ASSERT_TRUE(text.find("must be an array") == std::string::npos);
    }
}

// Break caught: the semantic failures in the bridge answered with the
// identifier as the whole message, which is the string a client shows a
// person. `target_method_not_found` does not say which method was looked for or
// on which node, so the one thing the caller needed was the one thing missing.
// #406 and #424 fixed the argument rejections; these sit underneath them
// (#441).
static void test_every_bridge_identifier_says_a_sentence() {
    const auto& sentences = didi::godot::bridgeErrorSentenceTable();
    ASSERT_TRUE(!sentences.empty());

    for (const auto& [identifier, sentence] : sentences) {
        // A sentence, not the identifier spelled differently: real words, a
        // full stop, and no snake_case left in it.
        ASSERT_TRUE(!sentence.empty());
        ASSERT_TRUE(sentence.find('_') == std::string::npos);
        ASSERT_TRUE(sentence.back() == '.');
        ASSERT_TRUE(sentence.find(' ') != std::string::npos);
        ASSERT_TRUE(sentence != identifier);
    }

    // The identifier is worth keeping as a stable code, so both of the pair
    // #443 is about are present and say different things.
    ASSERT_TRUE(sentences.count("tilemap_target_not_found") == 1);
    ASSERT_TRUE(sentences.count("tilemap_target_wrong_type") == 1);
    ASSERT_TRUE(sentences.at("tilemap_target_not_found") !=
                sentences.at("tilemap_target_wrong_type"));
    ASSERT_TRUE(sentences.count("gridmap_target_not_found") == 1);
    ASSERT_TRUE(sentences.count("gridmap_target_wrong_type") == 1);

    // The ones the report reached by hand.
    for (const auto* named : {"target_method_not_found", "declared_signal_not_found",
                              "camera_path_does_not_resolve_to_camera3d",
                              "tilemap_layer_has_no_tileset", "gridmap_item_not_found",
                              "unsupported_existing_connection_flags",
                              "missing_or_ambiguous_signal_connection"}) {
        ASSERT_TRUE(sentences.count(named) == 1);
    }
}

struct RegisterPhase7TileGridBehavior {
    RegisterPhase7TileGridBehavior() {
        registerTest("Phase7TileGrid.PublishedCellShapeIsEnforced",
                     test_the_published_cell_shape_is_enforced_by_the_schema);

        registerTest("Phase7TileGrid.BridgeIdentifiersSaySentences",
                     test_every_bridge_identifier_says_a_sentence);
        registerTest("Phase7TileGrid.StrictBatchValidation", test_phase7_tile_grid_handlers_reject_invalid_batches_before_dispatch);
        registerTest("Phase7TileGrid.ExactForwarding", test_phase7_tile_grid_handlers_forward_exact_requests_once);
        registerTest("Phase7TileGrid.EditorOnlyAdmission", test_phase7_tile_grid_methods_are_editor_only_but_reach_the_bridge);
    }
} g_registerPhase7TileGridBehavior;

} // namespace
// TASK 4 TILE GRID BEHAVIOR END
