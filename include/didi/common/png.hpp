#pragma once

#include <cstdint>
#include <string>

namespace didi {
namespace png {

std::string encodeRgbaBase64(const uint8_t* rgba_data, int width, int height);

} // namespace png
} // namespace didi
