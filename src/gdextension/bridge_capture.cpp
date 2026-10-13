#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/runtime/segmentation.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

namespace {

// A viewport control that is not the one on screen has no size, and Godot hands
// back its 2x2 minimum rather than refusing. Capturing that produced a
// four-pixel image reported as a successful live frame, which a caller cannot
// tell from a scene that happens to be empty. Nothing smaller than this is a
// picture of anything.
constexpr int64_t kMinimumCaptureEdge = 8;

// The refusal every capture makes on an engine that cannot draw.
//
// 409 rather than 404: nothing the caller named is missing. The diagnosis is
// the same for both kinds of process and the remedy is not, so the sentence
// asks which one it is in (#777).
//
// For an editor nothing about the request can fix it, which is what the wording
// below says, and the synthesised preview is a real second route.
//
// For a game every word of that was wrong. It is not an editor, restarting an
// editor with a display does nothing for it, and detaching does not produce a
// preview of a game. `runtime_launch` defaults to `headless: true`, so this is
// the state a caller who did not think about it is in, and the fix is one
// argument on the call that started the process. Saying "nothing about the
// request can fix that" there reads as "nothing can fix this", which sent
// callers away from a loop this surface supports.
//
// The kind is the one the session descriptor published, which is the same fact
// `editor_save_scene` and `scene_get_hierarchy` already refuse a game with.
Error headlessCaptureRefusal() {
    const auto kind = EditorHook::instance().sessionKind();
    const std::string driver = displayServerName();
    const bool is_game = kind == runtime::SessionKind::game;

    std::string message;
    if (is_game) {
        message = "This game is running headless (display driver '" + driver +
                  "'), so there is no rendering device behind its viewports and no frame to "
                  "capture. runtime_launch defaults to headless: true; stop this session and "
                  "launch it again with headless: false to capture frames from it.";
    } else if (kind == runtime::SessionKind::editor) {
        message = "This editor is running headless (display driver '" + driver +
                  "'), so there is no rendering device behind its viewports and no frame to "
                  "capture. Nothing about the request can fix that. Run the editor with a "
                  "display, or detach and call again with no editor attached to get the "
                  "synthesised preview.";
    } else {
        // An extension older than the session-kind field, or a process that
        // published none. Say what is true of both rather than guess which.
        message = "This engine is running headless (display driver '" + driver +
                  "'), so there is no rendering device behind its viewports and no frame to "
                  "capture. An editor has to be run with a display; a game has to be launched "
                  "again with headless: false.";
    }
    // The prose says which process this is and the data lets a caller branch on
    // it without reading the sentence, which is what the editor-only refusals
    // beside it already do.
    return Error(409, message,
                 {{"code", "headless_engine"},
                  {"display_driver", driver},
                  {"session_kind", kind == runtime::SessionKind::game      ? json("game")
                                   : kind == runtime::SessionKind::editor  ? json("editor")
                                                                           : json(nullptr)},
                  {"relaunch_argument", is_game ? json("headless") : json(nullptr)}});
}

Result<ViewportPixels> captureViewportObject(GDExtensionObjectPtr viewport_object,
                                             const std::string& described_target) {
    if (!viewport_object) return Error::notFound("Viewport is unavailable");
    // Asked before the texture, because on this engine the answer is the same
    // whatever the viewport turns out to be, and the texture route ends in
    // "Viewport image is unavailable", which names nothing.
    if (!engineCanRender()) return headlessCaptureRefusal();
    auto texture = callObject(viewport_object, "Viewport", "get_texture", 1746695840LL);
    if (texture.isErr()) return texture.error();
    auto texture_object = objectFromVariant(texture.value());
    if (texture_object.isErr() || !texture_object.value()) return Error::notFound("Viewport texture is unavailable");
    auto texture_width_value = callObject(texture_object.value(), "Texture2D", "get_width", 3905245786LL);
    auto texture_height_value = callObject(texture_object.value(), "Texture2D", "get_height", 3905245786LL);
    if (texture_width_value.isErr()) return texture_width_value.error();
    if (texture_height_value.isErr()) return texture_height_value.error();
    auto texture_width = scalarFromVariant<int64_t>(texture_width_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto texture_height = scalarFromVariant<int64_t>(texture_height_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (texture_width.isErr()) return texture_width.error();
    if (texture_height.isErr()) return texture_height.error();
    if (texture_width.value() <= 0 || texture_height.value() <= 0) {
        return Error::notFound("Viewport has no rendered texture in the current display mode");
    }
    if (texture_width.value() < kMinimumCaptureEdge || texture_height.value() < kMinimumCaptureEdge) {
        // An editor viewport has no size while its main screen is not the one
        // showing, and the tool's own select_main_screen switches to it. The
        // remedy the floor gave was to read the scene's paths, and no node was
        // named (#1302).
        const bool editor_viewport = described_target == "editor_2d" || described_target == "editor_3d";
        json data = {{"code", "not_found"}, {"retryable", editor_viewport}};
        if (editor_viewport) data["retry_with"] = {{"select_main_screen", true}};
        return Error(404,
                     "Viewport '" + described_target + "' has no size on screen (" +
                         std::to_string(texture_width.value()) + "x" +
                         std::to_string(texture_height.value()) +
                         "), so there is no frame to capture." +
                         (editor_viewport ? std::string(" Its main screen is not the one selected; "
                                                        "select_main_screen: true switches to it.")
                                          : std::string()),
                     std::move(data));
    }
    const auto texture_size = image::checkedRgbaSize(texture_width.value(), texture_height.value());
    if (texture_size.isErr()) return texture_size.error();
    auto image = callObject(texture_object.value(), "Texture2D", "get_image", 4190603485LL);
    if (image.isErr()) return image.error();
    auto image_object = objectFromVariant(image.value());
    if (image_object.isErr() || !image_object.value()) return Error::notFound("Viewport image is unavailable");
    auto rgba8 = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(5));
    auto converted = callObject(image_object.value(), "Image", "convert", 2120693146LL, {&rgba8.value()});
    if (converted.isErr()) return converted.error();
    auto width_value = callObject(image_object.value(), "Image", "get_width", 3905245786LL);
    auto height_value = callObject(image_object.value(), "Image", "get_height", 3905245786LL);
    if (width_value.isErr()) return width_value.error();
    if (height_value.isErr()) return height_value.error();
    auto width = scalarFromVariant<int64_t>(width_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto height = scalarFromVariant<int64_t>(height_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (width.isErr()) return width.error();
    if (height.isErr()) return height.error();
    const auto expected_size = image::checkedRgbaSize(width.value(), height.value());
    if (expected_size.isErr()) return expected_size.error();
    auto data = callObject(image_object.value(), "Image", "get_data", 2362200018LL);
    if (data.isErr()) return data.error();
    auto size_value = callVariant(data.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    const int64_t expected = static_cast<int64_t>(expected_size.value());
    if (size.value() != expected) return Error::internal("RGBA8 viewport byte count does not match dimensions");
    auto to_packed = GodotApi::instance().get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY);
    if (!to_packed) return Error::internal("PackedByteArray conversion API is unavailable");
    NativeValue packed(GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY);
    to_packed(packed.ptr(), data.value().ptr());
    packed.markInitialized();
    ViewportPixels output;
    output.width = static_cast<int>(width.value());
    output.height = static_cast<int>(height.value());
    const auto* first = GodotApi::instance().packed_byte_array_operator_index_const(packed.ptr(), 0);
    if (!first) return Error::internal("PackedByteArray viewport read failed");
    output.rgba.resize(static_cast<size_t>(expected));
    std::memcpy(output.rgba.data(), first, static_cast<size_t>(expected));
    return output;
}

} // namespace

namespace {

// The scene drawn again with every geometry node's material replaced, so the
// picture answers one question instead of showing one appearance.
//
// Every pass writes the inverse of the sRGB curve the framebuffer applies on
// the way out, which is what keeps a mid grey from arriving as a much lighter
// one. It does not make the stored byte the written byte: the viewport applies
// its own post-processing after this, and how much it changes depends on the
// engine. A 4.7.2 editor returns these values unchanged and a 4.5.1 editor
// returns them scaled by about a quarter. So a pass is an ordering that can be
// read and compared, not a calibrated measurement, and nothing here claims
// otherwise.
constexpr const char* kPassStoreHelper = R"(
vec3 didi_store(vec3 c) {
	vec3 lo = c / 12.92;
	vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
	return mix(hi, lo, step(c, vec3(0.04045)));
}
)";

std::string passShaderSource(const std::string& kind) {
    std::string source = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n";
    if (kind == "segmentation") {
        // One flat colour a material sets per node, so the picture is a map
        // from pixel to node rather than a picture of the scene.
        source += "uniform vec3 didi_colour = vec3(1.0);\n";
        source += kPassStoreHelper;
        source += "void fragment() {\n"
                  "\tALBEDO = didi_store(didi_colour);\n"
                  "}\n";
        return source;
    }
    if (kind == "depth") {
        source += "uniform float didi_far = 100.0;\n";
        source += kPassStoreHelper;
        // VERTEX is view space in a fragment shader and the camera looks down
        // -Z, so -VERTEX.z is the distance in front of the camera.
        source += "void fragment() {\n"
                  "\tALBEDO = didi_store(vec3(clamp(-VERTEX.z / didi_far, 0.0, 1.0)));\n"
                  "}\n";
        return source;
    }
    // World space rather than view space, so a surface that faces up reads the
    // same whichever way the camera happens to be turned.
    source += kPassStoreHelper;
    source += "void fragment() {\n"
              "\tvec3 world_normal = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);\n"
              "\tALBEDO = didi_store(world_normal * 0.5 + 0.5);\n"
              "}\n";
    return source;
}

struct PassSubject {
    GDExtensionObjectPtr node{nullptr};
    std::string path;
    std::string class_name;
};

constexpr size_t kMaxPassNodes = 4096;

Result<void> collectPassSubjects(GDExtensionObjectPtr node, GDExtensionObjectPtr root, bool editor,
                                 std::vector<PassSubject>& subjects, size_t& examined,
                                 bool& limit_reached) {
    if (examined >= kMaxPassNodes) {
        limit_reached = true;
        return Result<void>::ok();
    }
    ++examined;
    auto paintable = objectIsClass(node, "GeometryInstance3D");
    if (paintable.isErr()) return paintable.error();
    if (paintable.value()) {
        PassSubject subject;
        subject.node = node;
        auto class_name = nodeString(node, "get_class", 201670096LL);
        if (class_name.isErr()) return class_name.error();
        subject.class_name = boundUtf8(class_name.value(), 256).value;
        auto path = editor ? logicalPathFromEditedRoot(root, node)
                           : nodeString(node, "get_path", 4075236667LL);
        if (path.isErr()) return path.error();
        subject.path = boundUtf8(path.value(), 1024).value;
        subjects.push_back(std::move(subject));
    }

    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
    if (children.isErr()) return children.error();
    auto size_value = callVariant(children.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    for (int64_t index = 0; index < size.value(); ++index) {
        auto slot = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (slot.isErr()) return slot.error();
        auto child_value = callVariant(children.value(), "get", {&slot.value()});
        if (child_value.isErr()) return child_value.error();
        auto child = objectFromVariant(child_value.value());
        if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
        auto walked = collectPassSubjects(child.value(), root, editor, subjects, examined, limit_reached);
        if (walked.isErr()) return walked.error();
        if (limit_reached) break;
    }
    return Result<void>::ok();
}

// Holds what every painted node had before, and puts it back.
//
// The previous material is kept as a Variant rather than as a pointer on
// purpose: an inline material's only reference is often the override itself, so
// a raw pointer would be left pointing at a freed resource the moment the
// replacement went on.
class MaterialOverrides {
public:
    ~MaterialOverrides() { (void)restore(); }

    Result<void> paint(const std::vector<PassSubject>& subjects,
                       const std::vector<GDExtensionObjectPtr>& materials) {
        for (size_t index = 0; index < subjects.size(); ++index) {
            auto previous = callObject(subjects[index].node, "GeometryInstance3D",
                                       "get_material_override", 5934680LL);
            if (previous.isErr()) return previous.error();
            m_previous.emplace_back(subjects[index].node, std::move(previous.value()));
            auto replacement = makeObject(materials[index]);
            if (replacement.isErr()) return replacement.error();
            auto applied = callObject(subjects[index].node, "GeometryInstance3D",
                                      "set_material_override", 2757459619LL, {&replacement.value()});
            if (applied.isErr()) return applied.error();
        }
        return Result<void>::ok();
    }

    // Every node is attempted even when one fails, because stopping at the
    // first failure would leave the rest of the scene wearing a debug material.
    Result<void> restore() {
        Result<void> outcome = Result<void>::ok();
        for (auto& entry : m_previous) {
            auto applied = callObject(entry.first, "GeometryInstance3D", "set_material_override",
                                      2757459619LL, {&entry.second});
            if (applied.isErr() && outcome.isOk()) outcome = applied.error();
        }
        m_previous.clear();
        return outcome;
    }

private:
    std::vector<std::pair<GDExtensionObjectPtr, VariantValue>> m_previous;
};

Result<GDExtensionObjectPtr> makePassShader(const std::string& kind) {
    NativeName shader_class("Shader");
    auto shader = constructObject(shader_class.ptr());
    if (!shader) return Error(501, "Godot ClassDB could not construct a Shader");
    auto code = makeString(passShaderSource(kind));
    if (code.isErr()) return code.error();
    auto applied = callObject(shader, "Shader", "set_code", 83702148LL, {&code.value()});
    if (applied.isErr()) return applied.error();
    return shader;
}

Result<GDExtensionObjectPtr> makePassMaterial(GDExtensionObjectPtr shader) {
    NativeName material_class("ShaderMaterial");
    auto material = constructObject(material_class.ptr());
    if (!material) return Error(501, "Godot ClassDB could not construct a ShaderMaterial");
    auto shader_value = makeObject(shader);
    if (shader_value.isErr()) return shader_value.error();
    auto applied = callObject(material, "ShaderMaterial", "set_shader", 3341921675LL,
                              {&shader_value.value()});
    if (applied.isErr()) return applied.error();
    return material;
}

// The shader takes the colour in the 0..1 range the framebuffer will store, so
// the byte the legend matches on is the byte the palette names.
Result<VariantValue> makeColorVector3(const runtime::SegmentationColor& colour) {
    return makeVector3(static_cast<double>(colour.r) / 255.0,
                       static_cast<double>(colour.g) / 255.0,
                       static_cast<double>(colour.b) / 255.0);
}

Result<void> setPassParameter(GDExtensionObjectPtr material, const char* name, VariantValue& value) {
    auto parameter = makeStringName(name);
    if (parameter.isErr()) return parameter.error();
    auto applied = callObject(material, "ShaderMaterial", "set_shader_parameter", 3776071444LL,
                              {&parameter.value(), &value});
    return applied.isOk() ? Result<void>::ok() : Result<void>(applied.error());
}

// The far distance a depth pass divides by. The camera's own far plane is the
// honest default: it is the distance past which that camera draws nothing.
Result<double> cameraFarPlane(const std::string& camera_identifier, const std::string& session_kind) {
    auto required = requireMethodBind("Camera3D", "get_far", 1740695150LL);
    if (required.isErr()) return Error(501, required.error().message);
    Result<GDExtensionObjectPtr> camera = Error::notFound("No camera");
    if (session_kind == "game") {
        auto tree = liveSceneTree();
        if (tree.isErr()) return tree.error();
        auto root = liveSceneTreeRoot(tree.value());
        if (root.isErr()) return root.error();
        auto found = callObject(root.value(), "Viewport", "get_camera_3d", 2285090890LL);
        if (found.isErr()) return found.error();
        auto object = objectFromVariant(found.value());
        if (object.isErr() || !object.value()) {
            return Error(409, "The game viewport has no 3D camera to take a far plane from", {{"code", "no_camera_far_plane"}});
        }
        camera = object.value();
    } else {
        (void)camera_identifier;
        auto editor = editorInterface();
        if (editor.isErr()) return editor.error();
        auto root = editedSceneRoot(editor.value());
        if (root.isErr()) return root.error();
        auto viewport = callObject(root.value(), "Node", "get_viewport", 3596683776LL);
        if (viewport.isErr()) return viewport.error();
        auto viewport_object = objectFromVariant(viewport.value());
        if (viewport_object.isErr() || !viewport_object.value()) {
            return Error(409, "The edited scene is not in a viewport with a camera", {{"code", "no_camera_far_plane"}});
        }
        auto found = callObject(viewport_object.value(), "Viewport", "get_camera_3d", 2285090890LL);
        if (found.isErr()) return found.error();
        auto object = objectFromVariant(found.value());
        if (object.isErr() || !object.value()) {
            return Error(409, "No 3D camera is rendering this viewport to take a far plane from", {{"code", "no_camera_far_plane"}});
        }
        camera = object.value();
    }
    if (camera.isErr()) return camera.error();
    auto far_value = callObject(camera.value(), "Camera3D", "get_far", 1740695150LL);
    if (far_value.isErr()) return far_value.error();
    auto number = scalarFromVariant<double>(far_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
    if (number.isErr()) return number.error();
    if (!(number.value() > 0.0)) return Error(409, "The camera far plane is not a distance", {{"code", "no_camera_far_plane"}});
    return number.value();
}

} // namespace

Result<MultipassCapture> GodotBridge::captureViewportPasses(const std::vector<std::string>& passes,
                                                            const std::string& camera_identifier,
                                                            const std::string& session_kind,
                                                            double requested_depth_far) {
    // Before the camera question. Without this the colour, depth and normal
    // passes reached "No 3D camera is rendering this viewport", which is a real
    // sentence about a different problem, and segmentation reached the bare 404
    // (#676).
    if (!engineCanRender()) return headlessCaptureRefusal();
    for (const auto& bind : {std::make_tuple("GeometryInstance3D", "get_material_override", 5934680LL),
                             std::make_tuple("GeometryInstance3D", "set_material_override", 2757459619LL),
                             std::make_tuple("Shader", "set_code", 83702148LL),
                             std::make_tuple("ShaderMaterial", "set_shader", 3341921675LL),
                             std::make_tuple("ShaderMaterial", "set_shader_parameter", 3776071444LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }

    const bool editor = session_kind != "game";
    Result<GDExtensionObjectPtr> root = Error::internal("unresolved");
    if (editor) {
        auto interface = editorInterface();
        if (interface.isErr()) return interface.error();
        root = editedSceneRoot(interface.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return tree.error();
        root = liveSceneTreeRoot(tree.value());
    }
    if (root.isErr()) return root.error();

    MultipassCapture capture;
    const bool wants_depth =
        std::find(passes.begin(), passes.end(), "depth") != passes.end();
    if (wants_depth) {
        if (requested_depth_far > 0.0) {
            capture.depth_far = requested_depth_far;
        } else {
            auto far_plane = cameraFarPlane(camera_identifier, session_kind);
            if (far_plane.isErr()) return far_plane.error();
            capture.depth_far = far_plane.value();
        }
    }

    std::vector<PassSubject> subjects;
    size_t examined = 0;
    bool limit_reached = false;
    const bool needs_geometry =
        std::any_of(passes.begin(), passes.end(),
                    [](const std::string& kind) { return kind != "color"; });
    if (needs_geometry) {
        auto walked = collectPassSubjects(root.value(), root.value(), editor, subjects, examined,
                                          limit_reached);
        if (walked.isErr()) return walked.error();
    }
    capture.examined = static_cast<int>(examined);
    capture.painted = static_cast<int>(subjects.size());
    capture.scan_limit_reached = limit_reached;

    for (const auto& kind : passes) {
        if (kind == "color") {
            auto frame = editor ? captureEditorViewport(camera_identifier) : captureGameViewport();
            if (frame.isErr()) return frame.error();
            capture.frames.push_back(PassFrame{kind, std::move(frame.value())});
            continue;
        }
        // A scene with nothing to paint still answers, with a picture of an
        // empty pass rather than an error about a scene that is simply bare.
        auto shader = makePassShader(kind);
        if (shader.isErr()) return shader.error();

        auto material = makePassMaterial(shader.value());
        if (material.isErr()) return material.error();
        if (kind == "depth") {
            auto far_value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, capture.depth_far);
            if (far_value.isErr()) return far_value.error();
            auto applied = setPassParameter(material.value(), "didi_far", far_value.value());
            if (applied.isErr()) return applied.error();
        }
        // Who this pass paints, which is everyone except where segmentation
        // runs out of palette. A pass after it must still paint the whole
        // scene, so the shared subject list is not what gets shortened.
        std::vector<PassSubject> painted_subjects = subjects;
        std::vector<GDExtensionObjectPtr> materials(painted_subjects.size(), material.value());
        if (kind == "segmentation") {
            // The one pass where every node wears a different material, because
            // the colour is the answer rather than the subject.
            const auto& palette = runtime::segmentationPalette();
            if (painted_subjects.size() > palette.size()) {
                // A node with no entry left is not painted at all, so it keeps
                // the colour it already had rather than borrowing another
                // node's and being reported as it.
                for (size_t index = palette.size(); index < painted_subjects.size(); ++index) {
                    capture.unsegmented.push_back(painted_subjects[index].path);
                }
                painted_subjects.resize(palette.size());
                materials.resize(palette.size());
            }
            for (size_t index = 0; index < painted_subjects.size(); ++index) {
                auto node_material = makePassMaterial(shader.value());
                if (node_material.isErr()) return node_material.error();
                auto colour = makeColorVector3(palette[index]);
                if (colour.isErr()) return colour.error();
                auto applied = setPassParameter(node_material.value(), "didi_colour", colour.value());
                if (applied.isErr()) return applied.error();
                materials[index] = node_material.value();
                capture.segmented.push_back(
                    SegmentedNode{painted_subjects[index].path, painted_subjects[index].class_name,
                                  index});
            }
        }

        MaterialOverrides overrides;
        auto painted = overrides.paint(painted_subjects, materials);
        if (painted.isErr()) return painted.error();
        auto drawn = forceDraw();
        if (drawn.isErr()) return drawn.error();
        auto frame = editor ? captureEditorViewport(camera_identifier) : captureGameViewport();
        // The restore runs whatever the capture did, and its failure is the one
        // that gets reported: a scene left wearing a debug material matters more
        // than a frame that did not arrive.
        auto restored = overrides.restore();
        auto redrawn = forceDraw();
        if (restored.isErr()) {
            DIDI_LOG_ERROR("GODOT_BRIDGE", "Pass materials could not be restored: ",
                           restored.error().message);
            return restored.error();
        }
        if (redrawn.isErr()) return redrawn.error();
        if (frame.isErr()) return frame.error();
        capture.frames.push_back(PassFrame{kind, std::move(frame.value())});
    }
    return capture;
}

Result<ViewportPixels> GodotBridge::captureEditorViewport(const std::string& camera_identifier) {
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return editor_result.error();
    const auto selected = selectEditorViewport(camera_identifier);
    if (!selected.has_value()) {
        return Error::invalidArgument("camera_identifier must be one of " +
                                      editorViewportIdentifierList() + "; received \"" +
                                      camera_identifier + "\"");
    }
    const bool capture_2d = *selected == EditorViewport::TwoD;
    Result<VariantValue> viewport = capture_2d
        ? callObject(editor_result.value(), "EditorInterface", "get_editor_viewport_2d", 3750751911LL)
        : [&]() -> Result<VariantValue> {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
            if (index.isErr()) return index.error();
            return callObject(editor_result.value(), "EditorInterface", "get_editor_viewport_3d", 1970834490LL, {&index.value()});
        }();
    if (viewport.isErr()) return viewport.error();
    auto viewport_object = objectFromVariant(viewport.value());
    if (viewport_object.isErr() || !viewport_object.value()) {
        return Error::notFound("Editor SubViewport is unavailable");
    }
    return captureViewportObject(viewport_object.value(), camera_identifier);
}

// A game has one root viewport and no camera selection to make. The frame is
// already in the process Didi is attached to, so a caller that can pause the
// game, step it and read its tree can now also see it.
Result<ViewportPixels> GodotBridge::captureGameViewport() {
    auto tree = liveSceneTree();
    if (tree.isErr()) return tree.error();
    auto root = liveSceneTreeRoot(tree.value());
    if (root.isErr()) return root.error();
    return captureViewportObject(root.value(), "root_viewport");
}

Result<ViewportIsolationState> GodotBridge::beginViewportIsolation(
    const std::string& node_path, const std::string& camera_identifier,
    const std::string& isolation_background) {
    if (node_path.empty()) return Error::invalidArgument("node_isolation_path must not be empty");
    if (isolation_background != "original" && isolation_background != "transparent") {
        return Error::invalidArgument("isolation_background must be original or transparent");
    }
    // Refused here, before anything in the editor is touched, because the
    // transparent branch below picks a viewport from this name and a rollback
    // is a worse way to find out the name was not one.
    if (!selectEditorViewport(camera_identifier).has_value()) {
        return Error::invalidArgument("camera_identifier must be one of " +
                                      editorViewportIdentifierList() + "; received \"" +
                                      camera_identifier + "\"");
    }
    auto& api = GodotApi::instance();
    if (!api.object_get_instance_id || !api.object_get_instance_from_id) {
        return Error::internal("Godot object identity API is unavailable");
    }
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    auto target = resolveNode(root.value(), node_path);
    if (target.isErr()) return target.error();
    auto canonical = logicalPathFromEditedRoot(root.value(), target.value());
    if (canonical.isErr()) return canonical.error();

    auto is_class = [&](GDExtensionObjectPtr object, const char* class_name) -> Result<bool> {
        auto name = makeString(class_name);
        if (name.isErr()) return name.error();
        auto called = callObject(object, "Object", "is_class", 3927539163LL, {&name.value()});
        if (called.isErr()) return called.error();
        auto value = scalarFromVariant<GDExtensionBool>(called.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (value.isErr()) return value.error();
        return value.value() != 0;
    };
    auto is_ancestor = [&](GDExtensionObjectPtr ancestor, GDExtensionObjectPtr node) -> Result<bool> {
        auto node_value = makeObject(node);
        if (node_value.isErr()) return node_value.error();
        auto called = callObject(ancestor, "Node", "is_ancestor_of", 3093956946LL,
                                 {&node_value.value()});
        if (called.isErr()) return called.error();
        auto value = scalarFromVariant<GDExtensionBool>(called.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (value.isErr()) return value.error();
        return value.value() != 0;
    };

    ViewportIsolationState state;
    state.canonical_node_path = canonical.value();
    state.isolation_background = isolation_background;
    std::vector<GDExtensionObjectPtr> stack{root.value()};
    constexpr size_t kMaxIsolationNodes = 100000;
    size_t visited = 0;
    while (!stack.empty()) {
        auto current = stack.back();
        stack.pop_back();
        if (++visited > kMaxIsolationNodes) {
            return Error::invalidArgument("Edited scene exceeds the 100000-node isolation limit");
        }

        bool keep = current == target.value();
        if (!keep) {
            auto current_is_ancestor = is_ancestor(current, target.value());
            if (current_is_ancestor.isErr()) return current_is_ancestor.error();
            auto target_is_ancestor = is_ancestor(target.value(), current);
            if (target_is_ancestor.isErr()) return target_is_ancestor.error();
            keep = current_is_ancestor.value() || target_is_ancestor.value();
        }
        if (!keep) {
            std::string owner;
            auto canvas = is_class(current, "CanvasItem");
            if (canvas.isErr()) return canvas.error();
            if (canvas.value()) owner = "CanvasItem";
            else {
                auto node3d = is_class(current, "Node3D");
                if (node3d.isErr()) return node3d.error();
                if (node3d.value()) owner = "Node3D";
            }
            if (!owner.empty()) {
                auto visible_value = callObject(current, owner.c_str(), "is_visible", 36873697LL);
                if (visible_value.isErr()) return visible_value.error();
                auto visible = scalarFromVariant<GDExtensionBool>(visible_value.value(),
                                                                  GDEXTENSION_VARIANT_TYPE_BOOL);
                if (visible.isErr()) return visible.error();
                if (visible.value()) {
                    state.visibility.push_back({static_cast<uint64_t>(api.object_get_instance_id(current)),
                                                owner, true});
                }
            }
        }

        auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        if (include_internal.isErr()) return include_internal.error();
        auto children = callObject(current, "Node", "get_children", 873284517LL,
                                   {&include_internal.value()});
        if (children.isErr()) return children.error();
        auto size_value = callVariant(children.value(), "size");
        if (size_value.isErr()) return size_value.error();
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        for (int64_t i = size.value(); i-- > 0;) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return index.error();
            auto child_value = callVariant(children.value(), "get", {&index.value()});
            if (child_value.isErr()) return child_value.error();
            auto child = objectFromVariant(child_value.value());
            if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child during isolation");
            stack.push_back(child.value());
        }
    }

    RestorationGuard mutation_guard([&]() {
        return restoreViewportIsolation(state);
    });
    auto rollback = [&]() -> Result<ViewportIsolationState> {
        auto restored = restoreViewportIsolation(state);
        mutation_guard.dismiss();
        if (restored.isErr()) {
            return Error(500, "Viewport isolation failed and rollback was incomplete: " +
                              restored.error().message);
        }
        return Error::internal("Viewport isolation mutation failed");
    };
    auto hidden = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (hidden.isErr()) return hidden.error();
    for (const auto& point : state.visibility) {
        auto object = api.object_get_instance_from_id(static_cast<GDObjectInstanceID>(point.instance_id));
        if (!object) return rollback();
        auto changed = callObject(object, point.class_name.c_str(), "set_visible", 2586408642LL,
                                  {&hidden.value()});
        if (changed.isErr()) return rollback();
    }

    if (isolation_background == "transparent") {
        // Checked at the top of this function, so the name is one of the two
        // lists by the time the isolation has touched anything.
        const bool capture_2d =
            *selectEditorViewport(camera_identifier) == EditorViewport::TwoD;
        Result<VariantValue> viewport = capture_2d
            ? callObject(editor.value(), "EditorInterface", "get_editor_viewport_2d", 3750751911LL)
            : [&]() -> Result<VariantValue> {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
                if (index.isErr()) return index.error();
                return callObject(editor.value(), "EditorInterface", "get_editor_viewport_3d",
                                  1970834490LL, {&index.value()});
            }();
        if (viewport.isErr()) return rollback();
        auto viewport_object = objectFromVariant(viewport.value());
        if (viewport_object.isErr() || !viewport_object.value()) return rollback();
        auto original_value = callObject(viewport_object.value(), "Viewport",
                                         "has_transparent_background", 36873697LL);
        if (original_value.isErr()) return rollback();
        auto original = scalarFromVariant<GDExtensionBool>(original_value.value(),
                                                           GDEXTENSION_VARIANT_TYPE_BOOL);
        if (original.isErr()) return rollback();
        state.viewport_instance_id = static_cast<uint64_t>(
            api.object_get_instance_id(viewport_object.value()));
        state.original_transparent_background = original.value() != 0;
        state.restore_transparent_background = !state.original_transparent_background;
        if (state.restore_transparent_background) {
            auto enabled = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
            if (enabled.isErr()) return rollback();
            auto changed = callObject(viewport_object.value(), "Viewport", "set_transparent_background",
                                      2586408642LL, {&enabled.value()});
            if (changed.isErr()) return rollback();
        }
    }
    mutation_guard.dismiss();
    return state;
}

Result<void> GodotBridge::restoreViewportIsolation(const ViewportIsolationState& state) {
    auto& api = GodotApi::instance();
    if (!api.object_get_instance_from_id) return Error::internal("Godot object identity API is unavailable");
    std::vector<std::string> failures;
    if (state.restore_transparent_background) {
        auto viewport = api.object_get_instance_from_id(
            static_cast<GDObjectInstanceID>(state.viewport_instance_id));
        if (!viewport) failures.push_back("viewport was freed");
        else {
            auto original = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                       static_cast<GDExtensionBool>(state.original_transparent_background));
            auto restored = original.isOk()
                ? callObject(viewport, "Viewport", "set_transparent_background", 2586408642LL,
                             {&original.value()})
                : Result<VariantValue>(original.error());
            if (restored.isErr()) failures.push_back("viewport background restore failed");
        }
    }
    for (auto it = state.visibility.rbegin(); it != state.visibility.rend(); ++it) {
        auto object = api.object_get_instance_from_id(static_cast<GDObjectInstanceID>(it->instance_id));
        if (!object) {
            failures.push_back("scene object was freed");
            continue;
        }
        auto visible = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                  static_cast<GDExtensionBool>(it->visible));
        if (visible.isErr()) {
            failures.push_back("visibility value construction failed");
            continue;
        }
        auto restored = callObject(object, it->class_name.c_str(), "set_visible", 2586408642LL,
                                   {&visible.value()});
        if (restored.isErr()) failures.push_back("scene visibility restore failed");
    }
    if (!failures.empty()) {
        return Error(409, "Temporary viewport isolation could not fully restore editor state (" +
                          std::to_string(failures.size()) + " failure(s))",
                     {{"code", "isolation_restore_incomplete"}});
    }
    return Result<void>::ok();
}

Result<void> GodotBridge::forceDraw() {
    auto& api = GodotApi::instance();
    if (!api.isLiveReady()) return Error::notConnected("Godot main-loop bridge is not ready");
    NativeName name("RenderingServer");
    auto server = api.global_get_singleton(name.ptr());
    if (!server) return Error::notConnected("RenderingServer singleton is unavailable");
    auto swap_buffers = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    auto frame_step = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, 0.0);
    if (swap_buffers.isErr()) return swap_buffers.error();
    if (frame_step.isErr()) return frame_step.error();
    auto drawn = callObject(server, "RenderingServer", "force_draw", 1076185472LL,
                            {&swap_buffers.value(), &frame_step.value()});
    if (drawn.isErr()) return drawn.error();
    return Result<void>::ok();
}


} // namespace didi::godot
