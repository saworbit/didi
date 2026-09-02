#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace runtime {

// One event from a runtime.injectInput batch, validated and with contract
// defaults applied, so the engine side only constructs objects and never
// interprets JSON.
struct InjectedInputEvent {
    enum class Kind { action, key, mouse_button, joypad_button, joypad_motion };

    Kind kind{Kind::action};
    bool pressed{false};
    // -1 means "no specific device" for action, key and mouse events.
    // Joypad events require a device in 0..31.
    int64_t device{-1};

    // action
    std::string action_name;
    double strength{1.0};

    // key. Zero means not set; the contract requires at least one identity.
    int64_t keycode{0};
    int64_t physical_keycode{0};
    int64_t unicode{0};
    bool echo{false};
    bool shift_pressed{false};
    bool alt_pressed{false};
    bool ctrl_pressed{false};
    bool meta_pressed{false};

    // mouse_button and joypad_button
    int64_t button_index{0};
    bool double_click{false};
    double factor{1.0};
    double pressure{1.0};

    // joypad_motion
    int64_t axis{0};
    double axis_value{0.0};

    const char* kindName() const;
};

constexpr size_t kMaxInjectedEvents = 32;
constexpr size_t kMaxInjectedRequestBytes = 32u * 1024u;

// Validates a runtime.injectInput request against the approved contract.
// Errors carry 400 for a malformed request and 413 when the serialized
// request exceeds the byte cap.
Result<std::vector<InjectedInputEvent>> parseInputInjectionRequest(const json& params);

} // namespace runtime
} // namespace didi
