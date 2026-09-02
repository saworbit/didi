#include "didi/runtime/input_injection.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>

namespace didi {
namespace runtime {

namespace {

bool onlyKeys(const json& object, std::initializer_list<const char*> allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (const auto* key : allowed) {
            if (it.key() == key) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

Result<int64_t> integerField(const json& event, const char* key, int64_t minimum, int64_t maximum,
                             bool required, int64_t fallback) {
    if (!event.contains(key)) {
        if (required) return Error::invalidArgument(std::string(key) + " is required");
        return fallback;
    }
    const auto& value = event[key];
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return Error::invalidArgument(std::string(key) + " must be an integer");
    }
    if (value.is_number_unsigned() &&
        value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return Error::invalidArgument(std::string(key) + " is out of range");
    }
    const auto number = value.get<int64_t>();
    if (number < minimum || number > maximum) {
        return Error::invalidArgument(std::string(key) + " must be from " + std::to_string(minimum) +
                                      " to " + std::to_string(maximum));
    }
    return number;
}

Result<double> numberField(const json& event, const char* key, double minimum, double maximum,
                           bool required, double fallback) {
    if (!event.contains(key)) {
        if (required) return Error::invalidArgument(std::string(key) + " is required");
        return fallback;
    }
    const auto& value = event[key];
    if (!value.is_number()) return Error::invalidArgument(std::string(key) + " must be a number");
    const auto number = value.get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        return Error::invalidArgument(std::string(key) + " must be finite and within " +
                                      std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return number;
}

Result<bool> boolField(const json& event, const char* key, bool required, bool fallback) {
    if (!event.contains(key)) {
        if (required) return Error::invalidArgument(std::string(key) + " is required");
        return fallback;
    }
    if (!event[key].is_boolean()) return Error::invalidArgument(std::string(key) + " must be a boolean");
    return event[key].get<bool>();
}

#define DIDI_TRY(target, expression)                       \
    {                                                      \
        auto result_ = (expression);                       \
        if (result_.isErr()) return result_.error();       \
        target = result_.value();                          \
    }

Result<InjectedInputEvent> parseEvent(const json& event) {
    using Kind = InjectedInputEvent::Kind;
    if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
        return Error::invalidArgument("Each event must be an object with a string type");
    }
    const auto& type = event["type"].get_ref<const std::string&>();
    InjectedInputEvent parsed;
    if (type == "action") {
        parsed.kind = Kind::action;
        if (!onlyKeys(event, {"type", "action_name", "pressed", "strength"})) {
            return Error::invalidArgument("action event contains an unknown property");
        }
        if (!event.contains("action_name") || !event["action_name"].is_string()) {
            return Error::invalidArgument("action_name is required and must be a string");
        }
        parsed.action_name = event["action_name"].get<std::string>();
        if (parsed.action_name.empty() || parsed.action_name.size() > 128) {
            return Error::invalidArgument("action_name must be 1 to 128 bytes");
        }
        DIDI_TRY(parsed.pressed, boolField(event, "pressed", true, false));
        DIDI_TRY(parsed.strength, numberField(event, "strength", 0.0, 1.0, false, 1.0));
    } else if (type == "key") {
        parsed.kind = Kind::key;
        if (!onlyKeys(event, {"type", "keycode", "physical_keycode", "unicode", "pressed", "echo",
                              "shift_pressed", "alt_pressed", "ctrl_pressed", "meta_pressed",
                              "device"})) {
            return Error::invalidArgument("key event contains an unknown property");
        }
        DIDI_TRY(parsed.keycode, integerField(event, "keycode", 1, 2147483647LL, false, 0));
        DIDI_TRY(parsed.physical_keycode,
                 integerField(event, "physical_keycode", 1, 2147483647LL, false, 0));
        DIDI_TRY(parsed.unicode, integerField(event, "unicode", 1, 1114111LL, false, 0));
        if (parsed.keycode == 0 && parsed.physical_keycode == 0 && parsed.unicode == 0) {
            return Error::invalidArgument("key event requires keycode, physical_keycode, or unicode");
        }
        DIDI_TRY(parsed.pressed, boolField(event, "pressed", true, false));
        DIDI_TRY(parsed.echo, boolField(event, "echo", false, false));
        DIDI_TRY(parsed.shift_pressed, boolField(event, "shift_pressed", false, false));
        DIDI_TRY(parsed.alt_pressed, boolField(event, "alt_pressed", false, false));
        DIDI_TRY(parsed.ctrl_pressed, boolField(event, "ctrl_pressed", false, false));
        DIDI_TRY(parsed.meta_pressed, boolField(event, "meta_pressed", false, false));
        DIDI_TRY(parsed.device, integerField(event, "device", -1, 31, false, -1));
    } else if (type == "mouse_button") {
        parsed.kind = Kind::mouse_button;
        if (!onlyKeys(event, {"type", "button_index", "pressed", "double_click", "factor", "device"})) {
            return Error::invalidArgument("mouse_button event contains an unknown property");
        }
        DIDI_TRY(parsed.button_index, integerField(event, "button_index", 1, 9, true, 0));
        DIDI_TRY(parsed.pressed, boolField(event, "pressed", true, false));
        DIDI_TRY(parsed.double_click, boolField(event, "double_click", false, false));
        DIDI_TRY(parsed.factor, numberField(event, "factor", 0.0, 8.0, false, 1.0));
        DIDI_TRY(parsed.device, integerField(event, "device", -1, 31, false, -1));
    } else if (type == "joypad_button") {
        parsed.kind = Kind::joypad_button;
        if (!onlyKeys(event, {"type", "button_index", "pressed", "pressure", "device"})) {
            return Error::invalidArgument("joypad_button event contains an unknown property");
        }
        DIDI_TRY(parsed.button_index, integerField(event, "button_index", 0, 21, true, 0));
        DIDI_TRY(parsed.pressed, boolField(event, "pressed", true, false));
        DIDI_TRY(parsed.pressure, numberField(event, "pressure", 0.0, 1.0, false, 1.0));
        DIDI_TRY(parsed.device, integerField(event, "device", 0, 31, true, 0));
    } else if (type == "joypad_motion") {
        parsed.kind = Kind::joypad_motion;
        if (!onlyKeys(event, {"type", "axis", "axis_value", "device"})) {
            return Error::invalidArgument("joypad_motion event contains an unknown property");
        }
        DIDI_TRY(parsed.axis, integerField(event, "axis", 0, 5, true, 0));
        DIDI_TRY(parsed.axis_value, numberField(event, "axis_value", -1.0, 1.0, true, 0.0));
        DIDI_TRY(parsed.device, integerField(event, "device", 0, 31, true, 0));
    } else {
        return Error::invalidArgument("Unsupported input event type: " + type);
    }
    return parsed;
}

#undef DIDI_TRY

} // namespace

const char* InjectedInputEvent::kindName() const {
    switch (kind) {
        case Kind::action: return "action";
        case Kind::key: return "key";
        case Kind::mouse_button: return "mouse_button";
        case Kind::joypad_button: return "joypad_button";
        case Kind::joypad_motion: return "joypad_motion";
    }
    return "action";
}

Result<std::vector<InjectedInputEvent>> parseInputInjectionRequest(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Input injection params must be an object");
    if (!onlyKeys(params, {"events", "target_context"})) {
        return Error::invalidArgument("Input injection request contains an unknown property");
    }
    if (params.contains("target_context") &&
        (!params["target_context"].is_string() || params["target_context"] != "game_input")) {
        return Error::invalidArgument("target_context must be \"game_input\"");
    }
    if (!params.contains("events") || !params["events"].is_array()) {
        return Error::invalidArgument("events is required and must be an array");
    }
    const auto& events = params["events"];
    if (events.empty() || events.size() > kMaxInjectedEvents) {
        return Error::invalidArgument("events must contain 1 to 32 entries");
    }
    // The byte cap is on the compact request, which is what crossed the wire.
    if (params.dump().size() > kMaxInjectedRequestBytes) {
        return Error(413, "Input injection request exceeds 32 KiB");
    }
    std::vector<InjectedInputEvent> parsed;
    parsed.reserve(events.size());
    for (const auto& event : events) {
        auto result = parseEvent(event);
        if (result.isErr()) return result.error();
        parsed.push_back(std::move(result.value()));
    }
    return parsed;
}

} // namespace runtime
} // namespace didi
