#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/test_runner.hpp"

namespace didi {
namespace mcp {

CallToolResult handleExecuteTestSession(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string scene_path = args.value("scene_path", "");
    int timeout_sec = args.value("timeout_seconds", 10);
    bool headless = args.value("headless", true);
    bool break_on_error = args.value("break_on_error", true);

    std::vector<std::string> extra_args;
    if (args.contains("extra_args") && args["extra_args"].is_array()) {
        for (const auto& a : args["extra_args"]) {
            if (a.is_string()) {
                extra_args.push_back(a.get<std::string>());
            }
        }
    }

    offline::TestRunner runner;
    auto session_res = runner.runSession(scene_path, timeout_sec, headless, break_on_error, extra_args);
    return CallToolResult::successJson(session_res.toJson());
}

CallToolResult handleInjectInputEvent(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string event_type = args.value("event_type", "action");

    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.injectInput", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to inject input event: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor/Game instance is offline. Launch Godot to inject interactive inputs.");
}

CallToolResult handleRuntimeGetCallStack(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.getCallStack", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to fetch debugger call stack: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot in debug mode to inspect live call stacks.");
}

CallToolResult handleRuntimeReadProfiler(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("runtime.readProfiler", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to read profiler telemetry from Godot: " + res.error().message);
    }

    return CallToolResult::error("Godot Editor / game instance is offline. Launch Godot to inspect live profiler metrics (FPS, frame time, draw calls).");
}

} // namespace mcp
} // namespace didi
