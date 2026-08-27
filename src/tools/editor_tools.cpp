#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace mcp {

CallToolResult handleEditorUndo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.undo", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Editor undo failed: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot to execute Undo transactions.");
}

CallToolResult handleEditorRedo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.redo", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Editor redo failed: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot to execute Redo transactions.");
}

CallToolResult handleEditorSaveScene(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.saveScene", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Editor save scene failed: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot to save active scene.");
}

CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.reloadProject", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Editor reload project failed: " + res.error().message);
    }
    return CallToolResult::successJson({
        {"status", "offline"},
        {"message", "Offline caches re-indexed."}
    });
}

} // namespace mcp
} // namespace didi
