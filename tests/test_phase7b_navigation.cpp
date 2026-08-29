#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Navigation fail-closed contract", "[phase7][navigation][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"nav_bake_mesh", "nav_query_path"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool && !tool->capability.implemented);
        ASSERT_TRUE(registry.callTool(name, didi::json::object()).isError);
    }
}
struct RegisterPhase7Navigation { RegisterPhase7Navigation() { registerTest("Phase7Navigation fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Navigation;
