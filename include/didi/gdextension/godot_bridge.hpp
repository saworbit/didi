#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace didi {
namespace godot {

struct ViewportPixels {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgba;
};

class GodotBridge {
public:
    static GodotBridge& instance();

    json execute(const std::string& method, const json& params);
    Result<ViewportPixels> captureEditorViewport(const std::string& camera_identifier);

private:
    GodotBridge() = default;
};

} // namespace godot
} // namespace didi
