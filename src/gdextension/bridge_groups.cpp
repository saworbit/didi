#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/common/json_bounds.hpp"
#include <algorithm>
#include <functional>

namespace didi::godot {
using namespace bridge_internal;
namespace {
Result<void> validateGroupName(const std::string& group) {
    if (group.empty() || group.size() > 128 || group.find('/') != std::string::npos ||
        group.find('\\') != std::string::npos) {
        return Error::invalidArgument("group must be a non-empty name without path separators");
    }
    return Result<void>::ok();
}

} // namespace

json GodotBridge::bridgeSceneGroups(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    const std::string group = params.value("group", "");
    if (method != "scene.listGroups") {
        auto valid_group = validateGroupName(group);
        if (valid_group.isErr()) return errorJson(valid_group.error());
    }
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());

    if (method == "scene.getGroupMembers") {
        auto group_name = makeStringName(group);
        if (group_name.isErr()) return errorJson(group_name.error());
        json members = json::array();
        // A group name nobody has ever used and one that was just emptied
        // were answered identically, field for field, and scene_list_groups
        // requires a target_node so nothing enumerated the groups a scene
        // has. An agent asking for "enemys" instead of "enemies" got a
        // successful empty answer and no second question available to ask
        // (#472).
        //
        // Godot has no empty group to find: membership lives on the nodes,
        // so removing the last member is the same thing as the name never
        // having been used. That is what makes the typo unanswerable, and
        // it is why the fix is the list rather than a flag. The walk
        // already visits every node, so the names in use cost one more
        // call per node, and "enemys" comes back beside "enemies".
        std::set<std::string> known_groups;
        bool known_groups_truncated = false;
        constexpr size_t kMaxReportedGroups = 128;
        std::function<Result<void>(GDExtensionObjectPtr)> visit = [&](GDExtensionObjectPtr current) -> Result<void> {
            auto member_value = callObject(current, "Node", "is_in_group", 2619796661LL, {&group_name.value()});
            if (member_value.isErr()) return member_value.error();
            auto member = scalarFromVariant<GDExtensionBool>(member_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (member.isErr()) return member.error();
            if (member.value()) {
                auto path = logicalPathFromEditedRoot(root.value(), current);
                if (path.isErr()) return path.error();
                members.push_back(path.value());
            }

            auto groups_value = callObject(current, "Node", "get_groups", 3995934104LL);
            if (groups_value.isErr()) return groups_value.error();
            auto group_count_value = callVariant(groups_value.value(), "size");
            if (group_count_value.isErr()) return group_count_value.error();
            auto group_count = scalarFromVariant<int64_t>(group_count_value.value(),
                                                          GDEXTENSION_VARIANT_TYPE_INT);
            if (group_count.isErr()) return group_count.error();
            for (int64_t g = 0; g < group_count.value(); ++g) {
                auto group_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, g);
                if (group_index.isErr()) return group_index.error();
                auto item = callVariant(groups_value.value(), "get", {&group_index.value()});
                if (item.isErr()) return item.error();
                const auto item_type = GodotApi::instance().variant_get_type(item.value().ptr());
                auto text = stringFromVariant(item.value(), item_type);
                if (text.isErr()) continue;
                // A group name starting with _ is Godot's own internal
                // bookkeeping and is not a name a caller would have meant.
                if (text.value().empty() || text.value().front() == '_') continue;
                if (known_groups.size() >= kMaxReportedGroups) {
                    known_groups_truncated = true;
                    break;
                }
                known_groups.insert(boundUtf8(text.value(), 128).value);
            }
            auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
            if (include_internal.isErr()) return include_internal.error();
            auto children = callObject(current, "Node", "get_children", 873284517LL, {&include_internal.value()});
            if (children.isErr()) return children.error();
            auto size_value = callVariant(children.value(), "size");
            if (size_value.isErr()) return size_value.error();
            auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return size.error();
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return index.error();
                auto child_value = callVariant(children.value(), "get", {&index.value()});
                if (child_value.isErr()) return child_value.error();
                auto child = objectFromVariant(child_value.value());
                if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
                auto nested = visit(child.value());
                if (nested.isErr()) return nested;
            }
            return Result<void>::ok();
        };
        auto visited = visit(root.value());
        if (visited.isErr()) return errorJson(visited.error());
        std::sort(members.begin(), members.end());
        json names = json::array();
        for (const auto& name : known_groups) names.push_back(name);
        return liveResult({{"status", "success"}, {"group", group}, {"members", members},
                           // Whether any node in the edited scene is in this
                           // group. It tracks members exactly, because in
                           // Godot that is all a group is; it is here so a
                           // caller can branch without counting an array.
                           // known_groups is the field that answers the typo.
                           {"group_exists", known_groups.count(group) != 0},
                           {"known_groups", std::move(names)},
                           {"known_groups_truncated", known_groups_truncated}});
    }

    auto node = resolveNode(root.value(), params.value("target_node", ""));
    if (node.isErr()) return errorJson(node.error());
    if (method != "scene.listGroups") {
        if (auto refused = refuseUnsavableEdit(root.value(), node.value(),
                                               params.value("target_node", ""),
                                               SceneEdit::Property)) {
            return *refused;
        }
    }
    if (method == "scene.listGroups") {
        auto groups_value = callObject(node.value(), "Node", "get_groups", 3995934104LL);
        if (groups_value.isErr()) return errorJson(groups_value.error());
        auto size_value = callVariant(groups_value.value(), "size");
        if (size_value.isErr()) return errorJson(size_value.error());
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return errorJson(size.error());
        std::vector<std::string> groups;
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return errorJson(index.error());
            auto item = callVariant(groups_value.value(), "get", {&index.value()});
            if (item.isErr()) return errorJson(item.error());
            auto type = GodotApi::instance().variant_get_type(item.value().ptr());
            auto text = stringFromVariant(item.value(), type);
            if (text.isErr()) return errorJson(text.error());
            groups.push_back(text.value());
        }
        std::sort(groups.begin(), groups.end());
        return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                           {"groups", groups}});
    }

    auto group_name = makeStringName(group);
    if (group_name.isErr()) return errorJson(group_name.error());
    auto membership_value = callObject(node.value(), "Node", "is_in_group", 2619796661LL, {&group_name.value()});
    if (membership_value.isErr()) return errorJson(membership_value.error());
    auto membership = scalarFromVariant<GDExtensionBool>(membership_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (membership.isErr()) return errorJson(membership.error());
    const bool adding = method == "scene.addToGroup";
    if (adding && membership.value()) {
    // No retry_with: the node is in the group, so there is no argument
    // that would make the call do something different.
    return errorJson(409, "Target node is already in group: " + group,
                     {{"code", "already_exists"}});
}
    if (!adding && !membership.value()) return errorJson(404, "Target node is not in group: " + group);
    bool original_persistent = params.value("persistent", true);
    if (!adding) {
        NativeName packed_scene_name("PackedScene");
        auto packed_scene = constructObject(packed_scene_name.ptr());
        if (!packed_scene) return errorJson(500, "Godot could not inspect group persistence");
        auto packed_scene_value = makeObject(packed_scene);
        auto root_value = makeObject(root.value());
        if (packed_scene_value.isErr() || root_value.isErr()) return errorJson(500, "Failed to inspect group persistence");
        auto packed = callObject(packed_scene, "PackedScene", "pack", 2584678054LL, {&root_value.value()});
        if (packed.isErr()) return errorJson(packed.error());
        auto pack_code = scalarFromVariant<int64_t>(packed.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (pack_code.isErr() || pack_code.value() != 0) return errorJson(500, "Failed to snapshot scene groups");
        auto state_value = callObject(packed_scene, "PackedScene", "get_state", 3479783971LL);
        if (state_value.isErr()) return errorJson(state_value.error());
        auto state = objectFromVariant(state_value.value());
        if (state.isErr() || !state.value()) return errorJson(500, "PackedScene returned no SceneState");
        auto count_value = callObject(state.value(), "SceneState", "get_node_count", 3905245786LL);
        if (count_value.isErr()) return errorJson(count_value.error());
        auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (count.isErr()) return errorJson(count.error());
        auto relative_path = relativePathWithinEditedRoot(root.value(), node.value());
        if (relative_path.isErr()) return errorJson(relative_path.error());
        original_persistent = false;
        for (int64_t i = 0; i < count.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            auto for_parent = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
            if (index.isErr() || for_parent.isErr()) return errorJson(500, "Failed to inspect SceneState node path");
            auto state_path_value = callObject(state.value(), "SceneState", "get_node_path", 2272487792LL,
                                               {&index.value(), &for_parent.value()});
            if (state_path_value.isErr()) return errorJson(state_path_value.error());
            auto state_path = stringFromVariant(state_path_value.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
            if (state_path.isErr()) return errorJson(state_path.error());
            if (sceneStatePathWithinEditedRoot(state_path.value()) != relative_path.value()) continue;
            auto groups = callObject(state.value(), "SceneState", "get_node_groups", 647634434LL, {&index.value()});
            if (groups.isErr()) return errorJson(groups.error());
            auto group_count_value = callVariant(groups.value(), "size");
            if (group_count_value.isErr()) return errorJson(group_count_value.error());
            auto group_count = scalarFromVariant<int64_t>(group_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (group_count.isErr()) return errorJson(group_count.error());
            for (int64_t group_index = 0; group_index < group_count.value(); ++group_index) {
                auto native_group_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, group_index);
                if (native_group_index.isErr()) return errorJson(native_group_index.error());
                auto stored_group_value = callVariant(groups.value(), "get", {&native_group_index.value()});
                if (stored_group_value.isErr()) return errorJson(stored_group_value.error());
                auto stored_group = stringFromVariant(stored_group_value.value(),
                    GodotApi::instance().variant_get_type(stored_group_value.value().ptr()));
                if (stored_group.isErr()) return errorJson(stored_group.error());
                if (stored_group.value() == group) original_persistent = true;
            }
            break;
        }
    }
    auto persistent = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                 static_cast<GDExtensionBool>(original_persistent));
    if (persistent.isErr()) return errorJson(persistent.error());
    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto preflight = preflightUndoManagerBindings();
    if (preflight.isErr()) return errorJson(preflight.error());
    auto add_bind = requireMethodBind("Node", "add_to_group", 3683006648LL);
    auto remove_bind = requireMethodBind("Node", "remove_from_group", 3304788590LL);
    if (add_bind.isErr()) return errorJson(add_bind.error());
    if (remove_bind.isErr()) return errorJson(remove_bind.error());
    auto action = createAction(manager.value(), adding ? "Didi: add node to group" : "Didi: remove node from group", root.value());
    if (action.isErr()) return errorJson(action.error());
    auto apply = adding
        ? managerMethod(manager.value(), "add_do_method", node.value(), "add_to_group", {&group_name.value(), &persistent.value()})
        : managerMethod(manager.value(), "add_do_method", node.value(), "remove_from_group", {&group_name.value()});
    auto revert = adding
        ? managerMethod(manager.value(), "add_undo_method", node.value(), "remove_from_group", {&group_name.value()})
        : managerMethod(manager.value(), "add_undo_method", node.value(), "add_to_group", {&group_name.value(), &persistent.value()});
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
    if (takePhase7SignalTestSeam("group_undo_registration_failure")) {
        apply = Error::internal("The test seam failed the registration");
    }
#endif
    if (apply.isErr() || revert.isErr()) {
        // Closed, so the next mutation does not merge into it (#1152).
        abandonAction(manager.value());
        return errorJson(500, "Failed to register group UndoRedo transaction");
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(committed.error());
    // Membership read again after the commit, and the answer built from
    // it. added and removed were constants, and membership was read only
    // before the write, so a commit the node did not take read as done
    // (#1019).
    auto after_value = callObject(node.value(), "Node", "is_in_group", 2619796661LL, {&group_name.value()});
    if (after_value.isErr()) return errorJson(after_value.error());
    auto in_group = scalarFromVariant<GDExtensionBool>(after_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (in_group.isErr()) return errorJson(in_group.error());
    if (static_cast<bool>(in_group.value()) != adding) {
        return errorJson(500, std::string("The group change was committed and the node is ") +
                                  (in_group.value() ? "still" : "not") + " in group " + group +
                                  " afterwards");
    }
    // Membership as it was read before the commit (#1151). A removal also
    // says whether the scene file kept the membership, which its undo
    // puts back.
    json before_group = {{"in_group", static_cast<bool>(membership.value())}};
    if (!adding) before_group["persistent"] = original_persistent;
    return liveSceneMutation({{"status", "success"}, {"target_node", params.value("target_node", "")},
                              {"group", group}, {"in_group", static_cast<bool>(in_group.value())},
                              {"added", adding}, {"removed", !adding},
                              {"before", std::move(before_group)},
                              {"undo_redo_registered", true}});
}


} // namespace didi::godot
