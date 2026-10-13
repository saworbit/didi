#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;

json GodotBridge::bridgeVisionSetCameraTransform(const std::string& /*method*/, const json& params, const std::string& session_kind, GDExtensionObjectPtr editor) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    auto vector_is_valid = [](const json& value, double limit) {
        if (!value.is_object() || value.size() != 3) return false;
        for (const auto* axis : {"x", "y", "z"}) {
            if (!value.contains(axis) || !value[axis].is_number()) return false;
            const double component = value[axis].get<double>();
            if (!std::isfinite(component) || component < -limit || component > limit) {
                return false;
            }
        }
        return true;
    };
    if (!hasOnlyKeys(params, {"camera_path", "position", "rotation_degrees", "fov"}) ||
        !params.contains("camera_path") || !params["camera_path"].is_string() ||
        params["camera_path"].get_ref<const std::string&>().empty() ||
        params["camera_path"].get_ref<const std::string&>().size() > 1024 ||
        !params.contains("position") || !vector_is_valid(params["position"], 1000000.0) ||
        (params.contains("rotation_degrees") &&
         !vector_is_valid(params["rotation_degrees"], 360000.0)) ||
        (params.contains("fov") &&
         (!params["fov"].is_number() || !std::isfinite(params["fov"].get<double>()) ||
          params["fov"].get<double>() < 1.0 || params["fov"].get<double>() > 179.0))) {
        return bridgeError(400, "invalid_viewport_set_camera_transform_request");
    }

    for (const auto& bind : {
             std::make_tuple("Object", "is_class", 3927539163LL),
             std::make_tuple("Node3D", "get_position", 3360562783LL),
             std::make_tuple("Node3D", "set_position", 3460891852LL),
             std::make_tuple("Node3D", "get_rotation_degrees", 3360562783LL),
             std::make_tuple("Node3D", "set_rotation_degrees", 3460891852LL),
             std::make_tuple("Camera3D", "get_fov", 1740695150LL),
             std::make_tuple("Camera3D", "set_fov", 373806689LL),
             std::make_tuple("EditorUndoRedoManager", "get_object_history_id", 1107568780LL),
             std::make_tuple("EditorUndoRedoManager", "get_history_undo_redo", 2417974513LL),
             std::make_tuple("UndoRedo", "has_undo", 36873697LL),
             std::make_tuple("UndoRedo", "undo", 2240911060LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind),
                              std::get<2>(bind)).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    if (preflightUndoManagerBindings().isErr()) {
        return bridgeError(501, "required_bind_unavailable");
    }

    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    const auto camera_path = params["camera_path"].get<std::string>();
    auto camera = resolveNode(root.value(), camera_path);
    if (camera.isErr()) return errorJson(camera.error());
    if (auto refused = refuseUnsavableEdit(root.value(), camera.value(), camera_path,
                                           SceneEdit::Property)) {
        return *refused;
    }
    auto class_name = makeString("Camera3D");
    if (class_name.isErr()) return errorJson(500, class_name.error().message);
    auto class_result = callObject(camera.value(), "Object", "is_class", 3927539163LL,
                                   {&class_name.value()});
    if (class_result.isErr()) return errorJson(500, class_result.error().message);
    auto is_camera = scalarFromVariant<GDExtensionBool>(
        class_result.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (is_camera.isErr()) return errorJson(500, is_camera.error().message);
    if (!is_camera.value()) return bridgeError(404, "camera_path_does_not_resolve_to_camera3d");

    auto old_position = callObject(camera.value(), "Node3D", "get_position", 3360562783LL);
    auto old_rotation = callObject(camera.value(), "Node3D", "get_rotation_degrees", 3360562783LL);
    auto old_fov_value = callObject(camera.value(), "Camera3D", "get_fov", 1740695150LL);
    if (old_position.isErr() || old_rotation.isErr() || old_fov_value.isErr()) {
        return bridgeError(500, "camera_state_read_failed");
    }
    auto old_position_json = pointVariantToJson(old_position.value(), 3);
    auto old_rotation_json = pointVariantToJson(old_rotation.value(), 3);
    auto old_fov = scalarFromVariant<double>(old_fov_value.value(),
                                             GDEXTENSION_VARIANT_TYPE_FLOAT);
    if (old_position_json.isErr() || old_rotation_json.isErr() || old_fov.isErr()) {
        return bridgeError(500, "extension_protocol_error");
    }

    const auto& requested_position = params["position"];
    auto new_position = makeVector3(requested_position["x"].get<double>(),
                                    requested_position["y"].get<double>(),
                                    requested_position["z"].get<double>());
    if (new_position.isErr()) return bridgeError(501, "required_bind_unavailable");
    std::optional<VariantValue> new_rotation;
    if (params.contains("rotation_degrees")) {
        const auto& requested = params["rotation_degrees"];
        auto value = makeVector3(requested["x"].get<double>(), requested["y"].get<double>(),
                                 requested["z"].get<double>());
        if (value.isErr()) return bridgeError(501, "required_bind_unavailable");
        new_rotation.emplace(std::move(value.value()));
    }
    std::optional<VariantValue> new_fov;
    if (params.contains("fov")) {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT,
                                params["fov"].get<double>());
        if (value.isErr()) return bridgeError(501, "required_bind_unavailable");
        new_fov.emplace(std::move(value.value()));
    }

    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto action = createAction(manager.value(), "Set Camera3D Transform", camera.value());
    if (action.isErr()) return errorJson(500, action.error().message);
    auto add_step = [&](const char* operation, const char* property_method,
                        VariantValue& value) {
        return managerMethod(manager.value(), operation, camera.value(), property_method,
                             {&value});
    };
    auto registered = add_step("add_do_method", "set_position", new_position.value());
    if (registered.isOk()) {
        registered = add_step("add_undo_method", "set_position", old_position.value());
    }
    if (registered.isOk() && new_rotation.has_value()) {
        registered = add_step("add_do_method", "set_rotation_degrees", *new_rotation);
    }
    if (registered.isOk() && new_rotation.has_value()) {
        registered = add_step("add_undo_method", "set_rotation_degrees", old_rotation.value());
    }
    if (registered.isOk() && new_fov.has_value()) {
        registered = add_step("add_do_method", "set_fov", *new_fov);
    }
    if (registered.isOk() && new_fov.has_value()) {
        registered = add_step("add_undo_method", "set_fov", old_fov_value.value());
    }
    if (registered.isErr()) {
        abandonAction(manager.value());
        return bridgeError(500, "camera_undo_registration_failed");
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(500, committed.error().message);

    auto observed_position = callObject(camera.value(), "Node3D", "get_position", 3360562783LL);
    auto observed_rotation = callObject(camera.value(), "Node3D", "get_rotation_degrees", 3360562783LL);
    auto observed_fov_value = callObject(camera.value(), "Camera3D", "get_fov", 1740695150LL);
    if (observed_position.isErr() || observed_rotation.isErr() || observed_fov_value.isErr()) {
        (void)undoLastAction(manager.value(), root.value());
        return bridgeError(500, "camera_postcondition_read_failed");
    }
    auto observed_position_json = pointVariantToJson(observed_position.value(), 3);
    auto observed_rotation_json = pointVariantToJson(observed_rotation.value(), 3);
    auto observed_fov = scalarFromVariant<double>(observed_fov_value.value(),
                                                  GDEXTENSION_VARIANT_TYPE_FLOAT);
    if (observed_position_json.isErr() || observed_rotation_json.isErr() ||
        observed_fov.isErr()) {
        (void)undoLastAction(manager.value(), root.value());
        return bridgeError(500, "extension_protocol_error");
    }
    auto close_enough = [](double left, double right) {
        return std::abs(left - right) <=
               1e-5 * std::max({1.0, std::abs(left), std::abs(right)});
    };
    auto vector_matches = [&](const json& observed, const json& requested) {
        for (const auto* axis : {"x", "y", "z"}) {
            if (!close_enough(observed[axis].get<double>(), requested[axis].get<double>())) {
                return false;
            }
        }
        return true;
    };
    bool matched = vector_matches(observed_position_json.value(), requested_position);
    if (params.contains("rotation_degrees")) {
        matched = matched && vector_matches(observed_rotation_json.value(),
                                            params["rotation_degrees"]);
    }
    if (params.contains("fov")) {
        matched = matched && close_enough(observed_fov.value(), params["fov"].get<double>());
    }
    if (!matched) {
        const auto restored = undoLastAction(manager.value(), root.value());
        return bridgeError(500, "camera_postcondition_mismatch",
                         {{"outcome", restored.isOk() ? "rolled_back" : "unknown"},
                          {"rollback", restored.isOk() ? "completed" : "failed"},
                          {"retryable", false}});
    }
    return liveSceneMutation({
        {"camera_path", camera_path},
        {"old", {{"position", old_position_json.value()},
                 {"rotation_degrees", old_rotation_json.value()},
                 {"fov", old_fov.value()}}},
        {"new", {{"position", observed_position_json.value()},
                 {"rotation_degrees", observed_rotation_json.value()},
                 {"fov", observed_fov.value()}}},
        {"undo_redo_registered", true}, {"outcome", "completed"},
        {"rollback", "undo_redo"}});
}


json GodotBridge::bridgeVisionToggleDebugDraw(const std::string& /*method*/, const json& params, const std::string& session_kind, GDExtensionObjectPtr /*editor*/) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (!hasOnlyKeys(params, {"collision_shapes", "navigation_mesh", "wireframe"}) ||
        (!params.contains("collision_shapes") && !params.contains("navigation_mesh")) ||
        (params.contains("collision_shapes") && !params["collision_shapes"].is_boolean()) ||
        (params.contains("navigation_mesh") && !params["navigation_mesh"].is_boolean()) ||
        (params.contains("wireframe") &&
         (!params["wireframe"].is_boolean() || params["wireframe"].get<bool>()))) {
        return bridgeError(400, "invalid_viewport_toggle_debug_draw_request");
    }
    for (const auto& bind : {
             std::make_pair("is_debugging_collisions_hint", 36873697LL),
             std::make_pair("set_debug_collisions_hint", 2586408642LL),
             std::make_pair("is_debugging_navigation_hint", 36873697LL),
             std::make_pair("set_debug_navigation_hint", 2586408642LL)}) {
        if (requireMethodBind("SceneTree", bind.first, bind.second).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    auto tree = liveSceneTree();
    if (tree.isErr()) return errorJson(tree.error());
    auto read_hint = [&](const char* getter) -> Result<bool> {
        auto value = callObject(tree.value(), "SceneTree", getter, 36873697LL);
        if (value.isErr()) return value.error();
        auto enabled = scalarFromVariant<GDExtensionBool>(
            value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        return enabled.isOk() ? Result<bool>(enabled.value() != 0)
                              : Result<bool>(enabled.error());
    };
    auto old_collision = read_hint("is_debugging_collisions_hint");
    auto old_navigation = read_hint("is_debugging_navigation_hint");
    if (old_collision.isErr() || old_navigation.isErr()) {
        return bridgeError(500, "debug_hint_read_failed");
    }
    auto set_hint = [&](const char* setter, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(tree.value(), "SceneTree", setter, 2586408642LL,
                                 {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    auto restore_hints = [&]() {
        const auto collision = set_hint("set_debug_collisions_hint", old_collision.value());
        const auto navigation = set_hint("set_debug_navigation_hint", old_navigation.value());
        return collision.isOk() && navigation.isOk();
    };
    if (params.contains("collision_shapes")) {
        auto changed = set_hint("set_debug_collisions_hint",
                                params["collision_shapes"].get<bool>());
        if (changed.isErr()) {
            const bool restored = restore_hints();
            return errorJson(500, changed.error().message,
                             {{"outcome", restored ? "rolled_back" : "unknown"},
                              {"rollback", restored ? "completed" : "failed"},
                              {"retryable", false}});
        }
    }
    if (params.contains("navigation_mesh")) {
        auto changed = set_hint("set_debug_navigation_hint",
                                params["navigation_mesh"].get<bool>());
        if (changed.isErr()) {
            const bool restored = restore_hints();
            return errorJson(500, changed.error().message,
                             {{"outcome", restored ? "rolled_back" : "unknown"},
                              {"rollback", restored ? "completed" : "failed"},
                              {"retryable", false}});
        }
    }
    auto observed_collision = read_hint("is_debugging_collisions_hint");
    auto observed_navigation = read_hint("is_debugging_navigation_hint");
    const bool matched = observed_collision.isOk() && observed_navigation.isOk() &&
        (!params.contains("collision_shapes") ||
         observed_collision.value() == params["collision_shapes"].get<bool>()) &&
        (!params.contains("navigation_mesh") ||
         observed_navigation.value() == params["navigation_mesh"].get<bool>()) &&
        (params.contains("collision_shapes") ||
         observed_collision.value() == old_collision.value()) &&
        (params.contains("navigation_mesh") ||
         observed_navigation.value() == old_navigation.value());
    if (!matched) {
        const bool restored = restore_hints();
        return bridgeError(500, "debug_draw_postcondition_mismatch",
                         {{"outcome", restored ? "rolled_back" : "unknown"},
                          {"rollback", restored ? "completed" : "failed"},
                          {"retryable", false}});
    }
    return liveResult({
        {"previous", {{"collision_shapes", old_collision.value()},
                      {"navigation_mesh", old_navigation.value()}}},
        {"observed", {{"collision_shapes", observed_collision.value()},
                      {"navigation_mesh", observed_navigation.value()}}},
        {"effective_scope", "future_games_run_from_editor"},
        {"outcome", "completed"}, {"rollback", "explicit_restore"}});
}


} // namespace didi::godot
