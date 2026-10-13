#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/runtime/input_injection.hpp"
#include "didi/common/godot_error.hpp"
#include <set>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

// runtime.injectInput. Every event is constructed and fully configured before
// the first one is dispatched, so a bad event in position five fails the
// batch with nothing sent. Dispatch is Input.parse_input_event, which returns
// void: the count is calls made, not events accepted, and only a fixture that
// observes _input can say more.
namespace bridge_internal {

constexpr int64_t kInputParseInputEventHash = 3754044979LL;

Result<VariantValue> buildInjectedEvent(const runtime::InjectedInputEvent& spec) {
    using Kind = runtime::InjectedInputEvent::Kind;
    const char* class_name = nullptr;
    switch (spec.kind) {
        case Kind::action: class_name = "InputEventAction"; break;
        case Kind::key: class_name = "InputEventKey"; break;
        case Kind::mouse_button: class_name = "InputEventMouseButton"; break;
        case Kind::mouse_motion: class_name = "InputEventMouseMotion"; break;
        case Kind::joypad_button: class_name = "InputEventJoypadButton"; break;
        case Kind::joypad_motion: class_name = "InputEventJoypadMotion"; break;
        case Kind::window_focus:
            return Error::internal("A window_focus event is told to the window, not built as an InputEvent");
    }
    NativeName native_class(class_name);
    if (!native_class.valid()) return Error::internal("Failed to construct input event class name");
    auto object = constructObject(native_class.ptr());
    if (!object) return Error::internal(std::string("Godot could not construct ") + class_name);
    auto fail = [&](const Error& error) -> Result<VariantValue> {
        GodotApi::instance().object_destroy(object);
        return error;
    };
    auto set_int = [&](const char* owner, const char* method, int64_t hash, int64_t number) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, number);
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, hash, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    auto set_bool = [&](const char* owner, const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    auto set_float = [&](const char* owner, const char* method, double number) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, number);
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, 373806689LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    // InputEventMouse.set_position, set_global_position and
    // InputEventMouseMotion.set_relative all carry 743155724 on 4.5.1, 4.6.2
    // and 4.7.2.
    auto set_vector2 = [&](const char* owner, const char* method, double x, double y) -> Result<void> {
        auto value = makeVector2(x, y);
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, 743155724LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    // A click has one position in the viewport, and the same point in its
    // canvas layer unless the caller said otherwise.
    auto set_mouse_position = [&]() -> Result<void> {
        auto placed = set_vector2("InputEventMouse", "set_position", spec.position_x, spec.position_y);
        if (placed.isErr()) return placed;
        const double global_x = spec.has_global_position ? spec.global_position_x : spec.position_x;
        const double global_y = spec.has_global_position ? spec.global_position_y : spec.position_y;
        return set_vector2("InputEventMouse", "set_global_position", global_x, global_y);
    };
    auto check = [&](const Result<void>& step) { return step.isErr(); };

    Result<void> step = Result<void>::ok();
    switch (spec.kind) {
        case Kind::action: {
            auto action = makeStringName(spec.action_name);
            if (action.isErr()) return fail(action.error());
            auto set = callObject(object, "InputEventAction", "set_action", 3304788590LL, {&action.value()});
            if (set.isErr()) return fail(set.error());
            if (check(step = set_bool("InputEventAction", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_float("InputEventAction", "set_strength", spec.strength))) return fail(step.error());
            break;
        }
        case Kind::key: {
            if (spec.keycode > 0 &&
                check(step = set_int("InputEventKey", "set_keycode", 888074362LL, spec.keycode))) return fail(step.error());
            if (spec.physical_keycode > 0 &&
                check(step = set_int("InputEventKey", "set_physical_keycode", 888074362LL, spec.physical_keycode))) return fail(step.error());
            if (spec.unicode > 0 &&
                check(step = set_int("InputEventKey", "set_unicode", 1286410249LL, spec.unicode))) return fail(step.error());
            if (check(step = set_bool("InputEventKey", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventKey", "set_echo", spec.echo))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_shift_pressed", spec.shift_pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_alt_pressed", spec.alt_pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_ctrl_pressed", spec.ctrl_pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_meta_pressed", spec.meta_pressed))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::mouse_button: {
            if (check(step = set_int("InputEventMouseButton", "set_button_index", 3624991109LL, spec.button_index))) return fail(step.error());
            if (check(step = set_bool("InputEventMouseButton", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventMouseButton", "set_double_click", spec.double_click))) return fail(step.error());
            if (check(step = set_float("InputEventMouseButton", "set_factor", spec.factor))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            if (spec.has_position && check(step = set_mouse_position())) return fail(step.error());
            break;
        }
        case Kind::mouse_motion: {
            if (check(step = set_mouse_position())) return fail(step.error());
            if (check(step = set_vector2("InputEventMouseMotion", "set_relative", spec.relative_x, spec.relative_y))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::joypad_button: {
            if (check(step = set_int("InputEventJoypadButton", "set_button_index", 1466368136LL, spec.button_index))) return fail(step.error());
            if (check(step = set_bool("InputEventJoypadButton", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_float("InputEventJoypadButton", "set_pressure", spec.pressure))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::joypad_motion: {
            if (check(step = set_int("InputEventJoypadMotion", "set_axis", 1332685170LL, spec.axis))) return fail(step.error());
            if (check(step = set_float("InputEventJoypadMotion", "set_axis_value", spec.axis_value))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::window_focus:
            break;
    }
    auto wrapped = makeObject(object);
    if (wrapped.isErr()) return fail(wrapped.error());
    return wrapped;
}

// Which of these action names the project's InputMap does not define.
//
// An InputEventAction naming an action nobody declared dispatches without
// complaint and moves nothing, so an exploration run driving one would watch a
// game it was never touching and report the stillness as a stuck interval. That
// is the one answer that tool must never give, so the names are checked before
// the run starts rather than inferred from its result.
//
// InputMap.has_action is 2619796661 on Godot 4.5.1, 4.6.2 and 4.7.2, checked by
// dumping the extension API from each rather than assuming hashes are stable.
json inputMapMissingActions(const json& params, const std::string& session_kind) {
    if (session_kind != "game") return bridgeError(409, "session_kind_rejected");
    if (!params.is_object() || !params.contains("actions") || !params["actions"].is_array()) {
        return errorJson(400, "actions must be an array");
    }
    auto input_map = singleton("InputMap");
    if (input_map.isErr()) {
        return errorJson(501, "InputMap singleton is unavailable: " + input_map.error().message);
    }
    auto bind = requireMethodBind("InputMap", "has_action", 2619796661LL);
    if (bind.isErr()) {
        return errorJson(501, "InputMap.has_action is unavailable: " + bind.error().message);
    }

    json missing = json::array();
    for (const auto& entry : params["actions"]) {
        if (!entry.is_string()) return errorJson(400, "each action must be a string");
        const auto action = entry.get<std::string>();
        auto name = makeStringName(action);
        if (name.isErr()) return errorJson(500, "Failed to construct an action name");
        auto answered = callObject(input_map.value(), "InputMap", "has_action", 2619796661LL,
                                   {&name.value()});
        if (answered.isErr()) return errorJson(500, answered.error().message);
        auto defined = scalarFromVariant<GDExtensionBool>(answered.value(),
                                                          GDEXTENSION_VARIANT_TYPE_BOOL);
        if (defined.isErr()) return errorJson(500, defined.error().message);
        if (defined.value() == 0) missing.push_back(action);
    }
    return liveResult({{"missing", std::move(missing)}, {"session_kind", session_kind}});
}

// Events injected while the tree was paused, held until it runs again.
//
// A paused tree delivers _input only to nodes whose process_mode lets them run
// while paused, and a node that pauses never sees an event dispatched during
// the pause: Input hands it out on the next frame, which processes nothing for
// that node, and by the frame after that it is gone. Pause, press, step, look
// is the sequence the three tools exist for, and it reported success at every
// step and did nothing (#594). So a batch injected while paused is held here
// and handed to Input the moment the tree resumes, through runtime.setPaused,
// which the step also goes through, so it lands in the first frame that
// processes. Main thread only, like everything else in this file.
std::vector<VariantValue> g_queuedInjectedInput;
constexpr size_t kMaxQueuedInjectedEvents = 256;

Result<bool> liveSceneTreeIsPaused() {
    auto tree = liveSceneTree();
    if (tree.isErr()) return tree.error();
    auto paused = callObject(tree.value(), "SceneTree", "is_paused", 36873697LL);
    if (paused.isErr()) return paused.error();
    auto flag = scalarFromVariant<GDExtensionBool>(paused.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (flag.isErr()) return flag.error();
    return flag.value() != 0;
}

// What Input holds for each control a batch touched, in the order the batch
// first named it. A pressed control reads back pressed only once the engine
// has handled the event, so this is the game's view, not the request's.
// Mouse motion moves nothing Input exposes, so it has no entry. Every bind
// below carries the same hash on 4.5.1, 4.6.2 and 4.7.2.
Result<json> injectedInputState(const std::vector<runtime::InjectedInputEvent>& specs,
                                GDExtensionObjectPtr input) {
    using Kind = runtime::InjectedInputEvent::Kind;
    const auto read_bool = [&](const char* method, int64_t hash,
                               std::vector<const VariantValue*> args) -> Result<bool> {
        auto value = callObject(input, "Input", method, hash, args);
        if (value.isErr()) return value.error();
        auto flag = scalarFromVariant<GDExtensionBool>(value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (flag.isErr()) return flag.error();
        return flag.value() != 0;
    };
    const auto read_float = [&](const char* method, int64_t hash,
                                std::vector<const VariantValue*> args) -> Result<double> {
        auto value = callObject(input, "Input", method, hash, args);
        if (value.isErr()) return value.error();
        return scalarFromVariant<double>(value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
    };
    json state = json::array();
    std::set<std::string> seen;
    for (const auto& spec : specs) {
        switch (spec.kind) {
            case Kind::action: {
                if (!seen.insert("action:" + spec.action_name).second) break;
                auto name = makeStringName(spec.action_name);
                auto exact = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
                if (name.isErr()) return name.error();
                if (exact.isErr()) return exact.error();
                auto pressed = read_bool("is_action_pressed", 1558498928LL, {&name.value(), &exact.value()});
                if (pressed.isErr()) return pressed.error();
                auto strength = read_float("get_action_strength", 801543509LL, {&name.value(), &exact.value()});
                if (strength.isErr()) return strength.error();
                state.push_back({{"type", "action"}, {"action_name", spec.action_name},
                                 {"pressed", pressed.value()}, {"strength", strength.value()}});
                break;
            }
            case Kind::key: {
                // A key named only by its physical position reads back that way.
                const bool physical = spec.keycode == 0 && spec.physical_keycode != 0;
                const int64_t code = physical ? spec.physical_keycode : spec.keycode;
                const char* field = physical ? "physical_keycode" : "keycode";
                if (code == 0 || !seen.insert(std::string(field) + ":" + std::to_string(code)).second) break;
                auto key = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, code);
                if (key.isErr()) return key.error();
                auto pressed = read_bool(physical ? "is_physical_key_pressed" : "is_key_pressed",
                                         1938909964LL, {&key.value()});
                if (pressed.isErr()) return pressed.error();
                state.push_back({{"type", "key"}, {field, code}, {"pressed", pressed.value()}});
                break;
            }
            case Kind::mouse_button: {
                if (!seen.insert("mouse:" + std::to_string(spec.button_index)).second) break;
                auto button = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, spec.button_index);
                if (button.isErr()) return button.error();
                auto pressed = read_bool("is_mouse_button_pressed", 1821097125LL, {&button.value()});
                if (pressed.isErr()) return pressed.error();
                state.push_back({{"type", "mouse_button"}, {"button_index", spec.button_index},
                                 {"pressed", pressed.value()}});
                break;
            }
            case Kind::joypad_button: {
                if (!seen.insert("joy:" + std::to_string(spec.device) + ":" +
                                 std::to_string(spec.button_index)).second) break;
                auto device = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, spec.device);
                auto button = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, spec.button_index);
                if (device.isErr()) return device.error();
                if (button.isErr()) return button.error();
                auto pressed = read_bool("is_joy_button_pressed", 787208542LL, {&device.value(), &button.value()});
                if (pressed.isErr()) return pressed.error();
                state.push_back({{"type", "joypad_button"}, {"device", spec.device},
                                 {"button_index", spec.button_index}, {"pressed", pressed.value()}});
                break;
            }
            case Kind::joypad_motion: {
                if (!seen.insert("axis:" + std::to_string(spec.device) + ":" +
                                 std::to_string(spec.axis)).second) break;
                auto device = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, spec.device);
                auto axis = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, spec.axis);
                if (device.isErr()) return device.error();
                if (axis.isErr()) return axis.error();
                auto value = read_float("get_joy_axis", 4063175957LL, {&device.value(), &axis.value()});
                if (value.isErr()) return value.error();
                state.push_back({{"type", "joypad_motion"}, {"device", spec.device}, {"axis", spec.axis},
                                 {"axis_value", value.value()}});
                break;
            }
            case Kind::mouse_motion:
            case Kind::window_focus:
                break;
        }
    }
    return state;
}

// What a real change of window focus tells a game, told to it (#1197): the
// window's NOTIFICATION_WM_WINDOW_FOCUS_OUT, its focus_exited signal, and the
// main loop's NOTIFICATION_APPLICATION_FOCUS_OUT, which the SceneTree passes to
// every node; the IN ones for a gain. Measured on 4.5.1, 4.6.2 and 4.7.2, paused
// or not, each reached a game's own handler in that order. The operating
// system's focus does not move, so the answer says the change was synthetic and
// what Window.has_focus() still answers.
json injectWindowFocus(const std::vector<runtime::InjectedInputEvent>& events,
                       const std::string& session_kind) {
    auto tree = liveSceneTree();
    if (tree.isErr()) return errorJson(tree.error());
    auto root = liveSceneTreeRoot(tree.value());
    if (root.isErr()) return errorJson(root.error());
    json changes = json::array();
    json event_types = json::array();
    size_t dispatched = 0;
    for (const auto& event : events) {
        const bool in = event.focused;
        auto window_what = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(in ? 1004 : 1005));
        auto loop_what = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(in ? 2016 : 2017));
        auto reversed = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        auto signal = makeStringName(in ? "focus_entered" : "focus_exited");
        if (window_what.isErr() || loop_what.isErr() || reversed.isErr() || signal.isErr()) {
            return errorJson(500, "Failed to build the focus notification");
        }
        Result<VariantValue> step = callObject(root.value(), "Node", "propagate_notification",
                                               1286410249LL, {&window_what.value()});
        if (step.isOk()) {
            step = callObject(root.value(), "Object", "emit_signal", 4047867050LL, {&signal.value()});
        }
        if (step.isOk()) {
            step = callObject(tree.value(), "Object", "notification", 4023243586LL,
                              {&loop_what.value(), &reversed.value()});
        }
        if (step.isErr()) {
            return errorJson(dispatched == 0 ? 500 : 504, step.error().message,
                             {{"outcome", dispatched == 0 ? "not_started" : "unknown_outcome"},
                              {"dispatched_event_count", dispatched}, {"retryable", false}});
        }
        ++dispatched;
        event_types.push_back("window_focus");
        changes.push_back({{"focused", in},
                           {"delivered", json::array({in ? "NOTIFICATION_WM_WINDOW_FOCUS_IN"
                                                         : "NOTIFICATION_WM_WINDOW_FOCUS_OUT",
                                                      in ? "focus_entered" : "focus_exited",
                                                      in ? "NOTIFICATION_APPLICATION_FOCUS_IN"
                                                         : "NOTIFICATION_APPLICATION_FOCUS_OUT"})}});
    }
    auto has_focus = callObject(root.value(), "Window", "has_focus", 36873697LL);
    json os_focused = nullptr;
    if (has_focus.isOk()) {
        auto flag = scalarFromVariant<GDExtensionBool>(has_focus.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (flag.isOk()) os_focused = flag.value() != 0;
    }
    auto paused = liveSceneTreeIsPaused();
    json answer = {{"dispatched_event_count", dispatched}, {"queued_event_count", 0},
                   {"event_types", std::move(event_types)},
                   {"outcome", "completed"}, {"rollback", "not_available"},
                   {"paused", paused.isOk() ? json(paused.value()) : json(false)},
                   {"delivery", "synthetic_focus"},
                   // No control of Input's was touched, so it holds nothing to report.
                   {"input_state", json::array()},
                   {"window_focus", {{"synthetic", true}, {"changes", std::move(changes)},
                                     {"os_window_focused", os_focused}}},
                   {"limitation",
                    "Synthetic: the game's window and main loop were told its focus changed, which "
                    "runs the game's own handlers. The operating system's focus did not change, so "
                    "Window.has_focus() answers as before, held keys were not released, and mouse "
                    "capture is as it was."},
                   {"session_kind", session_kind}};
    return liveResult(answer);
}

json injectInput(const json& params, const std::string& session_kind) {
    if (session_kind != "game") return bridgeError(409, "session_kind_rejected");
    auto parsed = runtime::parseInputInjectionRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    // The parser keeps a focus change in a batch of its own.
    if (!parsed.value().empty() &&
        parsed.value().front().kind == runtime::InjectedInputEvent::Kind::window_focus) {
        return injectWindowFocus(parsed.value(), session_kind);
    }
    auto input = singleton("Input");
    if (input.isErr()) return errorJson(501, "Input singleton is unavailable: " + input.error().message);
    auto bind = requireMethodBind("Input", "parse_input_event", kInputParseInputEventHash);
    if (bind.isErr()) return errorJson(501, "Input.parse_input_event is unavailable: " + bind.error().message);

    // An action the game's InputMap does not define is refused before
    // anything is dispatched or queued. Godot dispatches an InputEventAction
    // naming one without complaint and nothing reacts to it, so the call
    // answered "completed" for a misspelled action exactly as for a real one,
    // and runtime_explore_scene already refused the same batch for this
    // reason. It is also the state a game is in when an action was written
    // with project_set_input_action after the game started.
    json action_names = json::array();
    for (const auto& spec : parsed.value()) {
        if (spec.kind == runtime::InjectedInputEvent::Kind::action) {
            action_names.push_back(spec.action_name);
        }
    }
    if (!action_names.empty()) {
        const auto checked = inputMapMissingActions({{"actions", action_names}}, session_kind);
        if (checked.contains("error")) return checked;
        const auto undefined = checked.value("missing", json::array());
        if (!undefined.empty()) {
            std::string listed;
            for (const auto& name : undefined) {
                listed += (listed.empty() ? "\"" : ", \"") + name.get<std::string>() + "\"";
            }
            return errorJson(
                400,
                "The game's InputMap does not define " + listed +
                    ", so an event pressing it would be dispatched and nothing would react to "
                    "it. No event in this batch was sent. An action added with "
                    "project_set_input_action takes effect when a game starts, so a game that "
                    "was already running does not have it until it is restarted.",
                {{"code", "invalid_arguments"},
                 {"reason", "undefined_input_action"},
                 {"undefined_actions", undefined},
                 {"retryable", false}});
        }
    }

    std::vector<VariantValue> events;
    events.reserve(parsed.value().size());
    json event_types = json::array();
    for (const auto& spec : parsed.value()) {
        auto built = buildInjectedEvent(spec);
        if (built.isErr()) return errorJson(built.error());
        events.push_back(std::move(built.value()));
        event_types.push_back(spec.kindName());
    }
    auto paused = liveSceneTreeIsPaused();
    if (paused.isErr()) return errorJson(paused.error());
    // A pause menu runs with PROCESS_MODE_ALWAYS, and a paused tree still
    // hands input to nodes that process while paused, the way it hands them a
    // person's click. Asked for, the batch goes to Input now: the menu sees
    // it, a paused node does not, and no frame is stepped (#1191).
    const bool deliver_while_paused = paused.value() && runtime::deliversWhilePaused(params);
    if (paused.value() && !deliver_while_paused) {
        const size_t count = events.size();
        if (g_queuedInjectedInput.size() + count > kMaxQueuedInjectedEvents) {
            return bridgeError(409, "input_queue_full",
                               {{"queued_event_count", g_queuedInjectedInput.size()},
                                {"max_queued_events", kMaxQueuedInjectedEvents}});
        }
        for (auto& event : events) g_queuedInjectedInput.push_back(std::move(event));
        // Nothing reached Input, and what it holds says so: a held press
        // reads back as not pressed until the tree resumes.
        auto held_state = injectedInputState(parsed.value(), input.value());
        json held = {{"dispatched_event_count", 0}, {"queued_event_count", count},
                     {"event_types", std::move(event_types)},
                     {"outcome", "queued"}, {"rollback", "not_available"},
                     {"paused", true}, {"delivery", "next_unpaused_frame"},
                     {"message", "The game is paused, so a node that pauses would not receive "
                                 "these events. They are held and handed to Input when "
                                 "runtime_step or runtime_set_paused resumes the tree, and "
                                 "land in the first frame that processes."},
                     {"session_kind", session_kind}};
        if (held_state.isOk()) {
            held["input_state"] = std::move(held_state.value());
        } else {
            held["input_state"] = nullptr;
            held["limitation"] = "What Input holds could not be read: " + held_state.error().message;
        }
        return liveResult(held);
    }
    size_t dispatched = 0;
    for (auto& event : events) {
        auto sent = callObject(input.value(), "Input", "parse_input_event", kInputParseInputEventHash, {&event});
        if (sent.isErr()) {
            // Some events went out and this one did not. That is exactly the
            // ambiguous case the contract calls unknown, and it is not retried.
            return errorJson(dispatched == 0 ? 500 : 504, sent.error().message,
                             {{"outcome", dispatched == 0 ? "not_started" : "unknown_outcome"},
                              {"dispatched_event_count", dispatched}, {"retryable", false}});
        }
        ++dispatched;
    }
    json answer = {{"dispatched_event_count", dispatched}, {"queued_event_count", 0},
                   {"event_types", std::move(event_types)},
                   {"outcome", "completed"}, {"rollback", "not_available"},
                   {"paused", paused.value()},
                   {"delivery", deliver_while_paused ? "while_paused" : "immediate"},
                   {"session_kind", session_kind}};
    if (deliver_while_paused) {
        answer["message"] = "The game stays paused. Nodes that process while paused, such as a "
                            "PROCESS_MODE_ALWAYS pause menu, received these events; paused nodes "
                            "did not, and no frame was stepped.";
    }
    // parse_input_event only buffers an event. Input's state, and every
    // node's _input, see it when the engine next flushes, which it does at
    // least once a frame: asked of 4.5.1, 4.6.2 and 4.7.2 from a headless
    // game, nothing had changed straight after parse_input_event and
    // everything had after flush_buffered_events. So the batch is flushed
    // here, the call Godot documents for exactly this, and the answer says
    // what Input holds now rather than how many calls were made (#1019).
    auto flushed = callObject(input.value(), "Input", "flush_buffered_events", 3218959716LL);
    auto state = flushed.isOk() ? injectedInputState(parsed.value(), input.value())
                                : Result<json>(flushed.error());
    if (state.isOk()) {
        answer["input_state"] = std::move(state.value());
    } else {
        // The events went out either way; only the read of what they did failed.
        answer["input_state"] = nullptr;
        answer["limitation"] = "The events were sent, but what Input holds afterwards could not be "
                               "read: " + state.error().message;
    }
    // The flush runs the handlers, and a Retry button that processes while
    // paused unpauses the tree in its own. The pause read before sending was
    // reported, so a click that resumed the game said it stayed paused (#1303).
    if (deliver_while_paused) {
        auto after = liveSceneTreeIsPaused();
        if (after.isOk() && !after.value()) {
            answer["paused"] = false;
            answer["message"] = "The game is no longer paused: something these events reached "
                                "unpaused it. Nodes that process while paused received them while "
                                "the tree was paused, and no frame was stepped.";
        }
    }
    return liveResult(answer);
}

} // namespace

Result<size_t> GodotBridge::releaseQueuedInput() {
    if (g_queuedInjectedInput.empty()) return size_t{0};
    std::vector<VariantValue> pending;
    pending.swap(g_queuedInjectedInput);
    auto input = singleton("Input");
    if (input.isErr()) return input.error();
    size_t released = 0;
    for (auto& event : pending) {
        auto sent = callObject(input.value(), "Input", "parse_input_event", kInputParseInputEventHash, {&event});
        if (sent.isErr()) {
            return Error(sent.error().code,
                         "Released " + std::to_string(released) + " held input event(s) before Input "
                         "refused one; the rest were dropped: " + sent.error().message);
        }
        ++released;
    }
    return released;
}

} // namespace didi::godot
