#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/common/base64.hpp"
#include "didi/common/png.hpp"
#include "didi/common/logger.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
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

Result<std::string> makeUniqueCaptureId(CaptureCache& cache) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = makeCaptureId();
        if (!cache.contains(candidate)) return candidate;
    }
    return Error::internal("Unable to allocate a unique live capture ID");
}

json rendererError(const Error& error) {
    return {{"error", {{"code", error.code}, {"message", error.message}}}};
}

struct CapturedFrame {
    image::RgbaImage pixels;
    json metadata;
};

Result<CapturedFrame> captureSelectedFrame(const json& params) {
    if (!params.is_object()) return Error::invalidArgument("Viewport capture params must be an object");
    if (params.contains("camera_identifier") && !params["camera_identifier"].is_string()) {
        return Error::invalidArgument("camera_identifier must be a string");
    }
    if (params.contains("node_isolation_path") && !params["node_isolation_path"].is_string()) {
        return Error::invalidArgument("node_isolation_path must be a string");
    }
    if (params.contains("isolation_background") && !params["isolation_background"].is_string()) {
        return Error::invalidArgument("isolation_background must be a string");
    }
    const std::string camera = params.value("camera_identifier", "active_editor_view");
    const std::string node_path = params.value("node_isolation_path", "");
    const std::string background = params.value("isolation_background", "original");
    if (background != "original" && background != "transparent") {
        return Error::invalidArgument("isolation_background must be original or transparent");
    }
    if (node_path.empty() && background != "original") {
        return Error::invalidArgument("transparent isolation_background requires node_isolation_path");
    }

    auto& bridge = GodotBridge::instance();
    std::optional<ViewportIsolationState> isolation;
    std::unique_ptr<RestorationGuard> guard;
    if (!node_path.empty()) {
        auto begun = bridge.beginViewportIsolation(node_path, camera, background);
        if (begun.isErr()) return begun.error();
        isolation = std::move(begun.value());
        guard = std::make_unique<RestorationGuard>([&]() {
            auto restored = bridge.restoreViewportIsolation(*isolation);
            auto redrawn = bridge.forceDraw();
            if (restored.isErr()) {
                DIDI_LOG_ERROR("VIEWPORT_RENDERER", restored.error().message);
                return restored;
            }
            return redrawn;
        });
        auto drawn = bridge.forceDraw();
        if (drawn.isErr()) return drawn.error();
    }

    auto capture = bridge.captureEditorViewport(camera);
    if (capture.isErr()) return capture.error();
    if (guard) {
        auto restored = guard->restoreNow();
        if (restored.isErr()) return restored.error();
    }

    CapturedFrame result;
    result.pixels = {capture.value().width, capture.value().height, capture.value().rgba};
    result.metadata = {
        {"camera_identifier", camera},
        {"resolution", {{"width", capture.value().width}, {"height", capture.value().height}}},
        {"source", "godot_editor_viewport_texture"},
        {"execution_mode", "live"},
        {"is_live_frame", true}
    };
    if (isolation) {
        result.metadata["isolated"] = true;
        result.metadata["node_isolation_path"] = isolation->canonical_node_path;
        result.metadata["isolation_background"] = isolation->isolation_background;
        result.metadata["temporarily_hidden_count"] = isolation->visibility.size();
        result.metadata["state_restored"] = true;
    } else {
        result.metadata["isolated"] = false;
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
    try {
        auto frame = captureSelectedFrame(params);
        if (frame.isErr()) return rendererError(frame.error());
        auto& pixels = frame.value().pixels;
        std::string b64_png = encodeImageToPngBase64(pixels.rgba.data(), pixels.width, pixels.height);
        if (b64_png.empty()) return rendererError(Error::internal("Failed to encode captured viewport pixels"));
        auto capture_id = makeUniqueCaptureId(m_captureCache);
        if (capture_id.isErr()) return rendererError(capture_id.error());
        auto cached = m_captureCache.store(capture_id.value(), pixels);
        if (cached.isErr()) return rendererError(cached.error());

        json result = std::move(frame.value().metadata);
        result["capture_id"] = capture_id.value();
        result["format"] = "image/png";
        result["image_base64"] = std::move(b64_png);
        result["description"] = "Live Godot editor viewport frame from '" +
                                result["camera_identifier"].get<std::string>() + "' (" +
                                std::to_string(pixels.width) + "x" + std::to_string(pixels.height) + ")";
        return result;
    } catch (const std::exception& exception) {
        return rendererError(Error::internal(std::string("Viewport capture failed: ") + exception.what()));
    } catch (...) {
        return rendererError(Error::internal("Viewport capture failed with an unknown exception"));
    }
}

json ViewportRenderer::diffViewport(const json& params) {
    try {
        if (!params.is_object() || !params.contains("baseline_capture_id") ||
            !params["baseline_capture_id"].is_string()) {
            return rendererError(Error::invalidArgument("baseline_capture_id is required"));
        }
        const auto baseline_id = params["baseline_capture_id"].get<std::string>();
        if (baseline_id.size() != 32 || !std::all_of(baseline_id.begin(), baseline_id.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            return rendererError(Error::invalidArgument(
                "baseline_capture_id must contain exactly 32 lowercase hexadecimal characters"));
        }
        int threshold = 0;
        if (params.contains("threshold")) {
            const auto& value = params["threshold"];
            const bool valid = value.is_number_unsigned()
                ? value.get<uint64_t>() <= 255u
                : value.is_number_integer() && value.get<int64_t>() >= 0 &&
                  value.get<int64_t>() <= 255;
            if (!valid) {
                return rendererError(Error::invalidArgument("threshold must be an integer from 0 to 255"));
            }
            threshold = value.get<int>();
        }
        auto baseline = m_captureCache.find(baseline_id);
        if (!baseline) return rendererError(Error::notFound("Baseline capture ID is missing or has been evicted"));
        auto frame = captureSelectedFrame(params);
        if (frame.isErr()) return rendererError(frame.error());
        auto diff = image::diffRgba(*baseline, frame.value().pixels, static_cast<uint8_t>(threshold));
        if (diff.isErr()) return rendererError(diff.error());
        auto b64_png = encodeImageToPngBase64(diff.value().diff_rgba.data(),
                                              diff.value().width, diff.value().height);
        if (b64_png.empty()) return rendererError(Error::internal("Failed to encode viewport diff pixels"));
        auto comparison_id = makeUniqueCaptureId(m_captureCache);
        if (comparison_id.isErr()) return rendererError(comparison_id.error());
        auto cached = m_captureCache.store(comparison_id.value(), std::move(frame.value().pixels));
        if (cached.isErr()) return rendererError(cached.error());

        json result = diff.value().toJson();
        result.update(frame.value().metadata);
        result["baseline_capture_id"] = baseline_id;
        result["comparison_capture_id"] = comparison_id.value();
        result["threshold"] = threshold;
        result["format"] = "image/png";
        result["image_base64"] = std::move(b64_png);
        result["description"] = "Exact RGBA viewport diff against live capture " + baseline_id;
        return result;
    } catch (const std::exception& exception) {
        return rendererError(Error::internal(std::string("Viewport diff failed: ") + exception.what()));
    } catch (...) {
        return rendererError(Error::internal("Viewport diff failed with an unknown exception"));
    }
}

} // namespace godot
} // namespace didi
