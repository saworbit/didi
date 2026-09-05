#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/runtime/segmentation.hpp"
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

Result<CapturedFrame> captureSelectedFrame(const json& params, const std::string& session_kind) {
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
    const bool game_session = session_kind == "game";
    const std::string camera = params.value("camera_identifier", "active_editor_view");
    const std::string node_path = params.value("node_isolation_path", "");
    const std::string background = params.value("isolation_background", "original");
    // A game has one root viewport. Accepting an editor identifier there would
    // answer a question about the editor with a picture of the game.
    if (game_session && params.contains("camera_identifier")) {
        return Error::invalidArgument(
            "camera_identifier selects an editor viewport and a game session has only its root "
            "viewport; omit it");
    }
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

    auto capture = game_session ? bridge.captureGameViewport() : bridge.captureEditorViewport(camera);
    if (capture.isErr()) return capture.error();
    if (guard) {
        auto restored = guard->restoreNow();
        if (restored.isErr()) {
            // One more attempt before giving up, because the alternative is
            // leaving the user's scene with nodes hidden. Whatever happens, say
            // which it was: a caller that gets a bare error will reasonably
            // assume the editor looks the way it did before the call.
            auto retried = guard->restoreNow();
            auto failure = retried.isErr() ? retried.error() : restored.error();
            failure.data = {{"isolated", true},
                            {"node_isolation_path", isolation->canonical_node_path},
                            {"temporarily_hidden_count", isolation->visibility.size()},
                            {"state_restored", retried.isOk()}};
            if (retried.isErr()) {
                DIDI_LOG_ERROR("VIEWPORT_RENDERER",
                               "Viewport isolation could not be restored; nodes remain hidden: ",
                               failure.message);
            }
            return failure;
        }
    }

    CapturedFrame result;
    result.pixels = {capture.value().width, capture.value().height, capture.value().rgba};
    result.metadata = {
        {"resolution", {{"width", capture.value().width}, {"height", capture.value().height}}},
        {"source", game_session ? "godot_game_viewport_texture" : "godot_editor_viewport_texture"},
        {"execution_mode", "live"},
        {"session_kind", session_kind},
        {"is_live_frame", true}
    };
    if (game_session) {
        result.metadata["camera_identifier"] = "root_viewport";
    } else {
        result.metadata["camera_identifier"] = camera;
    }
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

namespace {

// The passes a caller may ask for, in the order they are named here when the
// request does not fix one.
const char* const kPassKinds[] = {"color", "depth", "normal", "segmentation"};

json colorJson(const runtime::SegmentationColor& colour) {
    return {{"r", colour.r}, {"g", colour.g}, {"b", colour.b}};
}

Result<std::vector<std::string>> parseRequestedPasses(const json& params) {
    if (!params.contains("passes") || !params["passes"].is_array()) {
        return Error::invalidArgument("passes must be an array");
    }
    const auto& requested = params["passes"];
    if (requested.empty() || requested.size() > 4) {
        return Error::invalidArgument("passes must contain 1 to 4 entries");
    }
    std::vector<std::string> kinds;
    for (const auto& entry : requested) {
        if (!entry.is_string()) return Error::invalidArgument("passes entries must be strings");
        const auto kind = entry.get<std::string>();
        if (std::find(std::begin(kPassKinds), std::end(kPassKinds), kind) == std::end(kPassKinds)) {
            return Error::invalidArgument("passes entries must be color, depth, normal, or segmentation");
        }
        // A pass asked for twice would be drawn twice and returned twice, which
        // is a request nobody means to make.
        if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) {
            return Error::invalidArgument("passes names " + kind + " more than once");
        }
        kinds.push_back(kind);
    }
    return kinds;
}

} // namespace

json ViewportRenderer::capturePasses(const json& params, const std::string& session_kind) {
    try {
        if (!params.is_object()) {
            return rendererError(Error::invalidArgument("Viewport pass params must be an object"));
        }
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it.key() != "passes" && it.key() != "camera_identifier" &&
                it.key() != "depth_far") {
                return rendererError(Error::invalidArgument("Viewport pass request contains an unknown property"));
            }
        }
        auto kinds = parseRequestedPasses(params);
        if (kinds.isErr()) return rendererError(kinds.error());

        const bool game_session = session_kind == "game";
        if (game_session && params.contains("camera_identifier")) {
            return rendererError(Error::invalidArgument(
                "camera_identifier selects an editor viewport and a game session has only its root "
                "viewport; omit it"));
        }
        if (params.contains("camera_identifier") && !params["camera_identifier"].is_string()) {
            return rendererError(Error::invalidArgument("camera_identifier must be a string"));
        }
        const std::string camera = params.value("camera_identifier", "active_editor_view");

        double depth_far = 0.0;
        if (params.contains("depth_far")) {
            if (!params["depth_far"].is_number()) {
                return rendererError(Error::invalidArgument("depth_far must be a number"));
            }
            depth_far = params["depth_far"].get<double>();
            if (!std::isfinite(depth_far) || depth_far <= 0.0 || depth_far > 1000000.0) {
                return rendererError(Error::invalidArgument("depth_far must be greater than 0 and at most 1000000"));
            }
        }
        auto captured = GodotBridge::instance().captureViewportPasses(kinds.value(), camera,
                                                                     session_kind, depth_far);
        if (captured.isErr()) return rendererError(captured.error());
        auto& capture = captured.value();

        json passes = json::array();
        json legend = json::array();
        uint64_t unclaimed = 0;
        int width = 0;
        int height = 0;
        for (auto& frame : capture.frames) {
            // Read the segmentation frame back before it is encoded. The legend
            // reports the colour that is in the picture rather than the one the
            // shader was given, because the viewport post-processes after the
            // shader writes and by how much depends on the engine.
            if (frame.kind == "segmentation") {
                const auto scan = runtime::readSegmentation(
                    frame.pixels.rgba.data(), frame.pixels.width, frame.pixels.height,
                    capture.segmented.size());
                unclaimed = scan.unclaimed_pixels;
                const auto& palette = runtime::segmentationPalette();
                for (const auto& node : capture.segmented) {
                    const auto& region = scan.regions[node.entry];
                    json entry = {
                        {"id", static_cast<int>(node.entry)},
                        {"node_path", node.path},
                        {"class", node.class_name},
                        {"color", colorJson(palette[node.entry])},
                        {"pixels", region.pixels}
                    };
                    if (region.pixels > 0) {
                        entry["observed_color"] = colorJson(region.observed);
                        entry["bounds"] = {{"x", region.min_x}, {"y", region.min_y},
                                           {"width", region.max_x - region.min_x + 1},
                                           {"height", region.max_y - region.min_y + 1}};
                    } else {
                        // Painted and not visible: behind something, outside the
                        // frame, or drawing nothing. Reported rather than left
                        // out, because a node missing from a legend reads as a
                        // node that was never painted.
                        entry["observed_color"] = nullptr;
                        entry["bounds"] = nullptr;
                    }
                    legend.push_back(std::move(entry));
                }
            }
            auto encoded = encodeImageToPngBase64(frame.pixels.rgba.data(), frame.pixels.width,
                                                  frame.pixels.height);
            if (encoded.empty()) {
                return rendererError(Error::internal("Failed to encode a captured pass"));
            }
            width = frame.pixels.width;
            height = frame.pixels.height;
            passes.push_back({{"kind", frame.kind}, {"image_base64", std::move(encoded)}});
        }

        json result = {
            {"execution_mode", "live"},
            {"session_kind", session_kind},
            {"camera_identifier", game_session ? "root_viewport" : camera},
            {"format", "image/png"},
            // The pass shaders undo the sRGB curve the framebuffer applies, but
            // the viewport post-processes after that and by how much depends on
            // the engine. A pass is an ordering to read and compare, not a
            // calibrated measurement.
            {"encoding", "srgb8_relative"},
            {"resolution", {{"width", width}, {"height", height}}},
            {"passes", std::move(passes)},
            {"painted_node_count", capture.painted},
            {"examined_node_count", capture.examined},
            {"scan_limit_reached", capture.scan_limit_reached}
        };
        if (std::find(kinds.value().begin(), kinds.value().end(), "segmentation") !=
            kinds.value().end()) {
            result["segmentation"] = std::move(legend);
            result["segmentation_unclaimed_pixels"] = unclaimed;
            result["segmentation_unpainted"] = capture.unsegmented;
            result["segmentation_capacity"] = static_cast<int>(runtime::segmentationPalette().size());
        }
        const bool wants_depth = std::find(kinds.value().begin(), kinds.value().end(), "depth") !=
                                 kinds.value().end();
        if (wants_depth) {
            result["depth_far"] = capture.depth_far;
            result["depth_encoding"] = "grey rises with distance in front of the camera, with depth_far mapped to white; read it as an ordering rather than as calibrated distances";
        }
        return result;
    } catch (const std::exception& error) {
        return rendererError(Error::internal(std::string("Viewport pass capture failed: ") + error.what()));
    }
}

json ViewportRenderer::captureViewport(const json& params, const std::string& session_kind) {
    try {
        auto frame = captureSelectedFrame(params, session_kind);
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
        result["description"] = std::string("Live Godot ") +
                                (session_kind == "game" ? "game" : "editor") +
                                " viewport frame from '" +
                                result["camera_identifier"].get<std::string>() + "' (" +
                                std::to_string(pixels.width) + "x" + std::to_string(pixels.height) + ")";
        return result;
    } catch (const std::exception& exception) {
        return rendererError(Error::internal(std::string("Viewport capture failed: ") + exception.what()));
    } catch (...) {
        return rendererError(Error::internal("Viewport capture failed with an unknown exception"));
    }
}

json ViewportRenderer::diffViewport(const json& params, const std::string& session_kind) {
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
        auto frame = captureSelectedFrame(params, session_kind);
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
