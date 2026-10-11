// GodotBridge::execute serves its methods from one table (#1255). The editor
// hook keeps its own list of the methods it hands to execute, and nothing held
// the two together: a method the hook routed and the table did not serve was a
// 501 only a live call would find, and one the table served and the hook did
// not route was never reached.

#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/godot_bridge.hpp"

#include <functional>
#include <set>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
void registerTest(const std::string& name, std::function<void()> fn);

namespace {

void every_routed_method_has_one_entry() {
    const auto names = didi::godot::GodotBridge::methodNames();
    const std::set<std::string> table(names.begin(), names.end());
    ASSERT_EQ(table.size(), names.size());

    std::set<std::string> routed(didi::godot::EditorHook::liveBridgeMethods().begin(),
                                 didi::godot::EditorHook::liveBridgeMethods().end());
    // Served before the table, and only in the test build's extension.
    routed.erase("phase7SignalTest.configure");
    // Asked in process by runtime_explore_scene, never over the wire.
    ASSERT_TRUE(routed.count("runtime.missingInputActions") == 0);
    routed.insert("runtime.missingInputActions");

    std::string missing;
    for (const auto& method : routed) {
        if (!table.count(method)) missing += " " + method;
    }
    std::string unrouted;
    for (const auto& method : table) {
        if (!routed.count(method)) unrouted += " " + method;
    }
    if (!missing.empty() || !unrouted.empty()) {
        throw std::runtime_error("routed and not in the table:" + missing + "; in the table and not routed:" +
                                 unrouted);
    }
}

struct RegisterBridgeMethodTests {
    RegisterBridgeMethodTests() {
        registerTest("BridgeMethods.EveryRoutedMethodHasOneEntry", every_routed_method_has_one_entry);
    }
} register_bridge_method_tests;

} // namespace
