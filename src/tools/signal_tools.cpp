#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"

namespace didi {
namespace mcp {

CallToolResult handleSignalListConnections(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    if (target_node.empty()) {
        return CallToolResult::error("Parameter 'target_node' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("signal.listConnections", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to query signal connections: " + res.error().message);
    }

    // Offline mode: provide sample signals structure based on node conventions
    json offline_signals = {
        {"status", "offline"},
        {"target_node", target_node},
        {"signals", json::array({
            {
                {"name", "tree_entered"},
                {"arguments", json::array()},
                {"connections", json::array()}
            },
            {
                {"name", "tree_exited"},
                {"arguments", json::array()},
                {"connections", json::array()}
            },
            {
                {"name", "ready"},
                {"arguments", json::array()},
                {"connections", json::array()}
            }
        })},
        {"message", "Godot Editor is offline. Connect Godot with Didi plugin to inspect live signal bindings."}
    };
    return CallToolResult::successJson(offline_signals);
}

CallToolResult handleSignalConnect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string emitter = args.value("emitter_node", "");
    std::string signal_name = args.value("signal_name", "");
    std::string target = args.value("target_node", "");
    std::string method = args.value("target_method", "");

    if (emitter.empty() || signal_name.empty() || target.empty() || method.empty()) {
        return CallToolResult::error("Parameters 'emitter_node', 'signal_name', 'target_node', and 'target_method' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("signal.connect", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to connect signal: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to bind signals dynamically with EditorUndoRedoManager.");
}

CallToolResult handleSignalDisconnect(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string emitter = args.value("emitter_node", "");
    std::string signal_name = args.value("signal_name", "");
    std::string target = args.value("target_node", "");
    std::string method = args.value("target_method", "");

    if (emitter.empty() || signal_name.empty() || target.empty() || method.empty()) {
        return CallToolResult::error("Parameters 'emitter_node', 'signal_name', 'target_node', and 'target_method' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("signal.disconnect", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to disconnect signal: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to unbind signals.");
}

CallToolResult handleSignalEmit(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_node = args.value("target_node", "");
    std::string signal_name = args.value("signal_name", "");
    json signal_args = args.value("arguments", json::array());

    if (target_node.empty() || signal_name.empty()) {
        return CallToolResult::error("Parameters 'target_node' and 'signal_name' are required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("signal.emit", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to emit signal: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor or game session to trigger signals dynamically.");
}

} // namespace mcp
} // namespace didi
