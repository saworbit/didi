#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include <fstream>

namespace didi {
namespace mcp {

CallToolResult handleQueryProjectResources(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string search_path = args.value("search_path", "res://");
    std::string type_filter = args.value("type_filter", "");
    std::string fuzzy_query = args.value("fuzzy_query", "");
    bool include_uid = args.value("include_uid", true);

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("asset.query", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
    }

    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto results = indexer.query(search_path, type_filter, fuzzy_query);

    json res_arr = json::array();
    for (const auto& item : results) {
        json j = item.toJson();
        if (!include_uid) j.erase("uid");
        res_arr.push_back(j);
    }

    json out = {
        {"total_found", results.size()},
        {"resources", res_arr}
    };
    return CallToolResult::successJson(out);
}

CallToolResult handleResourceCreate(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string resource_type = args.value("resource_type", "StandardMaterial3D");
    std::string save_path = args.value("save_path", "");
    json properties = args.value("properties", json::object());

    if (save_path.empty()) {
        return CallToolResult::error("Parameter 'save_path' is required (e.g. res://materials/wood.tres).");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("resource.create", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to create resource in Godot: " + res.error().message);
    }

    // Offline generator for common .tres resources
    std::string disk_path = save_path;
    if (strings::startsWith(disk_path, "res://")) disk_path = disk_path.substr(6);

    std::ofstream out(disk_path);
    if (out.is_open()) {
        out << "[gd_resource type=\"" << resource_type << "\" format=3]\n\n"
            << "[resource]\n";
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            out << it.key() << " = " << it.value().dump() << "\n";
        }
        out.close();
        return CallToolResult::successJson({
            {"status", "created_offline"},
            {"save_path", save_path},
            {"resource_type", resource_type}
        });
    }

    return CallToolResult::error("Failed to write resource file to disk: " + disk_path);
}

CallToolResult handleResourceInspect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string resource_path = args.value("resource_path", "");
    if (resource_path.empty()) {
        return CallToolResult::error("Parameter 'resource_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("resource.inspect", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
    }

    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto results = indexer.query(resource_path);
    if (!results.empty()) {
        return CallToolResult::successJson(results[0].toJson());
    }

    return CallToolResult::error("Resource not found: " + resource_path);
}

CallToolResult handleProjectGetUidMap(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto all_res = indexer.query("res://");

    json uid_map = json::object();
    for (const auto& r : all_res) {
        if (!r.uid.empty()) {
            uid_map[r.uid] = r.path;
        }
    }

    return CallToolResult::successJson({
        {"total_uids", uid_map.size()},
        {"uid_map", uid_map}
    });
}

CallToolResult handleInstantiateAsset(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string asset_path = args.value("asset_path", "");
    std::string parent_path = args.value("parent_path", "/root");

    if (asset_path.empty()) {
        return CallToolResult::error("Parameter 'asset_path' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("asset.instantiate", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to instantiate asset in Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to instantiate assets directly into the scene tree.");
}

} // namespace mcp
} // namespace didi
