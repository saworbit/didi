#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include "didi/tools/editor_copy_refresh.hpp"
#include "didi/tools/hierarchy_view.hpp"
#include <fstream>
#include <regex>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <map>

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

// Applies the view options to a built tree and records what it did, so the live
// and offline paths answer with the same shape and the same metadata.
static CallToolResult shapedHierarchyResult(json payload,
                                            const HierarchyViewOptions& options) {
    if (!payload.contains("scene_tree")) {
        if (!payload.contains("truncated")) payload["truncated"] = false;
        return CallToolResult::successJson(std::move(payload));
    }

    HierarchyViewStats stats;
    payload["scene_tree"] = shapeHierarchy(payload["scene_tree"], options, stats);
    payload["node_count"] = stats.node_count;
    if (options.summary) {
        payload["summary"] = true;
    } else {
        if (!options.class_filter.empty()) {
            payload["class_filter"] = json(options.class_filter);
            payload["matched_nodes"] = stats.matched_nodes;
        }
        if (options.max_nodes > 0) payload["max_nodes"] = options.max_nodes;
    }
    // On every answer from both paths, and covering the bridge's own budget
    // cuts as well as max_depth and max_nodes: it used to appear only when set,
    // so a whole tree and an unflagged one looked the same (Q5).
    payload["truncated"] = payload.value("truncated", false) || stats.truncated;
    return CallToolResult::successJson(std::move(payload));
}

CallToolResult handleGetSceneHierarchy(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    auto view = parseHierarchyViewOptions(args);
    if (view.isErr()) {
        return CallToolResult::fromError(view.error(), "Invalid scene hierarchy request: ");
    }

    // A .tscn root_path is a question about a file, and the file is readable
    // whether or not an editor is attached. It used to go to the live bridge,
    // which resolves node paths only, so the documented "node path or .tscn
    // file path" came back as a 404 naming the file it had just been handed
    // (#483).
    const std::string requested = args.value("root_path", "");
    const bool file_request = strings::endsWith(requested, ".tscn");

    if (ipc && ipc->isConnected() && !file_request) {
        auto res = ipc->sendRequest("scene.getHierarchy", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return shapedHierarchyResult(res.value(), view.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to query scene hierarchy from Godot: ");
    }

    // Offline mode. With no editor there is no scene tree to walk, so a request
    // is answered from a .tscn file or it is refused. Anything that was not a
    // .tscn used to fall back to the main scene silently, so a node path that
    // does not exist, a res://project.godot, or a binary .scn all came back as
    // the whole main scene with nothing saying the question had been replaced
    // (#401).
    const bool substituted = requested.empty() || requested == "/root" || requested == ".";
    std::string root = requested;
    if (substituted) {
        root = findProjectMainScene();
    } else if (!strings::endsWith(root, ".tscn")) {
        return CallToolResult::notConnected(
            "With no editor attached this reads a .tscn file, and '" + requested +
            "' is not one. Pass a res:// path to a .tscn, or omit root_path for the "
            "project's main scene. A node path can only be scoped to with an editor "
            "attached.",
            {{"field", "root_path"}});
    }
    if (root.empty()) {
        return CallToolResult::notConnected(
            "No offline scene path was provided and project.godot has no run/main_scene.",
            {{"field", "root_path"}});
    }
    auto resolved = paths::resolveProjectFile(root);
    if (resolved.isErr()) {
        auto error = resolved.error();
        if (!error.data.is_object()) error.data = json::object();
        error.data["field"] = "root_path";
        return CallToolResult::fromError(error, "Invalid offline scene path: ");
    }
    if (resolved.value().extension() != ".tscn") {
        return CallToolResult::errorJson(
            400, "Invalid offline scene path: path must identify a .tscn file",
            {{"field", "root_path"}});
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
        json empty_res = {{"source", "parsed_tscn_file"}, {"file_path", root},
                          {"scene_tree", empty_tree}};
        if (substituted) {
            empty_res["requested_root_path"] = requested;
            empty_res["substituted_main_scene"] = true;
        }
        return CallToolResult::successJson(std::move(empty_res));
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
        bool depth_truncated = false;

        // What a depth cut removed, counted by type, so a stopped branch reads
        // as stopped rather than as a leaf (#484). The tally covers the whole
        // subtree, which is what children_omitted means for a max_nodes cut.
        auto countSubtree = [&](int idx, std::map<std::string, size_t>& counts, size_t& total) {
            std::vector<int> pending{idx};
            while (!pending.empty()) {
                const int current = pending.back();
                pending.pop_back();
                ++total;
                ++counts[nodes[current].type];
                const auto found = children_by_index.find(current);
                if (found == children_by_index.end()) continue;
                for (int child_idx : found->second) pending.push_back(child_idx);
            }
        };

        std::function<json(int, int)> buildNode = [&](int idx, int depth) -> json {
            const auto& ne = nodes[idx];
            json n = {
                {"name", ne.name},
                {"type", ne.type},
                {"path", node_full_paths[idx]},
                {"properties", ne.properties},
                {"children", json::array()}
            };
            // The scene an instance root comes from, under the name the live
            // walk uses. The root's own instance= line is the scene this one
            // inherits, reported once at the top rather than as the root being
            // an instance of itself (#591).
            if (!ne.instance_path.empty() && idx != 0) {
                n["instance_of"] = ne.instance_path;
            }
            if (!ne.transform.empty()) {
                n["transform"] = ne.transform;
            }
            if (children_by_index.count(idx)) {
                if (depth < max_depth) {
                    for (int child_idx : children_by_index[idx]) {
                        n["children"].push_back(buildNode(child_idx, depth + 1));
                    }
                } else {
                    std::map<std::string, size_t> omitted_counts;
                    size_t omitted_total = 0;
                    for (int child_idx : children_by_index[idx]) {
                        countSubtree(child_idx, omitted_counts, omitted_total);
                    }
                    if (omitted_total > 0) {
                        json summary = json::object();
                        for (const auto& entry : omitted_counts) summary[entry.first] = entry.second;
                        n["children_omitted"] = omitted_total;
                        n["children_summary"] = std::move(summary);
                        depth_truncated = true;
                    }
                }
            }
            return n;
        };

        json tree = buildNode(0, 0);
        json tree_res = {
            {"source", "parsed_tscn_file"},
            {"file_path", root},
            {"scene_tree", std::move(tree)}
        };
        if (!nodes[0].instance_path.empty()) tree_res["inherits"] = nodes[0].instance_path;
        if (depth_truncated) tree_res["truncated"] = true;
        // The caller asked for the main scene without naming it. Say which file
        // answered, so the reply cannot be read as a scoped one.
        if (substituted) {
            tree_res["requested_root_path"] = requested;
            tree_res["substituted_main_scene"] = true;
        }
        return shapedHierarchyResult(std::move(tree_res), view.value());
    }

    // The path resolved to a file and the file would not open. This used to say
    // the editor was offline, which is true of every call that reaches here and
    // is not why it failed. It is the state the script tools answer 403
    // unreadable for, in the same words.
    return CallToolResult::errorJson(
        403,
        "This scene file is there and cannot be read: " + root +
            ". Check its permissions, or whether another process is holding it open.",
        {{"code", "forbidden"}, {"reason", "unreadable"}, {"file_path", root}});
}

CallToolResult handleSceneInstantiateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string node_type = args.value("node_type", "");
    std::string scene_path = args.value("scene_path", "");
    std::string parent_path = args.value("parent_path", "/root");
    std::string name = args.value("name", "");

    // {} is what a caller sends when it has not decided yet, when a schema
    // lookup failed, or when an argument-building step produced nothing.
    // Everywhere else on this surface that costs one 400, because every other
    // mutation either declares required arguments or sits behind the dry-run
    // gate. Here it added a bare Node named Node to the edited scene and said
    // nothing about having chosen both the parent and the type itself (#471).
    //
    // Defaulting parent_path to the edited root is reasonable: there is one
    // obvious answer. Defaulting the thing being instantiated is not.
    if (node_type.empty() && scene_path.empty()) {
        return CallToolResult::errorJson(
            400,
            "Name what to instantiate: node_type for a built-in ClassDB type, or "
            "scene_path for a res:// .tscn to instance. There is no default, because "
            "adding a bare Node to the edited scene is not what an empty request means.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.instantiateNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to instantiate node in Godot: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to instantiate nodes interactively.");
}

CallToolResult handleSceneRemoveNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    if (target_node.empty()) {
        return CallToolResult::errorJson(400, "Parameter 'target_node' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.removeNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to remove node in Godot: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to delete nodes with UndoRedo.");
}

CallToolResult handleSceneReparentNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    std::string new_parent = args.value("new_parent_path", "");
    bool keep_global = args.value("keep_global_transform", true);
    (void)keep_global;

    if (target_node.empty() || new_parent.empty()) {
        return CallToolResult::errorJson(400, "Parameters 'target_node' and 'new_parent_path' are required and must not be empty.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.reparentNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to reparent node in Godot: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to reparent nodes with UndoRedo.");
}

// Calls a method the target node's own script declares.
//
// The bounds are checked here as well as in the bridge, the way signal_emit's
// are, so a malformed request is refused without waking an engine. The bridge
// owns the part this side cannot know: which methods the script actually
// declares, and whether the one named turned out to be a coroutine (#389).
CallToolResult handleSceneCallMethod(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) {
        return CallToolResult::errorJson(400, "Invalid scene_call_method request: arguments must be an object.");
    }
    const std::string target_node = args.value("target_node", "");
    const std::string method_name = args.value("method_name", "");
    if (target_node.empty() || target_node.size() > 1024) {
        return CallToolResult::errorJson(400, "Parameter 'target_node' is required.");
    }
    if (method_name.empty() || method_name.size() > 128) {
        return CallToolResult::errorJson(400, "Parameter 'method_name' is required and must be 1 to 128 characters.");
    }
    if (method_name.front() == '_') {
        return CallToolResult::errorJson(
            400,
            "Refusing to call \"" + method_name +
            "\". A leading underscore is Godot's mark for an engine callback or a script's "
            "private helper, and calling one by hand corrupts node state. Expose the behaviour "
            "under a name without the underscore.",
            {{"field", "method_name"}});
    }
    if (args.contains("arguments")) {
        if (!args["arguments"].is_array() || args["arguments"].size() > 8) {
            return CallToolResult::errorJson(
                400, "Parameter 'arguments' must be an array of at most 8 values.");
        }
        try {
            if (args["arguments"].dump().size() > 8u * 1024u) {
                return CallToolResult::errorJson(400, "Parameter 'arguments' exceeds 8 KiB.");
            }
        } catch (const json::exception&) {
            return CallToolResult::errorJson(400, "Parameter 'arguments' is not valid JSON text.");
        }
    }
    if (args.contains("timeout_seconds")) {
        const auto& value = args["timeout_seconds"];
        if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
            value.get<int64_t>() < 1 || value.get<int64_t>() > 120) {
            return CallToolResult::errorJson(400, "Parameter 'timeout_seconds' must be an integer from 1 to 120.");
        }
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.callMethod", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) return CallToolResult::successJson(res.value());
        return CallToolResult::fromError(res.error(), "Failed to call the method: ");
    }
    return CallToolResult::notConnected(
        "Godot Editor is offline. scene_call_method runs project code in the editor's own "
        "process, so it has no offline meaning. Launch Godot and attach.");
}

namespace {

// One call, or a batch of them in `key` (Q7). A batch and the single form
// together is refused rather than merged: which one the caller meant is not
// something to guess. The bridge checks all of this again, because
// mutate_scene_tree reaches it without passing through here; this refuses a
// malformed request without waking an engine.
std::optional<CallToolResult> refuseMalformedBatch(const json& args, const char* key,
                                                   std::initializer_list<const char*> single,
                                                   bool needs_value) {
    for (const auto* name : single) {
        if (args.contains(name)) {
            return CallToolResult::errorJson(
                400, std::string("Send one call as ") +
                         (needs_value ? "target_node, property_name and value" : "target_node and property_name") +
                         ", or several as " + key + ", not both.",
                {{"field", key}});
        }
    }
    const auto& items = args[key];
    if (!items.is_array() || items.empty() || items.size() > 64) {
        return CallToolResult::errorJson(
            400, std::string("Parameter '") + key + "' must be an array of 1 to 64 objects.", {{"field", key}});
    }
    for (size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const bool named = item.is_object() && item.contains("target_node") &&
                           item["target_node"].is_string() && !item["target_node"].get<std::string>().empty() &&
                           item.contains("property_name") && item["property_name"].is_string() &&
                           !item["property_name"].get<std::string>().empty();
        if (!named || (needs_value && !item.contains("value"))) {
            return CallToolResult::errorJson(
                400, std::string(key) + "[" + std::to_string(index) + "] needs a non-empty target_node and property_name" +
                         (needs_value ? ", and a value." : "."),
                {{"field", key}, {"index", index}});
        }
    }
    return std::nullopt;
}

}  // namespace

CallToolResult handleSceneSetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (args.contains("writes")) {
        if (auto refused = refuseMalformedBatch(args, "writes", {"target_node", "property_name", "value"}, true)) {
            return *refused;
        }
    } else {
        std::string target_node = args.value("target_node", "");
        std::string property_name = args.value("property_name", "");
        if (target_node.empty() || property_name.empty() || !args.contains("value")) {
            return CallToolResult::errorJson(400, "Parameters 'target_node', 'property_name', and 'value' are required, and the names must not be empty. Several writes go in 'writes' instead.");
        }
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.setProperty", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to set node property: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to mutate node properties.");
}

CallToolResult handleSceneGetProperty(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (args.contains("reads")) {
        if (auto refused = refuseMalformedBatch(args, "reads", {"target_node", "property_name"}, false)) {
            return *refused;
        }
    } else {
        std::string target_node = args.value("target_node", "");
        std::string property_name = args.value("property_name", "");
        if (target_node.empty() || property_name.empty()) {
            return CallToolResult::errorJson(400, "Parameters 'target_node' and 'property_name' are required and must not be empty. Several reads go in 'reads' instead.");
        }
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.getProperty", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to query node property: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to inspect live node properties.");
}

CallToolResult handleSceneDuplicateNode(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    if (target_node.empty()) {
        return CallToolResult::errorJson(400, "Parameter 'target_node' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.duplicateNode", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to duplicate node: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to duplicate nodes with UndoRedo.");
}

CallToolResult handleMutateSceneTree(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string action = args.value("action", "");
    std::string target = args.value("target_node", "");

    if (action.empty() || target.empty()) {
        return CallToolResult::errorJson(400, "Missing required parameters: 'action' and 'target_node'.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("scene.mutate", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Failed to mutate scene tree in Godot: ");
    }

    return CallToolResult::notConnected("Godot Editor is offline. Please launch Godot Editor to execute live SceneTree mutations with EditorUndoRedoManager.");
}

static CallToolResult forwardLiveSceneWiring(const json& args,
                                             const std::shared_ptr<ipc::IIpcClient>& ipc,
                                             const char* method,
                                             const char* operation) {
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::notConnected(std::string("Godot Editor is offline. Launch Godot to ") + operation + ".");
    }
    auto response = ipc->sendRequest(method, args, ipc::kWaitForDefinitiveResponse);
    if (response.isErr()) {
        return CallToolResult::fromError(response.error(), std::string("Failed to ") + operation + ": ");
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
    auto result = forwardLiveSceneWiring(args, ipc, "scene.getGroupMembers", "query group members");
    // members is whole; the list of other groups offered beside it is capped at
    // 128, which only known_groups_truncated said (Q5).
    if (!result.isError && result.structuredContent.has_value() &&
        result.structuredContent->is_object()) {
        auto& payload = *result.structuredContent;
        payload["truncated"] = payload.value("known_groups_truncated", false);
        for (auto& item : result.content) {
            if (item.type == "text") item.text = payload.dump();
        }
    }
    return result;
}
CallToolResult handleSceneCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    auto created = forwardLiveSceneWiring(args, ipc, "scene.create", "create a scene");
    if (created.isError || !created.structuredContent.has_value() ||
        !created.structuredContent->is_object() ||
        !created.structuredContent->value("scene_tab_stale", false)) {
        return created;
    }
    // The overwritten scene was open in a tab, which the bridge made current
    // and left holding the tree from before the write. It is rebuilt here, on
    // a later request than the one that switched to it, because on 4.5 and 4.6
    // the editor ignores a scene change for the rest of the frame (#1079).
    // overwrite: true is the consent to lose that tab's changes, as it is for
    // scene_pack_branch.
    // An editor that never answers the rebuild leaves scene_tab_stale saying so.
    auto payload = *created.structuredContent;
    const auto scene_path = payload.value("scene_path", args.value("scene_path", std::string()));
    const auto refresh = refreshEditorCopies(ipc, {scene_path}, true);
    if (refresh.answered) payload.erase("scene_tab_stale");
    reportEditorCopy(payload, refresh);
    return CallToolResult::successJson(payload);
}
CallToolResult handleSceneOpen(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.open", "open a scene");
}
CallToolResult handleSceneClose(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveSceneWiring(args, ipc, "scene.close", "close the active scene");
}
CallToolResult handleScenePackBranch(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    auto packed = forwardLiveSceneWiring(args, ipc, "scene.packBranch", "pack a scene branch");
    if (packed.isError || !packed.structuredContent.has_value() ||
        !packed.structuredContent->is_object() ||
        !packed.structuredContent->value("saved", false)) {
        return packed;
    }
    // The pack writes its file inside the editor, and a tab that has that scene
    // open keeps the tree it had, so the next save put the old scene back over
    // the pack (#1072). Replacing an existing scene took overwrite: true, which
    // is the caller accepting its loss, so the tab is rebuilt from the new file
    // whatever it holds, the way scene_create reloads the scene it overwrites.
    auto payload = *packed.structuredContent;
    const auto scene_path = payload.value("scene_path", args.value("scene_path", std::string()));
    reportEditorCopy(payload, refreshEditorCopies(ipc, {scene_path}, true));
    return CallToolResult::successJson(payload);
}

} // namespace mcp
} // namespace didi
