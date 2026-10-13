#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/runtime/ghost_preview.hpp"
#include <map>
#include <tuple>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;
namespace bridge_internal {

// Wireframe previews of a mutation nobody has made yet.
//
// These are handed to the rendering server directly rather than added to the
// scene, which is the whole point: the scene tree, the scene dock and the file
// on disk are untouched, so looking at a proposal cannot dirty the project. The
// cost is that they are not owned by anything the editor cleans up, so every
// RID made here is remembered and freed by hand.
struct GhostBatch {
    int dimension{3};
    size_t shape_count{0};
    // Instances first, then the meshes and materials they referred to.
    std::vector<VariantValue> rids;
};

std::map<std::string, GhostBatch>& ghostBatches() {
    static std::map<std::string, GhostBatch> batches;
    return batches;
}

size_t liveGhostShapes() {
    size_t total = 0;
    for (const auto& entry : ghostBatches()) total += entry.second.shape_count;
    return total;
}

std::string makeGhostId() {
    static uint64_t counter = 0;
    return "ghost_" + std::to_string(++counter);
}

Result<GDExtensionObjectPtr> renderingServer() {
    return singleton("RenderingServer");
}

Result<void> freeGhostRids(GhostBatch& batch) {
    auto server = renderingServer();
    if (server.isErr()) return server.error();
    Result<void> outcome = Result<void>::ok();
    for (auto& rid : batch.rids) {
        auto freed = callObject(server.value(), "RenderingServer", "free_rid", 2722037293LL, {&rid});
        if (freed.isErr() && outcome.isOk()) outcome = freed.error();
    }
    batch.rids.clear();
    batch.shape_count = 0;
    return outcome;
}

// A tinted, unshaded line material. depth_draw_never keeps a preview visible
// through the geometry it is being compared against, which is what makes it
// readable as an overlay rather than something buried in the scene.
constexpr const char* kGhostShaderSource = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, depth_test_disabled;
uniform vec4 didi_tint : source_color = vec4(0.0, 1.0, 1.0, 1.0);
void fragment() {
	ALBEDO = didi_tint.rgb;
	ALPHA = didi_tint.a;
}
)";

Result<VariantValue> makeGhostMaterial(GDExtensionObjectPtr server,
                                       const runtime::GhostColor& color,
                                       std::vector<VariantValue>& owned) {
    auto shader = callObject(server, "RenderingServer", "shader_create", 529393457LL);
    if (shader.isErr()) return shader.error();
    auto code = makeString(kGhostShaderSource);
    if (code.isErr()) return code.error();
    auto coded = callObject(server, "RenderingServer", "shader_set_code", 2726140452LL,
                            {&shader.value(), &code.value()});
    if (coded.isErr()) return coded.error();
    auto material = callObject(server, "RenderingServer", "material_create", 529393457LL);
    if (material.isErr()) return material.error();
    auto attached = callObject(server, "RenderingServer", "material_set_shader", 395945892LL,
                               {&material.value(), &shader.value()});
    if (attached.isErr()) return attached.error();
    auto tint_name = makeStringName("didi_tint");
    if (tint_name.isErr()) return tint_name.error();
    auto tint = makeColor(color.red, color.green, color.blue, 1.0);
    if (tint.isErr()) return tint.error();
    auto tinted = callObject(server, "RenderingServer", "material_set_param", 3477296213LL,
                             {&material.value(), &tint_name.value(), &tint.value()});
    if (tinted.isErr()) return tinted.error();

    // The shader outlives this call as part of the material, so its RID is
    // handed back to be freed with the batch rather than dropped here.
    owned.push_back(std::move(shader.value()));
    return std::move(material.value());
}

// The twelve edges of a unit box centred on the origin, as line pairs. The
// instance transform scales and places it, so one vertex layout serves every
// preview.
Result<VariantValue> makeGhostBoxMesh(GDExtensionObjectPtr server) {
    auto& api = GodotApi::instance();
    auto vertex_constructor =
        api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY, 0);
    if (!vertex_constructor) {
        return Error::internal("Godot PackedVector3Array constructor is unavailable");
    }
    NativeValue native_vertices(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY);
    vertex_constructor(native_vertices.ptr(), nullptr);
    native_vertices.markInitialized();
    auto vertices = variantFromNative(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY,
                                      native_vertices.ptr());
    if (vertices.isErr()) return vertices.error();

    const auto corner = [](int mask, int axis) {
        return (mask & (1 << axis)) ? 0.5 : -0.5;
    };
    for (int mask = 0; mask < 8; ++mask) {
        for (int axis = 0; axis < 3; ++axis) {
            const int other = mask ^ (1 << axis);
            if (other <= mask) continue;
            for (const int end : {mask, other}) {
                auto point = makeVector3(corner(end, 0), corner(end, 1), corner(end, 2));
                if (point.isErr()) return point.error();
                auto appended = callVariant(vertices.value(), "append", {&point.value()});
                if (appended.isErr()) return appended.error();
            }
        }
    }

    auto array_constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_ARRAY, 0);
    if (!array_constructor) return Error::internal("Godot Array constructor is unavailable");
    NativeValue native_arrays(GDEXTENSION_VARIANT_TYPE_ARRAY);
    array_constructor(native_arrays.ptr(), nullptr);
    native_arrays.markInitialized();
    auto arrays = variantFromNative(GDEXTENSION_VARIANT_TYPE_ARRAY, native_arrays.ptr());
    if (arrays.isErr()) return arrays.error();
    // RenderingServer.ARRAY_MAX is 13 and ARRAY_VERTEX is 0.
    auto array_max = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(13));
    if (array_max.isErr()) return array_max.error();
    auto resized = callVariant(arrays.value(), "resize", {&array_max.value()});
    if (resized.isErr()) return resized.error();
    auto vertex_slot = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (vertex_slot.isErr()) return vertex_slot.error();
    auto stored = callVariant(arrays.value(), "set", {&vertex_slot.value(), &vertices.value()});
    if (stored.isErr()) return stored.error();

    auto mesh = callObject(server, "RenderingServer", "mesh_create", 529393457LL);
    if (mesh.isErr()) return mesh.error();
    auto lines = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (lines.isErr()) return lines.error();
    NativeValue empty_blend(GDEXTENSION_VARIANT_TYPE_ARRAY);
    array_constructor(empty_blend.ptr(), nullptr);
    empty_blend.markInitialized();
    auto blend = variantFromNative(GDEXTENSION_VARIANT_TYPE_ARRAY, empty_blend.ptr());
    if (blend.isErr()) return blend.error();
    auto dictionary_constructor =
        api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY, 0);
    if (!dictionary_constructor) return Error::internal("Godot Dictionary constructor is unavailable");
    NativeValue native_lods(GDEXTENSION_VARIANT_TYPE_DICTIONARY);
    dictionary_constructor(native_lods.ptr(), nullptr);
    native_lods.markInitialized();
    auto lods = variantFromNative(GDEXTENSION_VARIANT_TYPE_DICTIONARY, native_lods.ptr());
    if (lods.isErr()) return lods.error();
    auto format = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (format.isErr()) return format.error();
    auto surfaced = callObject(server, "RenderingServer", "mesh_add_surface_from_arrays",
                               2342446560LL,
                               {&mesh.value(), &lines.value(), &arrays.value(), &blend.value(),
                                &lods.value(), &format.value()});
    if (surfaced.isErr()) return surfaced.error();
    return std::move(mesh.value());
}

// A basis with the requested rotation applied, in Godot's YXZ order, scaled to
// the requested size.
Result<VariantValue> makeGhostTransform(const runtime::GhostShape& shape) {
    auto& api = GodotApi::instance();
    auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, 3);
    if (!constructor) return Error::internal("Godot Transform3D constructor is unavailable");
    auto to_native = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    if (!to_native) return Error::internal("Godot Vector3 conversion is unavailable");

    constexpr double kPi = 3.14159265358979323846;
    const double x = shape.rotation_degrees[0] * kPi / 180.0;
    const double y = shape.rotation_degrees[1] * kPi / 180.0;
    const double z = shape.rotation_degrees[2] * kPi / 180.0;
    const double cx = std::cos(x), sx = std::sin(x);
    const double cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    // Godot composes Euler angles as Y then X then Z.
    const double basis[3][3] = {
        {cy * cz + sy * sx * sz, cz * sy * sx - cy * sz, cx * sy},
        {cx * sz, cx * cz, -sx},
        {cy * sx * sz - sy * cz, sy * sz + cy * sx * cz, cy * cx}};

    const double scale[3] = {shape.size.x, shape.size.y, shape.size.z};
    NativeValue axes[4] = {NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3)};
    for (int column = 0; column < 3; ++column) {
        auto vector = makeVector3(basis[0][column] * scale[column],
                                  basis[1][column] * scale[column],
                                  basis[2][column] * scale[column]);
        if (vector.isErr()) return vector.error();
        to_native(axes[column].ptr(), vector.value().ptr());
        axes[column].markInitialized();
    }
    auto origin = makeVector3(shape.position.x, shape.position.y, shape.position.z);
    if (origin.isErr()) return origin.error();
    to_native(axes[3].ptr(), origin.value().ptr());
    axes[3].markInitialized();

    NativeValue native(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D);
    const void* arguments[] = {axes[0].ptr(), axes[1].ptr(), axes[2].ptr(), axes[3].ptr()};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, native.ptr());
}

Result<VariantValue> editedSceneScenario() {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    auto is_spatial = objectIsClass(root.value(), "Node3D");
    if (is_spatial.isErr()) return is_spatial.error();
    if (!is_spatial.value()) {
        return Error(409, "The edited scene has no 3D world to draw a preview in; its root is not a Node3D",
                     {{"code", "preview_dimension_mismatch"}});
    }
    auto world = callObject(root.value(), "Node3D", "get_world_3d", 317588385LL);
    if (world.isErr()) return world.error();
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr() || !world_object.value()) {
        return Error(409, "The edited scene is not in a viewport with a 3D world", {{"code", "no_world"}});
    }
    return callObject(world_object.value(), "World3D", "get_scenario", 2944877500LL);
}

Result<VariantValue> editedSceneCanvas() {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    auto is_canvas = objectIsClass(root.value(), "CanvasItem");
    if (is_canvas.isErr()) return is_canvas.error();
    if (!is_canvas.value()) {
        return Error(409, "The edited scene has no 2D canvas to draw a preview in; its root is not a CanvasItem",
                     {{"code", "preview_dimension_mismatch"}});
    }
    return callObject(root.value(), "CanvasItem", "get_canvas", 2944877500LL);
}

json ghostPreviewRender(const json& params) {
    auto parsed = runtime::parseGhostPreviewRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();

    for (const auto& bind : {std::make_tuple("RenderingServer", "free_rid", 2722037293LL),
                             std::make_tuple("RenderingServer", "shader_create", 529393457LL),
                             std::make_tuple("RenderingServer", "shader_set_code", 2726140452LL),
                             std::make_tuple("RenderingServer", "material_create", 529393457LL),
                             std::make_tuple("RenderingServer", "material_set_shader", 395945892LL),
                             std::make_tuple("RenderingServer", "material_set_param", 3477296213LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }
    const bool flat = request.dimension() == 2;
    for (const auto& bind : flat
             ? std::vector<std::tuple<const char*, const char*, int64_t>>{
                   {"RenderingServer", "canvas_item_create", 529393457LL},
                   {"RenderingServer", "canvas_item_set_parent", 395945892LL},
                   {"RenderingServer", "canvas_item_add_line", 1819681853LL},
                   {"RenderingServer", "canvas_item_set_z_index", 3411492887LL}}
             : std::vector<std::tuple<const char*, const char*, int64_t>>{
                   {"RenderingServer", "mesh_create", 529393457LL},
                   {"RenderingServer", "mesh_add_surface_from_arrays", 2342446560LL},
                   {"RenderingServer", "instance_create2", 746547085LL},
                   {"RenderingServer", "instance_set_transform", 3935195649LL},
                   {"RenderingServer", "instance_geometry_set_material_override", 395945892LL}}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    auto server = renderingServer();
    if (server.isErr()) return errorJson(server.error());

    // The world the shapes go in, resolved before anything already on screen is
    // torn down.
    //
    // A 2D preview asked for on a Node3D root is refused by the engine, and the
    // refusal used to arrive after the replace had already freed the preview the
    // caller could see. It reported 409, which reads as "nothing happened", and
    // said nothing about what it had destroyed: the agent's own bookkeeping
    // still said the proposal was up, and the next clear answered
    // cleared_previews: 0, which is what "nothing was there" looks like (#707).
    // The argument validation in front of this is why a malformed call was
    // always safe and this one was not.
    Result<VariantValue> world = flat ? editedSceneCanvas() : editedSceneScenario();
    if (world.isErr()) return errorJson(world.error());

    size_t cleared_previews = 0;
    size_t cleared_shapes = 0;
    if (request.replace) {
        for (auto& entry : ghostBatches()) {
            cleared_shapes += entry.second.shape_count;
            ++cleared_previews;
            (void)freeGhostRids(entry.second);
        }
        ghostBatches().clear();
    }
    // What a refusal after the teardown has to say. Everything reachable from
    // here has already changed the viewport, so a bare code would leave the
    // caller believing a proposal is on screen that is not.
    const auto lost = [&](json data) {
        if (cleared_previews > 0) {
            data["cleared_previews"] = static_cast<int64_t>(cleared_previews);
            data["cleared_shapes"] = static_cast<int64_t>(cleared_shapes);
            data["previews_were_replaced"] = true;
        }
        return data;
    };
    if (liveGhostShapes() + request.shapes.size() > runtime::kMaxLiveGhostShapes) {
        return errorJson(409, "This would leave more than " +
                                  std::to_string(runtime::kMaxLiveGhostShapes) +
                                  " preview shapes on screen; clear some first",
                         lost({{"code", "ghost_budget_exceeded"}}));
    }

    GhostBatch batch;
    batch.dimension = request.dimension();
    // Anything already made is freed if a later shape fails, so a refused call
    // does not leave half a proposal drawn over the scene.
    const auto abandon = [&](const Error& error) {
        (void)freeGhostRids(batch);
        return errorJson(error.code, error.message, lost(json::object()));
    };

    if (flat) {
        auto& canvas = world;
        for (const auto& shape : request.shapes) {
            auto item = callObject(server.value(), "RenderingServer", "canvas_item_create",
                                   529393457LL);
            if (item.isErr()) return abandon(item.error());
            auto parented = callObject(server.value(), "RenderingServer", "canvas_item_set_parent",
                                       395945892LL, {&item.value(), &canvas.value()});
            if (parented.isErr()) return abandon(parented.error());
            auto z_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(4096));
            if (z_index.isErr()) return abandon(z_index.error());
            auto layered = callObject(server.value(), "RenderingServer", "canvas_item_set_z_index",
                                      3411492887LL, {&item.value(), &z_index.value()});
            if (layered.isErr()) return abandon(layered.error());

            const double half_x = shape.size.x / 2.0;
            const double half_y = shape.size.y / 2.0;
            const double corners[4][2] = {{shape.position.x - half_x, shape.position.y - half_y},
                                          {shape.position.x + half_x, shape.position.y - half_y},
                                          {shape.position.x + half_x, shape.position.y + half_y},
                                          {shape.position.x - half_x, shape.position.y + half_y}};
            auto tint = makeColor(shape.color.red, shape.color.green, shape.color.blue, 1.0);
            if (tint.isErr()) return abandon(tint.error());
            for (int edge = 0; edge < 4; ++edge) {
                const int next = (edge + 1) % 4;
                auto from = makeVector2(corners[edge][0], corners[edge][1]);
                auto to = makeVector2(corners[next][0], corners[next][1]);
                if (from.isErr()) return abandon(from.error());
                if (to.isErr()) return abandon(to.error());
                auto width = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, 1.0);
                auto antialiased = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                              static_cast<GDExtensionBool>(0));
                if (width.isErr()) return abandon(width.error());
                if (antialiased.isErr()) return abandon(antialiased.error());
                auto drawn = callObject(server.value(), "RenderingServer", "canvas_item_add_line",
                                        1819681853LL,
                                        {&item.value(), &from.value(), &to.value(), &tint.value(),
                                         &width.value(), &antialiased.value()});
                if (drawn.isErr()) return abandon(drawn.error());
            }
            batch.rids.push_back(std::move(item.value()));
            ++batch.shape_count;
        }
    } else {
        auto& scenario = world;
        for (const auto& shape : request.shapes) {
            auto mesh = makeGhostBoxMesh(server.value());
            if (mesh.isErr()) return abandon(mesh.error());
            std::vector<VariantValue> owned;
            auto material = makeGhostMaterial(server.value(), shape.color, owned);
            if (material.isErr()) return abandon(material.error());
            auto instance = callObject(server.value(), "RenderingServer", "instance_create2",
                                       746547085LL, {&mesh.value(), &scenario.value()});
            if (instance.isErr()) return abandon(instance.error());
            auto overridden = callObject(server.value(), "RenderingServer",
                                         "instance_geometry_set_material_override", 395945892LL,
                                         {&instance.value(), &material.value()});
            if (overridden.isErr()) return abandon(overridden.error());
            auto transform = makeGhostTransform(shape);
            if (transform.isErr()) return abandon(transform.error());
            auto placed = callObject(server.value(), "RenderingServer", "instance_set_transform",
                                     3935195649LL, {&instance.value(), &transform.value()});
            if (placed.isErr()) return abandon(placed.error());

            // Instance first: freeing a mesh an instance still points at is the
            // order that leaves a dangling reference.
            batch.rids.push_back(std::move(instance.value()));
            batch.rids.push_back(std::move(mesh.value()));
            batch.rids.push_back(std::move(material.value()));
            for (auto& extra : owned) batch.rids.push_back(std::move(extra));
            ++batch.shape_count;
        }
    }

    const std::string id = makeGhostId();
    json drawn = json::array();
    for (size_t index = 0; index < request.shapes.size(); ++index) {
        const auto& shape = request.shapes[index];
        json entry = {{"index", static_cast<int64_t>(index)},
                      {"kind", runtime::nameForGhostKind(shape.kind)},
                      {"color", shape.color.toJson()},
                      {"color_defaulted", !shape.color_given}};
        if (!shape.label.empty()) entry["label"] = boundUtf8(shape.label, 256).value;
        drawn.push_back(std::move(entry));
    }
    const size_t shapes = batch.shape_count;
    ghostBatches().emplace(id, std::move(batch));
    // The viewport redraws when something it owns changes, and these shapes are
    // deliberately not among them. Without this the proposal only appears on
    // whatever frame the editor drew next for its own reasons.
    (void)GodotBridge::instance().forceDraw();

    return liveResult({{"preview_id", id},
                       {"dimension", request.dimension()},
                       {"drawn", static_cast<int64_t>(shapes)},
                       {"previews", std::move(drawn)},
                       {"replaced_shapes", static_cast<int64_t>(cleared_shapes)},
                       {"live_shapes", static_cast<int64_t>(liveGhostShapes())},
                       // Nothing here touched the scene, so there is nothing to
                       // undo and nothing to save.
                       {"scene_modified", false}});
}

json ghostPreviewClear(const json& params) {
    auto parsed = runtime::parseGhostClearRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    auto required = requireMethodBind("RenderingServer", "free_rid", 2722037293LL);
    if (required.isErr()) return errorJson(501, required.error().message);

    size_t batches = 0;
    size_t shapes = 0;
    Result<void> outcome = Result<void>::ok();
    if (parsed.value().preview_id.empty()) {
        for (auto& entry : ghostBatches()) {
            ++batches;
            shapes += entry.second.shape_count;
            auto freed = freeGhostRids(entry.second);
            if (freed.isErr() && outcome.isOk()) outcome = freed.error();
        }
        ghostBatches().clear();
    } else {
        auto found = ghostBatches().find(parsed.value().preview_id);
        if (found == ghostBatches().end()) {
            return errorJson(404, "No preview with id " + parsed.value().preview_id);
        }
        batches = 1;
        shapes = found->second.shape_count;
        outcome = freeGhostRids(found->second);
        ghostBatches().erase(found);
    }
    if (outcome.isErr()) return errorJson(outcome.error());
    // Same reason as drawing them: freeing the shapes does not by itself put a
    // frame on screen without them.
    (void)GodotBridge::instance().forceDraw();
    return liveResult({{"cleared_previews", static_cast<int64_t>(batches)},
                       {"cleared_shapes", static_cast<int64_t>(shapes)},
                       {"live_shapes", static_cast<int64_t>(liveGhostShapes())},
                       {"scene_modified", false}});
}

} // namespace bridge_internal
} // namespace didi::godot
