#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Viewport fail-closed contract", "[phase7][viewport][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"viewport_set_camera_transform", "viewport_toggle_debug_draw"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool && tool->capability.implemented);
        ASSERT_TRUE(registry.callTool(name, didi::json::object()).isError);
    }
}
struct RegisterPhase7Viewport { RegisterPhase7Viewport() { registerTest("Phase7Viewport fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Viewport;

// TASK 3 VIEWPORT BEHAVIOR BEGIN
#include "didi/common/ipc_channel.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/tools/resolved_tool_binding.hpp"

#include <limits>
#include <memory>
#include <vector>

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

namespace didi::mcp {
CallToolResult handleViewportSetCameraTransform(const ResolvedToolBinding&, const json&,
                                                std::shared_ptr<ipc::IIpcClient>);
CallToolResult handleViewportToggleDebugDraw(const ResolvedToolBinding&, const json&,
                                             std::shared_ptr<ipc::IIpcClient>);
}

namespace {

class ViewportRecordingClient final : public didi::ipc::IIpcClient {
public:
    bool connect(const std::string&, int) override { return true; }
    void disconnect() override { connected = false; }
    bool isConnected() const override { return connected; }
    didi::Result<didi::json> sendRequest(const std::string& method,
                                         const didi::json& params,
                                         int timeout_ms) override {
        ++requests;
        last_method = method;
        last_params = params;
        last_timeout_ms = timeout_ms;
        return didi::json{{"native_observed", true}};
    }

    bool connected{true};
    int requests{0};
    int last_timeout_ms{0};
    std::string last_method;
    didi::json last_params;
};

didi::json viewportResultPayload(const didi::mcp::CallToolResult& result) {
    ASSERT_TRUE(!result.content.empty());
    return didi::json::parse(result.content.front().text);
}

void assertViewportRejected(const didi::mcp::CallToolResult& result,
                            const std::shared_ptr<ViewportRecordingClient>& client) {
    ASSERT_TRUE(result.isError);
    const auto payload = viewportResultPayload(result);
    ASSERT_EQ(payload["error"]["code"], 400);
    ASSERT_EQ(payload["error"]["data"]["retryable"], false);
    ASSERT_EQ(client->requests, 0);
}

didi::json vector3(double x, double y, double z) {
    return {{"x", x}, {"y", y}, {"z", z}};
}

void test_phase7_viewport_handlers_reject_invalid_requests_before_dispatch() {
    using namespace didi::mcp;
    const auto camera_binding = resolveAliasBinding("viewport_set_camera_transform");
    auto camera_client = std::make_shared<ViewportRecordingClient>();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const auto& invalid : std::vector<didi::json>{
             nullptr, didi::json::array(), didi::json::object(),
             {{"camera_path", ""}, {"position", vector3(0, 0, 0)}},
             {{"camera_path", std::string(1025, 'c')}, {"position", vector3(0, 0, 0)}},
             {{"camera_path", "/root/Camera3D"}},
             {{"camera_path", "/root/Camera3D"}, {"position", {{"x", 0}, {"y", 0}}}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(1000001, 0, 0)}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(infinity, 0, 0)}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(0, 0, 0)},
              {"rotation_degrees", vector3(0, -360001, 0)}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(0, 0, 0)}, {"fov", 0}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(0, 0, 0)}, {"fov", 180}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(0, 0, 0)}, {"fov", "75"}},
             {{"camera_path", "/root/Camera3D"}, {"position", vector3(0, 0, 0)}, {"extra", true}}}) {
        assertViewportRejected(
            handleViewportSetCameraTransform(camera_binding, invalid, camera_client), camera_client);
    }

    const auto debug_binding = resolveAliasBinding("viewport_toggle_debug_draw");
    auto debug_client = std::make_shared<ViewportRecordingClient>();
    for (const auto& invalid : std::vector<didi::json>{
             nullptr, didi::json::array(), didi::json::object(),
             {{"wireframe", false}}, {{"wireframe", true}},
             {{"collision_shapes", 1}}, {{"navigation_mesh", "yes"}},
             {{"collision_shapes", true}, {"extra", false}}}) {
        assertViewportRejected(
            handleViewportToggleDebugDraw(debug_binding, invalid, debug_client), debug_client);
    }
}

void test_phase7_viewport_handlers_forward_exact_requests_once() {
    using namespace didi::mcp;
    {
        auto client = std::make_shared<ViewportRecordingClient>();
        const didi::json arguments = {
            {"camera_path", "/root/Main/Camera3D"}, {"position", vector3(1.25, -2.5, 3.75)}};
        auto result = handleViewportSetCameraTransform(
            resolveAliasBinding("viewport_set_camera_transform"), arguments, client);
        ASSERT_TRUE(!result.isError);
        ASSERT_EQ(client->requests, 1);
        ASSERT_EQ(client->last_method, "vision.setCameraTransform");
        ASSERT_EQ(client->last_params, arguments);
        ASSERT_EQ(client->last_timeout_ms, 17000);
    }
    {
        auto client = std::make_shared<ViewportRecordingClient>();
        const didi::json arguments = {{"navigation_mesh", true}};
        auto result = handleViewportToggleDebugDraw(
            resolveAliasBinding("viewport_toggle_debug_draw"), arguments, client);
        ASSERT_TRUE(!result.isError);
        ASSERT_EQ(client->requests, 1);
        ASSERT_EQ(client->last_method, "vision.toggleDebugDraw");
        ASSERT_EQ(client->last_params, arguments);
        ASSERT_EQ(client->last_timeout_ms, 17000);
    }
}

void test_phase7_viewport_methods_are_editor_only_but_reach_the_bridge() {
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("viewport test reset");
    const didi::json camera = {
        {"camera_path", "/root/Main/Camera3D"}, {"position", vector3(0, 0, 0)}};

    didi::godot::EditorHookTestAccess::setSessionKind(
        hook, didi::runtime::SessionKind::editor);
    for (const auto* method : {"vision.setCameraTransform", "vision.toggleDebugDraw"}) {
        const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, method, method == std::string_view("vision.setCameraTransform")
                              ? camera : didi::json{{"collision_shapes", true}});
        ASSERT_TRUE(response.contains("error"));
        ASSERT_TRUE(response["error"]["message"] != "session_kind_rejected");
        ASSERT_TRUE(response["error"]["message"].get<std::string>().find("no trustworthy live implementation") ==
                    std::string::npos);
    }

    didi::godot::EditorHookTestAccess::setSessionKind(
        hook, didi::runtime::SessionKind::game);
    for (const auto* method : {"vision.setCameraTransform", "vision.toggleDebugDraw"}) {
        const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, method, didi::json::object());
        ASSERT_EQ(response["error"]["code"], 409);
        ASSERT_EQ(response["error"]["message"], "session_kind_rejected");
    }
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
}

struct RegisterPhase7ViewportBehavior {
    RegisterPhase7ViewportBehavior() {
        registerTest("Phase7Viewport.StrictHandlerValidation",
                     test_phase7_viewport_handlers_reject_invalid_requests_before_dispatch);
        registerTest("Phase7Viewport.ExactForwarding",
                     test_phase7_viewport_handlers_forward_exact_requests_once);
        registerTest("Phase7Viewport.EditorOnlyAdmission",
                     test_phase7_viewport_methods_are_editor_only_but_reach_the_bridge);
    }
} g_registerPhase7ViewportBehavior;

} // namespace
// TASK 3 VIEWPORT BEHAVIOR END
