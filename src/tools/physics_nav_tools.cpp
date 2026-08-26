#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace mcp {

CallToolResult handlePhysicsRaycastQuery(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.contains("from") || !args.contains("to")) {
        return CallToolResult::error("Parameters 'from' (vector3/2) and 'to' (vector3/2) are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("physics.raycast", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Physics raycast query failed: " + res.error().message);
    }

    // Offline mock response
    json offline_res = {
        {"status", "offline"},
        {"hit", false},
        {"collider", nullptr},
        {"position", nullptr},
        {"normal", nullptr},
        {"message", "Godot Editor is offline. Launch Godot with Didi plugin to perform live 2D/3D physics raycasting."}
    };
    return CallToolResult::successJson(offline_res);
}

CallToolResult handlePhysicsSimulateStep(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    int steps = args.value("steps", 1);
    float delta = args.value("delta", 0.0166667f);

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("physics.simulateStep", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Physics simulation step failed: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot with Didi plugin to step physics deterministically.");
}

CallToolResult handleNavBakeMesh(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string nav_node = args.value("nav_node_path", "");

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("nav.bakeMesh", args, 30000);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Navmesh baking failed: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to bake NavigationMesh dynamically.");
}

CallToolResult handleNavQueryPath(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.contains("start_point") || !args.contains("end_point")) {
        return CallToolResult::error("Parameters 'start_point' and 'end_point' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("nav.queryPath", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Navigation path query failed: " + res.error().message);
    }

    json offline_nav = {
        {"status", "offline"},
        {"points", json::array({
            args["start_point"],
            args["end_point"]
        })},
        {"path_length", 0.0},
        {"walkable", true},
        {"message", "Godot Editor is offline. Straight-line approximate path returned."}
    };
    return CallToolResult::successJson(offline_nav);
}

CallToolResult handleAnimListTracks(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string player_path = args.value("animation_player_path", "");
    if (player_path.empty()) {
        return CallToolResult::error("Parameter 'animation_player_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("anim.listTracks", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to query animation tracks: " + res.error().message);
    }

    json offline_anim = {
        {"status", "offline"},
        {"animation_player", player_path},
        {"animations", json::array({"idle", "walk", "run", "jump"})},
        {"message", "Godot Editor is offline. Standard animation tracks listed."}
    };
    return CallToolResult::successJson(offline_anim);
}

CallToolResult handleAnimPlayTrack(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string player_path = args.value("animation_player_path", "");
    std::string anim_name = args.value("animation_name", "");

    if (player_path.empty() || anim_name.empty()) {
        return CallToolResult::error("Parameters 'animation_player_path' and 'animation_name' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("anim.playTrack", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to play animation track: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to preview live animation playback.");
}

} // namespace mcp
} // namespace didi
