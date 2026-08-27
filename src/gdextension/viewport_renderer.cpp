#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/common/base64.hpp"
#include "didi/common/png.hpp"
#include "didi/common/logger.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace didi {
namespace godot {

namespace {

std::string makeCaptureId() {
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device entropy;
    std::string result(32, '0');
    for (size_t i = 0; i < 16; ++i) {
        const auto byte = static_cast<uint8_t>(entropy());
        result[i * 2] = digits[byte >> 4u];
        result[i * 2 + 1] = digits[byte & 0x0Fu];
    }
    return result;
}

} // namespace

ViewportRenderer& ViewportRenderer::instance() {
    static ViewportRenderer s_instance;
    return s_instance;
}

std::string ViewportRenderer::encodeImageToPngBase64(const uint8_t* rgba_data, int width, int height) {
    std::string encoded = png::encodeRgbaBase64(rgba_data, width, height);
    if (encoded.empty()) {
        DIDI_LOG_ERROR("VIEWPORT_RENDERER", "Failed to encode PNG buffer via STB");
    }
    return encoded;
}

json ViewportRenderer::captureViewport(const json& params) {
    std::string cam_id = params.value("camera_identifier", "active_editor_view");
    auto capture = GodotBridge::instance().captureEditorViewport(cam_id);
    if (capture.isErr()) {
        return {{"error", {{"code", capture.error().code}, {"message", capture.error().message}}}};
    }
    const auto& pixels = capture.value();
    std::string b64_png = encodeImageToPngBase64(pixels.rgba.data(), pixels.width, pixels.height);
    if (b64_png.empty()) {
        return {{"error", {{"code", 500}, {"message", "Failed to encode captured viewport pixels"}}}};
    }
    const auto capture_id = makeCaptureId();
    auto cached = m_captureCache.store(capture_id,
                                       image::RgbaImage{pixels.width, pixels.height, pixels.rgba});
    if (cached.isErr()) {
        return {{"error", {{"code", cached.error().code}, {"message", cached.error().message}}}};
    }

    return {
        {"capture_id", capture_id},
        {"camera_identifier", cam_id},
        {"resolution", {{"width", pixels.width}, {"height", pixels.height}}},
        {"format", "image/png"},
        {"source", "godot_editor_viewport_texture"},
        {"execution_mode", "live"},
        {"is_live_frame", true},
        {"image_base64", b64_png},
        {"description", "Live Godot editor viewport frame from '" + cam_id + "' (" +
                        std::to_string(pixels.width) + "x" + std::to_string(pixels.height) + ")"}
    };
}

} // namespace godot
} // namespace didi
