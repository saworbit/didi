#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Diagnostics fail-closed contract", "[phase7][diagnostics][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"runtime_get_call_stack"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool && !tool->capability.implemented);
        ASSERT_TRUE(registry.callTool(name, didi::json::object()).isError);
    }
}
struct RegisterPhase7Diagnostics { RegisterPhase7Diagnostics() { registerTest("Phase7Diagnostics fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Diagnostics;
