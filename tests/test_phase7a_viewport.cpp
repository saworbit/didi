#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Viewport fail-closed contract", "[phase7][viewport][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"viewport_set_camera_transform", "viewport_toggle_debug_draw"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool && !tool->capability.implemented);
        ASSERT_TRUE(registry.callTool(name, didi::json::object()).isError);
    }
}
struct RegisterPhase7Viewport { RegisterPhase7Viewport() { registerTest("Phase7Viewport fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Viewport;
