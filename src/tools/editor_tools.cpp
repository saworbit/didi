#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"

namespace didi {
namespace mcp {

CallToolResult handleEditorUndo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.undo", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor undo failed: ");
    }
    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to execute Undo transactions.");
}

CallToolResult handleEditorRedo(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.redo", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor redo failed: ");
    }
    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to execute Redo transactions.");
}

CallToolResult handleEditorSaveScene(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.saveScene", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor save scene failed: ");
    }
    return CallToolResult::notConnected("Godot Editor is offline. Launch Godot to save active scene.");
}

CallToolResult handleEditorReloadProject(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("editor.reloadProject", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::fromError(res.error(), "Editor reload project failed: ");
    }
    // Offline this is the whole job. Callers reach for it after changing files
    // outside Didi, and it used to report a re-index while the cached index
    // carried on answering with what it read before.
    offline::ResourceIndexer::invalidateSharedIndex();
    return CallToolResult::successJson({
        {"status", "offline"},
        {"message", "Offline caches re-indexed."}
    });
}

} // namespace mcp
} // namespace didi
