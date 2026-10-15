#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace didi::offline {

// One input event as project_set_input_action takes it, checked once for both of
// its routes: the bridge builds the engine's InputEvent from it, and with no
// editor attached it is written into project.godot (#1379).
struct InputEventSpec {
    std::string type;        // key, mouse_button, joypad_button or joypad_motion
    std::string class_name;  // the InputEvent class Godot builds for it
    // An event with no device is bound to all of them, -1, as the editor's
    // Input Map dialog binds one. The engine's own default differs by line and
    // by class, 0 on 4.5.1 and 16 for a key on 4.7.2 (#1295).
    int64_t device{-1};
    std::optional<int64_t> keycode;
    std::optional<int64_t> physical_keycode;
    std::optional<int64_t> unicode;
    std::optional<bool> shift_pressed;
    std::optional<bool> alt_pressed;
    std::optional<bool> ctrl_pressed;
    std::optional<bool> meta_pressed;
    std::optional<int64_t> button_index;
    std::optional<int64_t> axis;
    std::optional<double> axis_value;
};

// 400 naming what is wrong.
Result<InputEventSpec> parseInputEventDescriptor(const json& descriptor);

// 1 to 128 bytes, with no whitespace or control character.
Result<void> validateInputActionName(const std::string& action);

// What project.godot holds for an action, on one line, which the offline
// settings writer requires and Godot's parser takes as it takes its own
// layout. Each event names its class, its device and the properties the call
// set, and Godot fills every other property with its default when it loads the
// file, which is what the live route's newly built event holds as well. Once an
// engine saves the project, the action's text is byte for byte what the live
// route writes, on 4.5.1, 4.6.2 and 4.7.2.
std::string inputActionLiteral(double deadzone, const std::vector<InputEventSpec>& events);

} // namespace didi::offline
