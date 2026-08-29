#include "didi/gdextension/editor_hook.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
void registerTest(const std::string& name, std::function<void()> fn);

namespace {

void phase7_signal_production_admission_closed() {
    auto& hook = didi::godot::EditorHook::instance();
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);

    for (const std::string method : {
             "signal.listConnections",
             "signal.connect",
             "signal.disconnect",
             "signal.emit",
         }) {
        const auto result = didi::godot::EditorHookTestAccess::executeOnMainThread(
            hook, method, didi::json::object());
        ASSERT_EQ(result.at("error").at("code"), 501);
        ASSERT_EQ(result.at("error").at("message"),
                  "Method is registered for compatibility but has no trustworthy live implementation: " + method);
    }
}

struct RegisterPhase7SignalAdmission {
    RegisterPhase7SignalAdmission() {
        registerTest("Phase7Signals.ProductionAdmissionClosed",
                     [] { phase7_signal_production_admission_closed(); });
    }
} register_phase7_signal_admission;

} // namespace
