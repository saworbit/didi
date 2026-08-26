#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/common/base64.hpp"
#include "didi/common/png.hpp"
#include "didi/common/logger.hpp"
#include <cmath>

namespace didi {
namespace godot {

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

    return {
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
