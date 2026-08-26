#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include <fstream>
#include <regex>

namespace didi {
namespace mcp {

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.getHierarchy", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to query scene hierarchy from Godot: " + res.error().message);
    }

    // Offline mode: attempt to parse root .tscn file if specified in args or default scene
    std::string root = args.value("root_path", "");
    if (strings::endsWith(root, ".tscn")) {
        std::string disk_path = root;
        if (strings::startsWith(disk_path, "res://")) disk_path = disk_path.substr(6);

        std::ifstream file(disk_path);
        if (!file.is_open() && std::filesystem::exists("demo/" + disk_path)) {
            disk_path = "demo/" + disk_path;
            file.open(disk_path);
        }

        if (file.is_open()) {
            json nodes = json::array();
            std::string line;
            static const std::regex node_regex(R"re(\[node name="([^"]+)"(?:\s+type="([^"]+)")?(?:\s+parent="([^"]+)")?)re");
            while (std::getline(file, line)) {
                std::smatch match;
                if (std::regex_search(line, match, node_regex)) {
                    json n = {
                        {"name", match[1].str()},
                        {"type", match[2].matched ? match[2].str() : "Instance"},
                        {"parent", match.size() > 3 && match[3].matched ? match[3].str() : "."}
                    };
                    nodes.push_back(n);
                }
            }
            json tree_res = {
                {"source", "parsed_tscn_file"},
                {"file_path", root},
                {"nodes", nodes}
            };
            return CallToolResult::successJson(tree_res);
        }
    }

    json offline_msg = {
        {"status", "offline"},
        {"message", "Godot Editor is offline. Connect Godot Editor with Didi GDExtension to inspect live in-memory SceneTree, or provide a '.tscn' path in root_path."}
    };
    return CallToolResult::error("Godot Editor is offline. " + offline_msg.dump(2));
}

CallToolResult handleMutateSceneTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string action = args.value("action", "");
    std::string target = args.value("target_node", "");

    if (action.empty() || target.empty()) {
        return CallToolResult::error("Missing required parameters: 'action' and 'target_node'.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.mutate", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to mutate scene tree in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Please launch Godot Editor to execute live SceneTree mutations with EditorUndoRedoManager.");
}

} // namespace mcp
} // namespace didi
