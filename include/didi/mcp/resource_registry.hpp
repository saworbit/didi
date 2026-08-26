#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"

namespace didi {
namespace mcp {

class ResourceRegistry {
public:
    static ResourceRegistry& instance();

    void registerResource(ResourceDefinition res);
    const ResourceDefinition* getResource(const std::string& uri) const;
    std::vector<ResourceDefinition> listResources() const;
    Result<std::string> readResource(const std::string& uri);

    void setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client);
    void registerAllDefaultResources();

private:
    ResourceRegistry() = default;
    std::unordered_map<std::string, ResourceDefinition> m_resources;
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
};

} // namespace mcp
} // namespace didi
