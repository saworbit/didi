// Phase 7 signal admission, from the dispatcher's side.
//
// This file previously asserted the opposite: that every signal method was
// refused in production. That was correct while the feature had never been
// trialled in a production-configuration binary. It has now been, on Godot
// 4.5.1, 4.6.2 and 4.7.2, so the contract these assert is the new one:
// signals dispatch, and the failure-injection seam does not exist.

#include "didi/gdextension/editor_hook.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)
#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
void registerTest(const std::string& name, std::function<void()> fn);

namespace {

// Admitted means reaching the bridge. Without an engine the bridge cannot
// satisfy the call, but the one answer that must no longer appear is the 501
// that said the method has no implementation at all.
void phase7_signal_production_admission_open() {
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
        if (!result.contains("error")) continue;
        const auto code = result.at("error").at("code").get<int>();
        ASSERT_TRUE(code != 501);
        ASSERT_TRUE(code != 404);
    }
}

// The seam configurator is compiled out of every build except the dedicated
// signal-test extension. The native test binary is not that build, so the
// method must be unknown here -- the same answer a release would give.
void phase7_signal_test_seam_absent_from_production() {
    auto& hook = didi::godot::EditorHook::instance();
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    const auto result = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "phase7SignalTest.configure", {{"seam", "missing_required_api"}});
    ASSERT_TRUE(result.contains("error"));
    ASSERT_EQ(result.at("error").at("code").get<int>(), 404);
}

struct RegisterPhase7SignalAdmission {
    RegisterPhase7SignalAdmission() {
        registerTest("Phase7Signals.ProductionAdmissionOpen",
                     [] { phase7_signal_production_admission_open(); });
        registerTest("Phase7Signals.TestSeamAbsentFromProduction",
                     [] { phase7_signal_test_seam_absent_from_production(); });
    }
} register_phase7_signal_admission;

} // namespace
