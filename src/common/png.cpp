#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "didi/common/stb_image_write.h"
#include "didi/common/png.hpp"
#include "didi/common/base64.hpp"
#include <vector>

namespace didi {
namespace png {
namespace {

void writeCallback(void* context, void* data, int size) {
    auto* bytes = static_cast<std::vector<uint8_t>*>(context);
    const auto* first = static_cast<const uint8_t*>(data);
    bytes->insert(bytes->end(), first, first + size);
}

} // namespace

std::string encodeRgbaBase64(const uint8_t* rgba_data, int width, int height) {
    if (!rgba_data || width <= 0 || height <= 0) return {};
    std::vector<uint8_t> encoded;
    const int ok = stbi_write_png_to_func(writeCallback, &encoded, width, height, 4,
                                          rgba_data, width * 4);
    return ok && !encoded.empty() ? base64::encode(encoded) : std::string{};
}

} // namespace png
} // namespace didi
