#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace mcp {

CallToolResult handleTilemapSetCells(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string tilemap_path = args.value("tilemap_path", "");
    json cells = args.value("cells", json::array());

    if (tilemap_path.empty() || cells.empty()) {
        return CallToolResult::error("Parameters 'tilemap_path' and 'cells' array are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("tilemap.setCells", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to update TileMap cells: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot with Didi plugin to edit TileMapLayer cells interactively.");
}

CallToolResult handleTilemapGetUsedRect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string tilemap_path = args.value("tilemap_path", "");
    if (tilemap_path.empty()) {
        return CallToolResult::error("Parameter 'tilemap_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("tilemap.getUsedRect", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to query TileMap used rectangle: " + res.error().message);
    }

    json offline_rect = {
        {"status", "offline"},
        {"tilemap_path", tilemap_path},
        {"position", {{"x", 0}, {"y", 0}}},
        {"size", {{"width", 32}, {"height", 32}}},
        {"message", "Godot Editor is offline."}
    };
    return CallToolResult::successJson(offline_rect);
}

CallToolResult handleGridmapSetCells(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string gridmap_path = args.value("gridmap_path", "");
    json cells = args.value("cells", json::array());

    if (gridmap_path.empty() || cells.empty()) {
        return CallToolResult::error("Parameters 'gridmap_path' and 'cells' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("gridmap.setCells", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to update GridMap cells: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot with Didi plugin to edit 3D GridMap cells.");
}

} // namespace mcp
} // namespace didi
