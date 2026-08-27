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

    json execute(const std::string& method, const json& params,
                 const std::string& session_kind = "editor");
    Result<ViewportPixels> captureEditorViewport(const std::string& camera_identifier);

private:
    GodotBridge() = default;
};

Result<std::string> resolveGodotProjectPath();

} // namespace godot
} // namespace didi
