#include "didi/mcp/project_tools.hpp"

namespace didi::mcp {
namespace {

CallToolResult forwardLiveProject(const json& args,
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

} // namespace

CallToolResult handleProjectListAutoloads(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.listAutoloads", "list project autoloads");
}
CallToolResult handleProjectSetAutoload(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.setAutoload", "persist a project autoload");
}
CallToolResult handleProjectRemoveAutoload(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.removeAutoload", "remove a project autoload");
}
CallToolResult handleProjectListInputActions(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.listInputActions", "list project input actions");
}
CallToolResult handleProjectSetInputAction(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.setInputAction", "persist a project input action");
}
CallToolResult handleProjectRemoveInputAction(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.removeInputAction", "remove a project input action");
}
CallToolResult handleProjectGetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.getSetting", "read a project setting");
}
CallToolResult handleProjectSetSetting(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    return forwardLiveProject(args, ipc, "project.setSetting", "persist a project setting");
}

} // namespace didi::mcp
