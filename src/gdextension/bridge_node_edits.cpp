#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include <functional>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;
namespace {
Result<std::vector<GDExtensionObjectPtr>> collectNodesOwnedBy(GDExtensionObjectPtr root,
                                                              GDExtensionObjectPtr node) {
    std::vector<GDExtensionObjectPtr> owned;
    std::vector<GDExtensionObjectPtr> pending{node};
    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    while (!pending.empty()) {
        GDExtensionObjectPtr current = pending.back();
        pending.pop_back();
        auto owner_value = callObject(current, "Node", "get_owner", 3160264692LL);
        if (owner_value.isErr()) return owner_value.error();
        auto owner = objectFromVariant(owner_value.value());
        if (owner.isErr()) return owner.error();
        if (owner.value() == root) owned.push_back(current);
        auto children = callObject(current, "Node", "get_children", 873284517LL, {&include_internal.value()});
        if (children.isErr()) return children.error();
        auto size_value = callVariant(children.value(), "size");
        if (size_value.isErr()) return size_value.error();
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return index.error();
            auto child_variant = callVariant(children.value(), "get", {&index.value()});
            if (child_variant.isErr()) return child_variant.error();
            auto child = objectFromVariant(child_variant.value());
            if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
            pending.push_back(child.value());
        }
    }
    return owned;
}


// The descendants of a freshly duplicated branch that the edited root has to
// own, which is every node whose counterpart in the source branch it owns.
// Node::duplicate returns the copy's root owned by nobody and every descendant
// the same, so the caller sets the root's owner and this finds the rest;
// without it PackedScene::pack kept only the copy's root and the next save
// dropped the whole subtree (#659).
//
// Mirroring the source is the rule rather than owning everything, because two
// kinds of descendant must not be owned: a child added at runtime that the
// scene never held, which would start being saved, and a node inside an
// instantiated sub-scene, which is owned by that instance and would be
// flattened out of it. Checked against 4.5.1, 4.6.2 and 4.7.2, which agree.
//
// The two trees have the same shape by construction. A divergence stops the
// walk rather than pairing nodes by position that are not each other's copy.
Result<std::vector<GDExtensionObjectPtr>> collectDuplicateDescendantsToOwn(
    GDExtensionObjectPtr root, GDExtensionObjectPtr source, GDExtensionObjectPtr duplicate) {
    std::vector<GDExtensionObjectPtr> to_own;
    std::function<Result<void>(GDExtensionObjectPtr, GDExtensionObjectPtr, bool)> visit =
        [&](GDExtensionObjectPtr source_node, GDExtensionObjectPtr duplicate_node,
            bool is_branch_root) -> Result<void> {
        if (!is_branch_root) {
            auto owner_value = callObject(source_node, "Node", "get_owner", 3160264692LL);
            if (owner_value.isErr()) return owner_value.error();
            auto owner = objectFromVariant(owner_value.value());
            if (owner.isErr()) return owner.error();
            if (owner.value() == root) to_own.push_back(duplicate_node);
        }
        auto source_children = childNodes(source_node);
        if (source_children.isErr()) return source_children.error();
        auto duplicate_children = childNodes(duplicate_node);
        if (duplicate_children.isErr()) return duplicate_children.error();
        if (source_children.value().size() != duplicate_children.value().size()) {
            return Error::internal("Godot's duplicate does not match the branch it copied");
        }
        for (size_t i = 0; i < source_children.value().size(); ++i) {
            auto nested = visit(source_children.value()[i], duplicate_children.value()[i], false);
            if (nested.isErr()) return nested;
        }
        return Result<void>::ok();
    };
    auto visited = visit(source, duplicate, true);
    if (visited.isErr()) return visited.error();
    return to_own;
}

} // namespace

json GodotBridge::bridgeSceneNodeEdits(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto node = resolveNode(root.value(), params.value("target_node", ""));
    if (node.isErr()) return errorJson(node.error());
    if (node.value() == root.value()) return errorJson(400, "Cannot mutate the edited scene root");
    if (auto refused = refuseUnsavableEdit(root.value(), node.value(),
                                           params.value("target_node", ""),
                                           SceneEdit::Structure)) {
        return *refused;
    }
    auto parent_variant = callObject(node.value(), "Node", "get_parent", 3160264692LL);
    if (parent_variant.isErr()) return errorJson(parent_variant.error());
    auto parent = objectFromVariant(parent_variant.value());
    if (parent.isErr() || !parent.value()) return errorJson(400, "Cannot mutate the edited scene root");
    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return errorJson(include_internal.error());
    auto old_index = callObject(node.value(), "Node", "get_index", 894402480LL, {&include_internal.value()});
    if (old_index.isErr()) return errorJson(old_index.error());
    // Where the node was, read before anything moves it, so the change
    // journal can say what a removal or a reparent replaced (#1151).
    json before_node = json::object();
    if (auto path = logicalPathFromEditedRoot(root.value(), node.value()); path.isOk()) {
        before_node["path"] = path.value();
    }
    if (auto path = logicalPathFromEditedRoot(root.value(), parent.value()); path.isOk()) {
        before_node["parent"] = path.value();
    }
    if (auto index = scalarFromVariant<int64_t>(old_index.value(), GDEXTENSION_VARIANT_TYPE_INT); index.isOk()) {
        before_node["index"] = index.value();
    }
    before_node["type"] = nodeClassName(node.value());
    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto child = makeObject(node.value());
    auto readable = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
    auto internal = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (child.isErr() || readable.isErr() || internal.isErr()) return errorJson(500, "Failed to construct scene transaction arguments");

    if (method == "scene.removeNode") {
        auto preflight = preflightNodeUndoTransaction();
        if (preflight.isErr()) return errorJson(preflight.error());
        auto action = createAction(manager.value(), "Didi: remove node", root.value());
        auto keep = managerReference(manager.value(), "add_undo_reference", node.value());
        auto remove = managerMethod(manager.value(), "add_do_method", parent.value(), "remove_child", {&child.value()});
        auto restore = managerMethod(manager.value(), "add_undo_method", parent.value(), "add_child",
                                     {&child.value(), &readable.value(), &internal.value()});
        auto restore_index = managerMethod(manager.value(), "add_undo_method", parent.value(), "move_child",
                                           {&child.value(), &old_index.value()});
        // Godot clears the owner of every node whose owner is no longer an
        // ancestor when a branch leaves the tree, so the undo put the node
        // back without its ownership and the next save would have dropped
        // it, and every mutation after the undo now refuses it as not
        // owned. The editor's own delete restores owners on undo, and so
        // does this. Registered after add_child, because undo operations
        // run in the order they were added and set_owner needs the node in
        // the tree.
        auto owned = collectNodesOwnedBy(root.value(), node.value());
        auto owner_value = makeObject(root.value());
        bool restore_owners = owned.isOk() && owner_value.isOk();
        if (restore_owners) {
            for (auto owned_node : owned.value()) {
                auto restore_owner = managerMethod(manager.value(), "add_undo_method", owned_node,
                                                   "set_owner", {&owner_value.value()});
                if (restore_owner.isErr()) {
                    restore_owners = false;
                    break;
                }
            }
        }
        if (action.isErr() || keep.isErr() || remove.isErr() || restore_index.isErr() || restore.isErr() ||
            !restore_owners) {
            if (action.isOk()) abandonAction(manager.value());
            return errorJson(500, "Failed to register remove UndoRedo transaction");
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error());
        // Read after the commit: the path the call named no longer
        // resolves. The answer used to be `action` alone, with nothing read
        // after the change (#1019).
        const std::string target = params.value("target_node", "");
        if (resolveNode(root.value(), target).isOk()) {
            return errorJson(500, "The remove was committed and " + target + " still resolves");
        }
        return liveSceneMutation({{"status", "success"}, {"action", "remove_node"}, {"exists", false},
                                  {"before", std::move(before_node)},
                                  {"undo_redo_registered", true}});
    }

    if (method == "scene.reparentNode") {
        auto new_parent = resolveNode(root.value(), params.value("new_parent_path", ""));
        if (new_parent.isErr()) return errorJson(new_parent.error());
        if (auto refused = refuseUnsavableEdit(root.value(), new_parent.value(),
                                               params.value("new_parent_path", ""),
                                               SceneEdit::AddChild)) {
            return *refused;
        }
        if (new_parent.value() == node.value()) {
            return errorJson(400, "Cannot reparent a node to itself");
        }
        auto new_parent_value = makeObject(new_parent.value());
        auto old_parent_value = makeObject(parent.value());
        auto keep_global = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                      static_cast<GDExtensionBool>(params.value("keep_global_transform", true)));
        if (new_parent_value.isErr() || old_parent_value.isErr() || keep_global.isErr()) {
            return errorJson(500, "Failed to construct reparent transaction arguments");
        }
        auto descendant_check = callObject(node.value(), "Node", "is_ancestor_of", 3093956946LL,
                                           {&new_parent_value.value()});
        if (descendant_check.isErr()) {
            return errorJson(descendant_check.error());
        }
        auto new_parent_is_descendant = scalarFromVariant<GDExtensionBool>(
            descendant_check.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (new_parent_is_descendant.isErr()) {
            return errorJson(new_parent_is_descendant.error());
        }
        if (new_parent_is_descendant.value()) {
            return errorJson(400, "Cannot reparent a node beneath one of its descendants");
        }
        // Node.reparent adds the node without asking for a readable name,
        // so a parent that already had a child of that name gave it one
        // like @Node3D@20131. Setting the same name again once it has moved
        // makes Godot pick a readable one, Twin2, as the editor's own
        // reparent does, and setting it back on undo restores it (#1126).
        auto original_name = nodeString(node.value(), "get_name", 2002593661LL);
        if (original_name.isErr()) return errorJson(original_name.error());
        auto name_value = makeStringName(original_name.value());
        if (name_value.isErr()) return errorJson(name_value.error());
        auto preflight = preflightNodeUndoTransaction();
        if (preflight.isErr()) return errorJson(preflight.error());
        auto action = createAction(manager.value(), "Didi: reparent node", root.value());
        if (action.isErr()) return errorJson(action.error());
        auto move = managerMethod(manager.value(), "add_do_method", node.value(), "reparent",
                                  {&new_parent_value.value(), &keep_global.value()});
        auto rename = managerMethod(manager.value(), "add_do_method", node.value(), "set_name",
                                    {&name_value.value()});
        auto restore = managerMethod(manager.value(), "add_undo_method", node.value(), "reparent",
                                     {&old_parent_value.value(), &keep_global.value()});
        auto restore_name = managerMethod(manager.value(), "add_undo_method", node.value(), "set_name",
                                          {&name_value.value()});
        auto restore_index = managerMethod(manager.value(), "add_undo_method", parent.value(), "move_child",
                                           {&child.value(), &old_index.value()});
        if (move.isErr() || rename.isErr() || restore.isErr() || restore_name.isErr() || restore_index.isErr()) {
            abandonAction(manager.value());
            return errorJson(500, "Failed to register reparent UndoRedo transaction");
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error());
        // Where the node is now, read from the node after the commit. The
        // answer used to be `action` alone, so the caller had to guess the
        // new path to reach the node again (#1019).
        auto moved_parent_value = callObject(node.value(), "Node", "get_parent", 3160264692LL);
        auto moved_parent = moved_parent_value.isOk() ? objectFromVariant(moved_parent_value.value())
                                                      : Result<GDExtensionObjectPtr>(moved_parent_value.error());
        if (moved_parent.isErr() || moved_parent.value() != new_parent.value()) {
            const std::string wanted_parent = params.value("new_parent_path", std::string());
            return errorJson(500, "The reparent was committed and the node is not under " +
                                      wanted_parent + " afterwards");
        }
        auto moved_path = logicalPathFromEditedRoot(root.value(), node.value());
        if (moved_path.isErr()) return errorJson(moved_path.error());
        json result = {{"status", "success"}, {"action", "reparent_node"},
                       {"node_path", moved_path.value()}, {"undo_redo_registered", true}};
        before_node.erase("index");
        before_node.erase("type");
        result["before"] = std::move(before_node);
        // In scene_instantiate_node's words, so a caller building its next
        // path from the old name learns why it no longer resolves.
        auto moved_name = nodeString(node.value(), "get_name", 2002593661LL);
        if (moved_name.isOk()) {
            result["node_name"] = moved_name.value();
            if (moved_name.value() != original_name.value()) {
                result["requested_name"] = original_name.value();
                result["name_substituted"] = true;
            }
        }
        return liveSceneMutation(std::move(result));
    }

    auto flags = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(15));
    if (flags.isErr()) return errorJson(flags.error());
    auto duplicated = callObject(node.value(), "Node", "duplicate", 3511555459LL, {&flags.value()});
    if (duplicated.isErr()) return errorJson(duplicated.error());
    auto duplicate_node = objectFromVariant(duplicated.value());
    if (duplicate_node.isErr() || !duplicate_node.value()) return errorJson(500, "Godot failed to duplicate node");
    // The copy keeps the source's name, and add_child below asks Godot for a
    // readable unique one, as the editor's Duplicate does: NE becomes NE2,
    // following the project's editor/naming settings. A fixed Copy suffix gave
    // NECopy, a name a person never gets from the editor (#1319).
    auto duplicate_name = nodeString(duplicate_node.value(), "get_name", 2002593661LL);
    if (duplicate_name.isErr()) {
        GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(duplicate_name.error());
    }
    auto logical_parent = logicalPathFromEditedRoot(root.value(), parent.value());
    if (logical_parent.isErr()) {
        GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(logical_parent.error());
    }
    auto duplicate_value = makeObject(duplicate_node.value());
    auto owner = makeObject(root.value());
    if (duplicate_value.isErr() || owner.isErr()) {
        GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(500, "Failed to construct duplicate transaction arguments");
    }
    auto descendants_to_own =
        collectDuplicateDescendantsToOwn(root.value(), node.value(), duplicate_node.value());
    if (descendants_to_own.isErr()) {
        GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(descendants_to_own.error());
    }
    auto preflight = preflightNodeUndoTransaction();
    if (preflight.isErr()) {
        GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(preflight.error());
    }
    auto action = createAction(manager.value(), "Didi: duplicate node", root.value());
    if (action.isErr()) {
        GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(action.error());
    }
    auto keep = managerReference(manager.value(), "add_do_reference", duplicate_node.value());
    auto add = managerMethod(manager.value(), "add_do_method", parent.value(), "add_child",
                             {&duplicate_value.value(), &readable.value(), &internal.value()});
    auto own = managerMethod(manager.value(), "add_do_method", duplicate_node.value(), "set_owner", {&owner.value()});
    // The copy's root is not the whole copy. Godot leaves every duplicated
    // descendant unowned and the packer keeps only what the edited root
    // owns, so a branch duplicated here reached the file as a bare root
    // and the children the editor was showing were gone (#659). Registered
    // after add_child, because UndoRedo runs do operations in the order
    // they were added and set_owner needs the node in the tree; that is
    // the same ordering rule scene.removeNode states for its undo side.
    bool owned_descendants = true;
    for (auto descendant : descendants_to_own.value()) {
        auto own_descendant =
            managerMethod(manager.value(), "add_do_method", descendant, "set_owner", {&owner.value()});
        if (own_descendant.isErr()) {
            owned_descendants = false;
            break;
        }
    }
    auto remove = managerMethod(manager.value(), "add_undo_method", parent.value(), "remove_child", {&duplicate_value.value()});
    if (keep.isErr() || add.isErr() || own.isErr() || !owned_descendants || remove.isErr()) {
        abandonAction(manager.value());
        if (keep.isErr()) GodotApi::instance().object_destroy(duplicate_node.value());
        return errorJson(500, "Failed to register duplicate UndoRedo transaction");
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(committed.error());
    // duplicate_name was read before add_child, so it is the requested name
    // rather than the one Godot settled on. Read the path back from the tree.
    auto duplicate_path = logicalPathFromEditedRoot(root.value(), duplicate_node.value());
    json result = {{"status", "success"}, {"action", "duplicate_node"},
                   {"duplicated_node",
                    duplicate_path.isOk()
                        ? duplicate_path.value()
                        : logical_parent.value() + "/" + duplicate_name.value()},
                   {"undo_redo_registered", true}};
    if (duplicate_path.isErr()) result["node_path_verified"] = false;
    return liveSceneMutation(result);
}

} // namespace didi::godot
