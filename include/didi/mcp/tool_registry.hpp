#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/runtime/session_client.hpp"
#include "didi/mcp/mutation_safety.hpp"

namespace didi {
namespace mcp {

// A machine-readable description of the registered tool surface.
//
// This exists so that documentation can be validated against the software
// instead of against other documentation. `didi --dump-tool-manifest` emits
// toJson(), and tools/validate_documentation.py derives every published count
// from it. Name lists are sorted so the artifact is byte-stable across runs.
struct ToolManifest {
    std::vector<std::string> canonical;      // every non-legacy registration
    std::vector<std::string> legacy;         // registrations from kLegacyToolNames
    std::vector<std::string> implemented;    // canonical names with a real execution path
    std::vector<std::string> unimplemented;  // canonical names reserved for compatibility

    json toJson() const;
};

class ToolRegistry {
public:
    static ToolRegistry& instance();

    void registerTool(ToolDefinition tool);
    const ToolDefinition* getTool(const std::string& name) const;
    std::vector<ToolDefinition> listTools() const;
    ToolManifest buildManifest() const;
    CallToolResult callTool(const std::string& name, const json& arguments);

    void setIpcClient(std::shared_ptr<ipc::IIpcClient> ipc_client);
    std::shared_ptr<ipc::IIpcClient> getIpcClient() const;
    void setRuntimeSessionClient(std::shared_ptr<runtime::IRuntimeSessionClient> session_client);
    std::shared_ptr<runtime::IRuntimeSessionClient> getRuntimeSessionClient() const;

    void registerAllDefaultTools();

private:
    ToolRegistry() = default;
    std::unordered_map<std::string, ToolDefinition> m_tools;
    std::shared_ptr<ipc::IIpcClient> m_sourceIpcClient;
    std::shared_ptr<ipc::IIpcClient> m_ipcClient;
    std::shared_ptr<runtime::IRuntimeSessionClient> m_runtimeSessionClient;
    MutationSafety m_mutationSafety;
};

} // namespace mcp
} // namespace didi
