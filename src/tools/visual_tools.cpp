#include "didi/mcp/mcp_protocol.hpp"
#include "didi/tools/visual_test_lab_path.hpp"
#include "didi/tools/editor_copy_refresh.hpp"
#include "didi/common/atomic_write.hpp"
#include "didi/common/project_path.hpp"
#include "didi/tools/phase7_live_forward.hpp"
#include "didi/common/ipc_channel.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/resource_indexer.hpp"
#include "didi/offline/test_runner.hpp"
#include "didi/common/png.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <sstream>
#include <string_view>
#include <vector>
#include "didi/mcp/tool_registration.hpp"

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

// A sentence naming what was wrong and what to send instead, which is the
// standard the rest of this server holds. These two answered with a raw C++
// identifier -- invalid_viewport_toggle_debug_draw_request -- which is neither
// prose a person can act on nor a code a client can branch on, and carried no
// "code": "invalid_arguments" the way the structured argument errors do (#424).
CallToolResult viewportRequestError(const ResolvedToolBinding& binding,
                                    std::string_view message) {
    return CallToolResult::errorJson(400, std::string(message),
                                     {{"tool", binding.invoked_name},
                                      {"canonical_tool", binding.canonical_name},
                                      {"code", "invalid_arguments"},
                                      {"retryable", false}});
}

bool hasOnlyViewportKeys(const json& value,
                         std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool isBoundedViewportString(const json& value, size_t minimum, size_t maximum) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() < minimum || text.size() > maximum) return false;
    try {
        (void)json(text).dump();
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool isFiniteVector3(const json& value, double limit) {
    if (!value.is_object() || value.size() != 3) return false;
    for (const auto* axis : {"x", "y", "z"}) {
        if (!value.contains(axis) || !value[axis].is_number()) return false;
        const double component = value[axis].get<double>();
        if (!std::isfinite(component) || component < -limit || component > limit) return false;
    }
    return true;
}

} // namespace

CallToolResult handleEditorRenderGhostPreview(const ResolvedToolBinding& binding, const json& args,
                                             std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleEditorClearGhostPreviews(const ResolvedToolBinding& binding, const json& args,
                                              std::shared_ptr<ipc::IIpcClient> ipc) {
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleViewportCapturePasses(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!ipc || !ipc->isConnected()) {
        // The same 503 not_connected every other live-only tool answers
        // offline with, so a client's reattach-and-retry rule reaches this
        // tool too (#548).
        return CallToolResult::errorJson(
            503,
            "Rendering the scene again with replacement materials needs a live Godot session; there "
            "is no offline frame to draw passes from.",
            {{"retryable", true}});
    }
    auto res = ipc->sendRequest("vision.capturePasses", args, ::didi::ipc::kWaitForDefinitiveResponse);
    if (res.isErr()) {
        return CallToolResult::fromError(res.error(), "Failed to capture viewport passes via Godot GDExtension: ");
    }
    json data = res.value();
    if (!data.is_object() || !data.contains("passes") || !data["passes"].is_array() ||
        data["passes"].empty()) {
        return CallToolResult::errorJson(502, "Live pass capture returned a malformed response.");
    }

    // One image block per pass, in the order they were asked for, rather than
    // one stacked picture. A stacked image would need labels to be read, and
    // there is no font here to draw them with; separate blocks keep each pass
    // its own picture and let the text below name them in order.
    CallToolResult result;
    std::vector<std::string> order;
    for (auto& pass : data["passes"]) {
        if (!pass.is_object() || !pass.contains("kind") || !pass["kind"].is_string() ||
            !pass.contains("image_base64") || !pass["image_base64"].is_string()) {
            return CallToolResult::errorJson(502, "Live pass capture returned a malformed pass entry.");
        }
        auto image = pass["image_base64"].get<std::string>();
        if (image.empty()) {
            return CallToolResult::errorJson(502, "Live pass capture returned a pass with no PNG image.");
        }
        order.push_back(pass["kind"].get<std::string>());
        result.content.push_back(ContentItem::makeImagePng(std::move(image)));
    }
    data.erase("passes");
    data["pass_order"] = order;
    // Whether every node was drawn: the walk has a node limit and the
    // segmentation palette a colour limit, and each had only its own field (Q5).
    const auto unpainted = data.find("segmentation_unpainted");
    data["truncated"] = data.value("scan_limit_reached", false) ||
                        (unpainted != data.end() && unpainted->is_array() && !unpainted->empty());
    result.content.push_back(ContentItem::makeText(data.dump()));
    result.structuredContent = data;
    result.isError = false;
    return result;
}

CallToolResult handleCaptureViewport(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    if (ipc && ipc->isConnected()) {
        auto res = ipc->sendRequest("vision.captureViewport", args, ::didi::ipc::kWaitForDefinitiveResponse);
        if (res.isOk()) {
            json result_data = res.value();
            if (!result_data.is_object()) {
                return CallToolResult::errorJson(502, "Live viewport capture returned a malformed response.");
            }
            if (!result_data.contains("image_base64") || !result_data["image_base64"].is_string()) {
                return CallToolResult::errorJson(502, "Live viewport capture returned a missing or malformed PNG image.");
            }
            std::string b64 = result_data["image_base64"].get<std::string>();
            if (b64.empty()) return CallToolResult::errorJson(502, "Live viewport capture returned no PNG image.");
            if (!result_data.contains("capture_id") || !isCaptureId(result_data["capture_id"])) {
                return CallToolResult::errorJson(502, "Live viewport capture returned a missing or malformed capture_id.");
            }
            result_data.erase("image_base64");
            return CallToolResult::successImage(std::move(b64), result_data.dump());
        }
        return CallToolResult::fromError(res.error(), "Failed to capture viewport via Godot GDExtension: ");
    }

    if (args.contains("node_isolation_path")) {
        if (!args["node_isolation_path"].is_string()) {
            return CallToolResult::errorJson(400, "Invalid viewport capture request: node_isolation_path must be a string.");
        }
        if (!args["node_isolation_path"].get<std::string>().empty()) {
            return CallToolResult::notConnected("Viewport node isolation requires a live Godot editor.");
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
    if (encoded.empty()) return CallToolResult::errorJson(500, "Failed to encode offline viewport preview.");
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

CallToolResult handleViewportDiffCapture(const ResolvedToolBinding& binding, const json& args,
                                         std::shared_ptr<ipc::IIpcClient> ipc) {
    // The published schema types and bounds every argument this tool takes, and
    // dispatchTool checks it before any handler runs, so the copies that used to
    // stand here for threshold, camera_identifier, node_isolation_path,
    // isolation_background, min_ssim and max_hamming_distance could not be
    // reached (#628). What survives is the one rule the schema states and the
    // checker does not model: the capture id's `pattern`. It answers with the
    // envelope now rather than the bare string the removed checks used, which is
    // the same correction #424 made to the two tools either side of this one.
    //
    // Perceptual tolerances. A per-pixel threshold cannot tell shadow filtering
    // or antialiasing jitter from a real regression; min_ssim and
    // max_hamming_distance let a caller say what "still looks the same" means
    // for its own pipeline, and the schema bounds both.
    if (!isCaptureId(args.value("baseline_capture_id", json()))) {
        return viewportRequestError(
            binding,
            "baseline_capture_id must be exactly 32 lowercase hexadecimal characters.");
    }
    if (!ipc || !ipc->isConnected()) {
        return CallToolResult::notConnected("Viewport diff capture requires a live Godot editor.");
    }
    auto res = ipc->sendRequest("vision.diffViewport", args, ::didi::ipc::kWaitForDefinitiveResponse);
    if (res.isErr()) {
        return CallToolResult::fromError(res.error(), "Failed to diff viewport via Godot GDExtension: ");
    }
    json result_data = res.value();
    if (!result_data.is_object()) {
        return CallToolResult::errorJson(502, "Live viewport diff returned a malformed response.");
    }
    if (!result_data.contains("image_base64") || !result_data["image_base64"].is_string()) {
        return CallToolResult::errorJson(502, "Live viewport diff returned a missing or malformed PNG image.");
    }
    const std::string b64 = result_data["image_base64"].get<std::string>();
    if (b64.empty()) {
        return CallToolResult::errorJson(502, "Live viewport diff returned no PNG image.");
    }
    if (!result_data.contains("comparison_capture_id") ||
        !isCaptureId(result_data["comparison_capture_id"])) {
        return CallToolResult::errorJson(502, "Live viewport diff returned a missing or malformed comparison_capture_id.");
    }
    // Applied here rather than in the engine: the metrics are a property of the
    // two frames, the tolerance is a property of the caller's pipeline.
    if (args.contains("min_ssim") || args.contains("max_hamming_distance")) {
        bool within = true;
        json applied = json::object();
        if (args.contains("min_ssim")) {
            const double minimum = args["min_ssim"].get<double>();
            applied["min_ssim"] = minimum;
            within = within && result_data.value("ssim", 0.0) >= minimum;
        }
        if (args.contains("max_hamming_distance")) {
            const int64_t maximum = args["max_hamming_distance"].get<int64_t>();
            applied["max_hamming_distance"] = maximum;
            const auto& hashes = result_data["perceptual_hash"];
            within = within && hashes.is_object() &&
                     hashes.value("hamming_distance", 65) <= maximum;
        }
        result_data["perceptual_tolerance"] = std::move(applied);
        result_data["perceptually_identical"] = within;
    }

    result_data.erase("image_base64");
    return CallToolResult::successImage(b64, result_data.dump());
}

CallToolResult handleViewportSetCameraTransform(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!hasOnlyViewportKeys(args, {"camera_path", "position", "rotation_degrees", "fov"})) {
        return viewportRequestError(
            binding, "This tool accepts: camera_path, fov, position, rotation_degrees.");
    }
    if (!args.contains("camera_path") || !isBoundedViewportString(args["camera_path"], 1, 1024)) {
        return viewportRequestError(
            binding, "camera_path is required and must be a node path of 1 to 1024 characters.");
    }
    if (!args.contains("position") || !isFiniteVector3(args["position"], 1000000.0)) {
        return viewportRequestError(
            binding, "position is required and must be an object with finite x, y and z numbers "
                     "no larger than 1000000, for example {\"x\": 0, \"y\": 2, \"z\": 5}.");
    }
    if (args.contains("rotation_degrees") &&
        !isFiniteVector3(args["rotation_degrees"], 360000.0)) {
        return viewportRequestError(
            binding, "rotation_degrees must be an object with finite x, y and z numbers no "
                     "larger than 360000.");
    }
    if (args.contains("fov") &&
        (!args["fov"].is_number() || !std::isfinite(args["fov"].get<double>()) ||
         args["fov"].get<double>() < 1.0 || args["fov"].get<double>() > 179.0)) {
        return viewportRequestError(binding, "fov must be a number between 1 and 179 degrees.");
    }
    return sendPhase7LiveRequest(binding, args, ipc);
}

CallToolResult handleCreateVisualTestLab(const json& args, std::shared_ptr<ipc::IIpcClient> ipc) {
    std::string target_path = args.value("target_resource_path", "");
    std::string env = args.value("environment", "studio_neutral");
    bool ortho = args.value("orthographic", false);
    json rig = args.value("camera_rig", json::array({"front", "top", "isometric"}));
    if (args.contains("overwrite") && !args["overwrite"].is_boolean()) {
        return CallToolResult::errorJson(400, "Parameter 'overwrite' must be a boolean.");
    }
    const bool overwrite = args.value("overwrite", false);

    // The target has to be a real resource beneath the project root before it
    // can be referenced from the generated scene. Checked first, before the
    // filesystem is touched: the handler used to create a directory and only
    // then refuse a target that did not exist, leaving the project with a
    // folder it did not have (#564). The empty string is checked like every
    // other value: it used to skip this block and write a lab with no target
    // in it, answered with the same success as a lab with one (#554).
    std::string target_resource;
    std::string target_type;
    {
        auto resolved = paths::resolveProjectFile(target_path);
        if (resolved.isErr()) {
            return CallToolResult::fromError(resolved.error(),
                                             "Invalid target_resource_path: ");
        }
        // The readers' spelling of the path, the way every writer reports it
        // now, so the ext_resource line and the result name the same file.
        target_resource = paths::resourcePathOf(resolved.value());
        const auto extension = resolved.value().extension().string();
        target_type = extension == ".tscn" || extension == ".scn" ? "PackedScene" : "Resource";
    }

    // Offline generator: an isolated visual testbed scene on disk, at the one
    // path the mutation gate also knows. At the project root, where the audit
    // and the search can see it, rather than inside the addon's own folder.
    const std::string lab_scene_path(tools::kVisualTestLabScenePath);
    const std::string disk_path(tools::kVisualTestLabDiskPath);

    if (std::filesystem::exists(disk_path) && !overwrite) {
        return CallToolResult::errorJson(
            409, "Visual test lab already exists; pass overwrite: true to replace it: " +
            lab_scene_path,
            {{"code", "already_exists"}, {"retry_with", {{"overwrite", true}}}});
    }

    std::ostringstream scene_file;
    scene_file << "[gd_scene";
    if (!target_resource.empty()) scene_file << " load_steps=2";
    // No uid, as resource_create writes none. The one this carried,
    // uid://didi_test_lab_sandbox, is not a uid: Godot reads only a to z and
    // digits after uid://, so it dropped it without a line and every lab had
    // none (#1164).
    scene_file << " format=3]\n\n";
    if (!target_resource.empty()) {
        scene_file << "[ext_resource type=\"" << target_type << "\" path=\""
                   << target_resource << "\" id=\"1_didi_target\"]\n\n";
    }
    scene_file
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

    if (!target_resource.empty()) {
        if (target_type == "PackedScene") {
            scene_file << "[node name=\"TargetInstance\" parent=\".\" "
                       << "instance=ExtResource(\"1_didi_target\")]\n";
        } else {
            // A plain Resource cannot be a node, so hang it off a holder the
            // cameras can still frame.
            scene_file << "[node name=\"TargetInstance\" type=\"Node3D\" parent=\".\"]\n"
                       << "metadata/didi_target = ExtResource(\"1_didi_target\")\n";
        }
    }

    auto written = files::writeFileAtomically(paths::projectPathFromUtf8(disk_path),
                                              scene_file.str());
    if (written.isErr()) {
        return CallToolResult::fromError(written.error(), "Failed to generate visual test lab sandbox scene file: ");
    }
    offline::ResourceIndexer::invalidateSharedIndex();
    // The file as it landed, read back from disk, the way resource_create
    // reports what it wrote rather than what it rendered (#1164).
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(paths::projectPathFromUtf8(disk_path), size_error);
    if (size_error) {
        return CallToolResult::errorJson(
            500, "Wrote the visual test lab at " + lab_scene_path +
                     " and could not read its size back: " + size_error.message());
    }

    json res = {
        {"status", "created_offline"},
        {"scene_path", lab_scene_path},
        {"file_bytes", file_bytes},
        {"target_resource_path", target_resource},
        // Which of the two scenes was written. A PackedScene target is
        // instanced under the lab; any other resource hangs off a holder node
        // as metadata, and the description used to say neither happened (#565).
        {"target_instanced", target_type == "PackedScene"},
        {"environment", env},
        {"camera_rig", rig},
        // runtime_launch, not the execute_test_session alias this used to name.
        // A client that lists tools by canonical name and follows the message
        // is being steered onto the deprecated surface (#408).
        {"message", "Created sandbox scene at " + lab_scene_path + ". Open Godot Editor to view live or run `runtime_launch`."}
    };
    // Replacing an existing lab took overwrite: true, which is the caller
    // accepting its loss, so a tab that has the lab open is reloaded from the
    // new file whatever it holds, the way scene_create reloads the scene it
    // overwrites. Left alone, the next save put the old lab back (#1068).
    reportEditorCopy(res, refreshEditorCopies(ipc, {lab_scene_path}, true));
    return CallToolResult::successJson(res);
}

CallToolResult handleViewportToggleDebugDraw(const ResolvedToolBinding& binding, const json& args,
                         std::shared_ptr<ipc::IIpcClient> ipc) {
    if (!hasOnlyViewportKeys(args, {"collision_shapes", "navigation_mesh", "wireframe"})) {
        return viewportRequestError(
            binding, "This tool accepts: collision_shapes, navigation_mesh, wireframe.");
    }
    // The schema says this with anyOf, which the validator does not enforce, so
    // calling it with {} produced the raw identifier as the whole explanation
    // and said nothing about which flag it wanted.
    if (!args.contains("collision_shapes") && !args.contains("navigation_mesh")) {
        return viewportRequestError(
            binding, "At least one debug draw flag is required: set collision_shapes or "
                     "navigation_mesh to a boolean.");
    }
    if (args.contains("collision_shapes") && !args["collision_shapes"].is_boolean()) {
        return viewportRequestError(binding, "collision_shapes must be a boolean.");
    }
    if (args.contains("navigation_mesh") && !args["navigation_mesh"].is_boolean()) {
        return viewportRequestError(binding, "navigation_mesh must be a boolean.");
    }
    if (args.contains("wireframe") &&
        (!args["wireframe"].is_boolean() || args["wireframe"].get<bool>())) {
        return viewportRequestError(
            binding, "wireframe must be false. Godot has no per-viewport wireframe toggle to "
                     "turn on from here.");
    }
    return sendPhase7LiveRequest(binding, args, ipc);
}

// The tools whose handlers this file holds. registerAllDefaultTools calls
// each domain's in turn (#1256).
void ToolRegistry::registerVisualTools() {

    {
        ToolDefinition t;
        t.name = "viewport_capture_frame";
        t.description = "Captures the active editor 2D/3D viewport as PNG when live, or returns an attributed synthetic grid preview offline.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_identifier", {{"type", "string"}, {"default", "active_editor_view"}}},
                {"resolution", {{"type", "object"}, {"default", {{"width", 256}, {"height", 192}}}, {"description", "Offline preview size; reserved and ignored by live capture"}}},
                {"render_debug_flags", {{"type", "array"}}},
                {"node_isolation_path", {{"type", "string"}}},
                {"isolation_background", {{"type", "string"}, {"enum", {"original", "transparent"}}, {"default", "original"}}},
                {"select_main_screen", {{"type", "boolean"}, {"default", false}}}
            }}
        };
        t.handler = [this](const json& args) { return handleCaptureViewport(args, m_ipcClient); };
        registerTool(t);

        // Alias
        t.name = "capture_viewport";
        t.handler = [this](const json& args) { return handleCaptureViewport(args, m_ipcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "viewport_diff_capture";
        t.description = "Captures a fresh live editor viewport frame and returns an exact RGBA pixel diff against a prior process-local capture ID.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"baseline_capture_id", {{"type", "string"}, {"minLength", 32}, {"maxLength", 32}, {"pattern", "^[0-9a-f]{32}$"}}},
                {"camera_identifier", {{"type", "string"}, {"default", "active_editor_view"}}},
                {"resolution", {{"type", "object"}, {"description", "Reserved and ignored by live capture"}}},
                {"node_isolation_path", {{"type", "string"}}},
                {"isolation_background", {{"type", "string"}, {"enum", {"original", "transparent"}}, {"default", "original"}}},
                {"threshold", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}, {"default", 0}}},
                {"min_ssim", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0},
                              {"description", "Structural similarity at or above which the frames count as perceptually identical. Sets perceptually_identical in the result."}}},
                {"max_hamming_distance", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64},
                                          {"description", "Largest perceptual-hash distance that still counts as perceptually identical. Sets perceptually_identical in the result."}}},
                {"select_main_screen", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", {"baseline_capture_id"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportDiffCapture(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_set_camera_transform";
        t.description = "Updates an in-scene Camera3D transform and optional field of view through the editor UndoRedo history.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"camera_path", {{"type", "string"}}},
                {"position", {{"type", "object"}, {"description", "Vector3 {x, y, z}"}}},
                {"rotation_degrees", {{"type", "object"}, {"description", "Optional Vector3 {x, y, z} in degrees"}}},
                {"fov", {{"type", "number"}}}
            }},
            {"required", {"camera_path", "position"}}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportSetCameraTransform(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_create_test_lab";
        t.description = "Writes a basic offline sandbox .tscn at res://didi_test_lab.tscn with lighting, a ground box, and three cameras. A PackedScene target is instanced under the lab as TargetInstance; any other resource is attached to a TargetInstance holder as metadata/didi_target, and the result says which with target_instanced.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"target_resource_path", {{"type", "string"}}},
                {"environment", {{"type", "string"}, {"default", "studio_neutral"}}},
                {"orthographic", {{"type", "boolean"}, {"default", false}}},
                {"camera_rig", {{"type", "array"}, {"default", {"front", "top", "isometric"}}, {"description", "Metadata only; generated scene contains front, top, and isometric cameras"}}},
                {"overwrite", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", {"target_resource_path"}}
        };
        t.handler = [this](const json& args) { return handleCreateVisualTestLab(args, m_sourceIpcClient); };
        registerTool(t);

        // Alias
        t.name = "create_visual_test_lab";
        t.handler = [this](const json& args) { return handleCreateVisualTestLab(args, m_sourceIpcClient); };
        registerTool(t);
    }
    {
        ToolDefinition t;
        t.name = "viewport_toggle_debug_draw";
        t.description = "Sets editor SceneTree collision and navigation debug hints for future games run from that editor.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"collision_shapes", {{"type", "boolean"}, {"default", true}}},
                {"navigation_mesh", {{"type", "boolean"}, {"default", false}}},
                {"wireframe", {{"type", "boolean"}, {"default", false}}}
            }}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleViewportToggleDebugDraw(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_render_ghost_preview";
        t.description = "Draws translucent wireframe boxes in the open editor viewport to show where a proposed mutation would land, without adding anything to the scene, so the scene never becomes dirty and there is nothing to undo.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"previews", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                    {"description", "Shapes to draw. All of them share one dimension, because a 2D rectangle and a 3D box are drawn into different worlds."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"position", {{"type", "object"}, {"description", "Centre of the shape: {x,y} for 2D or {x,y,z} for 3D."}}},
                            {"size", {{"type", "object"}, {"description", "Full extents, the size a person would type into the inspector. Every axis must be greater than 0."}}},
                            {"rotation_degrees", {{"type", "object"}, {"description", "3D only: {x,y,z} Euler degrees. A 2D preview is an axis-aligned rectangle."}}},
                            {"kind", {{"type", "string"}, {"enum", json::array({"addition", "translation", "deletion"})},
                                      {"default", "addition"},
                                      {"description", "Chooses the colour: cyan for an addition, yellow for a translation, red for a deletion."}}},
                            {"color", {{"type", "object"}, {"description", "Overrides the colour the kind would pick. Components r, g and b from 0 to 1."}}},
                            {"label", {{"type", "string"}, {"maxLength", 256}, {"description", "Echoed back so a caller can tell one shape from another. It is not drawn."}}}
                        }},
                        {"required", json::array({"position", "size"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"replace", {{"type", "boolean"}, {"default", true},
                             {"description", "Clear the previews already on screen first. A preview usually stands for one proposal, so replacing is the default."}}}
            }},
            {"required", json::array({"previews"})},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleEditorRenderGhostPreview(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "editor_clear_ghost_previews";
        t.description = "Removes wireframe previews from the editor viewport. With no argument it clears every preview, which is the call that works whatever left them behind.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"preview_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                                {"description", "Clear just this batch. Omit to clear all of them."}}}
            }},
            {"additionalProperties", false}
        };
        t.boundHandler = [this](const ResolvedToolBinding& binding, const json& args) {
            return handleEditorClearGhostPreviews(binding, args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
    {
        ToolDefinition t;
        t.name = "viewport_capture_passes";
        t.description = "Draws the live 3D scene again with replacement materials and returns a depth or world-space normal image alongside the ordinary colour frame, so which thing is nearer and which way a surface faces can be read off the pixels rather than guessed.";
        t.inputSchema = {
            {"type", "object"},
            {"properties", {
                {"passes", {
                    {"type", "array"}, {"minItems", 1}, {"maxItems", 4},
                    // Declared, not only described: the engine refuses a repeat,
                    // and a caller offline deserves the same answer.
                    {"uniqueItems", true},
                    {"description", "Which pictures to take, returned as one image each in this order. A pass named twice is refused."},
                    {"items", {{"type", "string"},
                               {"enum", json::array({"color", "depth", "normal", "segmentation"})}}}
                }},
                {"camera_identifier", {{"type", "string"}, {"description", "Editor sessions only; a game has one root viewport."}}},
                {"depth_far", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000000},
                               {"description", "The distance mapped to white in the depth pass. Defaults to the rendering camera's own far plane, and the value used is reported back."}}},
                {"select_main_screen", {{"type", "boolean"}, {"default", false}}}
            }},
            {"required", json::array({"passes"})},
            {"additionalProperties", false}
        };
        t.handler = [this](const json& args) {
            return handleViewportCapturePasses(args, m_ipcClient);
        };
        registerTool(std::move(t));
    }
}

} // namespace mcp
} // namespace didi
