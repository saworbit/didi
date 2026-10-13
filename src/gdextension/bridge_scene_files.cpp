#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/common/godot_error.hpp"
#include "didi/common/project_path.hpp"
#include <algorithm>
#include <filesystem>
#include <functional>

namespace didi::godot {
using namespace bridge_internal;

json GodotBridge::bridgeSceneFiles(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    if (method == "scene.close") {
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error());
        auto path_value = callObject(root.value(), "Node", "get_scene_file_path", 201670096LL);
        if (path_value.isErr()) return errorJson(path_value.error());
        auto path = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (path.isErr()) return errorJson(path.error());
        const bool discard_unsaved = params.value("discard_unsaved", false);
        // Godot 4.5 and 4.6 expose only the write side of dirty state
        // (mark_scene_as_unsaved). The read side, get_unsaved_scenes,
        // arrives in 4.7, so this is a runtime bind probe rather than a
        // version-string comparison: the guard stays wherever it is absent.
        const bool dirty_state_readable =
            unsavedScenesBindAvailable();
        bool verified_clean = false;
        if (!discard_unsaved) {
            if (!dirty_state_readable) {
                return errorJson(409, "This Godot build cannot report active-scene dirty state; "
                                      "EditorInterface.get_unsaved_scenes arrives in Godot 4.7. "
                                      "Pass discard_unsaved: true to close explicitly",
                                 {{"code", "dirty_state_unavailable"},
                                  {"retry_with", {{"discard_unsaved", true}}}});
            }
            if (path.value().empty()) {
                return errorJson(409, "The active scene has never been saved, so the engine cannot "
                                      "report it as clean; save it with editor_save_scene or pass "
                                      "discard_unsaved: true to close explicitly",
                                 {{"code", "scene_never_saved"},
                                  {"retry_with", {{"discard_unsaved", true}}}});
            }
            auto unsaved = callObject(editor, "EditorInterface", "get_unsaved_scenes", 1139954409LL);
            if (unsaved.isErr()) return errorJson(unsaved.error());
            auto unsaved_size = callVariant(unsaved.value(), "size");
            if (unsaved_size.isErr()) return errorJson(500, unsaved_size.error().message);
            auto unsaved_count = scalarFromVariant<int64_t>(unsaved_size.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (unsaved_count.isErr()) return errorJson(500, unsaved_count.error().message);
            // The list holds one entry per open scene, so this bound is far
            // above anything an editor produces. Refusing past it matters
            // because a partial scan cannot prove the active scene absent.
            constexpr int64_t kMaxUnsavedScenesScanned = 1024;
            if (unsaved_count.value() > kMaxUnsavedScenesScanned) {
                return errorJson(409, "The editor reports more unsaved scenes than Didi will scan (" +
                                      std::to_string(unsaved_count.value()) + "), so this scene cannot be "
                                      "proven clean; pass discard_unsaved: true to close explicitly",
                                 {{"code", "unsaved_scan_limit"},
                                  {"retry_with", {{"discard_unsaved", true}}}});
            }
            for (int64_t index = 0; index < unsaved_count.value(); ++index) {
                auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
                if (index_value.isErr()) return errorJson(500, index_value.error().message);
                auto entry = callVariant(unsaved.value(), "get", {&index_value.value()});
                if (entry.isErr()) return errorJson(500, entry.error().message);
                auto entry_path = stringFromVariant(
                    entry.value(), GodotApi::instance().variant_get_type(entry.value().ptr()));
                if (entry_path.isErr()) return errorJson(500, entry_path.error().message);
                if (entry_path.value() == path.value()) {
                    return errorJson(409, "The active scene has unsaved changes: " + path.value() +
                                          "; save it with editor_save_scene or pass "
                                          "discard_unsaved: true to discard them",
                                     {{"code", "unsaved_changes"},
                                      {"retry_with", {{"discard_unsaved", true}}}});
                }
            }
            verified_clean = true;
        }
        auto closed = callObject(editor, "EditorInterface", "close_scene", 166280745LL);
        if (closed.isErr()) return errorJson(closed.error());
        auto code = scalarFromVariant<int64_t>(closed.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (code.isErr()) return errorJson(code.error());
        if (code.value() != 0) {
            return errorJson(500, "Godot close_scene failed with " +
                                      ::didi::godot::describeGodotError(code.value()));
        }
        // Whether a tab still holds the scene, read after the close rather
        // than taken from close_scene's return code (#1019). A scene that
        // was never saved has no path to look for, so it is null.
        json still_open = nullptr;
        if (!path.value().empty()) {
            auto open = openScenePaths(editor);
            if (open.isErr()) {
                return errorJson(500, "Godot closed the scene, and the open tabs could not be "
                                      "read back to confirm it: " + open.error().message);
            }
            still_open = std::find(open.value().begin(), open.value().end(), path.value()) !=
                         open.value().end();
        }
        return liveResult({{"status", "success"}, {"closed", true}, {"scene_path", path.value()},
                           {"still_open", still_open},
                           {"discarded_unsaved", discard_unsaved},
                           {"dirty_state_readable", dirty_state_readable},
                           {"dirty_state", verified_clean ? "clean" : "unchecked"}});
    }

    const std::string scene_path = params.value("scene_path", "");
    auto valid_path = validateResPath(scene_path, ".tscn");
    if (valid_path.isErr()) return errorJson(valid_path.error());
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return errorJson(loader.error());
    auto path = makeString(scene_path);
    auto packed_hint = makeString("PackedScene");
    if (path.isErr() || packed_hint.isErr()) return errorJson(500, "Failed to construct scene resource arguments");
    auto exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                   {&path.value(), &packed_hint.value()});
    if (exists_value.isErr()) return errorJson(exists_value.error());
    auto target_exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (target_exists.isErr()) return errorJson(target_exists.error());

    auto open_and_verify = [&]() -> Result<void> {
        auto inherited = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        if (inherited.isErr()) return inherited.error();
        auto opened = callObject(editor, "EditorInterface", "open_scene_from_path", 1168363258LL,
                                 {&path.value(), &inherited.value()});
        if (opened.isErr()) return opened.error();
        auto opened_root = editedSceneRoot(editor);
        if (opened_root.isErr()) return opened_root.error();
        auto opened_path_value = callObject(opened_root.value(), "Node", "get_scene_file_path", 201670096LL);
        if (opened_path_value.isErr()) return opened_path_value.error();
        auto opened_path = stringFromVariant(opened_path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (opened_path.isErr()) return opened_path.error();
        if (opened_path.value() != scene_path) {
            return Error::internal("Godot did not activate the requested scene: " + scene_path);
        }
        return Result<void>::ok();
    };

    if (method == "scene.open") {
        if (!target_exists.value()) return errorJson(404, "PackedScene not found: " + scene_path);
        auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
        if (cache_mode.isErr()) return errorJson(cache_mode.error());
        auto resource = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                   {&path.value(), &packed_hint.value(), &cache_mode.value()});
        if (resource.isErr()) return errorJson(resource.error());
        auto packed = objectFromVariant(resource.value());
        if (packed.isErr() || !packed.value()) {
            return errorJson(422, "Resource is not a loadable PackedScene: " + scene_path,
                             {{"code", "not_a_packed_scene"}});
        }
        auto class_name = makeString("PackedScene");
        auto class_value = class_name.isOk()
            ? callObject(packed.value(), "Object", "is_class", 3927539163LL, {&class_name.value()})
            : Result<VariantValue>(class_name.error());
        auto is_packed = class_value.isOk()
            ? scalarFromVariant<GDExtensionBool>(class_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
            : Result<GDExtensionBool>(class_value.error());
        if (is_packed.isErr() || !is_packed.value()) {
            return errorJson(422, "Resource is not a PackedScene: " + scene_path,
                             {{"code", "not_a_packed_scene"}});
        }
        auto opened = open_and_verify();
        if (opened.isErr()) return errorJson(opened.error());
        return liveResult({{"status", "success"}, {"opened", true}, {"scene_path", scene_path}});
    }

    if (target_exists.value() && !params.value("overwrite", false)) {
        return errorJson(409, "Scene target already exists; pass overwrite: true to replace it",
                              {{"code", "already_exists"}, {"retry_with", {{"overwrite", true}}}});
    }

    // Only the two writers reach this line, and ResourceSaver cannot create
    // the directory it writes into: a missing parent came back as a bare
    // Error 19 with nothing in it naming the cause. script_create and
    // resource_create both create theirs. validateResPath has already
    // refused traversal, absolute paths and backslashes, so the target is
    // project-contained by construction and the check below is a second
    // reading of that rather than the only one.
    {
        namespace fs = std::filesystem;
        auto project_path = resolveGodotProjectPath();
        if (project_path.isErr()) {
            return errorJson(project_path.error());
        }
        std::error_code ec;
        const auto root = fs::weakly_canonical(
            didi::paths::projectPathFromUtf8(project_path.value()), ec);
        if (ec || !fs::is_directory(root, ec)) {
            return errorJson(500, "Godot project root is unavailable");
        }
        const auto target = root / didi::paths::projectPathFromUtf8(scene_path.substr(6));
        const auto parent = target.parent_path();
        if (!pathWithin(root, parent)) {
            return errorJson(400, "Scene path resolves outside the project: " + scene_path);
        }
        std::error_code directory_error;
        fs::create_directories(parent, directory_error);
        if (directory_error && !fs::is_directory(parent, ec)) {
            return errorJson(500, "Cannot create the directory for " + scene_path + ": " +
                                  directory_error.message());
        }
    }

    GDExtensionObjectPtr packed_root = nullptr;
    if (method == "scene.create") {
        // Any ClassDB type that inherits Node, which is what
        // scene_instantiate_node has always taken.
        //
        // The enum was Node2D, Node3D and Control, and almost every scene
        // in a real project has a root outside it: a player is a
        // CharacterBody2D, a pickup an Area2D, terrain a StaticBody2D, a
        // HUD a CanvasLayer. The route that worked was to create a throwaway
        // Node2D scene, instantiate the type you wanted under it, build the
        // subtree there and scene_pack_branch it to the real path -- four
        // calls, nothing saying so, and a scratch scene left on disk that
        // the caller then has to remember to delete (#740).
        const std::string root_type = params.value("root_type", "Node2D");
        if (root_type.empty() || root_type.size() > 128) {
            return errorJson(400, "root_type must be a Godot class name that inherits Node");
        }
        const std::string root_name = params.value("root_name", "Root");
        if (root_name.empty() || root_name.find('/') != std::string::npos || root_name.find('\\') != std::string::npos) {
            return errorJson(400, "root_name must be a non-empty node name without path separators");
        }
        if (!engineHasClass(root_type)) {
            return errorJson(400, "Godot ClassDB could not instantiate scene root type: " +
                                      root_type + ". The engine has no class by that name.");
        }
        NativeName native_type(root_type);
        packed_root = constructObject(native_type.ptr());
        if (!packed_root) {
            return errorJson(400, "Godot ClassDB could not instantiate scene root type: " +
                                      root_type);
        }
        {
            // A Variant type or a RefCounted resource constructs and is not
            // a node, and a .tscn whose root is not a Node cannot be
            // instanced into anything. Same check, same words, as the
            // sibling that instantiates one.
            auto node_class = makeString("Node");
            auto is_node_variant = node_class.isOk()
                ? callObject(packed_root, "Object", "is_class", 3927539163LL, {&node_class.value()})
                : Result<VariantValue>(node_class.error());
            auto is_node = is_node_variant.isOk()
                ? scalarFromVariant<GDExtensionBool>(is_node_variant.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                : Result<GDExtensionBool>(is_node_variant.error());
            if (is_node.isErr() || !is_node.value()) {
                GodotApi::instance().object_destroy(packed_root);
                return is_node.isErr()
                    ? errorJson(is_node.error())
                    : errorJson(400, "Godot ClassDB type does not inherit Node: " + root_type);
            }
        }
        auto name = makeStringName(root_name);
        auto named = name.isOk()
            ? callObject(packed_root, "Node", "set_name", 3304788590LL, {&name.value()})
            : Result<VariantValue>(name.error());
        if (named.isErr()) {
            GodotApi::instance().object_destroy(packed_root);
            return errorJson(named.error());
        }
    } else {
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error());
        auto target = resolveNode(root.value(), params.value("target_node", ""));
        if (target.isErr()) return errorJson(target.error());
        auto flags = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(15));
        if (flags.isErr()) return errorJson(flags.error());
        auto duplicated = callObject(target.value(), "Node", "duplicate", 3511555459LL, {&flags.value()});
        if (duplicated.isErr()) return errorJson(duplicated.error());
        auto duplicate_object = objectFromVariant(duplicated.value());
        if (duplicate_object.isErr() || !duplicate_object.value()) return errorJson(500, "Godot failed to duplicate the branch");
        packed_root = duplicate_object.value();
        auto owner = makeObject(packed_root);
        if (owner.isErr()) {
            GodotApi::instance().object_destroy(packed_root);
            return errorJson(owner.error());
        }
        std::function<Result<void>(GDExtensionObjectPtr)> normalize_owner = [&](GDExtensionObjectPtr current) -> Result<void> {
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
                if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid duplicate child");
                auto owned = callObject(child.value(), "Node", "set_owner", 1078189570LL, {&owner.value()});
                if (owned.isErr()) return owned.error();
                auto nested = normalize_owner(child.value());
                if (nested.isErr()) return nested;
            }
            return Result<void>::ok();
        };
        auto normalized = normalize_owner(packed_root);
        if (normalized.isErr()) {
            GodotApi::instance().object_destroy(packed_root);
            return errorJson(normalized.error());
        }
    }

    NativeName packed_scene_name("PackedScene");
    auto packed_scene = constructObject(packed_scene_name.ptr());
    if (!packed_scene) {
        GodotApi::instance().object_destroy(packed_root);
        return errorJson(500, "Godot could not construct PackedScene");
    }
    auto root_value = makeObject(packed_root);
    auto packed_value = makeObject(packed_scene);
    if (root_value.isErr() || packed_value.isErr()) {
        GodotApi::instance().object_destroy(packed_root);
        return errorJson(500, "Failed to construct PackedScene arguments");
    }
    auto packed = callObject(packed_scene, "PackedScene", "pack", 2584678054LL, {&root_value.value()});
    if (packed.isErr()) {
        GodotApi::instance().object_destroy(packed_root);
        return errorJson(packed.error());
    }
    auto pack_code = scalarFromVariant<int64_t>(packed.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (pack_code.isErr() || pack_code.value() != 0) {
        GodotApi::instance().object_destroy(packed_root);
        return errorJson(500, "PackedScene.pack failed");
    }
    auto saver = singleton("ResourceSaver");
    auto flags = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (saver.isErr() || flags.isErr()) {
        GodotApi::instance().object_destroy(packed_root);
        return errorJson(500, "ResourceSaver is unavailable");
    }
    auto saved = callObject(saver.value(), "ResourceSaver", "save", 2983274697LL,
                            {&packed_value.value(), &path.value(), &flags.value()});
    GodotApi::instance().object_destroy(packed_root);
    if (saved.isErr()) return errorJson(saved.error());
    auto save_code = scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (save_code.isErr()) return errorJson(save_code.error());
    if (save_code.value() != 0) {
        // A 300-character filename reached ResourceSaver and came back as
        // "Error 19": a 500 that says the server broke, with no next move
        // and no name on the number. The engine's file-and-path errors are
        // the caller's argument to fix, so they answer 400 (#535).
        const auto engine_code = save_code.value();
        const bool caller_path = ::didi::godot::isGodotPathError(engine_code);
        return errorJson(caller_path ? 400 : 500,
                         "ResourceSaver.save could not write " + scene_path + ": " +
                             ::didi::godot::describeGodotError(engine_code) +
                             (caller_path ? ". Check that scene_path is a writable res:// "
                                            "path and that its name is not too long."
                                          : ""));
    }

    // The file is on disk with a uid in its header. Whether the engine
    // knows that uid is a separate question, and the answer is no whenever
    // the editor filesystem happens to be scanning.
    const auto uid_state = registerWrittenResourceUid(scene_path);
    const auto uidFields = [&](json result) {
        if (!uid_state.uid.empty()) result["uid"] = uid_state.uid;
        result["uid_registered"] = uid_state.registered;
        if (uid_state.deferred) {
            result["uid_registration_deferred"] = true;
            result["limitation"] =
                "The scene is saved and carries its uid, but the editor had not indexed "
                "it when this answered: it was still scanning, or still listing a folder "
                "new to it. Anything loading a scene that references this one warns and "
                "falls back to the text path until it has. Didi indexes it once the "
                "editor has applied the scan.";
        }
        return result;
    };

    if (method == "scene.create") {
        // Everything below this point runs after ResourceSaver.save returned
        // Error 0, so the .tscn is already on disk. A bare error here told
        // the caller the create failed; the retry with overwrite:false then
        // hit 409 already exists and the agent concluded nothing had worked.
        // Carry saved: true through every one of these failures.
        const auto openFailure = [&](const Error& error) {
            json failure = errorJson(error.code, "The scene file was written but could not be "
                                                 "opened in the editor: " + error.message);
            failure["error"]["data"] = {{"saved", true}, {"opened", false},
                                        {"scene_path", scene_path}};
            return failure;
        };
        // What was open before this call replaces it. `opened: true` said
        // the new scene was open and nothing said the old one no longer
        // was, so every later scene_* call answered about a different file
        // with no field naming either one. Read before anything moves it.
        auto previous_root = editedSceneRoot(editor);
        const std::string previous_scene =
            previous_root.isOk() ? editedScenePath(previous_root.value()) : std::string();
        bool tab_stale = false;
        if (target_exists.value()) {
            auto replace_cache = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(4));
            if (replace_cache.isErr()) return openFailure(replace_cache.error());
            auto refreshed = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                        {&path.value(), &packed_hint.value(), &replace_cache.value()});
            if (refreshed.isErr()) return openFailure(refreshed.error());
            auto open_root = openSceneRootId(editor, scene_path);
            if (open_root.isErr()) return openFailure(open_root.error());
            tab_stale = open_root.value() != 0;
        }
        // A tab that holds the scene still has the tree from before the
        // write. The open below makes it current, and the server rebuilds
        // it from the file on a later request, through the reload every
        // writer uses. Rebuilding it here first left the editor switching
        // scenes for the rest of the frame on 4.5 and 4.6, so the open did
        // nothing and a tab right of the current one never came to the
        // front (#1079). A current tab stays current when it is rebuilt.
        auto opened = open_and_verify();
        if (opened.isErr()) return openFailure(opened.error());
        json created = uidFields({{"status", "success"}, {"saved", true}, {"opened", true},
                                  {"scene_path", scene_path}});
        created["edited_scene_changed"] = previous_scene != scene_path;
        created["previous_scene_file_path"] =
            previous_scene.empty() ? json(nullptr) : json(previous_scene);
        created["scene_file_path"] = scene_path;
        if (tab_stale) created["scene_tab_stale"] = true;
        return liveResult(created);
    }
    return liveResult(uidFields({{"status", "success"}, {"saved", true},
                                 {"scene_path", scene_path},
                                 {"source_node", params.value("target_node", "")}}));
}

} // namespace didi::godot
