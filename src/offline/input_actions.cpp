#include "didi/offline/input_actions.hpp"
#include "didi/offline/project_settings_file.hpp"

#include <cctype>
#include <cmath>
#include <initializer_list>

namespace didi::offline {
namespace {

bool hasOnlyKeys(const json& value, std::initializer_list<const char*> allowed) {
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

// The first property of `value` that `allowed` does not list, and the list
// itself, as a sentence.
//
// Every other argument refusal on this surface names the property and says what
// the tool takes, which is what #397 and #418 delivered and why the mistake is
// normally cheap. The input-event refusals said only that something was wrong,
// on a tool whose vocabulary was not published at all (#737).
std::string unknownPropertyDetail(const json& value, std::initializer_list<const char*> allowed) {
    std::string offender;
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) {
            offender = it.key();
            break;
        }
    }
    std::string accepted;
    for (const auto* key : allowed) {
        if (!accepted.empty()) accepted += ", ";
        accepted += key;
    }
    return "Unknown property '" + offender + "'. This event type accepts: " + accepted + ".";
}

// One modifier, under either spelling.
//
// Godot's own property is shift_pressed, which is what runtime_inject_input
// takes and what project_set_input_action writes into project.godot. It read
// `shift` and refused `shift_pressed`, so one surface spelled one thing two
// ways and the engine's own name was the one that did not work (#737). The
// engine spelling is the answer; the short one still works, because something
// may depend on it.
Result<std::optional<bool>> modifierFlag(const json& descriptor, const std::string& name) {
    const std::string engine = name + "_pressed";
    const bool has_engine = descriptor.contains(engine);
    const bool has_short = descriptor.contains(name);
    if (has_engine && has_short && descriptor[engine] != descriptor[name]) {
        return Error::invalidArgument("Input event sets '" + engine + "' and '" + name +
                                      "' to different values, and they are the same modifier. "
                                      "Send one, preferably '" + engine +
                                      "', which is Godot's own name for it.");
    }
    if (!has_engine && !has_short) return std::optional<bool>{};
    const auto& value = has_engine ? descriptor[engine] : descriptor[name];
    if (!value.is_boolean()) {
        return Error::invalidArgument(std::string(has_engine ? engine : name) + " must be boolean");
    }
    return std::optional<bool>(value.get<bool>());
}

std::string numberLiteral(double value) {
    auto literal = settingLiteral(json(value));
    return literal.isOk() ? literal.value() : std::to_string(value);
}

} // namespace

Result<void> validateInputActionName(const std::string& action) {
    if (action.empty() || action.size() > 128) return Error::invalidArgument("action must be a non-empty name");
    for (unsigned char character : action) {
        if (std::iscntrl(character) || std::isspace(character)) {
            return Error::invalidArgument("action may not contain whitespace or control characters");
        }
    }
    return Result<void>::ok();
}

Result<InputEventSpec> parseInputEventDescriptor(const json& descriptor) {
    if (!descriptor.is_object() || !descriptor.contains("type") || !descriptor["type"].is_string()) {
        return Error::invalidArgument("Each input event must be an object with a string type");
    }
    InputEventSpec spec;
    spec.type = descriptor["type"].get<std::string>();
    if (spec.type == "key") {
        const std::initializer_list<const char*> allowed = {
            "type", "keycode", "physical_keycode", "unicode", "shift_pressed", "alt_pressed",
            "ctrl_pressed", "meta_pressed", "shift", "alt", "ctrl", "meta", "device"};
        if (!hasOnlyKeys(descriptor, allowed)) {
            return Error::invalidArgument("Key event: " + unknownPropertyDetail(descriptor, allowed));
        }
        spec.class_name = "InputEventKey";
    } else if (spec.type == "mouse_button") {
        const std::initializer_list<const char*> allowed = {"type", "button_index", "device"};
        if (!hasOnlyKeys(descriptor, allowed)) {
            return Error::invalidArgument("Mouse-button event: " + unknownPropertyDetail(descriptor, allowed));
        }
        spec.class_name = "InputEventMouseButton";
    } else if (spec.type == "joypad_button") {
        const std::initializer_list<const char*> allowed = {"type", "button_index", "device"};
        if (!hasOnlyKeys(descriptor, allowed)) {
            return Error::invalidArgument("Joypad-button event: " + unknownPropertyDetail(descriptor, allowed));
        }
        spec.class_name = "InputEventJoypadButton";
    } else if (spec.type == "joypad_motion") {
        const std::initializer_list<const char*> allowed = {"type", "axis", "axis_value", "device"};
        if (!hasOnlyKeys(descriptor, allowed)) {
            return Error::invalidArgument("Joypad-motion event: " + unknownPropertyDetail(descriptor, allowed));
        }
        spec.class_name = "InputEventJoypadMotion";
    } else {
        return Error::invalidArgument("Unsupported input event type: " + spec.type +
                                      ". This tool accepts: key, mouse_button, joypad_button, joypad_motion.");
    }

    if (descriptor.contains("device") &&
        (!descriptor["device"].is_number_integer() || descriptor["device"].get<int64_t>() < -1)) {
        return Error::invalidArgument("Input event device must be an integer >= -1");
    }
    if (descriptor.contains("device")) spec.device = descriptor["device"].get<int64_t>();

    if (spec.type == "key") {
        for (const auto* key : {"keycode", "physical_keycode", "unicode"}) {
            if (!descriptor.contains(key)) continue;
            if (!descriptor[key].is_number_integer() || descriptor[key].get<int64_t>() <= 0) {
                return Error::invalidArgument(std::string(key) + " must be a positive integer");
            }
            const auto value = descriptor[key].get<int64_t>();
            if (std::string(key) == "keycode") spec.keycode = value;
            else if (std::string(key) == "physical_keycode") spec.physical_keycode = value;
            else spec.unicode = value;
        }
        if (!spec.keycode && !spec.physical_keycode && !spec.unicode) {
            return Error::invalidArgument("Key event requires keycode, physical_keycode, or unicode");
        }
        for (const auto* modifier : {"shift", "alt", "ctrl", "meta"}) {
            auto flag = modifierFlag(descriptor, modifier);
            if (flag.isErr()) return flag.error();
            if (!flag.value().has_value()) continue;
            const std::string name = modifier;
            if (name == "shift") spec.shift_pressed = flag.value();
            else if (name == "alt") spec.alt_pressed = flag.value();
            else if (name == "ctrl") spec.ctrl_pressed = flag.value();
            else spec.meta_pressed = flag.value();
        }
    } else if (spec.type == "mouse_button" || spec.type == "joypad_button") {
        if (!descriptor.contains("button_index") || !descriptor["button_index"].is_number_integer()) {
            return Error::invalidArgument("button_index is required and must be an integer");
        }
        const int64_t button = descriptor["button_index"].get<int64_t>();
        const int64_t maximum = spec.type == "mouse_button" ? 9 : 127;
        if (button < (spec.type == "mouse_button" ? 1 : 0) || button > maximum) {
            return Error::invalidArgument("button_index is outside the supported range");
        }
        spec.button_index = button;
    } else {
        if (!descriptor.contains("axis") || !descriptor["axis"].is_number_integer() ||
            descriptor["axis"].get<int64_t>() < 0 || descriptor["axis"].get<int64_t>() > 9) {
            return Error::invalidArgument("axis is required and must be in 0..9");
        }
        if (!descriptor.contains("axis_value") || !descriptor["axis_value"].is_number() ||
            !std::isfinite(descriptor["axis_value"].get<double>()) ||
            descriptor["axis_value"].get<double>() < -1.0 || descriptor["axis_value"].get<double>() > 1.0) {
            return Error::invalidArgument("axis_value is required and must be finite within -1.0..1.0");
        }
        spec.axis = descriptor["axis"].get<int64_t>();
        spec.axis_value = descriptor["axis_value"].get<double>();
    }
    return spec;
}

std::string inputActionLiteral(double deadzone, const std::vector<InputEventSpec>& events) {
    std::string text = "{\"deadzone\": " + numberLiteral(deadzone) + ", \"events\": [";
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (index) text += ", ";
        // The engine's own property order, so the line reads like one it wrote.
        text += "Object(" + event.class_name + ",\"device\":" + std::to_string(event.device);
        const auto flag = [&](const char* name, const std::optional<bool>& value) {
            if (value) text += std::string(",\"") + name + "\":" + (*value ? "true" : "false");
        };
        const auto integer = [&](const char* name, const std::optional<int64_t>& value) {
            if (value) text += std::string(",\"") + name + "\":" + std::to_string(*value);
        };
        flag("alt_pressed", event.alt_pressed);
        flag("shift_pressed", event.shift_pressed);
        flag("ctrl_pressed", event.ctrl_pressed);
        flag("meta_pressed", event.meta_pressed);
        integer("keycode", event.keycode);
        integer("physical_keycode", event.physical_keycode);
        integer("unicode", event.unicode);
        integer("button_index", event.button_index);
        integer("axis", event.axis);
        if (event.axis_value) text += ",\"axis_value\":" + numberLiteral(*event.axis_value);
        text += ")";
    }
    return text + "]}";
}

} // namespace didi::offline
