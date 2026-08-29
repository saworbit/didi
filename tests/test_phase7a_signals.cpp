#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Signals fail-closed contract", "[phase7][signals][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"signal_list_connections", "signal_connect", "signal_disconnect", "signal_emit"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool != nullptr); ASSERT_TRUE(!tool->capability.implemented);
        const auto result = registry.callTool(name, didi::json::object()); ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find(name) != std::string::npos);
    }
}
struct RegisterPhase7Signals { RegisterPhase7Signals() { registerTest("Phase7Signals fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Signals;
