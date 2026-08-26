#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"

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

    // Run offline indexer query
    offline::ResourceIndexer indexer;
    indexer.scan(".");
    auto results = indexer.query(search_path, type_filter, fuzzy_query, include_uid);

    json res_arr = json::array();
    for (const auto& r : results) {
        res_arr.push_back(r.toJson());
    }

    json out = {
        {"search_path", search_path},
        {"total_found", results.size()},
        {"resources", res_arr}
    };

    return CallToolResult::successJson(out);
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

    return CallToolResult::error("Godot Editor is offline. Instantiating assets into the live scene tree requires an active editor session.");
}

} // namespace mcp
} // namespace didi
