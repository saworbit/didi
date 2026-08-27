#include "didi/mcp/mcp_protocol.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/common/png.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace didi {
namespace mcp {

namespace {

bool isCaptureId(const json& value) {
    if (!value.is_string()) return false;
    const auto id = value.get<std::string>();
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

CallToolResult invalidViewportDiff(const std::string& message) {
    return CallToolResult::error("Invalid viewport diff request: " + message);
}

} // namespace

CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("vision.captureViewport", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            json result_data = res.value();
            std::string b64 = result_data.value("image_base64", "");
            if (!b64.empty()) {
                if (!result_data.contains("capture_id") || !isCaptureId(result_data["capture_id"])) {
                    return CallToolResult::error("Live viewport capture returned a missing or malformed capture_id.");
                }
                result_data.erase("image_base64");
                return CallToolResult::successImage(std::move(b64), result_data.dump(2));
            }
            return CallToolResult::successJson(result_data);
        }
        return CallToolResult::error("Failed to capture viewport via Godot GDExtension: " + res.error().message);
    }

    if (args.contains("node_isolation_path")) {
        if (!args["node_isolation_path"].is_string()) {
            return CallToolResult::error("Invalid viewport capture request: node_isolation_path must be a string.");
        }
        if (!args["node_isolation_path"].get<std::string>().empty()) {
            return CallToolResult::error("Viewport node isolation requires a live Godot editor.");
        }
    }

    int width = 256;
    int height = 192;
    if (args.contains("resolution") && args["resolution"].is_object()) {
        width = std::clamp(args["resolution"].value("width", width), 16, 1024);
        height = std::clamp(args["resolution"].value("height", height), 16, 1024);
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t offset = static_cast<size_t>(y * width + x) * 4;
            const bool grid = x % 32 == 0 || y % 32 == 0;
            pixels[offset] = grid ? 72 : 30;
            pixels[offset + 1] = grid ? 82 : 35;
            pixels[offset + 2] = grid ? 98 : 44;
            pixels[offset + 3] = 255;
        }
    }
    std::string encoded = png::encodeRgbaBase64(pixels.data(), width, height);
    if (encoded.empty()) return CallToolResult::error("Failed to encode offline viewport preview.");
    json metadata = {
        {"status", "offline_preview"},
        {"execution_mode", "offline_fallback"},
        {"is_live_frame", false},
        {"source", "synthesized_grid_preview"},
        {"camera_identifier", args.value("camera_identifier", "active_editor_view")},
        {"resolution", {{"width", width}, {"height", height}}},
        {"message", "Synthesized preview only; launch Godot Editor for a live viewport frame."}
    };
    return CallToolResult::successImage(std::move(encoded), metadata.dump());
}

CallToolResult handleViewportDiffCapture(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!args.is_object()) return invalidViewportDiff("arguments must be an object");
    if (!args.contains("baseline_capture_id") || !isCaptureId(args["baseline_capture_id"])) {
        return invalidViewportDiff("baseline_capture_id must be exactly 32 lowercase hexadecimal characters");
    }
    if (args.contains("threshold")) {
        const auto& threshold = args["threshold"];
        const bool valid = threshold.is_number_unsigned()
            ? threshold.get<uint64_t>() <= 255u
            : threshold.is_number_integer() && threshold.get<int64_t>() >= 0 &&
              threshold.get<int64_t>() <= 255;
        if (!valid) {
            return invalidViewportDiff("threshold must be an integer from 0 to 255");
        }
    }
    if (args.contains("camera_identifier") && !args["camera_identifier"].is_string()) {
        return invalidViewportDiff("camera_identifier must be a string");
    }
    if (args.contains("node_isolation_path") && !args["node_isolation_path"].is_string()) {
        return invalidViewportDiff("node_isolation_path must be a string");
    }
    if (args.contains("isolation_background")) {
        if (!args["isolation_background"].is_string()) {
            return invalidViewportDiff("isolation_background must be a string");
        }
        const auto background = args["isolation_background"].get<std::string>();
        if (background != "original" && background != "transparent") {
            return invalidViewportDiff("isolation_background must be original or transparent");
        }
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::error("Viewport diff capture requires a live Godot editor.");
    }
    auto res = ipc->sendRequest("vision.diffViewport", args, ::didi::ipc::kWaitForDefinitiveResponse);
    if (res.isErr()) {
        return CallToolResult::error("Failed to diff viewport via Godot GDExtension: " + res.error().message);
    }
    json result_data = res.value();
    const std::string b64 = result_data.value("image_base64", "");
    if (b64.empty()) {
        return CallToolResult::error("Live viewport diff returned no PNG image.");
    }
    if (!result_data.contains("comparison_capture_id") ||
        !isCaptureId(result_data["comparison_capture_id"])) {
        return CallToolResult::error("Live viewport diff returned a missing or malformed comparison_capture_id.");
    }
    result_data.erase("image_base64");
    return CallToolResult::successImage(b64, result_data.dump(2));
}

CallToolResult handleViewportSetCameraTransform(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("vision.setCameraTransform", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to set camera transform: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to position editor camera.");
}

CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    (void)ipc;
    std::string target_path = args.value("target_resource_path", "");
    std::string env = args.value("environment", "studio_neutral");
    bool ortho = args.value("orthographic", false);
    json rig = args.value("camera_rig", json::array({"front", "top", "isometric"}));

    // Offline generator: Create an isolated visual testbed scene (.tscn) on disk!
    std::string lab_scene_path = "res://addons/didi/test_lab_sandbox.tscn";
    std::string disk_path = "addons/didi/test_lab_sandbox.tscn";

    std::ofstream scene_file(disk_path);
    if (scene_file.is_open()) {
        scene_file << "[gd_scene format=3 uid=\"uid://didi_test_lab_sandbox\"]\n\n"
                   << "[node name=\"VisualTestLab\" type=\"Node3D\"]\n\n"
                   << "[node name=\"DirectionalLight3D\" type=\"DirectionalLight3D\" parent=\".\"]\n"
                   << "transform = Transform3D(0.866025, -0.25, 0.433013, 0, 0.866025, 0.5, -0.5, -0.433013, 0.75, 0, 5, 0)\n"
                   << "shadow_enabled = true\n\n"
                   << "[node name=\"WorldEnvironment\" type=\"WorldEnvironment\" parent=\".\"]\n\n"
                   << "[node name=\"GroundGrid\" type=\"CSGBox3D\" parent=\".\"]\n"
                   << "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.05, 0)\n"
                   << "size = Vector3(20, 0.1, 20)\n\n"
                   << "[node name=\"CameraFront\" type=\"Camera3D\" parent=\".\"]\n"
                   << "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1.5, 3)\n"
                   << (ortho ? "projection = 1\nsize = 4.0\n\n" : "\n")
                   << "[node name=\"CameraTop\" type=\"Camera3D\" parent=\".\"]\n"
                   << "transform = Transform3D(1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 4, 0)\n\n"
                   << "[node name=\"CameraIsometric\" type=\"Camera3D\" parent=\".\"]\n"
                   << "transform = Transform3D(0.707107, -0.353553, 0.612372, 0, 0.866025, 0.5, -0.707107, -0.353553, 0.612372, 3, 3, 3)\n\n";

        if (!target_path.empty()) {
            scene_file << "[node name=\"TargetInstance\" type=\"Node3D\" parent=\".\"]\n";
        }
        scene_file.close();

        json res = {
            {"status", "created_offline"},
            {"scene_path", lab_scene_path},
            {"target_resource_path", target_path},
            {"environment", env},
            {"camera_rig", rig},
            {"message", "Created sandbox scene at " + lab_scene_path + ". Open Godot Editor to view live or run `execute_test_session`."}
        };
        return CallToolResult::successJson(res);
    }

    return CallToolResult::error("Failed to generate visual test lab sandbox scene file.");
}

CallToolResult handleViewportToggleDebugDraw(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("vision.toggleDebugDraw", args);
        if (res.isOk()) {
            return CallToolResult::successJson(res.value());
        }
        return CallToolResult::error("Failed to toggle debug draw: " + res.error().message);
    }
    return CallToolResult::error("Godot Editor is offline. Launch Godot Editor to toggle debug draw modes.");
}

} // namespace mcp
} // namespace didi
