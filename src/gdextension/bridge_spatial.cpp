#include "bridge_internal.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/common/json_bounds.hpp"
#include "didi/common/config_file_syntax.hpp"
#include "didi/runtime/spatial_queries.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <tuple>

namespace didi::godot::bridge_internal {
namespace {

Result<VariantValue> makePoint(const runtime::SpatialPoint& point) {
    return point.dimension == 2 ? makeVector2(point.x, point.y)
                                : makeVector3(point.x, point.y, point.z);
}


// The world a spatial query asks its question of.
//
// In a game the running scene is under the root viewport, so the root
// viewport's world is the one holding the colliders. In an editor it is not:
// the edited scene is parented into a SubViewport inside the editor's own
// docks, and a Viewport carries its own World2D, so the root viewport's World2D
// is a different, empty space. A 2D ray asked of it passed through the level
// and reported a clean miss, indistinguishable from empty space (#743). The 3D
// case only looked healthy by accident -- a SubViewport has no World3D of its
// own, so it inherits the root's, which is where the edited scene's 3D bodies
// register.
//
// Viewport.find_world_2d and find_world_3d are the engine's own answer to
// "which world does this viewport actually use": each walks from the viewport
// up its ancestors to the first valid one. Asked of the SubViewport the edited
// scene sits in, that is the scene's own World2D and the shared World3D, which
// is right in both dimensions and on 4.5.1, 4.6.2 and 4.7.2, where both binds
// carry the same hash.
// The editor keeps its physics servers inactive, so they never step, and a
// body that joins a space before it has shapes keeps them on a list only a
// step applies. A TileMapLayer's tiles and a GridMap's cells are built that
// way, so a ray in the editor passed through a wall the game collided with
// (#1308); a StaticBody2D gets its shapes first and was always found.
// body_test_motion applies that list before it tests, and steps nothing, so a
// throwaway body's test motion makes them queryable. Measured on 4.5.1, 4.6.2
// and 4.7.2 in 2D and 3D. Best effort: a failure leaves the query as it was.
// World.get_space is 2944877500, body_create 529393457, body_set_space
// 395945892 and free_rid 2722037293 on both servers; body_test_motion is
// 1699844009 in 2D and 1944921792 in 3D, on all three lines.
void applyPendingPhysicsShapes(int dimension, GDExtensionObjectPtr world) {
    const char* server_class = dimension == 2 ? "PhysicsServer2D" : "PhysicsServer3D";
    const char* world_class = dimension == 2 ? "World2D" : "World3D";
    const char* params_class =
        dimension == 2 ? "PhysicsTestMotionParameters2D" : "PhysicsTestMotionParameters3D";
    // long long, as the literals beside it in the bind list are: int64_t is
    // long on Linux, and one initializer list cannot hold both tuples.
    const long long test_hash = dimension == 2 ? 1699844009LL : 1944921792LL;
    for (const auto& bind : {std::make_tuple(world_class, "get_space", 2944877500LL),
                             std::make_tuple(server_class, "body_create", 529393457LL),
                             std::make_tuple(server_class, "body_set_space", 395945892LL),
                             std::make_tuple(server_class, "body_test_motion", test_hash),
                             std::make_tuple(server_class, "free_rid", 2722037293LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) return;
    }
    auto server = singleton(server_class);
    if (server.isErr()) return;
    auto space = callObject(world, world_class, "get_space", 2944877500LL);
    if (space.isErr()) return;
    NativeName params_name(params_class);
    if (!params_name.valid()) return;
    auto params = constructObject(params_name.ptr());
    if (!params) return;
    // The Variant holds the parameters' only reference and frees them.
    auto params_value = makeObject(params);
    if (params_value.isErr()) {
        GodotApi::instance().object_destroy(params);
        return;
    }
    auto body = callObject(server.value(), server_class, "body_create", 529393457LL);
    if (body.isErr()) return;
    auto placed = callObject(server.value(), server_class, "body_set_space", 395945892LL,
                             {&body.value(), &space.value()});
    if (placed.isOk()) {
        (void)callObject(server.value(), server_class, "body_test_motion", test_hash,
                         {&body.value(), &params_value.value()});
    }
    (void)callObject(server.value(), server_class, "free_rid", 2722037293LL, {&body.value()});
}

Result<VariantValue> queriedWorld(int dimension, const std::string& session_kind) {
    const char* method = dimension == 2 ? "find_world_2d" : "find_world_3d";
    const int64_t hash = dimension == 2 ? 2339128592LL : 317588385LL;
    Result<GDExtensionObjectPtr> viewport = Error::internal("unresolved");
    if (session_kind == "editor") {
        auto editor = editorInterface();
        if (editor.isErr()) return editor.error();
        auto root = editedSceneRoot(editor.value());
        if (root.isErr()) return root.error();
        auto owning = callObject(root.value(), "Node", "get_viewport", 3596683776LL);
        if (owning.isErr()) return owning.error();
        auto object = objectFromVariant(owning.value());
        if (object.isErr() || !object.value()) {
            return Error(409, "The edited scene is not inside a viewport", {{"code", "no_world"}});
        }
        viewport = object.value();
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return tree.error();
        viewport = liveSceneTreeRoot(tree.value());
    }
    if (viewport.isErr()) return viewport.error();
    auto world = callObject(viewport.value(), "Viewport", method, hash);
    if (world.isErr()) return world.error();
    auto object = objectFromVariant(world.value());
    if (object.isErr() || !object.value()) {
        return Error(409, dimension == 2 ? "The queried viewport has no World2D"
                                         : "The queried viewport has no World3D",
                     {{"code", "no_world"}});
    }
    // A game's servers step, and apply their own list every physics frame.
    if (session_kind == "editor") applyPendingPhysicsShapes(dimension, object.value());
    return std::move(world.value());
}

// The collider's path in the form the rest of the surface takes.
//
// Node.get_path answers absolutely, and in an editor that path runs through the
// editor's own dock tree and its volatile instance ids -- 370 characters that
// no reader or writer on the surface accepts (#742). logicalPathFromEditedRoot
// is the same reduction every other editor answer uses. A collider the edited
// scene does not own cannot be reduced, and that is reported as a null path
// rather than as a path a caller would then fail to use.
Result<std::string> colliderLogicalPath(GDExtensionObjectPtr collider,
                                        const std::string& session_kind) {
    if (session_kind != "editor") {
        return nodeString(collider, "get_path", 4075236667LL);
    }
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    return logicalPathFromEditedRoot(root.value(), collider);
}

// The binds and the space state a ray needs, resolved once. A batch pays for
// this lookup once rather than once per ray, and every ray in the batch is then
// answered against the same physics state instead of against N successive ones.
// The direct space state of the root viewport's world, which every physics
// query in this file asks its question of. Shared so a ray and a shape sweep
// cannot end up reading two different worlds.
Result<GDExtensionObjectPtr> openDirectSpaceState(int dimension, const std::string& session_kind) {
    const int64_t state_hash = dimension == 2 ? 2506717822LL : 2069328350LL;
    auto required = requireMethodBind(dimension == 2 ? "World2D" : "World3D",
                                      "get_direct_space_state", state_hash);
    if (required.isErr()) return Error(501, required.error().message);
    auto world = queriedWorld(dimension, session_kind);
    if (world.isErr()) return world.error();
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr()) return Error::internal(world_object.error().message);
    auto state = callObject(world_object.value(), dimension == 2 ? "World2D" : "World3D",
                            "get_direct_space_state", state_hash);
    if (state.isErr()) return Error::internal(state.error().message);
    auto state_object = objectFromVariant(state.value());
    if (state_object.isErr() || !state_object.value()) {
        return Error(409, "The queried world has no direct space state", {{"code", "no_space_state"}});
    }
    return state_object.value();
}

struct RaycastSpace {
    int dimension{3};
    const char* params_class{nullptr};
    const char* state_class{nullptr};
    int64_t create_hash{0};
    int64_t ray_hash{0};
    GDExtensionObjectPtr state{nullptr};
};

Result<RaycastSpace> openRaycastSpace(int dimension, const std::string& session_kind) {
    RaycastSpace space;
    space.dimension = dimension;
    space.params_class = dimension == 2 ? "PhysicsRayQueryParameters2D" : "PhysicsRayQueryParameters3D";
    space.state_class = dimension == 2 ? "PhysicsDirectSpaceState2D" : "PhysicsDirectSpaceState3D";
    space.create_hash = dimension == 2 ? 3196569324LL : 3110599579LL;
    space.ray_hash = dimension == 2 ? 1590275562LL : 3957970750LL;
    const int64_t state_hash = dimension == 2 ? 2506717822LL : 2069328350LL;

    for (const auto& bind : {std::make_tuple(space.params_class, "create", space.create_hash),
                             std::make_tuple(space.state_class, "intersect_ray", space.ray_hash),
                             std::make_tuple(dimension == 2 ? "World2D" : "World3D",
                                             "get_direct_space_state", state_hash)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }

    auto state = openDirectSpaceState(dimension, session_kind);
    if (state.isErr()) return state.error();
    space.state = state.value();
    return space;
}

// One ray against an already-open space. The record is the same one
// physics_raycast_query returns, so a batch entry and a single call cannot
// describe the same hit differently.
// The collider's collision_layer, or null when it is not a CollisionObject.
// A TileMapLayer, a GridMap and a CSG shape collide without being one, and
// calling CollisionObject's getter on them read whatever sat at that offset:
// a TileMapLayer answered the bits of a float (#1290).
json colliderCollisionLayer(GDExtensionObjectPtr collider, int dimension) {
    const char* collision_class = dimension == 2 ? "CollisionObject2D" : "CollisionObject3D";
    auto class_name = makeString(collision_class);
    if (class_name.isErr()) return nullptr;
    auto is_collision = callObject(collider, "Object", "is_class", 3927539163LL, {&class_name.value()});
    if (is_collision.isErr()) return nullptr;
    auto flag = scalarFromVariant<GDExtensionBool>(is_collision.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (flag.isErr() || !flag.value()) return nullptr;
    auto layer = callObject(collider, collision_class, "get_collision_layer", 3905245786LL);
    if (layer.isErr()) return nullptr;
    auto layer_value = scalarFromVariant<int64_t>(layer.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (layer_value.isErr()) return nullptr;
    return layer_value.value();
}

Result<json> castOneRay(const RaycastSpace& space, const runtime::RaycastRequest& request,
                        const std::string& session_kind) {
    const int dimension = space.dimension;
    const char* params_class = space.params_class;
    auto from = makePoint(request.from);
    auto to = makePoint(request.to);
    auto mask = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.collision_mask);
    if (from.isErr() || to.isErr() || mask.isErr()) {
        return Error::internal("Failed to construct ray arguments");
    }
    Result<VariantValue> query = [&]() -> Result<VariantValue> {
        auto& api = GodotApi::instance();
        NativeName klass(params_class);
        NativeName method("create");
        if (!klass.valid() || !method.valid()) return Error::internal("Failed to construct ray query identifiers");
        auto bind = api.classdb_get_method_bind(klass.ptr(), method.ptr(), space.create_hash);
        if (!bind) return Error(501, "Godot method binding unavailable for ray query creation");
        const void* raw_args[] = {from.value().ptr(), to.value().ptr(), mask.value().ptr()};
        VariantValue created(VariantValue::Uninitialized{});
        GDExtensionCallError error{};
        api.object_method_bind_call(bind, nullptr, raw_args, 3, created.ptr(), &error);
        created.markInitialized();
        if (error.error != GDEXTENSION_CALL_OK) {
            return Error::internal("PhysicsRayQueryParameters.create failed (call error " +
                                   std::to_string(error.error) + ")");
        }
        return std::move(created);
    }();
    if (query.isErr()) return query.error();
    auto query_object = objectFromVariant(query.value());
    if (query_object.isErr() || !query_object.value()) {
        return Error::internal("Ray query parameters were not created");
    }
    auto set_flag = [&](const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(query_object.value(), params_class, method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    for (const auto& flag : {std::make_pair("set_collide_with_bodies", true),
                             std::make_pair("set_collide_with_areas", true),
                             std::make_pair("set_hit_from_inside", false)}) {
        auto set = set_flag(flag.first, flag.second);
        if (set.isErr()) return Error::internal(set.error().message);
    }
    if (dimension == 3) {
        auto set = set_flag("set_hit_back_faces", true);
        if (set.isErr()) return Error::internal(set.error().message);
    }

    auto hit = callObject(space.state, space.state_class, "intersect_ray", space.ray_hash,
                          {&query.value()});
    if (hit.isErr()) return Error::internal(hit.error().message);
    json result = {{"dimension", dimension}, {"hit", false}, {"collider_path", nullptr},
                   {"collider_class", nullptr}, {"position", nullptr}, {"normal", nullptr},
                   {"collision_layer", nullptr}};
    auto size = callVariant(hit.value(), "size");
    if (size.isErr()) return Error::internal(size.error().message);
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr() || count.value() == 0) return result;

    auto lookup = [&](const char* key) -> Result<VariantValue> {
        auto key_value = makeString(key);
        if (key_value.isErr()) return key_value.error();
        return callVariant(hit.value(), "get", {&key_value.value()});
    };
    auto position = lookup("position");
    auto normal = lookup("normal");
    auto collider = lookup("collider");
    if (position.isErr() || normal.isErr() || collider.isErr()) {
        return Error::internal("Ray hit dictionary is incomplete");
    }
    auto position_json = pointVariantToJson(position.value(), dimension);
    auto normal_json = pointVariantToJson(normal.value(), dimension);
    if (position_json.isErr() || normal_json.isErr()) {
        return Error::internal("Ray hit vectors could not be read");
    }
    result["hit"] = true;
    result["position"] = position_json.value();
    result["normal"] = normal_json.value();
    auto collider_object = objectFromVariant(collider.value());
    if (collider_object.isOk() && collider_object.value()) {
        auto class_name = nodeString(collider_object.value(), "get_class", 201670096LL);
        if (class_name.isOk()) {
            auto bounded = boundUtf8(class_name.value(), 256);
            result["collider_class"] = bounded.value;
        }
        auto is_node_name = makeString("Node");
        if (is_node_name.isOk()) {
            auto is_node = callObject(collider_object.value(), "Object", "is_class", 3927539163LL, {&is_node_name.value()});
            if (is_node.isOk()) {
                auto node_flag = scalarFromVariant<GDExtensionBool>(is_node.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (node_flag.isOk() && node_flag.value()) {
                    auto path = colliderLogicalPath(collider_object.value(), session_kind);
                    if (path.isOk() && !path.value().empty() && path.value().size() <= 1024) {
                        result["collider_path"] = path.value();
                    }
                }
            }
        }
        result["collision_layer"] = colliderCollisionLayer(collider_object.value(), dimension);
    }
    return result;
}

// A transform with no rotation at a point. A sweep asks where a shape fits, not
// which way it faces, so the basis is the identity and only the origin varies.
Result<VariantValue> makeUprightTransform(const runtime::SpatialPoint& origin) {
    auto& api = GodotApi::instance();
    if (origin.dimension == 2) {
        auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_TRANSFORM2D, 2);
        if (!constructor) return Error::internal("Godot Transform2D constructor is unavailable");
        double rotation = 0.0;
        auto position = makeVector2(origin.x, origin.y);
        if (position.isErr()) return position.error();
        NativeValue native_position(GDEXTENSION_VARIANT_TYPE_VECTOR2);
        auto to_native = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR2);
        if (!to_native) return Error::internal("Godot Vector2 conversion is unavailable");
        to_native(native_position.ptr(), position.value().ptr());
        native_position.markInitialized();
        NativeValue native(GDEXTENSION_VARIANT_TYPE_TRANSFORM2D);
        const void* arguments[] = {&rotation, native_position.ptr()};
        constructor(native.ptr(), arguments);
        native.markInitialized();
        return variantFromNative(GDEXTENSION_VARIANT_TYPE_TRANSFORM2D, native.ptr());
    }
    auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, 3);
    if (!constructor) return Error::internal("Godot Transform3D constructor is unavailable");
    auto to_native = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    if (!to_native) return Error::internal("Godot Vector3 conversion is unavailable");
    NativeValue axes[4] = {NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3)};
    const double components[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                     {origin.x, origin.y, origin.z}};
    for (int index = 0; index < 4; ++index) {
        auto vector = makeVector3(components[index][0], components[index][1], components[index][2]);
        if (vector.isErr()) return vector.error();
        to_native(axes[index].ptr(), vector.value().ptr());
        axes[index].markInitialized();
    }
    NativeValue native(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D);
    const void* arguments[] = {axes[0].ptr(), axes[1].ptr(), axes[2].ptr(), axes[3].ptr()};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, native.ptr());
}

// The shape the caller described, built through ClassDB the same way every
// other object in this file is.
constexpr int64_t kSetFloatHash = 373806689LL;

Result<GDExtensionObjectPtr> makeClearanceShape(const runtime::ClearanceRequest& request) {
    const int dimension = request.dimension();
    const char* class_name = nullptr;
    switch (request.shape) {
        case runtime::ClearanceShapeKind::box:
            class_name = dimension == 2 ? "RectangleShape2D" : "BoxShape3D";
            break;
        case runtime::ClearanceShapeKind::sphere:
            class_name = dimension == 2 ? "CircleShape2D" : "SphereShape3D";
            break;
        case runtime::ClearanceShapeKind::capsule:
            class_name = dimension == 2 ? "CapsuleShape2D" : "CapsuleShape3D";
            break;
    }
    NativeName type_name(class_name);
    auto shape = constructObject(type_name.ptr());
    if (!shape) return Error(501, std::string("Godot ClassDB could not construct ") + class_name);

    // void(float), which is what set_radius and set_height both are.
    const auto set_number = [&](const char* method, double value) -> Result<void> {
        auto argument = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, value);
        if (argument.isErr()) return argument.error();
        auto called = callObject(shape, class_name, method, kSetFloatHash, {&argument.value()});
        return called.isOk() ? Result<void>::ok() : Result<void>(called.error());
    };

    if (request.shape == runtime::ClearanceShapeKind::box) {
        auto size = dimension == 2 ? makeVector2(request.size.x, request.size.y)
                                   : makeVector3(request.size.x, request.size.y, request.size.z);
        if (size.isErr()) return size.error();
        // The same numbers as set_motion above, and not a copied constant. A
        // Godot method bind hash is computed from the signature and not the
        // name, so every void(Vector3) setter shares one, which is why
        // BoxShape3D.set_size and PhysicsShapeQueryParameters3D.set_motion
        // agree. Verified against the shipped extension_api dump.
        const int64_t hash = dimension == 2 ? 743155724LL : 3460891852LL;
        auto called = callObject(shape, class_name, "set_size", hash, {&size.value()});
        if (called.isErr()) return called.error();
    } else {
        auto radius = set_number("set_radius", request.radius);
        if (radius.isErr()) return radius.error();
        if (request.shape == runtime::ClearanceShapeKind::capsule) {
            auto height = set_number("set_height", request.height);
            if (height.isErr()) return height.error();
        }
    }
    return shape;
}

// Named so a hash appears once and carries the same type wherever it is used.
constexpr int64_t kSetShapeHash = 968641751LL;
constexpr int64_t kSetCollisionMaskHash = 1286410249LL;
constexpr int64_t kSetBoolHash = 2586408642LL;


} // namespace

json physicsClearance(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseClearanceRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();
    const int dimension = request.dimension();
    const char* params_class = dimension == 2 ? "PhysicsShapeQueryParameters2D"
                                              : "PhysicsShapeQueryParameters3D";
    const char* state_class = dimension == 2 ? "PhysicsDirectSpaceState2D"
                                             : "PhysicsDirectSpaceState3D";
    const int64_t cast_hash = dimension == 2 ? 711275086LL : 1778757334LL;
    const int64_t transform_hash = dimension == 2 ? 2761652528LL : 2952846383LL;
    const int64_t motion_hash = dimension == 2 ? 743155724LL : 3460891852LL;

    // Spelled out rather than deduced. int64_t is long on Linux and long long on
    // Windows, so a list mixing a hash variable with an LL literal has no common
    // type on gcc and only fails there.
    using ShapeBind = std::tuple<const char*, const char*, int64_t>;
    for (const ShapeBind& bind : {
             ShapeBind{state_class, "cast_motion", cast_hash},
             ShapeBind{params_class, "set_shape", kSetShapeHash},
             ShapeBind{params_class, "set_transform", transform_hash},
             ShapeBind{params_class, "set_motion", motion_hash},
             ShapeBind{params_class, "set_collision_mask", kSetCollisionMaskHash},
             ShapeBind{params_class, "set_collide_with_bodies", kSetBoolHash},
             ShapeBind{params_class, "set_collide_with_areas", kSetBoolHash}}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    auto state = openDirectSpaceState(dimension, session_kind);
    if (state.isErr()) return errorJson(state.error());

    auto shape = makeClearanceShape(request);
    if (shape.isErr()) return errorJson(shape.error());
    auto shape_value = makeObject(shape.value());
    if (shape_value.isErr()) return errorJson(500, shape_value.error().message);

    NativeName query_name(params_class);
    auto query = constructObject(query_name.ptr());
    if (!query) return errorJson(501, std::string("Godot ClassDB could not construct ") + params_class);
    auto query_value = makeObject(query);
    if (query_value.isErr()) return errorJson(500, query_value.error().message);

    auto transform = makeUprightTransform(request.from);
    if (transform.isErr()) return errorJson(transform.error());
    runtime::SpatialPoint motion_point = request.to;
    motion_point.x -= request.from.x;
    motion_point.y -= request.from.y;
    motion_point.z -= request.from.z;
    auto motion = makePoint(motion_point);
    if (motion.isErr()) return errorJson(500, motion.error().message);
    auto mask = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.collision_mask);
    if (mask.isErr()) return errorJson(500, mask.error().message);

    using ShapeSetter = std::tuple<const char*, int64_t, const VariantValue*>;
    for (const ShapeSetter& call : {
             ShapeSetter{"set_shape", kSetShapeHash, &shape_value.value()},
             ShapeSetter{"set_transform", transform_hash, &transform.value()},
             ShapeSetter{"set_motion", motion_hash, &motion.value()},
             ShapeSetter{"set_collision_mask", kSetCollisionMaskHash, &mask.value()}}) {
        auto called = callObject(query, params_class, std::get<0>(call), std::get<1>(call),
                                 {std::get<2>(call)});
        if (called.isErr()) return errorJson(500, called.error().message);
    }
    // Bodies block a body; an area does not. A trigger volume is not geometry,
    // and a corridor that reports blocked because a checkpoint sits in it is
    // answering a different question from the one asked. This differs from the
    // raycast on purpose, and the tool reference says so.
    for (const auto& flag : {std::make_pair("set_collide_with_bodies", true),
                             std::make_pair("set_collide_with_areas", false)}) {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                static_cast<GDExtensionBool>(flag.second));
        if (value.isErr()) return errorJson(500, value.error().message);
        auto called = callObject(query, params_class, flag.first, kSetBoolHash, {&value.value()});
        if (called.isErr()) return errorJson(500, called.error().message);
    }

    auto swept = callObject(state.value(), state_class, "cast_motion", cast_hash, {&query_value.value()});
    if (swept.isErr()) return errorJson(500, swept.error().message);
    auto size_value = callVariant(swept.value(), "size");
    if (size_value.isErr()) return errorJson(500, size_value.error().message);
    auto count = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr() || count.value() < 2) {
        return errorJson(500, "cast_motion did not return a safe and an unsafe fraction");
    }
    const auto fraction = [&](int64_t index) -> Result<double> {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto item = callVariant(swept.value(), "get", {&index_value.value()});
        if (item.isErr()) return item.error();
        return scalarFromVariant<double>(item.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
    };
    auto safe = fraction(0);
    auto unsafe = fraction(1);
    if (safe.isErr() || unsafe.isErr()) return errorJson(500, "cast_motion fractions could not be read");

    // Reported as the engine gave them, plus the arithmetic that turns a
    // fraction into the point a caller can act on. Nothing here interprets what
    // a particular pair of fractions means.
    runtime::SpatialPoint reached = request.from;
    reached.x += motion_point.x * safe.value();
    reached.y += motion_point.y * safe.value();
    if (dimension == 3) reached.z += motion_point.z * safe.value();

    return liveResult({{"dimension", dimension},
                       {"clear", safe.value() >= 1.0},
                       {"safe_fraction", safe.value()},
                       {"unsafe_fraction", unsafe.value()},
                       {"safe_position", reached.toJson()},
                       {"collide_with_areas", false}});
}

namespace {

// A frustum reduced to the six half-spaces every point in this file is tested
// against. Both request forms end here, so a node one form calls visible is
// never a node the other calls hidden.
//
// The frustum is stored as an origin and three unit axes rather than as a
// matrix, because that is what a half-space test actually reads and because a
// basis assembled once cannot drift from the one the response reports.
struct FrustumBasis {
    double origin[3]{0.0, 0.0, 0.0};
    double forward[3]{0.0, 0.0, -1.0};
    double up[3]{0.0, 1.0, 0.0};
    double right[3]{1.0, 0.0, 0.0};
    double near_plane{0.05};
    double far_plane{100.0};
    // Perspective: half extents at unit depth. Orthogonal: half extents flat.
    double half_x{1.0};
    double half_y{1.0};
    bool orthogonal{false};
    double aspect{1.0};
    // Reported back so the caller can see the frustum that answered, rather
    // than the one it believes it asked for.
    double fov_degrees{0.0};
    double ortho_size{0.0};
    std::string aspect_source{"parameters"};
};

void frustumAxis(double* out, double x, double y, double z) {
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length < 1e-12) {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 0.0;
        return;
    }
    out[0] = x / length;
    out[1] = y / length;
    out[2] = z / length;
}

void frustumCross(double* out, const double* a, const double* b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

double frustumDot(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

json frustumAxisJson(const double* axis) {
    return json{{"x", axis[0]}, {"y", axis[1]}, {"z", axis[2]}};
}

// Right and up are rebuilt from forward so a caller-supplied up that is merely
// near the view direction still yields an orthogonal basis.
Result<void> orientFrustum(FrustumBasis& basis, double fx, double fy, double fz,
                           double ux, double uy, double uz) {
    frustumAxis(basis.forward, fx, fy, fz);
    double raw_up[3];
    frustumAxis(raw_up, ux, uy, uz);
    if (frustumDot(basis.forward, basis.forward) < 0.5 || frustumDot(raw_up, raw_up) < 0.5) {
        return Error(409, "The camera basis has no direction to look along", {{"code", "camera_frustum_undefined"}});
    }
    frustumCross(basis.right, basis.forward, raw_up);
    frustumAxis(basis.right, basis.right[0], basis.right[1], basis.right[2]);
    if (frustumDot(basis.right, basis.right) < 0.5) {
        return Error(409, "The camera up direction is parallel to the view direction, which leaves "
                          "the roll of the frustum undefined", {{"code", "camera_frustum_undefined"}});
    }
    frustumCross(basis.up, basis.right, basis.forward);
    frustumAxis(basis.up, basis.up[0], basis.up[1], basis.up[2]);
    return Result<void>::ok();
}

// Inside is every value at or above zero. The planes are not unit length, which
// does not matter: only the sign is ever read.
void frustumHalfSpaces(const FrustumBasis& basis, const double* point, double* out) {
    const double delta[3] = {point[0] - basis.origin[0], point[1] - basis.origin[1],
                             point[2] - basis.origin[2]};
    const double depth = frustumDot(delta, basis.forward);
    const double lateral = frustumDot(delta, basis.right);
    const double vertical = frustumDot(delta, basis.up);
    out[0] = depth - basis.near_plane;
    out[1] = basis.far_plane - depth;
    if (basis.orthogonal) {
        out[2] = basis.half_x + lateral;
        out[3] = basis.half_x - lateral;
        out[4] = basis.half_y + vertical;
        out[5] = basis.half_y - vertical;
        return;
    }
    out[2] = basis.half_x * depth + lateral;
    out[3] = basis.half_x * depth - lateral;
    out[4] = basis.half_y * depth + vertical;
    out[5] = basis.half_y * depth - vertical;
}

enum class FrustumContainment { outside, intersecting, inside };

// Whole corners inside means inside. Every corner outside one single plane
// means outside. Anything else is called intersecting, which is the ordinary
// conservative frustum test: a box that straddles two planes without entering
// the volume is reported as intersecting rather than missed, because saying a
// node might be visible when it is not is the safe direction to be wrong in.
FrustumContainment frustumContains(const FrustumBasis& basis,
                                   const std::vector<std::array<double, 3>>& corners) {
    if (corners.empty()) return FrustumContainment::outside;
    bool all_inside = true;
    bool outside_plane[6] = {true, true, true, true, true, true};
    for (const auto& corner : corners) {
        double values[6];
        frustumHalfSpaces(basis, corner.data(), values);
        for (int plane = 0; plane < 6; ++plane) {
            if (values[plane] >= 0.0) {
                outside_plane[plane] = false;
            } else {
                all_inside = false;
            }
        }
    }
    for (int plane = 0; plane < 6; ++plane) {
        if (outside_plane[plane]) return FrustumContainment::outside;
    }
    return all_inside ? FrustumContainment::inside : FrustumContainment::intersecting;
}

Result<double> projectViewportAspect(std::string& source) {
    auto settings = singleton("ProjectSettings");
    if (settings.isErr()) return settings.error();
    const auto read = [&](const char* key) -> Result<double> {
        auto name = makeString(key);
        if (name.isErr()) return name.error();
        VariantValue fallback;
        auto value = callObject(settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                {&name.value(), &fallback});
        if (value.isErr()) return value.error();
        auto number = scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (number.isErr()) return number.error();
        return static_cast<double>(number.value());
    };
    auto width = read("display/window/size/viewport_width");
    auto height = read("display/window/size/viewport_height");
    if (width.isErr() || height.isErr() || width.value() <= 0.0 || height.value() <= 0.0) {
        return Error(409, "The project has no viewport size to take an aspect ratio from", {{"code", "no_aspect_ratio"}});
    }
    source = "project_settings";
    return width.value() / height.value();
}

// The frustum of a Camera3D already in the scene.
//
// Aspect ratio does not live on the camera, so where it comes from is a real
// choice and the answer changes with it. A running game renders through the
// camera's own viewport, so that is the frustum being drawn. An edited scene
// renders through an editor pane whose shape is a fact about the window, not
// about the game, so the project's configured viewport size is the honest
// source there. Which one was used is reported either way.
Result<FrustumBasis> frustumFromCamera(GDExtensionObjectPtr camera, const std::string& session_kind) {
    for (const auto& bind : {std::make_tuple("Node3D", "get_global_position", 3360562783LL),
                             std::make_tuple("Node3D", "to_global", 192990374LL),
                             std::make_tuple("Camera3D", "get_fov", 1740695150LL),
                             std::make_tuple("Camera3D", "get_near", 1740695150LL),
                             std::make_tuple("Camera3D", "get_far", 1740695150LL),
                             std::make_tuple("Camera3D", "get_size", 1740695150LL),
                             std::make_tuple("Camera3D", "get_projection", 2624185235LL),
                             std::make_tuple("Camera3D", "get_keep_aspect_mode", 2790278316LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }

    const auto read_point = [&](const char* method, int64_t hash,
                                const std::vector<const VariantValue*>& args)
        -> Result<std::array<double, 3>> {
        auto value = callObject(camera, "Node3D", method, hash, args);
        if (value.isErr()) return value.error();
        auto point = pointVariantToJson(value.value(), 3);
        if (point.isErr()) return point.error();
        return std::array<double, 3>{point.value()["x"].get<double>(), point.value()["y"].get<double>(),
                                     point.value()["z"].get<double>()};
    };
    auto origin = read_point("get_global_position", 3360562783LL, {});
    if (origin.isErr()) return origin.error();
    auto local_forward = makeVector3(0.0, 0.0, -1.0);
    auto local_up = makeVector3(0.0, 1.0, 0.0);
    if (local_forward.isErr() || local_up.isErr()) {
        return Error::internal("Failed to construct camera basis probes");
    }
    auto ahead = read_point("to_global", 192990374LL, {&local_forward.value()});
    if (ahead.isErr()) return ahead.error();
    auto above = read_point("to_global", 192990374LL, {&local_up.value()});
    if (above.isErr()) return above.error();

    FrustumBasis basis;
    basis.origin[0] = origin.value()[0];
    basis.origin[1] = origin.value()[1];
    basis.origin[2] = origin.value()[2];
    auto oriented = orientFrustum(basis,
                                  ahead.value()[0] - origin.value()[0],
                                  ahead.value()[1] - origin.value()[1],
                                  ahead.value()[2] - origin.value()[2],
                                  above.value()[0] - origin.value()[0],
                                  above.value()[1] - origin.value()[1],
                                  above.value()[2] - origin.value()[2]);
    if (oriented.isErr()) return oriented.error();

    const auto read_number = [&](const char* method, int64_t hash) -> Result<double> {
        auto value = callObject(camera, "Camera3D", method, hash);
        if (value.isErr()) return value.error();
        auto number = scalarFromVariant<double>(value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (number.isErr()) return number.error();
        return number.value();
    };
    const auto read_enum = [&](const char* method, int64_t hash) -> Result<int64_t> {
        auto value = callObject(camera, "Camera3D", method, hash);
        if (value.isErr()) return value.error();
        return scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    auto projection = read_enum("get_projection", 2624185235LL);
    if (projection.isErr()) return projection.error();
    if (projection.value() == 2) {
        // A frustum-mode camera carries an off-axis offset that this basis has
        // no place to put. Answering with the centred frustum would describe a
        // volume the camera is not looking through.
        return Error(409, "The camera uses the frustum projection mode, whose off-axis offset this "
                          "query does not model", {{"code", "camera_projection_unsupported"}});
    }
    if (projection.value() != 0 && projection.value() != 1) {
        return Error(409, "The camera uses a projection mode this query does not model", {{"code", "camera_projection_unsupported"}});
    }
    auto near_plane = read_number("get_near", 1740695150LL);
    auto far_plane = read_number("get_far", 1740695150LL);
    if (near_plane.isErr()) return near_plane.error();
    if (far_plane.isErr()) return far_plane.error();
    basis.near_plane = near_plane.value();
    basis.far_plane = far_plane.value();
    if (!(basis.near_plane > 0.0) || !(basis.far_plane > basis.near_plane)) {
        return Error(409, "The camera near and far planes do not describe a volume", {{"code", "camera_frustum_undefined"}});
    }

    if (session_kind == "game") {
        auto required = requireMethodBind("Viewport", "get_visible_rect", 1639390495LL);
        if (required.isErr()) return Error(501, required.error().message);
        auto viewport_value = callObject(camera, "Node", "get_viewport", 3596683776LL);
        if (viewport_value.isErr()) return viewport_value.error();
        auto viewport = objectFromVariant(viewport_value.value());
        if (viewport.isErr() || !viewport.value()) {
            return Error(409, "The camera is not inside a viewport to take an aspect ratio from", {{"code", "no_aspect_ratio"}});
        }
        auto rect_value = callObject(viewport.value(), "Viewport", "get_visible_rect", 1639390495LL);
        if (rect_value.isErr()) return rect_value.error();
        auto rect = rect2ToJson(rect_value.value());
        if (rect.isErr()) return rect.error();
        const double width = rect.value()["size"]["x"].get<double>();
        const double height = rect.value()["size"]["y"].get<double>();
        if (!(width > 0.0) || !(height > 0.0)) {
            return Error(409, "The camera viewport has no visible size to take an aspect ratio from", {{"code", "no_aspect_ratio"}});
        }
        basis.aspect = width / height;
        basis.aspect_source = "viewport";
    } else {
        auto aspect = projectViewportAspect(basis.aspect_source);
        if (aspect.isErr()) return aspect.error();
        basis.aspect = aspect.value();
    }

    auto keep_aspect = read_enum("get_keep_aspect_mode", 2790278316LL);
    if (keep_aspect.isErr()) return keep_aspect.error();
    // Godot names the enum after the axis the value is held on: KEEP_WIDTH is
    // 0 and makes fov horizontal, KEEP_HEIGHT is 1 and makes it vertical.
    const bool keep_width = keep_aspect.value() == 0;
    basis.orthogonal = projection.value() == 1;
    if (basis.orthogonal) {
        auto size = read_number("get_size", 1740695150LL);
        if (size.isErr()) return size.error();
        if (!(size.value() > 0.0)) return Error(409, "The orthogonal camera has no size", {{"code", "camera_frustum_undefined"}});
        basis.ortho_size = size.value();
        if (keep_width) {
            basis.half_x = size.value() / 2.0;
            basis.half_y = basis.half_x / basis.aspect;
        } else {
            basis.half_y = size.value() / 2.0;
            basis.half_x = basis.half_y * basis.aspect;
        }
    } else {
        auto fov = read_number("get_fov", 1740695150LL);
        if (fov.isErr()) return fov.error();
        if (!(fov.value() > 0.0) || fov.value() >= 180.0) {
            return Error(409, "The camera field of view does not describe a frustum", {{"code", "camera_frustum_undefined"}});
        }
        basis.fov_degrees = fov.value();
        const double half_angle = fov.value() * 3.14159265358979323846 / 360.0;
        if (keep_width) {
            basis.half_x = std::tan(half_angle);
            basis.half_y = basis.half_x / basis.aspect;
        } else {
            basis.half_y = std::tan(half_angle);
            basis.half_x = basis.half_y * basis.aspect;
        }
    }
    return basis;
}

// One node that survived the frustum test, held until every node has been seen
// so the nearest can be reported first.
struct FrustumCandidate {
    std::string path;
    std::string class_name;
    FrustumContainment containment{FrustumContainment::inside};
    bool bounded{false};
    bool visible_in_tree{true};
    double distance{0.0};
    std::vector<std::array<double, 3>> samples;
};

const char* frustumContainmentName(FrustumContainment containment) {
    return containment == FrustumContainment::inside ? "inside" : "intersecting";
}

// The eight corners of a node's own bounding box in world space.
//
// Node3D.to_global is asked for each corner rather than the transform being
// read and multiplied here, so a scaled, sheared or deeply nested node is
// placed by the same code the engine places it with.
Result<std::vector<std::array<double, 3>>> worldBoundsCorners(GDExtensionObjectPtr node) {
    auto aabb_value = callObject(node, "VisualInstance3D", "get_aabb", 1068685055LL);
    if (aabb_value.isErr()) return aabb_value.error();
    auto& api = GodotApi::instance();
    if (api.variant_get_type(aabb_value.value().ptr()) != GDEXTENSION_VARIANT_TYPE_AABB ||
        !api.variant_get_ptr_getter) {
        return Error::internal("Godot did not return an AABB for the node bounds");
    }
    auto converter = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_AABB);
    if (!converter) return Error::internal("Godot AABB conversion is unavailable");
    NativeValue native_aabb(GDEXTENSION_VARIANT_TYPE_AABB);
    converter(native_aabb.ptr(), aabb_value.value().ptr());
    native_aabb.markInitialized();
    double corner[3] = {0.0, 0.0, 0.0};
    double extent[3] = {0.0, 0.0, 0.0};
    for (const auto& member : {std::make_pair("position", corner), std::make_pair("size", extent)}) {
        NativeName name(member.first);
        auto getter = name.valid()
            ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_AABB, name.ptr()) : nullptr;
        if (!getter) return Error::internal("Godot AABB member getter is unavailable");
        NativeValue native_vector(GDEXTENSION_VARIANT_TYPE_VECTOR3);
        getter(native_aabb.ptr(), native_vector.ptr());
        native_vector.markInitialized();
        int axis_index = 0;
        for (const auto* axis : {"x", "y", "z"}) {
            NativeName axis_name(axis);
            auto axis_getter = axis_name.valid()
                ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_VECTOR3, axis_name.ptr()) : nullptr;
            if (!axis_getter) return Error::internal("Godot Vector3 member getter is unavailable");
            axis_getter(native_vector.ptr(), &member.second[axis_index]);
            ++axis_index;
        }
    }

    std::vector<std::array<double, 3>> corners;
    corners.reserve(8);
    for (int mask = 0; mask < 8; ++mask) {
        const double local_x = corner[0] + ((mask & 1) ? extent[0] : 0.0);
        const double local_y = corner[1] + ((mask & 2) ? extent[1] : 0.0);
        const double local_z = corner[2] + ((mask & 4) ? extent[2] : 0.0);
        auto local = makeVector3(local_x, local_y, local_z);
        if (local.isErr()) return local.error();
        auto world = callObject(node, "Node3D", "to_global", 192990374LL, {&local.value()});
        if (world.isErr()) return world.error();
        auto point = pointVariantToJson(world.value(), 3);
        if (point.isErr()) return point.error();
        corners.push_back({point.value()["x"].get<double>(), point.value()["y"].get<double>(),
                           point.value()["z"].get<double>()});
    }
    return corners;
}

// Nearest first, and no more of them held than will be reported.
//
// A scene can put far more nodes inside a frustum than a caller asked to see,
// and keeping all of them only to throw most away would make the cost of the
// query depend on the scene rather than on the question. The kept set is a
// max-heap on distance, so the furthest held node is the one displaced.
bool frustumFurther(const FrustumCandidate& a, const FrustumCandidate& b) {
    return a.distance < b.distance;
}

struct FrustumScan {
    const FrustumBasis* basis{nullptr};
    GDExtensionObjectPtr root{nullptr};
    bool editor{true};
    int64_t keep{64};
    int64_t examined{0};
    int64_t inside_count{0};
    bool examine_limit_hit{false};
    std::vector<FrustumCandidate> candidates;

    void offer(FrustumCandidate&& candidate) {
        if (static_cast<int64_t>(candidates.size()) < keep) {
            candidates.push_back(std::move(candidate));
            std::push_heap(candidates.begin(), candidates.end(), frustumFurther);
            return;
        }
        if (!(candidate.distance < candidates.front().distance)) return;
        std::pop_heap(candidates.begin(), candidates.end(), frustumFurther);
        candidates.back() = std::move(candidate);
        std::push_heap(candidates.begin(), candidates.end(), frustumFurther);
    }
};

Result<void> scanFrustumNode(GDExtensionObjectPtr node, FrustumScan& scan) {
    if (scan.examined >= runtime::kMaxFrustumNodesExamined) {
        scan.examine_limit_hit = true;
        return Result<void>::ok();
    }
    ++scan.examined;

    auto spatial = objectIsClass(node, "Node3D");
    if (spatial.isErr()) return spatial.error();
    if (spatial.value()) {
        auto bounded = objectIsClass(node, "VisualInstance3D");
        if (bounded.isErr()) return bounded.error();
        std::vector<std::array<double, 3>> corners;
        if (bounded.value()) {
            auto world_corners = worldBoundsCorners(node);
            if (world_corners.isErr()) return world_corners.error();
            corners = std::move(world_corners.value());
        } else {
            auto origin = callObject(node, "Node3D", "get_global_position", 3360562783LL);
            if (origin.isErr()) return origin.error();
            auto point = pointVariantToJson(origin.value(), 3);
            if (point.isErr()) return point.error();
            corners.push_back({point.value()["x"].get<double>(), point.value()["y"].get<double>(),
                               point.value()["z"].get<double>()});
        }
        const auto containment = frustumContains(*scan.basis, corners);
        if (containment != FrustumContainment::outside) {
            ++scan.inside_count;
            FrustumCandidate candidate;
            candidate.containment = containment;
            candidate.bounded = bounded.value();
            auto class_name = nodeString(node, "get_class", 201670096LL);
            if (class_name.isErr()) return class_name.error();
            candidate.class_name = boundUtf8(class_name.value(), 256).value;
            auto path = scan.editor ? logicalPathFromEditedRoot(scan.root, node)
                                    : nodeString(node, "get_path", 4075236667LL);
            if (path.isErr()) return path.error();
            candidate.path = boundUtf8(path.value(), 1024).value;
            auto visible = callObject(node, "Node3D", "is_visible_in_tree", 36873697LL);
            if (visible.isErr()) return visible.error();
            auto visible_flag = scalarFromVariant<GDExtensionBool>(visible.value(),
                                                                   GDEXTENSION_VARIANT_TYPE_BOOL);
            if (visible_flag.isErr()) return visible_flag.error();
            candidate.visible_in_tree = visible_flag.value() != 0;

            double centre[3] = {0.0, 0.0, 0.0};
            for (const auto& point : corners) {
                centre[0] += point[0] / static_cast<double>(corners.size());
                centre[1] += point[1] / static_cast<double>(corners.size());
                centre[2] += point[2] / static_cast<double>(corners.size());
            }
            const double delta[3] = {centre[0] - scan.basis->origin[0],
                                     centre[1] - scan.basis->origin[1],
                                     centre[2] - scan.basis->origin[2]};
            candidate.distance = std::sqrt(frustumDot(delta, delta));
            candidate.samples = std::move(corners);
            if (candidate.bounded) {
                candidate.samples.push_back({centre[0], centre[1], centre[2]});
            }
            scan.offer(std::move(candidate));
        }
    }

    // A node with no place in the world can still hold children that have one,
    // so the walk descends whatever the node itself turned out to be.
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
        auto walked = scanFrustumNode(child.value(), scan);
        if (walked.isErr()) return walked.error();
        if (scan.examine_limit_hit) break;
    }
    return Result<void>::ok();
}

// Whether a hit stands between the camera and the node, or is the node itself.
//
// A ray aimed at a node's own bounding box normally hits that node's collider
// just short of the sample point, which is not an obstruction. Godot puts a
// collider on a body that may be the node, its child, or its parent, so a hit
// is treated as the node when either path contains the other.
bool frustumHitIsSelf(const std::string& node_path, const std::string& collider_path) {
    if (collider_path.empty()) return false;
    if (collider_path == node_path) return true;
    // Equal lengths that are not equal strings are two different nodes, and the
    // separator check below would be reading one past the end of the shorter.
    if (collider_path.size() == node_path.size()) return false;
    const std::string& longer = collider_path.size() > node_path.size() ? collider_path : node_path;
    const std::string& shorter = collider_path.size() > node_path.size() ? node_path : collider_path;
    return longer.compare(0, shorter.size(), shorter) == 0 && longer[shorter.size()] == '/';
}


} // namespace

json visionFrustumQuery(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseFrustumRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();

    for (const auto& bind : {std::make_tuple("Node", "get_children", 873284517LL),
                             std::make_tuple("Node3D", "is_visible_in_tree", 36873697LL),
                             std::make_tuple("Node3D", "get_global_position", 3360562783LL),
                             std::make_tuple("Node3D", "to_global", 192990374LL),
                             std::make_tuple("VisualInstance3D", "get_aabb", 1068685055LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    Result<GDExtensionObjectPtr> root = Error::internal("unresolved");
    const bool editor = session_kind == "editor";
    if (editor) {
        auto interface = editorInterface();
        if (interface.isErr()) return errorJson(interface.error());
        root = editedSceneRoot(interface.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return errorJson(tree.error());
        root = liveSceneTreeRoot(tree.value());
    }
    if (root.isErr()) return errorJson(root.error());

    FrustumBasis basis;
    json camera_json = json::object();
    if (request.source == runtime::FrustumSource::camera_node) {
        auto node = resolveNode(root.value(), request.camera_node);
        if (node.isErr()) return errorJson(node.error());
        auto is_camera = objectIsClass(node.value(), "Camera3D");
        if (is_camera.isErr()) return errorJson(500, is_camera.error().message);
        if (!is_camera.value()) {
            return errorJson(409, "The node at " + request.camera_node + " is not a Camera3D",
                                  {{"code", "camera_path_does_not_resolve_to_camera3d"}});
        }
        auto built = frustumFromCamera(node.value(), session_kind);
        if (built.isErr()) return errorJson(built.error());
        basis = std::move(built.value());
        camera_json["source"] = "camera_node";
        camera_json["node_path"] = request.camera_node;
    } else {
        basis.origin[0] = request.position.x;
        basis.origin[1] = request.position.y;
        basis.origin[2] = request.position.z;
        auto oriented = orientFrustum(basis, request.look_at.x - request.position.x,
                                      request.look_at.y - request.position.y,
                                      request.look_at.z - request.position.z,
                                      request.up.x, request.up.y, request.up.z);
        if (oriented.isErr()) return errorJson(oriented.error());
        basis.near_plane = request.near_plane;
        basis.far_plane = request.far_plane;
        basis.aspect = request.aspect;
        basis.fov_degrees = request.fov_degrees;
        // The parameter form takes a vertical field of view, which is what a
        // Godot camera holds by default.
        basis.half_y = std::tan(request.fov_degrees * 3.14159265358979323846 / 360.0);
        basis.half_x = basis.half_y * basis.aspect;
        camera_json["source"] = "parameters";
        camera_json["node_path"] = nullptr;
        camera_json["up_defaulted"] = !request.up_given;
    }
    camera_json["position"] = frustumAxisJson(basis.origin);
    camera_json["forward"] = frustumAxisJson(basis.forward);
    camera_json["up"] = frustumAxisJson(basis.up);
    camera_json["right"] = frustumAxisJson(basis.right);
    camera_json["near"] = basis.near_plane;
    camera_json["far"] = basis.far_plane;
    camera_json["aspect"] = basis.aspect;
    camera_json["aspect_source"] = basis.aspect_source;
    camera_json["projection"] = basis.orthogonal ? "orthogonal" : "perspective";
    camera_json["fov_degrees"] = basis.orthogonal ? json(nullptr) : json(basis.fov_degrees);
    camera_json["orthogonal_size"] = basis.orthogonal ? json(basis.ortho_size) : json(nullptr);

    FrustumScan scan;
    scan.basis = &basis;
    scan.root = root.value();
    scan.editor = editor;
    scan.keep = request.max_results;
    auto walked = scanFrustumNode(root.value(), scan);
    if (walked.isErr()) return errorJson(walked.error());

    std::sort_heap(scan.candidates.begin(), scan.candidates.end(), frustumFurther);
    const size_t reported = scan.candidates.size();

    // Sightline sampling opens the space state once for the whole answer, for
    // the same reason a ray batch does.
    std::optional<RaycastSpace> space;
    int64_t rays_cast = 0;
    bool sightline_truncated = false;
    if (request.sightline && reported > 0) {
        auto opened = openRaycastSpace(3, session_kind);
        if (opened.isErr()) return errorJson(opened.error());
        space = opened.value();
    }

    json nodes = json::array();
    for (size_t index = 0; index < reported; ++index) {
        auto& candidate = scan.candidates[index];
        json entry = {{"path", candidate.path},
                      {"class", candidate.class_name},
                      {"containment", frustumContainmentName(candidate.containment)},
                      {"tested", candidate.bounded ? "bounds" : "origin"},
                      {"visible_in_tree", candidate.visible_in_tree},
                      {"distance", candidate.distance}};
        if (space.has_value()) {
            if (rays_cast + static_cast<int64_t>(candidate.samples.size()) >
                runtime::kMaxSightlineRays) {
                // The field is left off rather than filled in, because an
                // unmeasured sightline reported as clear is a claim nobody
                // made.
                sightline_truncated = true;
                nodes.push_back(std::move(entry));
                continue;
            }
            int64_t clear = 0;
            for (const auto& sample : candidate.samples) {
                runtime::RaycastRequest ray;
                ray.from = runtime::SpatialPoint{3, basis.origin[0], basis.origin[1], basis.origin[2]};
                ray.to = runtime::SpatialPoint{3, sample[0], sample[1], sample[2]};
                ray.collision_mask = request.collision_mask;
                const double span[3] = {sample[0] - basis.origin[0], sample[1] - basis.origin[1],
                                        sample[2] - basis.origin[2]};
                const double reach = std::sqrt(frustumDot(span, span));
                if (reach < 1e-6) {
                    // The camera stands on the sample, so nothing can be
                    // between them.
                    ++clear;
                    continue;
                }
                auto cast = castOneRay(space.value(), ray, session_kind);
                if (cast.isErr()) return errorJson(cast.error());
                ++rays_cast;
                const json& hit = cast.value();
                if (!hit["hit"].get<bool>()) {
                    ++clear;
                    continue;
                }
                const std::string collider = hit["collider_path"].is_string()
                    ? hit["collider_path"].get<std::string>() : std::string();
                if (frustumHitIsSelf(candidate.path, collider)) {
                    ++clear;
                    continue;
                }
                const double hit_point[3] = {hit["position"]["x"].get<double>(),
                                             hit["position"]["y"].get<double>(),
                                             hit["position"]["z"].get<double>()};
                const double gap[3] = {hit_point[0] - basis.origin[0], hit_point[1] - basis.origin[1],
                                       hit_point[2] - basis.origin[2]};
                const double hit_distance = std::sqrt(frustumDot(gap, gap));
                // An obstruction at or past the sample point is not between the
                // camera and the node.
                if (hit_distance >= reach - (0.001 + reach * 0.001)) ++clear;
            }
            const int64_t samples = static_cast<int64_t>(candidate.samples.size());
            entry["sightline"] = {
                {"samples", samples},
                {"samples_clear", clear},
                {"status", clear == samples ? "clear" : (clear == 0 ? "blocked" : "partial")}};
        }
        nodes.push_back(std::move(entry));
    }

    json result = {{"camera", std::move(camera_json)},
                   {"nodes", std::move(nodes)},
                   {"node_count", scan.inside_count},
                   {"examined", scan.examined},
                   {"truncated", static_cast<int64_t>(reported) < scan.inside_count ||
                                 scan.examine_limit_hit},
                   {"scan_limit_reached", scan.examine_limit_hit}};
    if (request.sightline) {
        result["sightline_rays"] = rays_cast;
        result["sightline_truncated"] = sightline_truncated;
    }
    return liveResult(result);
}


json physicsRaycastBatch(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseRaycastBatchRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();
    auto space = openRaycastSpace(request.dimension(), session_kind);
    if (space.isErr()) return errorJson(space.error());

    json results = json::array();
    size_t hits = 0;
    for (size_t index = 0; index < request.rays.size(); ++index) {
        auto cast = castOneRay(space.value(), request.rays[index], session_kind);
        // One ray that cannot be answered fails the batch. A partial batch that
        // looked complete would be read as fifty clear sightlines when it was
        // forty-nine and a silence.
        if (cast.isErr()) {
            return errorJson(cast.error().code,
                             "rays[" + std::to_string(index) + "]: " + cast.error().message);
        }
        auto record = cast.value();
        if (record.value("hit", false)) ++hits;
        record["index"] = index;
        results.push_back(std::move(record));
    }
    return liveResult({{"dimension", request.dimension()},
                       {"requested_rays", request.rays.size()},
                       {"hit_count", hits},
                       {"results", std::move(results)}});
}


json physicsRaycast(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseRaycastRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();
    const int dimension = request.dimension();
    const char* params_class = dimension == 2 ? "PhysicsRayQueryParameters2D" : "PhysicsRayQueryParameters3D";
    const char* state_class = dimension == 2 ? "PhysicsDirectSpaceState2D" : "PhysicsDirectSpaceState3D";
    const int64_t create_hash = dimension == 2 ? 3196569324LL : 3110599579LL;
    const int64_t state_hash = dimension == 2 ? 2506717822LL : 2069328350LL;
    const int64_t ray_hash = dimension == 2 ? 1590275562LL : 3957970750LL;

    for (const auto& bind : {std::make_tuple(params_class, "create", create_hash),
                             std::make_tuple(state_class, "intersect_ray", ray_hash),
                             std::make_tuple(dimension == 2 ? "World2D" : "World3D", "get_direct_space_state", state_hash)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    auto world = queriedWorld(dimension, session_kind);
    if (world.isErr()) return errorJson(world.error());
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr()) return errorJson(500, world_object.error().message);
    auto state = callObject(world_object.value(), dimension == 2 ? "World2D" : "World3D",
                            "get_direct_space_state", state_hash);
    if (state.isErr()) return errorJson(500, state.error().message);
    auto state_object = objectFromVariant(state.value());
    if (state_object.isErr() || !state_object.value()) {
        return errorJson(409, "The queried world has no direct space state",
                         {{"code", "no_space_state"}});
    }

    auto from = makePoint(request.from);
    auto to = makePoint(request.to);
    auto mask = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.collision_mask);
    if (from.isErr() || to.isErr() || mask.isErr()) return errorJson(500, "Failed to construct ray arguments");
    // Static: the bind is called with a null instance and the trailing exclude
    // list takes its default.
    Result<VariantValue> query = [&]() -> Result<VariantValue> {
        auto& api = GodotApi::instance();
        NativeName klass(params_class);
        NativeName method("create");
        if (!klass.valid() || !method.valid()) return Error::internal("Failed to construct ray query identifiers");
        auto bind = api.classdb_get_method_bind(klass.ptr(), method.ptr(), create_hash);
        if (!bind) return Error(501, "Godot method binding unavailable for ray query creation");
        const void* raw_args[] = {from.value().ptr(), to.value().ptr(), mask.value().ptr()};
        VariantValue created(VariantValue::Uninitialized{});
        GDExtensionCallError error{};
        api.object_method_bind_call(bind, nullptr, raw_args, 3, created.ptr(), &error);
        created.markInitialized();
        if (error.error != GDEXTENSION_CALL_OK) {
            return Error::internal("PhysicsRayQueryParameters.create failed (call error " +
                                   std::to_string(error.error) + ")");
        }
        return std::move(created);
    }();
    if (query.isErr()) return errorJson(query.error());
    auto query_object = objectFromVariant(query.value());
    if (query_object.isErr() || !query_object.value()) return errorJson(500, "Ray query parameters were not created");
    // Fixed flags from the contract: bodies and areas on, hit-from-inside off,
    // back faces on in 3D.
    auto set_flag = [&](const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(query_object.value(), params_class, method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    for (const auto& flag : {std::make_pair("set_collide_with_bodies", true),
                             std::make_pair("set_collide_with_areas", true),
                             std::make_pair("set_hit_from_inside", false)}) {
        auto set = set_flag(flag.first, flag.second);
        if (set.isErr()) return errorJson(500, set.error().message);
    }
    if (dimension == 3) {
        auto set = set_flag("set_hit_back_faces", true);
        if (set.isErr()) return errorJson(500, set.error().message);
    }

    auto hit = callObject(state_object.value(), state_class, "intersect_ray", ray_hash, {&query.value()});
    if (hit.isErr()) return errorJson(500, hit.error().message);
    json result = {{"dimension", dimension}, {"hit", false}, {"collider_path", nullptr},
                   {"collider_class", nullptr}, {"position", nullptr}, {"normal", nullptr},
                   {"collision_layer", nullptr}};
    auto size = callVariant(hit.value(), "size");
    if (size.isErr()) return errorJson(500, size.error().message);
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr() || count.value() == 0) return liveResult(result);

    auto lookup = [&](const char* key) -> Result<VariantValue> {
        auto key_value = makeString(key);
        if (key_value.isErr()) return key_value.error();
        return callVariant(hit.value(), "get", {&key_value.value()});
    };
    auto position = lookup("position");
    auto normal = lookup("normal");
    auto collider = lookup("collider");
    if (position.isErr() || normal.isErr() || collider.isErr()) return errorJson(500, "Ray hit dictionary is incomplete");
    auto position_json = pointVariantToJson(position.value(), dimension);
    auto normal_json = pointVariantToJson(normal.value(), dimension);
    if (position_json.isErr() || normal_json.isErr()) return errorJson(500, "Ray hit vectors could not be read");
    result["hit"] = true;
    result["position"] = position_json.value();
    result["normal"] = normal_json.value();
    auto collider_object = objectFromVariant(collider.value());
    if (collider_object.isOk() && collider_object.value()) {
        auto class_name = nodeString(collider_object.value(), "get_class", 201670096LL);
        if (class_name.isOk()) {
            auto bounded = boundUtf8(class_name.value(), 256);
            result["collider_class"] = bounded.value;
        }
        auto is_node_name = makeString("Node");
        if (is_node_name.isOk()) {
            auto is_node = callObject(collider_object.value(), "Object", "is_class", 3927539163LL, {&is_node_name.value()});
            if (is_node.isOk()) {
                auto node_flag = scalarFromVariant<GDExtensionBool>(is_node.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (node_flag.isOk() && node_flag.value()) {
                    auto path = colliderLogicalPath(collider_object.value(), session_kind);
                    if (path.isOk() && !path.value().empty() && path.value().size() <= 1024) {
                        result["collider_path"] = path.value();
                    }
                }
            }
        }
        result["collision_layer"] = colliderCollisionLayer(collider_object.value(), dimension);
    }
    return liveResult(result);
}


json navQueryPath(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseNavPathRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();
    const int dimension = request.dimension();
    const char* server_class = dimension == 2 ? "NavigationServer2D" : "NavigationServer3D";
    const int64_t path_hash = dimension == 2 ? 1279824844LL : 276783190LL;
    auto required = requireMethodBind(server_class, "map_get_path", path_hash);
    if (required.isErr()) return errorJson(501, required.error().message);
    auto map_bind = requireMethodBind(dimension == 2 ? "World2D" : "World3D", "get_navigation_map", 2944877500LL);
    if (map_bind.isErr()) return errorJson(501, map_bind.error().message);

    auto world = queriedWorld(dimension, session_kind);
    if (world.isErr()) return errorJson(world.error());
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr()) return errorJson(500, world_object.error().message);
    auto map = callObject(world_object.value(), dimension == 2 ? "World2D" : "World3D",
                          "get_navigation_map", 2944877500LL);
    if (map.isErr()) return errorJson(500, map.error().message);
    if (GodotApi::instance().variant_get_type(map.value().ptr()) != GDEXTENSION_VARIANT_TYPE_RID) {
        return errorJson(409, "Root viewport world has no navigation map",
                         {{"code", "no_navigation_map"}});
    }
    auto server = singleton(server_class);
    if (server.isErr()) return errorJson(501, server.error().message);

    auto start = makePoint(request.start_point);
    auto end = makePoint(request.end_point);
    auto optimize = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(request.optimize));
    auto layers = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.navigation_layers);
    if (start.isErr() || end.isErr() || optimize.isErr() || layers.isErr()) {
        return errorJson(500, "Failed to construct navigation arguments");
    }
    auto path = callObject(server.value(), server_class, "map_get_path", path_hash,
                           {&map.value(), &start.value(), &end.value(), &optimize.value(), &layers.value()});
    if (path.isErr()) return errorJson(500, path.error().message);
    auto size = callVariant(path.value(), "size");
    if (size.isErr()) return errorJson(500, size.error().message);
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return errorJson(500, count.error().message);

    constexpr int64_t kMaxPoints = 256;
    constexpr size_t kMaxBytes = 256u * 1024u;
    json points = json::array();
    bool truncated = false;
    for (int64_t index = 0; index < count.value(); ++index) {
        if (index >= kMaxPoints) { truncated = true; break; }
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return errorJson(500, index_value.error().message);
        auto point = callVariant(path.value(), "get", {&index_value.value()});
        if (point.isErr()) return errorJson(500, point.error().message);
        auto converted = pointVariantToJson(point.value(), dimension);
        if (converted.isErr()) return errorJson(500, converted.error().message);
        points.push_back(converted.value());
        if (points.dump().size() > kMaxBytes) {
            points.erase(points.size() - 1);
            truncated = true;
            break;
        }
    }
    return liveResult({{"dimension", dimension}, {"reachable", count.value() > 0},
                       {"points", std::move(points)}, {"truncated", truncated},
                       {"navigation_layers", request.navigation_layers},
                       {"optimize", request.optimize}});
}


} // namespace didi::godot::bridge_internal
