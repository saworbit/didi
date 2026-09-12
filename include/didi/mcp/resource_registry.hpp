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

    // The parameterised URI shapes this server serves but cannot enumerate.
    // Boards are created on demand, so the set of blackboard URIs is not
    // knowable in advance and only `default` can appear in listResources().
    std::vector<ResourceTemplate> listResourceTemplates() const;

    // The mime type for a URI this registry can serve, whether or not that
    // exact URI is registered.
    std::string mimeTypeFor(const std::string& uri) const;
    // The scope says which era asked and which runtime session it named, and
    // reaches the read handler through a thread local because the handler
    // signature is nullary. Defaults to a legacy read, which inherits the
    // attached route the way it always has.
    Result<std::string> readResource(const std::string& uri,
                                     const RequestScope& scope = RequestScope::legacy());

    void setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client);
    void registerAllDefaultResources();

private:
    ResourceRegistry() = default;
    std::unordered_map<std::string, ResourceDefinition> m_resources;
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
};

} // namespace mcp
} // namespace didi
