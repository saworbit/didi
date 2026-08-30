#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include <fstream>
#include <regex>
#include <filesystem>

namespace didi {
namespace mcp {

static std::string findProjectMainScene() {
    // Resolve through the configured project root rather than the process
    // working directory, so --project and DIDI_PROJECT_ROOT are honoured.
    const auto config = paths::resolveProjectFile("project.godot");
    if (config.isErr()) return "";
    std::ifstream cfg(config.value());
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
    return "";
}

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.getHierarchy", args, ::didi::ipc::kWaitForDefinitiveResponse);
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
    if (root.empty()) {
        return CallToolResult::error(
            "No offline scene path was provided and project.godot has no run/main_scene.");
    }
    auto resolved = paths::resolveProjectFile(root);
    if (resolved.isErr() || resolved.value().extension() != ".tscn") {
        return CallToolResult::error(
            "Invalid offline scene path: " +
            (resolved.isErr() ? resolved.error().message : std::string("path must identify a .tscn file")));
    }
    std::ifstream file(resolved.value());

    if (file.is_open()) {
        struct NodeEntry {
            std::string name;
            std::string type;
            std::string parent;
            std::string instance_path;
            json properties = json::object();
            json transform = json::object();
        };

        std::vector<NodeEntry> nodes;
        std::unordered_map<std::string, std::string> ext_resources;

        static const std::regex ext_res_regex(R"re(\[ext_resource type="([^"]+)" path="([^"]+)" id="([^"]+)"\])re");

        std::string line;
        NodeEntry* current_node = nullptr;

        while (std::getline(file, line)) {
            std::string trimmed = strings::trim(line);
            if (trimmed.empty()) continue;

            std::smatch ext_match;
            if (std::regex_match(trimmed, ext_match, ext_res_regex)) {
                ext_resources[ext_match[3].str()] = ext_match[2].str();
                continue;
            }

        if (strings::startsWith(trimmed, "[node ") && strings::endsWith(trimmed, "]")) {
            NodeEntry ne;
            static const std::regex name_regex(R"re(name="([^"]+)")re");
            static const std::regex type_regex(R"re(type="([^"]+)")re");
            static const std::regex parent_regex(R"re(parent="([^"]+)")re");
            static const std::regex inst_regex(R"re(instance=ExtResource\("([^"]+)"\))re");

            std::smatch match;
            if (std::regex_search(trimmed, match, name_regex)) {
                ne.name = match[1].str();
            } else {
                continue;
            }

            if (std::regex_search(trimmed, match, type_regex)) {
                ne.type = match[1].str();
            } else {
                ne.type = "Instance";
            }

            if (std::regex_search(trimmed, match, parent_regex)) {
                ne.parent = match[1].str();
            } else {
                ne.parent = "";
            }

            if (std::regex_search(trimmed, match, inst_regex)) {
                std::string ext_id = match[1].str();
                if (ext_resources.count(ext_id)) {
                    ne.instance_path = ext_resources[ext_id];
                }
            }

            nodes.push_back(ne);
            current_node = &nodes.back();
            continue;
        }

        if (current_node && trimmed.find('=') != std::string::npos && !strings::startsWith(trimmed, "[")) {
            auto eq_pos = trimmed.find('=');
            std::string key = strings::trim(trimmed.substr(0, eq_pos));
            std::string val = strings::trim(trimmed.substr(eq_pos + 1));

            // An array, dictionary or multiline string value continues on the
            // following lines. Read them until the brackets balance, or the
            // value is truncated to its first line and every continuation line
            // is silently dropped.
            auto unbalanced = [](const std::string& text) {
                int depth = 0;
                bool in_string = false;
                for (size_t i = 0; i < text.size(); ++i) {
                    const char character = text[i];
                    if (in_string) {
                        if (character == '\\') { ++i; continue; }
                        if (character == '"') in_string = false;
                        continue;
                    }
                    if (character == '"') in_string = true;
                    else if (character == '[' || character == '{' || character == '(') ++depth;
                    else if (character == ']' || character == '}' || character == ')') --depth;
                }
                return depth > 0 || in_string;
            };

            std::string continuation;
            while (unbalanced(val) && std::getline(file, continuation)) {
                val += "\n" + strings::trim(continuation);
            }

            if (key == "transform") {
                current_node->transform = {{"raw", val}};
            } else if (args.value("include_properties", true)) {
                current_node->properties[key] = val;
            }
        }
    }

    if (nodes.empty()) {
        json empty_tree = {
            {"name", "Root"},
            {"type", "Node"},
            {"path", "/root"},
            {"children", json::array()}
        };
        return CallToolResult::successJson({{"source", "parsed_tscn_file"}, {"file_path", root}, {"scene_tree", empty_tree}});
    }

    std::unordered_map<int, std::vector<int>> children_by_index;
    std::unordered_map<std::string, int> path_to_index;

    std::string root_name = nodes[0].name;
    path_to_index["."] = 0;
    path_to_index[root_name] = 0;

    std::vector<std::string> node_full_paths(nodes.size());
    node_full_paths[0] = "/root/" + root_name;

    for (size_t i = 1; i < nodes.size(); ++i) {
        std::string p = nodes[i].parent;
        int parent_idx = 0;
        std::string this_rel_path;

        if (p == "." || p.empty()) {
            parent_idx = 0;
            this_rel_path = nodes[i].name;
        } else {
            if (path_to_index.count(p)) {
                parent_idx = path_to_index[p];
            } else {
                parent_idx = 0;
            }
            this_rel_path = p + "/" + nodes[i].name;
        }

        node_full_paths[i] = node_full_paths[parent_idx] + "/" + nodes[i].name;
        path_to_index[this_rel_path] = static_cast<int>(i);
        children_by_index[parent_idx].push_back(static_cast<int>(i));
    }

        int max_depth = args.value("max_depth", 10);
        std::function<json(int, int)> buildNode = [&](int idx, int depth) -> json {
            const auto& ne = nodes[idx];
            json n = {
                {"name", ne.name},
                {"type", ne.type},
                {"path", node_full_paths[idx]},
                {"properties", ne.properties},
                {"children", json::array()}
            };
            if (!ne.instance_path.empty()) {
                n["instance"] = ne.instance_path;
            }
            if (!ne.transform.empty()) {
                n["transform"] = ne.transform;
            }
            if (depth < max_depth && children_by_index.count(idx)) {
                for (int child_idx : children_by_index[idx]) {
                    n["children"].push_back(buildNode(child_idx, depth + 1));
                }
            }
            return n;
        };

        json tree = buildNode(0, 0);
        json tree_res = {
            {"source", "parsed_tscn_file"},
            {"file_path", root},
            {"scene_tree", tree}
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
        auto res = ipc->sendRequest("scene.instantiateNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
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
        auto res = ipc->sendRequest("scene.removeNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
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
    (void)keep_global;

    if (target_node.empty() || new_parent.empty()) {
        return CallToolResult::error("Parameters 'target_node' and 'new_parent_path' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.reparentNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
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
        auto res = ipc->sendRequest("scene.setProperty", args, ::didi::ipc::kWaitForDefinitiveResponse);
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
        auto res = ipc->sendRequest("scene.getProperty", args, ::didi::ipc::kWaitForDefinitiveResponse);
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
        auto res = ipc->sendRequest("scene.duplicateNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
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

static CallToolResult forwardLiveSceneWiring(const json& args,
                                             const std::shared_ptr<ipc::IIpcClient>& ipc,
                                             const char* method,
                                             const char* operation) {
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error(std::string("Godot Editor is offline. Launch Godot to ") + operation + ".");
    }
    auto response = ipc->sendRequest(method, args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::error(std::string("Failed to ") + operation + ": " + response.error().message);
    }
    return CallToolResult::successJson(response.value());
}

CallToolResult handleSceneListGroups(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.listGroups", "list node groups");
}
CallToolResult handleSceneAddToGroup(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.addToGroup", "add a node to a group");
}
CallToolResult handleSceneRemoveFromGroup(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.removeFromGroup", "remove a node from a group");
}
CallToolResult handleSceneGetGroupMembers(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.getGroupMembers", "query group members");
}
CallToolResult handleSceneCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.create", "create a scene");
}
CallToolResult handleSceneOpen(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.open", "open a scene");
}
CallToolResult handleSceneClose(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.close", "close the active scene");
}
CallToolResult handleScenePackBranch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.packBranch", "pack a scene branch");
}

} // namespace mcp
} // namespace didi
