#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Input fail-closed contract", "[phase7][input][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    const auto* canonical = registry.getTool("runtime_inject_input");
    const auto* alias = registry.getTool("inject_input_event");
    ASSERT_TRUE(canonical && alias && !canonical->capability.implemented && !alias->capability.implemented);
    ASSERT_TRUE(canonical->inputSchema == alias->inputSchema);
    ASSERT_TRUE(registry.callTool("runtime_inject_input", didi::json::object()).isError);
    ASSERT_TRUE(registry.callTool("inject_input_event", didi::json::object()).isError);
}
struct RegisterPhase7Input { RegisterPhase7Input() { registerTest("Phase7Input fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Input;
