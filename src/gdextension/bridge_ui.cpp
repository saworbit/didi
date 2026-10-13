#include "bridge_internal.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/common/json_bounds.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>

namespace didi::godot::bridge_internal {
namespace {
// Enumerates the Control nodes under a root, with where each one is on screen
// and what it says.
//
// ui_hit_test answers "what is under this point", which is only useful once you
// already have a point. An agent that has just written a menu has no point: it
// knows a button is called Start and needs to know where Start is. In a running
// game nothing answered that at all -- runtime_get_tree carries structure with
// no rectangles and no text -- so the only way to press a button was to guess
// coordinates.
//
// Editor or game. The rectangle is Control.get_global_rect, which is the
// viewport-space rectangle ui_hit_test already reports for a hit, so the two
// tools agree about where a Control is.
// The rectangle a Control covers in its window, in the pixels an injected
// mouse event carries. get_global_rect is in the viewport's own coordinates,
// which a stretched project scales on the way to the window: drawn at
// 1600x900 into a 1280x720 window, a button's centre sits at 0.8 of where
// get_global_rect puts it, and a click aimed at the listed centre missed
// (#1189). Viewport.get_screen_transform is that mapping, measured identical
// on 4.5.1, 4.6.2 and 4.7.2. A rotated or skewed transform gives the
// rectangle that bounds the four corners.
Result<json> screenRectOf(GDExtensionObjectPtr control, const json& global_rect) {
    auto viewport_value = callObject(control, "Node", "get_viewport", 3596683776LL);
    if (viewport_value.isErr()) return viewport_value.error();
    auto viewport = objectFromVariant(viewport_value.value());
    if (viewport.isErr()) return viewport.error();
    if (!viewport.value()) return Error::internal("The Control is not inside a viewport");
    auto transform_value =
        callObject(viewport.value(), "Viewport", "get_screen_transform", 3814499831LL);
    if (transform_value.isErr()) return transform_value.error();
    auto transform = variantToJson(transform_value.value());
    if (transform.isErr()) return transform.error();
    try {
        const auto& t = transform.value();
        const double xx = t.at("x").at("x").get<double>(), xy = t.at("x").at("y").get<double>();
        const double yx = t.at("y").at("x").get<double>(), yy = t.at("y").at("y").get<double>();
        const double ox = t.at("origin").at("x").get<double>();
        const double oy = t.at("origin").at("y").get<double>();
        const double px = global_rect.at("position").at("x").get<double>();
        const double py = global_rect.at("position").at("y").get<double>();
        const double sx = global_rect.at("size").at("x").get<double>();
        const double sy = global_rect.at("size").at("y").get<double>();
        double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        bool first = true;
        for (const auto& [cx, cy] : {std::pair<double, double>{px, py}, {px + sx, py},
                                     {px, py + sy}, {px + sx, py + sy}}) {
            const double wx = xx * cx + yx * cy + ox;
            const double wy = xy * cx + yy * cy + oy;
            min_x = first ? wx : std::min(min_x, wx);
            min_y = first ? wy : std::min(min_y, wy);
            max_x = first ? wx : std::max(max_x, wx);
            max_y = first ? wy : std::max(max_y, wy);
            first = false;
        }
        return json{{"position", {{"x", min_x}, {"y", min_y}}},
                    {"size", {{"x", max_x - min_x}, {"y", max_y - min_y}}}};
    } catch (const json::exception& error) {
        return Error::internal(std::string("The screen transform could not be read: ") + error.what());
    }
}

// The other direction: a point in window pixels, where an injected click and
// viewport_capture_frame put it, as a point in the viewport's own coordinates,
// which is what hit-testing compares against (#1223).
Result<std::pair<double, double>> viewportPointOf(GDExtensionObjectPtr viewport, double x, double y) {
    auto transform_value =
        callObject(viewport, "Viewport", "get_screen_transform", 3814499831LL);
    if (transform_value.isErr()) return transform_value.error();
    auto transform = variantToJson(transform_value.value());
    if (transform.isErr()) return transform.error();
    try {
        const auto& t = transform.value();
        const double xx = t.at("x").at("x").get<double>(), xy = t.at("x").at("y").get<double>();
        const double yx = t.at("y").at("x").get<double>(), yy = t.at("y").at("y").get<double>();
        const double ox = t.at("origin").at("x").get<double>();
        const double oy = t.at("origin").at("y").get<double>();
        const double determinant = xx * yy - yx * xy;
        if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-12) {
            return Error(409, "The window shows this viewport at no size, so no window pixel maps into it",
                         {{"code", "no_viewport_size"}});
        }
        const double dx = x - ox;
        const double dy = y - oy;
        return std::make_pair((yy * dx - yx * dy) / determinant, (xx * dy - xy * dx) / determinant);
    } catch (const json::exception& error) {
        return Error::internal(std::string("The screen transform could not be read: ") + error.what());
    }
}


} // namespace

json uiListControls(const json& params, const std::string& session_kind) {
    if (!hasOnlyKeys(params, {"root_path", "max_results", "visible_only", "include_text",
                              "class_filter"})) {
        return bridgeError(400, "invalid_ui_list_controls_request");
    }
    if (params.contains("root_path") &&
        (!params["root_path"].is_string() || params["root_path"].get<std::string>().size() > 1024)) {
        return errorJson(400, "root_path must be a string of at most 1024 bytes");
    }
    if (params.contains("visible_only") && !params["visible_only"].is_boolean()) {
        return errorJson(400, "visible_only must be a boolean");
    }
    if (params.contains("include_text") && !params["include_text"].is_boolean()) {
        return errorJson(400, "include_text must be a boolean");
    }
    if (params.contains("max_results") && !params["max_results"].is_number_integer()) {
        return errorJson(400, "max_results must be an integer");
    }
    const int64_t max_results = params.value("max_results", static_cast<int64_t>(64));
    if (max_results < 1 || max_results > 256) {
        return errorJson(400, "max_results must be from 1 to 256");
    }

    std::vector<std::string> class_filter;
    if (params.contains("class_filter")) {
        if (!params["class_filter"].is_array() || params["class_filter"].empty() ||
            params["class_filter"].size() > 16) {
            return errorJson(400, "class_filter must be an array of 1 to 16 class names");
        }
        for (const auto& entry : params["class_filter"]) {
            if (!entry.is_string() || entry.get<std::string>().empty() ||
                entry.get<std::string>().size() > 64) {
                return errorJson(400, "class_filter entries must be 1 to 64 byte class names");
            }
            class_filter.push_back(entry.get<std::string>());
        }
    }

    for (const auto& bind : {std::make_tuple("Object", "is_class", 3927539163LL),
                             std::make_tuple("Object", "get_class", 201670096LL),
                             std::make_tuple("Node", "get_children", 873284517LL),
                             std::make_tuple("CanvasItem", "is_visible_in_tree", 36873697LL),
                             std::make_tuple("Control", "get_global_rect", 1639390495LL),
                             std::make_tuple("Control", "get_mouse_filter_with_override",
                                             1572545674LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    const bool editor = session_kind == "editor";
    Result<GDExtensionObjectPtr> root = Error::internal("unresolved");
    if (editor) {
        auto interface_result = editorInterface();
        if (interface_result.isErr()) {
            return errorJson(interface_result.error());
        }
        root = editedSceneRoot(interface_result.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return errorJson(tree.error());
        root = liveSceneTreeRoot(tree.value());
    }
    if (root.isErr()) return errorJson(root.error());

    GDExtensionObjectPtr traversal_root = root.value();
    const std::string requested_root = params.value("root_path", std::string());
    if (!requested_root.empty()) {
        auto resolved = resolveNode(root.value(), requested_root);
        if (resolved.isErr()) return errorJson(resolved.error());
        if (!resolved.value()) return errorJson(404, "No node at " + requested_root);
        traversal_root = resolved.value();
    }

    const bool visible_only = params.value("visible_only", true);
    const bool include_text = params.value("include_text", true);

    auto text_name = makeStringName("text");
    if (text_name.isErr()) return errorJson(500, "Failed to construct the text property name");
    // atr()'s context argument, empty, which is what a Control passes as it draws.
    auto no_context = makeStringName("");
    if (no_context.isErr()) return errorJson(500, "Failed to construct an empty translation context");

    // The classes that draw their text through atr(), so that in a localised
    // game the player reads something other than the property (#988). Every
    // instantiable Control with a text property was rendered on 4.5.1, 4.6.2
    // and 4.7.2 by tools/vibe/probes/control_text_engine.py, and these eight
    // drew the translation. LineEdit, TextEdit and CodeEdit drew the key,
    // because their text is what the user typed, and ColorPickerButton drew no
    // text at all, although atr() answered a translation for all four. The
    // class is matched exactly rather than with is_class: a subclass is not
    // known to draw what its parent does, and ColorPickerButton is the Button
    // that does not. A script extending one of these reports the native class,
    // so a scripted Label is still a Label here.
    static const std::set<std::string> draws_translated_text = {
        "Button", "CheckBox", "CheckButton", "Label", "LinkButton",
        "MenuButton", "OptionButton", "RichTextLabel"};

    json controls = json::array();
    uint64_t traversed = 0;
    uint64_t matched = 0;
    bool traversal_limit_hit = false;

    std::function<Result<void>(GDExtensionObjectPtr, int)> visit =
        [&](GDExtensionObjectPtr node, int depth) -> Result<void> {
            if (traversed >= 10000) {
                traversal_limit_hit = true;
                return Result<void>::ok();
            }
            ++traversed;

            auto is_control = objectIsClass(node, "Control");
            if (is_control.isErr()) return is_control.error();

            bool visible = true;
            if (is_control.value()) {
                auto visible_value = callObject(node, "CanvasItem", "is_visible_in_tree", 36873697LL);
                if (visible_value.isErr()) return visible_value.error();
                auto visible_flag = scalarFromVariant<GDExtensionBool>(
                    visible_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (visible_flag.isErr()) return visible_flag.error();
                visible = visible_flag.value() != 0;
            }

            if (is_control.value() && (!visible_only || visible)) {
                auto class_name = nodeString(node, "get_class", 201670096LL);
                if (class_name.isErr()) return class_name.error();
                const std::string bounded_class = boundUtf8(class_name.value(), 256).value;

                bool keep = class_filter.empty();
                for (const auto& wanted : class_filter) {
                    if (keep) break;
                    auto matches = objectIsClass(node, wanted.c_str());
                    // An unknown class name is not an error: it simply matches
                    // nothing, which is what a caller filtering on a typo means
                    // to the engine.
                    if (matches.isOk() && matches.value()) keep = true;
                }

                if (keep) {
                    ++matched;
                    if (static_cast<int64_t>(controls.size()) < max_results) {
                        auto path = editor ? logicalPathFromEditedRoot(root.value(), node)
                                           : nodeString(node, "get_path", 4075236667LL);
                        if (path.isErr()) return path.error();
                        auto rect_value = callObject(node, "Control", "get_global_rect",
                                                     1639390495LL);
                        if (rect_value.isErr()) return rect_value.error();
                        auto rect_json = rect2ToJson(rect_value.value());
                        if (rect_json.isErr()) return rect_json.error();
                        auto mouse_value = callObject(node, "Control",
                                                      "get_mouse_filter_with_override",
                                                      1572545674LL);
                        if (mouse_value.isErr()) return mouse_value.error();
                        auto mouse_filter = scalarFromVariant<int64_t>(
                            mouse_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                        if (mouse_filter.isErr()) return mouse_filter.error();
                        const char* filter_name = mouse_filter.value() == 0   ? "stop"
                                                  : mouse_filter.value() == 1 ? "pass"
                                                                              : "ignore";

                        // A Control hidden in the tree is not laid out, and
                        // its rectangle is whatever it held before: four
                        // hidden labels sat on one point at their minimum
                        // sizes. Reported as null so nothing hit-tests or
                        // clicks a rectangle that means nothing (#1315).
                        json entry = {{"node_path", boundUtf8(path.value(), 1024).value},
                                      {"class", bounded_class},
                                      {"global_rect", visible ? rect_json.value() : json(nullptr)},
                                      {"visible", visible},
                                      {"depth", depth},
                                      {"mouse_filter", filter_name},
                                      {"mouse_filter_value", mouse_filter.value()}};
                        // In a game, where runtime_inject_input can click it.
                        if (!editor && !visible) {
                            entry["screen_rect"] = nullptr;
                        } else if (!editor) {
                            auto screen_rect = screenRectOf(node, rect_json.value());
                            if (screen_rect.isErr()) return screen_rect.error();
                            entry["screen_rect"] = std::move(screen_rect.value());
                        }

                        // Text lives on a different class for every widget that
                        // has it, so it is read as a property rather than through
                        // one get_text bind per class. A Control without the
                        // property yields a nil Variant and simply has no text.
                        if (include_text) {
                            auto text_value = callObject(node, "Object", "get", 2760726917LL,
                                                         {&text_name.value()});
                            if (text_value.isOk() &&
                                GodotApi::instance().variant_get_type(text_value.value().ptr()) ==
                                    GDEXTENSION_VARIANT_TYPE_STRING) {
                                auto text = stringFromVariant(text_value.value(),
                                                              GDEXTENSION_VARIANT_TYPE_STRING);
                                if (text.isOk()) {
                                    const auto bounded = boundUtf8(text.value(), 256);
                                    entry["text"] = bounded.value;
                                    if (bounded.truncated) entry["text_truncated"] = true;
                                    // What the player reads, where that is not the
                                    // property. atr() is the call the control makes
                                    // as it draws, so it follows the node's own
                                    // auto-translate mode and translation domain; an
                                    // editor draws keys and atr() there answers the
                                    // key, so an edited scene carries no field.
                                    // Present only when it differs, so an
                                    // untranslated game pays nothing for it.
                                    if (draws_translated_text.count(class_name.value())) {
                                        auto shown_value = callObject(
                                            node, "Node", "atr", 3344478075LL,
                                            {&text_value.value(), &no_context.value()});
                                        if (shown_value.isOk()) {
                                            auto shown = stringFromVariant(
                                                shown_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                                            if (shown.isOk() && shown.value() != text.value()) {
                                                const auto bounded_shown = boundUtf8(shown.value(), 256);
                                                entry["displayed_text"] = bounded_shown.value;
                                                if (bounded_shown.truncated) {
                                                    entry["displayed_text_truncated"] = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        controls.push_back(std::move(entry));
                    }
                }
            }

            // A hidden Control hides its children, so there is nothing under it
            // worth reporting when only visible Controls were asked for.
            if (is_control.value() && visible_only && !visible) return Result<void>::ok();

            auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                               static_cast<GDExtensionBool>(0));
            if (include_internal.isErr()) return include_internal.error();
            auto children = callObject(node, "Node", "get_children", 873284517LL,
                                       {&include_internal.value()});
            if (children.isErr()) return children.error();
            auto size_value = callVariant(children.value(), "size");
            if (size_value.isErr()) return size_value.error();
            auto size = scalarFromVariant<int64_t>(size_value.value(),
                                                   GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return size.error();
            for (int64_t index_value = 0; index_value < size.value(); ++index_value) {
                if (traversal_limit_hit) break;
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index_value);
                if (index.isErr()) return index.error();
                auto child_value = callVariant(children.value(), "get", {&index.value()});
                if (child_value.isErr()) return child_value.error();
                auto child = objectFromVariant(child_value.value());
                if (child.isErr() || !child.value()) {
                    return Error::internal("Godot returned an invalid UI child");
                }
                auto nested = visit(child.value(), depth + 1);
                if (nested.isErr()) return nested.error();
            }
            return Result<void>::ok();
        };

    auto visited = visit(traversal_root, 0);
    if (visited.isErr()) return errorJson(visited.error());

    // root_path is an input to this tool, and the value echoed back could not
    // be sent to anything: not to this tool on the next call, not to
    // scene_get_hierarchy, not to ui_hit_test. The one field saying which
    // subtree the answer covers was the one field that was not a path, and
    // the sibling reported /root for the same two nodes, so a caller
    // comparing the two had no way to tell which spelling was real (#470).
    //
    // The resolved path is right there. logicalPathFromEditedRoot is what
    // every control in this same answer is already named with.
    std::string resolved_root = requested_root;
    if (resolved_root.empty()) {
        auto root_name = editor ? logicalPathFromEditedRoot(root.value(), traversal_root)
                                : nodeString(traversal_root, "get_path", 4075236667LL);
        if (root_name.isErr()) return errorJson(root_name.error());
        resolved_root = boundUtf8(root_name.value(), 1024).value;
    }

    return liveResult({
        {"root_path", resolved_root},
        {"controls", controls},
        {"returned_count", controls.size()},
        {"match_count_total", matched},
        {"traversed_nodes", traversed},
        {"truncated", matched > controls.size() || traversal_limit_hit},
        {"traversal_limit_hit", traversal_limit_hit},
        {"visible_only", visible_only},
        {"ordering", "scene_tree_order"},
        {"input_injected", false}
    });
}

namespace {

// ui.hitTest. Editor or game: the editor answers for the edited scene and a
// game for the running one, which is where a caller most needs to know what
// sits under a point, right before injecting a click there (#592).

} // namespace

json uiHitTest(const json& params, const std::string& session_kind) {
    if (!params.is_object() || !params.contains("point") || !params["point"].is_object()) {
        return errorJson(400, "point is required and must be an object");
    }
    const auto& point_json = params["point"];
    if (!point_json.contains("x") || !point_json["x"].is_number() ||
        !point_json.contains("y") || !point_json["y"].is_number()) {
        return errorJson(400, "point.x and point.y are required numbers");
    }
    const double x = point_json["x"].get<double>();
    const double y = point_json["y"].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y)) return errorJson(400, "point coordinates must be finite");
    if (params.contains("root_path") && !params["root_path"].is_string()) {
        return errorJson(400, "root_path must be a string");
    }
    if (params.contains("include_mouse_filter_ignore") &&
        !params["include_mouse_filter_ignore"].is_boolean()) {
        return errorJson(400, "include_mouse_filter_ignore must be a boolean");
    }
    if (params.contains("max_results") && !params["max_results"].is_number_integer()) {
        return errorJson(400, "max_results must be an integer");
    }
    const int max_results = params.value("max_results", 32);
    if (max_results < 1 || max_results > 256) return errorJson(400, "max_results must be from 1 to 256");
    const std::string space = params.value("space", std::string("viewport"));
    if (space != "viewport" && space != "screen") {
        return errorJson(400, "space must be viewport or screen", {{"field", "space"}});
    }

    // The editor answers for the scene being edited and a game for the one
    // being played, the same two roots uiListControls reads, so the tool's
    // own root_path description is true in both kinds of session (#592).
    const bool editor = session_kind == "editor";
    // Window pixels are a running game's: an editor draws the edited scene
    // inside its own window, where nothing is clicked by an injected event.
    if (editor && space == "screen") {
        return errorJson(400, "space: screen is a running game's window pixels. In an editor, give the "
                              "point in the edited scene's viewport coordinates.",
                         {{"field", "space"}, {"retry_with", {{"space", "viewport"}}}});
    }
    Result<GDExtensionObjectPtr> edited_root = Error::internal("unresolved");
    if (editor) {
        auto interface_result = editorInterface();
        if (interface_result.isErr()) {
            return errorJson(interface_result.error());
        }
        edited_root = editedSceneRoot(interface_result.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return errorJson(tree.error());
        edited_root = liveSceneTreeRoot(tree.value());
    }
    if (edited_root.isErr()) return errorJson(edited_root.error());
    const std::string requested_root = params.value("root_path", "/root");
    auto traversal_root = resolveNode(edited_root.value(), requested_root);
    if (traversal_root.isErr()) return errorJson(traversal_root.error());
    double viewport_x = x;
    double viewport_y = y;
    if (space == "screen") {
        auto mapped = viewportPointOf(edited_root.value(), x, y);
        if (mapped.isErr()) return errorJson(mapped.error());
        viewport_x = mapped.value().first;
        viewport_y = mapped.value().second;
    }
    auto point = makeVector2(viewport_x, viewport_y);
    if (point.isErr()) return errorJson(point.error());

    struct UiHit {
        json value;
        int64_t canvas_layer{0};
        int64_t effective_z{0};
        uint64_t draw_order{0};
    };
    std::vector<UiHit> hits;
    uint64_t traversed = 0;
    uint64_t draw_order = 0;
    const bool include_ignored = params.value("include_mouse_filter_ignore", false);
    auto control_name = makeString("Control");
    auto canvas_item_name = makeString("CanvasItem");
    if (control_name.isErr() || canvas_item_name.isErr()) {
        return errorJson(500, "Failed to construct live UI class identifiers");
    }

    std::function<Result<void>(GDExtensionObjectPtr, bool, int64_t)> visit =
        [&](GDExtensionObjectPtr node, bool ancestor_accepts_point, int64_t inherited_z) -> Result<void> {
            if (++traversed > 10000) return Error::invalidArgument("UI traversal exceeds the 10,000 node limit");
            const uint64_t current_order = draw_order++;
            auto is_control_value = callObject(node, "Object", "is_class", 3927539163LL,
                                               {&control_name.value()});
            auto is_canvas_value = callObject(node, "Object", "is_class", 3927539163LL,
                                              {&canvas_item_name.value()});
            if (is_control_value.isErr()) return is_control_value.error();
            if (is_canvas_value.isErr()) return is_canvas_value.error();
            auto is_control = scalarFromVariant<GDExtensionBool>(is_control_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            auto is_canvas = scalarFromVariant<GDExtensionBool>(is_canvas_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (is_control.isErr()) return is_control.error();
            if (is_canvas.isErr()) return is_canvas.error();

            bool visible = true;
            int64_t effective_z = inherited_z;
            int64_t canvas_layer = 0;
            if (is_canvas.value()) {
                auto visible_value = callObject(node, "CanvasItem", "is_visible_in_tree", 36873697LL);
                auto z_value = callObject(node, "CanvasItem", "get_z_index", 3905245786LL);
                auto relative_value = callObject(node, "CanvasItem", "is_z_relative", 36873697LL);
                if (visible_value.isErr()) return visible_value.error();
                if (z_value.isErr()) return z_value.error();
                if (relative_value.isErr()) return relative_value.error();
                auto visible_scalar = scalarFromVariant<GDExtensionBool>(visible_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                auto z_scalar = scalarFromVariant<int64_t>(z_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                auto relative_scalar = scalarFromVariant<GDExtensionBool>(relative_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (visible_scalar.isErr()) return visible_scalar.error();
                if (z_scalar.isErr()) return z_scalar.error();
                if (relative_scalar.isErr()) return relative_scalar.error();
                visible = visible_scalar.value() != 0;
                effective_z = relative_scalar.value() ? inherited_z + z_scalar.value() : z_scalar.value();
                auto layer_node_value = callObject(node, "CanvasItem", "get_canvas_layer_node", 2602762519LL);
                if (layer_node_value.isErr()) return layer_node_value.error();
                auto layer_node = objectFromVariant(layer_node_value.value());
                if (layer_node.isErr()) return layer_node.error();
                if (layer_node.value()) {
                    auto layer_value = callObject(layer_node.value(), "CanvasLayer", "get_layer", 3905245786LL);
                    if (layer_value.isErr()) return layer_value.error();
                    auto layer_scalar = scalarFromVariant<int64_t>(layer_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                    if (layer_scalar.isErr()) return layer_scalar.error();
                    canvas_layer = layer_scalar.value();
                }
            }

            bool current_has_point = false;
            bool clips_children = false;
            if (is_control.value() && visible && ancestor_accepts_point) {
                auto local_value = callObject(node, "CanvasItem", "make_canvas_position_local", 2656412154LL,
                                              {&point.value()});
                if (local_value.isErr()) return local_value.error();
                auto local_json = vector2ToJson(local_value.value());
                if (local_json.isErr()) return local_json.error();
                auto object_value = makeObject(node);
                if (object_value.isErr()) return object_value.error();
                auto hit_value = callVariant(object_value.value(), "_has_point", {&local_value.value()});
                if (hit_value.isOk()) {
                    auto hit_scalar = scalarFromVariant<GDExtensionBool>(hit_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                    if (hit_scalar.isErr()) return hit_scalar.error();
                    current_has_point = hit_scalar.value() != 0;
                } else {
                    auto size_value = callVariant(object_value.value(), "get_size");
                    if (size_value.isErr()) return size_value.error();
                    auto size_json = vector2ToJson(size_value.value());
                    if (size_json.isErr()) return size_json.error();
                    const double local_x = local_json.value().at("x").get<double>();
                    const double local_y = local_json.value().at("y").get<double>();
                    const double width = size_json.value().at("x").get<double>();
                    const double height = size_json.value().at("y").get<double>();
                    current_has_point = local_x >= 0.0 && local_y >= 0.0 &&
                                        local_x < width && local_y < height;
                }
                auto clip_value = callObject(node, "Control", "is_clipping_contents", 2240911060LL);
                if (clip_value.isErr()) return clip_value.error();
                auto clip_scalar = scalarFromVariant<GDExtensionBool>(clip_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (clip_scalar.isErr()) return clip_scalar.error();
                clips_children = clip_scalar.value() != 0;

                auto mouse_value = callObject(node, "Control", "get_mouse_filter_with_override", 1572545674LL);
                if (mouse_value.isErr()) return mouse_value.error();
                auto mouse_filter = scalarFromVariant<int64_t>(mouse_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                if (mouse_filter.isErr()) return mouse_filter.error();
                if (current_has_point && (include_ignored || mouse_filter.value() != 2)) {
                    auto path = editor ? logicalPathFromEditedRoot(edited_root.value(), node)
                                       : nodeString(node, "get_path", 4075236667LL);
                    auto class_name = nodeString(node, "get_class", 201670096LL);
                    auto global_rect_value = callObject(node, "Control", "get_global_rect", 1639390495LL);
                    auto rect_json = global_rect_value.isOk()
                        ? rect2ToJson(global_rect_value.value())
                        : Result<json>(global_rect_value.error());
                    if (path.isErr()) return path.error();
                    if (class_name.isErr()) return class_name.error();
                    if (local_json.isErr()) return local_json.error();
                    if (rect_json.isErr()) return rect_json.error();
                    const char* filter_name = mouse_filter.value() == 0 ? "stop" :
                                              mouse_filter.value() == 1 ? "pass" : "ignore";
                    json hit = {{"node_path", path.value()}, {"class", class_name.value()},
                                {"mouse_filter", filter_name}, {"mouse_filter_value", mouse_filter.value()},
                                {"canvas_layer", canvas_layer}, {"effective_z_index", effective_z},
                                {"draw_order", current_order}, {"local_point", local_json.value()},
                                {"global_rect", rect_json.value()}};
                    // Where a click lands, as ui_list_controls reports it, so a
                    // hit can be clicked without converting it by hand (#1223).
                    if (!editor) {
                        auto screen_rect = screenRectOf(node, rect_json.value());
                        if (screen_rect.isErr()) return screen_rect.error();
                        hit["screen_rect"] = std::move(screen_rect.value());
                    }
                    hits.push_back({std::move(hit), canvas_layer, effective_z, current_order});
                }
            }

            const bool children_accept = ancestor_accepts_point && visible &&
                (!is_control.value() || !clips_children || current_has_point);
            auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
            if (include_internal.isErr()) return include_internal.error();
            auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
            if (children.isErr()) return children.error();
            auto size_value = callVariant(children.value(), "size");
            if (size_value.isErr()) return size_value.error();
            auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return size.error();
            for (int64_t index_value = 0; index_value < size.value(); ++index_value) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index_value);
                if (index.isErr()) return index.error();
                auto child_value = callVariant(children.value(), "get", {&index.value()});
                if (child_value.isErr()) return child_value.error();
                auto child = objectFromVariant(child_value.value());
                if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid UI child");
                auto nested = visit(child.value(), children_accept, effective_z);
                if (nested.isErr()) return nested.error();
            }
            return Result<void>::ok();
        };

    auto visited = visit(traversal_root.value(), true, 0);
    if (visited.isErr()) return errorJson(visited.error());
    std::stable_sort(hits.begin(), hits.end(), [](const UiHit& left, const UiHit& right) {
        if (left.canvas_layer != right.canvas_layer) return left.canvas_layer > right.canvas_layer;
        if (left.effective_z != right.effective_z) return left.effective_z > right.effective_z;
        return left.draw_order > right.draw_order;
    });
    const size_t total_hits = hits.size();
    json output_hits = json::array();
    for (size_t i = 0; i < std::min(hits.size(), static_cast<size_t>(max_results)); ++i) {
        output_hits.push_back(hits[i].value);
    }
    json topmost = output_hits.empty() ? json(nullptr) : output_hits.front();
    auto hit_root_path = editor ? logicalPathFromEditedRoot(edited_root.value(), traversal_root.value())
                                : nodeString(traversal_root.value(), "get_path", 4075236667LL);
    if (hit_root_path.isErr()) {
        return errorJson(hit_root_path.error());
    }
    const std::string resolved_hit_root = boundUtf8(hit_root_path.value(), 1024).value;
    json answer = liveResult({
        // The sibling of the same defect. This echoed the literal default
        // "/root" while actually traversing from the edited scene root, so
        // the two tools reported different subtrees for the same two nodes
        // and a caller comparing them could not tell which was real (#470).
        // Named the way every hit in this same answer is named.
        {"point", {{"x", x}, {"y", y}}}, {"root_path", resolved_hit_root},
        {"hits", output_hits}, {"topmost", topmost}, {"hit_count_total", total_hits},
        {"returned_count", output_hits.size()}, {"traversed_nodes", traversed},
        {"truncated", total_hits > output_hits.size()},
        {"ordering", "canvas_layer_desc,effective_z_index_desc,scene_draw_order_desc"},
        {"input_injected", false}
    });
    // A point given in window pixels says so, and says what it was compared
    // against. A viewport point's answer is what it always was.
    if (space == "screen") {
        answer["space"] = "screen";
        answer["viewport_point"] = {{"x", viewport_x}, {"y", viewport_y}};
    }
    return answer;
}




} // namespace didi::godot::bridge_internal
