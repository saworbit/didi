#include "didi/mcp/resource_registry.hpp"
#include "didi/offline/resource_indexer.hpp"
#include <filesystem>
#include <fstream>

namespace didi {
namespace mcp {

ResourceRegistry& ResourceRegistry::instance() {
    static ResourceRegistry s_instance;
    return s_instance;
}

void ResourceRegistry::registerResource(ResourceDefinition res) {
    m_resources[res.uri] = std::move(res);
}

const ResourceDefinition* ResourceRegistry::getResource(const std::string& uri) const {
    auto it = m_resources.find(uri);
    if (it != m_resources.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<ResourceDefinition> ResourceRegistry::listResources() const {
    std::vector<ResourceDefinition> list;
    list.reserve(m_resources.size());
    for (const auto& [uri, r] : m_resources) {
        list.push_back(r);
    }
    return list;
}

Result<std::string> ResourceRegistry::readResource(const std::string& uri) {
    auto res = getResource(uri);
    if (!res) {
        return Error::notFound("Resource not found: " + uri);
    }
    return res->readHandler();
}

void ResourceRegistry::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
}

void ResourceRegistry::registerAllDefaultResources() {
    // 1. godot://project/tree
    ResourceDefinition proj_tree;
    proj_tree.uri = "godot://project/tree";
    proj_tree.name = "Godot Project Resource Tree";
    proj_tree.description = "Complete recursive layout of res:// including scene dependencies and UID maps.";
    proj_tree.mimeType = "application/json";
    proj_tree.readHandler = [this]() -> Result<std::string> {
        if (m_ipcClient && m_ipcClient->isConnected()) {
            auto res = m_ipcClient->sendRequest("asset.query", {{"search_path", "res://"}});
            if (res.isOk()) {
                return res.value().dump(2);
            }
        }
        // Fallback to offline indexer
        offline::ResourceIndexer indexer;
        auto tree = indexer.buildProjectTree(".");
        return tree.dump(2);
    };
    registerResource(std::move(proj_tree));

    // 2. godot://editor/state
    ResourceDefinition editor_state;
    editor_state.uri = "godot://editor/state";
    editor_state.name = "Godot Editor State";
    editor_state.description = "Currently selected scene, selected nodes, active camera position, and Undo/Redo stack depth.";
    editor_state.mimeType = "application/json";
    editor_state.readHandler = [this]() -> Result<std::string> {
        if (m_ipcClient && m_ipcClient->isConnected()) {
            auto res = m_ipcClient->sendRequest("editor.getState");
            if (res.isOk()) {
                return res.value().dump(2);
            }
            return Error::internal("Failed to retrieve editor state: " + res.error().message);
        }
        json offline_state = {
            {"status", "offline"},
            {"editor_connected", false},
            {"message", "Godot Editor GDExtension is not actively running. Start Godot Editor with the Didi plugin to inspect live state."}
        };
        return offline_state.dump(2);
    };
    registerResource(std::move(editor_state));

    // 3. godot://runtime/logs
    ResourceDefinition runtime_logs;
    runtime_logs.uri = "godot://runtime/logs";
    runtime_logs.name = "Godot Runtime Engine Logs";
    runtime_logs.description = "Real-time stream of engine logs, shader compile warnings, and debugger stack frames.";
    runtime_logs.mimeType = "application/json";
    runtime_logs.readHandler = [this]() -> Result<std::string> {
        if (m_ipcClient && m_ipcClient->isConnected()) {
            auto res = m_ipcClient->sendRequest("runtime.getLogs");
            if (res.isOk()) {
                return res.value().dump(2);
            }
        }
        // Check for local engine logs if available
        json logs = {
            {"logs", json::array({
                {{"level", "INFO"}, {"message", "Didi MCP server active."}}
            })}
        };
        return logs.dump(2);
    };
    registerResource(std::move(runtime_logs));
}

} // namespace mcp
} // namespace didi
