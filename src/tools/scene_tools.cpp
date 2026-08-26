#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include <fstream>
#include <regex>
#include <filesystem>

namespace didi {
namespace mcp {

static std::string findProjectMainScene() {
    for (const auto& p : {"project.godot", "demo/project.godot"}) {
        std::ifstream cfg(p);
        if (cfg.is_open()) {
            std::string line;
            while (std::getline(cfg, line)) {
                if (line.find("run/main_scene=\"") != std::string::npos) {
                    auto start = line.find('\"') + 1;
                    auto end = line.rfind('\"');
                    if (start < end) {
                        return line.substr(start, end - start);
                    }
                }
            }
        }
    }
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(".")) {
            if (entry.path().extension() == ".tscn") {
                return "res://" + entry.path().lexically_relative(".").generic_string();
            }
        }
    } catch (...) {}
    return "res://scenes/main.tscn";
}

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.getHierarchy", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to query scene hierarchy from Godot: " + res.error().message);
    }

    // Offline mode: resolve scene file path if root is a node path (/root) or empty
    std::string root = args.value("root_path", "");
    if (root.empty() || root == "/root" || root == "." || !strings::endsWith(root, ".tscn")) {
        root = findProjectMainScene();
    }
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

    json offline_msg = {
        {"status", "offline"},
        {"message", "Godot Editor is offline. Connect Godot Editor with Didi GDExtension to inspect live in-memory SceneTree, or provide a '.tscn' path in root_path."}
    };
    return CallToolResult::error("Godot Editor is offline. " + offline_msg.dump(2));
}

CallToolResult handleSceneInstantiateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string node_type = args.value("node_type", "Node");
    std::string scene_path = args.value("scene_path", "");
    std::string parent_path = args.value("parent_path", "/root");
    std::string name = args.value("name", "");

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.instantiateNode", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to instantiate node in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to instantiate nodes interactively.");
}

CallToolResult handleSceneRemoveNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    if (target_node.empty()) {
        return CallToolResult::error("Parameter 'target_node' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.removeNode", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to remove node in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to delete nodes with UndoRedo.");
}

CallToolResult handleSceneReparentNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    std::string new_parent = args.value("new_parent_path", "");
    bool keep_global = args.value("keep_global_transform", true);

    if (target_node.empty() || new_parent.empty()) {
        return CallToolResult::error("Parameters 'target_node' and 'new_parent_path' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.reparentNode", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to reparent node in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to reparent nodes with UndoRedo.");
}

CallToolResult handleSceneSetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    std::string property_name = args.value("property_name", "");

    if (target_node.empty() || property_name.empty() || !args.contains("value")) {
        return CallToolResult::error("Parameters 'target_node', 'property_name', and 'value' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.setProperty", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to set node property: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to mutate node properties.");
}

CallToolResult handleSceneGetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    std::string property_name = args.value("property_name", "");

    if (target_node.empty() || property_name.empty()) {
        return CallToolResult::error("Parameters 'target_node' and 'property_name' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.getProperty", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to query node property: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to inspect live node properties.");
}

CallToolResult handleSceneDuplicateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    if (target_node.empty()) {
        return CallToolResult::error("Parameter 'target_node' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.duplicateNode", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to duplicate node: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot to duplicate nodes with UndoRedo.");
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
