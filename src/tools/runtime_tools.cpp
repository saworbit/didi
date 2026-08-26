#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/test_runner.hpp"

namespace didi {
namespace mcp {

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string scene_path = args.value("scene_path", "");
    int timeout = args.value("timeout_seconds", 10);
    bool headless = args.value("headless", true);
    bool break_on_error = args.value("break_on_error", true);

    std::vector<std::string> extra_args;
    if (args.contains("extra_args") && args["extra_args"].is_array()) {
        for (const auto& a : args["extra_args"]) {
            if (a.is_string()) extra_args.push_back(a.get<std::string>());
        }
    }

    auto session_res = offline::TestRunner::runSession(scene_path, timeout, headless, break_on_error, extra_args);
    if (!session_res.success) {
        return CallToolResult::error("Test session failed: " + session_res.summary + "\n" + session_res.toJson().dump(2));
    }

    return CallToolResult::successJson(session_res.toJson());
}

CallToolResult handleInjectInputEvent(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string event_type = args.value("event_type", "");
    if (event_type.empty()) {
        return CallToolResult::error("Parameter 'event_type' is required.");
    }

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.injectInput", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to inject input event via GDExtension: " + res.error().message);
    }

    json offline_msg = {
        {"status", "offline"},
        {"message", "Godot game/editor instance is offline. Input event injection requires an active running Godot instance with Didi GDExtension."}
    };
    return CallToolResult::error("Godot instance is offline. " + offline_msg.dump(2));
}

} // namespace mcp
} // namespace didi
