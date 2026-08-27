#include "didi/mcp/resource_registry.hpp"
#include "didi/offline/resource_indexer.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace didi {
namespace mcp {

ResourceRegistry& ResourceRegistry::instance() {
    static ResourceRegistry s_instance;
    return s_instance;
}

void ResourceRegistry::registerResource(ResourceDefinition res) {
    if (res.uri == "godot://editor/state" || res.uri == "godot://runtime/logs") {
        res.capability = {{"live", "offline_fallback"}, true, {}};
    } else if (res.uri == "godot://project/tree") {
        res.capability = {{"offline_fallback"}, true, {}};
    }
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
    auto result = res->readHandler();
    if (result.isErr()) return result;

    try {
        auto payload = json::parse(result.value());
        if (payload.is_object() && !payload.contains("execution_mode")) {
            const bool supports_live = std::find(res->capability.modes.begin(), res->capability.modes.end(), "live") !=
                                       res->capability.modes.end();
            const bool live = supports_live && m_ipcClient && m_ipcClient->isConnected();
            payload["execution_mode"] = live ? "live" : "offline_fallback";
            return payload.dump(2);
        }
    } catch (const json::exception&) {
        return result;
    }
    return result;
}

void ResourceRegistry::setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client) {
    m_ipcClient = ipc_client;
}

void ResourceRegistry::registerAllDefaultResources() {
    // 1. godot://project/tree
    ResourceDefinition proj_tree;
    proj_tree.uri = "godot://project/tree";
    proj_tree.name = "Godot Project Resource Tree";
    proj_tree.description = "Offline filesystem/resource index rooted at the standalone server's project working directory.";
    proj_tree.mimeType = "application/json";
    proj_tree.readHandler = [this]() -> Result<std::string> {
        offline::ResourceIndexer indexer;
        auto tree = indexer.buildProjectTree(".");
        tree["execution_mode"] = "offline_fallback";
        return tree.dump(2);
    };
    registerResource(std::move(proj_tree));

    // 2. godot://editor/state
    ResourceDefinition editor_state;
    editor_state.uri = "godot://editor/state";
    editor_state.name = "Godot Editor State";
    editor_state.description = "Connection state and active edited-scene root when live, or an explicit offline status.";
    editor_state.mimeType = "application/json";
    editor_state.readHandler = [this]() -> Result<std::string> {
        if (m_ipcClient && m_ipcClient->isConnected()) {
            auto res = m_ipcClient->sendRequest("editor.getState", {}, ipc::kWaitForDefinitiveResponse);
            if (res.isOk()) {
                return res.value().dump(2);
            }
            return Error::internal("Failed to retrieve editor state: " + res.error().message);
        }
        json offline_state = {
            {"status", "offline"},
            {"editor_connected", false},
            {"execution_mode", "offline_fallback"},
            {"message", "Godot Editor GDExtension is not actively running. Start Godot Editor with the Didi plugin to inspect live state."}
        };
        return offline_state.dump(2);
    };
    registerResource(std::move(editor_state));

    // 3. godot://runtime/logs
    ResourceDefinition runtime_logs;
    runtime_logs.uri = "godot://runtime/logs";
    runtime_logs.name = "Godot Runtime Engine Logs";
    runtime_logs.description = "Incremental, sequence-cursored Didi runtime log records when connected, or one explicit standalone-status record offline; not a full Godot debugger stream.";
    runtime_logs.mimeType = "application/json";
    runtime_logs.readHandler = [this]() -> Result<std::string> {
        if (m_ipcClient && m_ipcClient->isConnected()) {
            auto res = m_ipcClient->sendRequest("runtime.getLogs", {}, ipc::kWaitForDefinitiveResponse);
            if (res.isOk()) {
                return res.value().dump(2);
            }
            return Error(res.error().code, "Failed to retrieve live runtime logs: " + res.error().message,
                         {{"execution_mode", "live"},
                          {"error", {{"code", res.error().code}, {"message", res.error().message}}}});
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json logs = {
            {"execution_mode", "offline_fallback"},
            {"records", json::array({
                {{"sequence", 1}, {"timestamp_ms", now}, {"level", "info"},
                 {"source", "standalone"}, {"message", "Didi MCP server active; no runtime session is attached."}}
            })},
            {"next_cursor", 2},
            {"oldest_cursor", 1},
            {"dropped_before_cursor", false}
        };
        return logs.dump(2);
    };
    registerResource(std::move(runtime_logs));
}

} // namespace mcp
} // namespace didi
