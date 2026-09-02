#include "didi/gdextension/editor_hook.hpp"
#include "didi/mcp/tool_registry.hpp"
#include "didi/runtime/input_injection.hpp"

#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) throw std::runtime_error("Assertion failed: " #a " == " #b);

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::json;
using didi::runtime::InjectedInputEvent;
using didi::runtime::parseInputInjectionRequest;

json batch(std::initializer_list<json> events) {
    return {{"events", json(events)}};
}

void test_registry_advertises_live_game_only_mutation() {
    auto& registry = didi::mcp::ToolRegistry::instance();
    registry.registerAllDefaultTools();
    const auto* canonical = registry.getTool("runtime_inject_input");
    const auto* alias = registry.getTool("inject_input_event");
    ASSERT_TRUE(canonical && alias);
    ASSERT_TRUE(canonical->capability.implemented && alias->capability.implemented);
    ASSERT_EQ(canonical->toJson()["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_EQ(alias->toJson()["_meta"]["didi"]["executionModes"], json::array({"live"}));
    ASSERT_EQ(canonical->inputSchema, alias->inputSchema);
    // A mutation, so it previews; not destructive, so no token.
    ASSERT_TRUE(canonical->inputSchema["properties"].contains("dry_run"));
    ASSERT_TRUE(!canonical->inputSchema["properties"].contains("confirmation_token"));
    // Without a session the call fails for want of a route, not an implementation.
    for (const auto* name : {"runtime_inject_input", "inject_input_event"}) {
        const auto result = registry.callTool(
            name, batch({{{"type", "action"}, {"action_name", "jump"}, {"pressed", true}}}));
        ASSERT_TRUE(result.isError);
        ASSERT_TRUE(result.content[0].text.find("no trustworthy execution path") == std::string::npos);
    }
}

void test_parses_every_event_kind_with_defaults() {
    auto parsed = parseInputInjectionRequest(batch({
        {{"type", "action"}, {"action_name", "jump"}, {"pressed", true}},
        {{"type", "key"}, {"keycode", 65}, {"pressed", true}, {"shift_pressed", true}},
        {{"type", "mouse_button"}, {"button_index", 1}, {"pressed", false}},
        {{"type", "joypad_button"}, {"button_index", 0}, {"pressed", true}, {"device", 0}},
        {{"type", "joypad_motion"}, {"axis", 0}, {"axis_value", -0.5}, {"device", 3}},
    }));
    ASSERT_TRUE(parsed.isOk());
    const auto& events = parsed.value();
    ASSERT_EQ(events.size(), 5u);
    ASSERT_EQ(events[0].kind, InjectedInputEvent::Kind::action);
    ASSERT_EQ(events[0].strength, 1.0);
    ASSERT_EQ(std::string(events[0].kindName()), "action");
    ASSERT_EQ(events[1].kind, InjectedInputEvent::Kind::key);
    ASSERT_EQ(events[1].keycode, 65);
    ASSERT_EQ(events[1].physical_keycode, 0);
    ASSERT_TRUE(events[1].shift_pressed && !events[1].alt_pressed && !events[1].echo);
    ASSERT_EQ(events[1].device, -1);
    ASSERT_EQ(events[2].kind, InjectedInputEvent::Kind::mouse_button);
    ASSERT_EQ(events[2].factor, 1.0);
    ASSERT_TRUE(!events[2].double_click && !events[2].pressed);
    ASSERT_EQ(events[3].kind, InjectedInputEvent::Kind::joypad_button);
    ASSERT_EQ(events[3].pressure, 1.0);
    ASSERT_EQ(events[3].device, 0);
    ASSERT_EQ(events[4].kind, InjectedInputEvent::Kind::joypad_motion);
    ASSERT_EQ(events[4].axis_value, -0.5);
    ASSERT_EQ(std::string(events[4].kindName()), "joypad_motion");
    ASSERT_TRUE(parseInputInjectionRequest(
        {{"events", json::array({{{"type", "action"}, {"action_name", "a"}, {"pressed", true}}})},
         {"target_context", "game_input"}}).isOk());
}

void test_rejects_malformed_batches() {
    const json bad[] = {
        json::object(),
        {{"events", json::array()}},
        {{"events", "jump"}},
        {{"events", json::array({{{"type", "action"}, {"action_name", "a"}, {"pressed", true}}})},
         {"target_context", "editor_input"}},
        {{"events", json::array({{{"type", "action"}, {"action_name", "a"}, {"pressed", true}}})},
         {"duration_ms", 100}},
        // Per-kind rules from the contract.
        batch({{{"type", "action"}, {"action_name", "jump"}}}),
        batch({{{"type", "action"}, {"action_name", ""}, {"pressed", true}}}),
        batch({{{"type", "action"}, {"action_name", std::string(129, 'j')}, {"pressed", true}}}),
        batch({{{"type", "action"}, {"action_name", "jump"}, {"pressed", true}, {"strength", 1.5}}}),
        batch({{{"type", "action"}, {"action_name", "jump"}, {"pressed", true}, {"duration_ms", 5}}}),
        batch({{{"type", "key"}, {"pressed", true}}}),
        batch({{{"type", "key"}, {"keycode", 0}, {"pressed", true}}}),
        batch({{{"type", "key"}, {"unicode", 1114112}, {"pressed", true}}}),
        batch({{{"type", "key"}, {"keycode", 65}, {"pressed", true}, {"device", 32}}}),
        batch({{{"type", "key"}, {"keycode", 65}, {"pressed", true}, {"shift", true}}}),
        batch({{{"type", "mouse_button"}, {"button_index", 0}, {"pressed", true}}}),
        batch({{{"type", "mouse_button"}, {"button_index", 10}, {"pressed", true}}}),
        batch({{{"type", "mouse_button"}, {"button_index", 1}, {"pressed", true}, {"factor", 9}}}),
        batch({{{"type", "joypad_button"}, {"button_index", 22}, {"pressed", true}, {"device", 0}}}),
        batch({{{"type", "joypad_button"}, {"button_index", 0}, {"pressed", true}}}),
        batch({{{"type", "joypad_button"}, {"button_index", 0}, {"pressed", true}, {"device", -1}}}),
        batch({{{"type", "joypad_motion"}, {"axis", 6}, {"axis_value", 0.0}, {"device", 0}}}),
        batch({{{"type", "joypad_motion"}, {"axis", 0}, {"axis_value", 1.5}, {"device", 0}}}),
        batch({{{"type", "joypad_motion"}, {"axis", 0}, {"device", 0}}}),
        batch({{{"type", "mouse_motion"}, {"pressed", true}}}),
        batch({"jump"}),
    };
    for (const auto& params : bad) {
        auto parsed = parseInputInjectionRequest(params);
        ASSERT_TRUE(parsed.isErr());
        ASSERT_EQ(parsed.error().code, 400);
    }
    // A bad event anywhere fails the whole batch; nothing is partially accepted.
    auto mixed = parseInputInjectionRequest(batch({
        {{"type", "action"}, {"action_name", "jump"}, {"pressed", true}},
        {{"type", "key"}, {"pressed", true}}}));
    ASSERT_TRUE(mixed.isErr());
}

void test_enforces_count_and_byte_caps() {
    json thirty_two = json::array();
    for (int index = 0; index < 32; ++index) {
        thirty_two.push_back({{"type", "action"}, {"action_name", "a"}, {"pressed", true}});
    }
    ASSERT_TRUE(parseInputInjectionRequest({{"events", thirty_two}}).isOk());
    json thirty_three = thirty_two;
    thirty_three.push_back({{"type", "action"}, {"action_name", "a"}, {"pressed", true}});
    auto too_many = parseInputInjectionRequest({{"events", thirty_three}});
    ASSERT_TRUE(too_many.isErr());
    ASSERT_EQ(too_many.error().code, 400);

    // 32 events of 128-byte names stay under the cap; padding the request
    // past 32 KiB with an oversized name is a 413, not a 400, so a caller can
    // tell "too big" from "wrong".
    json oversized = json::array();
    oversized.push_back({{"type", "action"}, {"action_name", std::string(40000, 'x')}, {"pressed", true}});
    auto too_big = parseInputInjectionRequest({{"events", oversized}});
    ASSERT_TRUE(too_big.isErr());
    ASSERT_EQ(too_big.error().code, 413);
}

void test_hook_rejects_editor_sessions_before_the_bridge() {
    // Defense in depth on the extension side: an editor session never reaches
    // Input.parse_input_event, whatever the standalone policy said.
    auto& hook = didi::godot::EditorHook::instance();
    hook.cancelPendingCommands("test reset");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, didi::runtime::SessionKind::editor);
    const auto response = didi::godot::EditorHookTestAccess::executeOnMainThread(
        hook, "runtime.injectInput",
        batch({{{"type", "action"}, {"action_name", "jump"}, {"pressed", true}}}));
    ASSERT_EQ(response["error"]["code"], 409);
    ASSERT_EQ(response["error"]["message"], "session_kind_rejected");
    didi::godot::EditorHookTestAccess::setSessionKind(hook, std::nullopt);
}

struct RegisterPhase7cInput {
    RegisterPhase7cInput() {
        registerTest("phase7c_input.registry_live_game_only", test_registry_advertises_live_game_only_mutation);
        registerTest("phase7c_input.parses_every_kind", test_parses_every_event_kind_with_defaults);
        registerTest("phase7c_input.rejects_malformed", test_rejects_malformed_batches);
        registerTest("phase7c_input.count_and_byte_caps", test_enforces_count_and_byte_caps);
        registerTest("phase7c_input.hook_rejects_editor", test_hook_rejects_editor_sessions_before_the_bridge);
    }
} g_registerPhase7cInput;

} // namespace
