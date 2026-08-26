#pragma once

#include "didi/common/types.hpp"
#include "didi/common/json.hpp"
#include <string>
#include <vector>

namespace didi {
namespace godot {

class ViewportRenderer {
public:
    static ViewportRenderer& instance();

    json captureViewport(const json& params);

    std::string encodeImageToPngBase64(const uint8_t* rgba_data, int width, int height);

private:
    ViewportRenderer() = default;
};

} // namespace godot
} // namespace didi
