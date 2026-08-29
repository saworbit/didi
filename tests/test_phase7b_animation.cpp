#include "didi/mcp/tool_registry.hpp"
#include <functional>
#include <stdexcept>
#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define TEST_CASE(name, tags) static void phase7_contract_test()
void registerTest(const std::string& name, std::function<void()> fn);
TEST_CASE("Phase7Animation fail-closed contract", "[phase7][animation][contract]") {
    auto& registry = didi::mcp::ToolRegistry::instance(); registry.registerAllDefaultTools();
    for (const auto* name : {"anim_list_tracks", "anim_play_track"}) {
        const auto* tool = registry.getTool(name); ASSERT_TRUE(tool && !tool->capability.implemented);
        ASSERT_TRUE(registry.callTool(name, didi::json::object()).isError);
    }
}
struct RegisterPhase7Animation { RegisterPhase7Animation() { registerTest("Phase7Animation fail-closed contract", [] { phase7_contract_test(); }); } } g_registerPhase7Animation;
