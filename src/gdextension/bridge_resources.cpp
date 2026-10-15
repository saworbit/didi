#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include <algorithm>
#include <tuple>
#include <utility>

namespace didi::godot::bridge_internal {
// Everything script.attachToNode checks before it touches the undo history,
// in one place, so the preview refuses the way the real call does (#603).
Result<AttachableScript> loadAttachableScript(GDExtensionObjectPtr node,
                                              const std::string& script_path) {
    auto valid_path = validateScriptPath(script_path);
    if (valid_path.isErr()) return valid_path.error();
    auto old_script = callObject(node, "Object", "get_script", 1214101251LL);
    if (old_script.isErr()) return old_script.error();
    if (GodotApi::instance().variant_get_type(old_script.value().ptr()) == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto existing = objectFromVariant(old_script.value());
        if (existing.isErr()) return existing.error();
        if (existing.value()) {
            const auto held = resourcePathOf(existing.value());
            json data = {{"code", "script_already_attached"}, {"retryable", false}};
            if (!held.empty()) data["script_path"] = held;
            return Error(409,
                         "Target node already has a script" +
                             (held.empty() ? std::string() : " (" + held + ")") +
                             "; detach it with script_detach_from_node before attaching another",
                         std::move(data));
        }
    }
    if (!resourceFileExists(script_path)) {
        return Error::notFound("Script resource not found: " + script_path);
    }
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    auto path = makeString(script_path);
    auto hint = makeString("Script");
    auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (path.isErr() || hint.isErr() || cache_mode.isErr()) {
        return Error::internal("Failed to construct script load arguments");
    }
    auto loaded = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                             {&path.value(), &hint.value(), &cache_mode.value()});
    if (loaded.isErr()) return loaded.error();
    auto resource = objectFromVariant(loaded.value());
    if (resource.isErr()) return resource.error();
    if (!resource.value()) return Error::notFound("Script resource not found: " + script_path);
    auto class_name = makeString("Script");
    if (class_name.isErr()) return class_name.error();
    auto is_script_value = callObject(resource.value(), "Object", "is_class", 3927539163LL,
                                      {&class_name.value()});
    if (is_script_value.isErr()) return is_script_value.error();
    auto is_script = scalarFromVariant<GDExtensionBool>(is_script_value.value(),
                                                        GDEXTENSION_VARIANT_TYPE_BOOL);
    if (is_script.isErr()) return is_script.error();
    if (!is_script.value()) return Error::invalidArgument("Resource is not a Script: " + script_path);
    auto base_type_value = callObject(resource.value(), "Script", "get_instance_base_type", 2002593661LL);
    if (base_type_value.isErr()) return base_type_value.error();
    auto base_type = stringFromVariant(base_type_value.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    if (base_type.isErr()) return base_type.error();
    auto base_class = makeString(base_type.value());
    if (base_class.isErr()) return base_class.error();
    auto compatible_value = callObject(node, "Object", "is_class", 3927539163LL, {&base_class.value()});
    if (compatible_value.isErr()) return compatible_value.error();
    auto compatible = scalarFromVariant<GDExtensionBool>(compatible_value.value(),
                                                         GDEXTENSION_VARIANT_TYPE_BOOL);
    if (compatible.isErr()) return compatible.error();
    if (!compatible.value()) {
        // Its own code, so the remedy is the node to attach it to (#1181).
        const auto node_class = nodeClassName(node);
        return Error(422,
                     "Script base type " + base_type.value() + " is incompatible with the target node" +
                         (node_class.empty() ? std::string() : ", a " + node_class) +
                         ". Attach it to a " + base_type.value() + " or a node that inherits from one.",
                     {{"code", "script_base_incompatible"},
                      {"base_type", base_type.value()},
                      {"node_class", node_class}});
    }
    AttachableScript attachable;
    attachable.object = resource.value();
    attachable.base_type = base_type.value();
    attachable.script = std::move(loaded.value());
    return attachable;
}

} // namespace didi::godot::bridge_internal

namespace didi::godot {
using namespace bridge_internal;
namespace {
// Reloads the editor's copy of one file the server has just written, if the
// editor holds one.
//
// A resource is reloaded with CACHE_MODE_REPLACE (2), which updates the cached
// resource and its subresources in place and keeps their identity, so anything
// holding one -- a player holding a library, a node holding a material -- shows
// the new contents. Not REPLACE_DEEP: that re-reads every file the written one
// depends on as well, which throws away an unsaved edit the editor holds in one
// of them, and reloading a VisualShader in place makes the engine print
// `Condition "g->nodes.has(p_id)" is true` for every node it already had.
// Neither mode does anything for a script: the load hands back the held copy
// with its old source and old behaviour, on 4.5.1, 4.6.2 and 4.7.2 (#1047). A
// script is reloaded the way the Script editor
// reloads one that changed on disk: read it with CACHE_MODE_IGNORE, give its
// text to the held copy, and reload that keeping state. Script.set_source_code
// is 83702148 and Script.reload 1633102583 on all three lines.
Result<json> refreshCachedCopy(GDExtensionObjectPtr loader, const std::string& path) {
    auto path_value = makeString(path);
    auto hint = makeString("");
    if (path_value.isErr() || hint.isErr()) return Error::internal("Failed to construct reload arguments");
    auto cached_value = callObject(loader, "ResourceLoader", "has_cached", 2323990056LL,
                                   {&path_value.value()});
    if (cached_value.isErr()) return cached_value.error();
    auto cached = scalarFromVariant<GDExtensionBool>(cached_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (cached.isErr()) return cached.error();
    if (!cached.value()) return json{{"path", path}, {"cached", false}, {"reloaded", false}};

    const auto load = [&](int64_t cache_mode) -> Result<VariantValue> {
        auto mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cache_mode);
        if (mode.isErr()) return mode.error();
        return callObject(loader, "ResourceLoader", "load", 3358495409LL,
                          {&path_value.value(), &hint.value(), &mode.value()});
    };
    auto held_value = load(1);
    if (held_value.isErr()) return held_value.error();
    auto held = objectFromVariant(held_value.value());
    auto script_class = makeString("Script");
    if (script_class.isErr()) return script_class.error();
    bool is_script = false;
    if (held.isOk() && held.value()) {
        auto answer = callObject(held.value(), "Object", "is_class", 3927539163LL, {&script_class.value()});
        if (answer.isErr()) return answer.error();
        auto flag = scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (flag.isErr()) return flag.error();
        is_script = flag.value();
    }
    if (!is_script) {
        auto reloaded = load(2);
        auto object = reloaded.isOk() ? objectFromVariant(reloaded.value())
                                      : Result<GDExtensionObjectPtr>(reloaded.error());
        json result{{"path", path}, {"cached", true},
                    {"reloaded", object.isOk() && object.value() != nullptr}};
        if (!result["reloaded"].get<bool>()) result["reload_error"] = "The file did not load.";
        return result;
    }

    auto fresh_value = load(0);
    auto fresh = fresh_value.isOk() ? objectFromVariant(fresh_value.value())
                                    : Result<GDExtensionObjectPtr>(fresh_value.error());
    if (fresh.isErr() || !fresh.value()) {
        return json{{"path", path}, {"cached", true}, {"reloaded", false},
                    {"reload_error", "The script file did not load."}};
    }
    auto source = callObject(fresh.value(), "Script", "get_source_code", 201670096LL);
    if (source.isErr()) return source.error();
    auto assigned = callObject(held.value(), "Script", "set_source_code", 83702148LL, {&source.value()});
    if (assigned.isErr()) return assigned.error();
    auto keep_state = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
    if (keep_state.isErr()) return keep_state.error();
    auto reloaded = callObject(held.value(), "Script", "reload", 1633102583LL, {&keep_state.value()});
    if (reloaded.isErr()) return reloaded.error();
    auto code = scalarFromVariant<int64_t>(reloaded.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (code.isErr()) return code.error();
    json result{{"path", path}, {"cached", true}, {"reloaded", code.value() == 0}};
    if (code.value() != 0) {
        // 43 is ERR_PARSE_ERROR, which is what a script that does not compile
        // answers, and the editor's copy is then one that cannot be instanced.
        result["reload_error"] = "The editor's copy did not compile after the write (Godot error " +
                                 std::to_string(code.value()) + ").";
    }
    return result;
}

// Where the editor's file index stands on a file.
enum class IndexListing {
    Unknown,          // no editor to ask, or it did not answer
    Listed,
    NotListed,        // its folder is listed and the file is not
    FolderNotListed,  // its folder is not listed, which update_file cannot change
};

IndexListing editorIndexListing(const std::string& resource_path) {
    const auto slash = resource_path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= resource_path.size()) return IndexListing::Unknown;
    auto editor = editorInterface();
    if (editor.isErr()) return IndexListing::Unknown;
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return IndexListing::Unknown;
    auto filesystem_object = objectFromVariant(filesystem.value());
    if (filesystem_object.isErr() || !filesystem_object.value()) return IndexListing::Unknown;
    auto directory_value = makeString(resource_path.substr(0, slash + 1));
    if (directory_value.isErr()) return IndexListing::Unknown;
    auto found = callObject(filesystem_object.value(), "EditorFileSystem", "get_filesystem_path",
                            3188521125LL, {&directory_value.value()});
    if (found.isErr()) return IndexListing::Unknown;
    auto directory_object = objectFromVariant(found.value());
    if (directory_object.isErr()) return IndexListing::Unknown;
    if (!directory_object.value()) return IndexListing::FolderNotListed;
    auto file_value = makeString(resource_path.substr(slash + 1));
    if (file_value.isErr()) return IndexListing::Unknown;
    auto index = callObject(directory_object.value(), "EditorFileSystemDirectory", "find_file_index",
                            1321353865LL, {&file_value.value()});
    if (index.isErr()) return IndexListing::Unknown;
    auto position = scalarFromVariant<int64_t>(index.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (position.isErr()) return IndexListing::Unknown;
    return position.value() >= 0 ? IndexListing::Listed : IndexListing::NotListed;
}

// Puts a file the server wrote into the editor's index when the index does not
// list it, which is what the editor does for a file it saves itself: its save
// callback calls update_file. Left out, the file waits for the editor's next
// scan, which meets it as a new file, and on 4.5 and 4.6 a new file whose .uid
// sidecar some other Godot process wrote makes that scan print `Unrecognized
// UID`: the engine looks the uid up before asking whether it knows it, and 4.7
// asks first. project_export runs such a process, and the editor scans every
// time its window takes focus, so the error met the user on clicking back into
// Godot. Measured on 4.6.2 with script_create, a headless editor on the same
// project, and scan_sources. A file no loader takes as it stands, an asset
// waiting for its import, is the import pass's to index.
//
// update_file never adds a file in a folder the editor does not list, so a
// script written into a new folder kept its class_name unknown to every check
// until the editor next scanned, and a script using it read as an error
// (#1177). Only a full scan lists a new folder (#1114). Such a file is marked
// index_pending, and the editor hook holds the answer while it asks for that
// scan and waits for it to be applied.
void indexUnlistedWrite(GDExtensionObjectPtr loader, const std::string& path, json& result) {
    const auto listing = editorIndexListing(path);
    if (listing == IndexListing::Unknown || listing == IndexListing::Listed) return;
    auto path_value = makeString(path);
    auto hint = makeString("");
    if (path_value.isErr() || hint.isErr()) return;
    auto loadable = callObject(loader, "ResourceLoader", "exists", 4185558881LL,
                               {&path_value.value(), &hint.value()});
    if (loadable.isErr()) return;
    auto exists = scalarFromVariant<GDExtensionBool>(loadable.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr() || !exists.value()) return;
    if (listing == IndexListing::FolderNotListed) {
        result["index_pending"] = true;
        return;
    }
    const auto indexed = GodotBridge::instance().indexWrittenResource(path);
    if (!indexed.has_value()) return;
    result["indexed"] = true;
    if (!indexed->uid.empty()) result["uid"] = indexed->uid;
    result["uid_registered"] = indexed->registered;
}


void reloadOpenSceneTab(GDExtensionObjectPtr editor, const std::string& path,
                        bool discard_unsaved, const json& unsaved, bool& reloaded_one,
                        json& result) {
    result["scene_open"] = true;
    result["scene_reloaded"] = false;
    const auto stale =
        " It still holds the scene from before the write, and saving it would put that back "
        "over the file. Close it with scene_close and discard_unsaved: true to keep the file "
        "as written.";
    if (!discard_unsaved) {
        if (!unsaved.value("unsaved_scenes_readable", false)) {
            result["scene_reload_error"] =
                std::string("The scene is open in the editor, and Godot before 4.7 cannot say "
                            "whether its tab has unsaved changes, so the tab was not reloaded.") +
                stale;
            return;
        }
        for (const auto& entry : unsaved["unsaved_scenes"]) {
            if (entry.is_string() && entry.get<std::string>() == path) {
                result["scene_reload_error"] =
                    std::string("The scene is open in the editor with unsaved changes, so its "
                                "tab was not reloaded.") +
                    stale;
                return;
            }
        }
    }
    if (reloaded_one) {
        result["scene_reload_pending"] = true;
        return;
    }
    // What the rebuild throws away, read before it: true or false where the
    // engine can say, null before 4.7 where it cannot (#1082).
    json held_unsaved = nullptr;
    if (unsaved.value("unsaved_scenes_readable", false) && unsaved.contains("unsaved_scenes") &&
        unsaved["unsaved_scenes"].is_array()) {
        held_unsaved = false;
        for (const auto& entry : unsaved["unsaved_scenes"]) {
            if (entry.is_string() && entry.get<std::string>() == path) held_unsaved = true;
        }
    }
    auto before = openSceneRootId(editor, path);
    if (before.isErr()) {
        result["scene_reload_error"] = "The editor's tab could not be read: " + before.error().message;
        return;
    }
    auto path_value = makeString(path);
    if (path_value.isErr()) {
        result["scene_reload_error"] = "Failed to construct the reload argument";
        return;
    }
    auto current_before = editedSceneRoot(editor);
    const std::string edited_before =
        current_before.isOk() ? editedScenePath(current_before.value()) : std::string();
    auto reloaded = callObject(editor, "EditorInterface", "reload_scene_from_path", 83702148LL,
                               {&path_value.value()});
    if (reloaded.isErr()) {
        result["scene_reload_error"] = "The editor did not reload the tab: " + reloaded.error().message;
        return;
    }
    auto after = openSceneRootId(editor, path);
    if (after.isErr()) {
        result["scene_reload_error"] = "The editor's tab could not be read after the reload: " +
                                       after.error().message;
        return;
    }
    if (after.value() == 0) {
        result["scene_reload_error"] =
            "The editor could not load the rewritten file, so it closed the scene's tab.";
        return;
    }
    if (after.value() == before.value()) {
        // The editor was still switching scenes, from this request or an
        // earlier one, and the call did nothing. Nothing was lost; ask again.
        result["scene_reload_pending"] = true;
        return;
    }
    reloaded_one = true;
    result["scene_reloaded"] = true;
    result["scene_discarded_unsaved"] = held_unsaved;
    auto current_after = editedSceneRoot(editor);
    if (current_before.isOk() && edited_before != path && current_after.isOk() &&
        editedScenePath(current_after.value()) == path) {
        result["edited_scene_moved_from"] = edited_before;
    }
}

// Enough for any model, and a bound on a scene that is not one.
constexpr int64_t kMaxPackedSceneNodes = 4096;
constexpr size_t kMaxSceneAnimations = 1024;

// The loop mode of every animation an imported scene holds, read from its
// packed state, so nothing in the scene is instanced inside the user's editor
// and no script of its runs (#1425). An AnimationMixer keeps its libraries as
// one Dictionary property, libraries, on 4.5, and as one Object property per
// library, libraries/<name>, from 4.6: measured on 4.5.1, 4.6.2 and 4.7.2 by
// tools/vibe/probes/scene_import_loop_engine.py, which reads the modes this way
// and by instancing and found them the same. A library other than the default
// one names its animations library/name, as the mixer does. Every bind below
// has the same hash on all three lines.
Result<json> packedSceneAnimations(GDExtensionObjectPtr packed) {
    for (const auto& bind : {std::make_tuple("PackedScene", "get_state", 3479783971LL),
                             std::make_tuple("SceneState", "get_node_count", 3905245786LL),
                             std::make_tuple("SceneState", "get_node_property_count", 923996154LL),
                             std::make_tuple("SceneState", "get_node_property_name", 351665558LL),
                             std::make_tuple("SceneState", "get_node_property_value", 678354945LL),
                             std::make_tuple("AnimationLibrary", "get_animation_list", 3995934104LL),
                             std::make_tuple("AnimationLibrary", "get_animation", 2933122410LL),
                             std::make_tuple("Animation", "get_loop_mode", 1988889481LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }
    const auto integer = [](Result<VariantValue> value) -> Result<int64_t> {
        if (value.isErr()) return value.error();
        return scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    const auto text = [](VariantValue& value) {
        return stringFromVariant(value, GodotApi::instance().variant_get_type(value.ptr()));
    };
    auto state_value = callObject(packed, "PackedScene", "get_state", 3479783971LL);
    if (state_value.isErr()) return state_value.error();
    auto state = objectFromVariant(state_value.value());
    if (state.isErr() || !state.value()) return Error::internal("The scene has no packed state to read");
    auto node_count = integer(callObject(state.value(), "SceneState", "get_node_count", 3905245786LL));
    if (node_count.isErr()) return node_count.error();

    json animations = json::object();
    int64_t mixers = 0;
    bool truncated = node_count.value() > kMaxPackedSceneNodes;
    const auto readLibrary = [&](GDExtensionObjectPtr library, const std::string& library_name) -> Result<void> {
        auto list = callObject(library, "AnimationLibrary", "get_animation_list", 3995934104LL);
        if (list.isErr()) return list.error();
        auto size = integer(callVariant(list.value(), "size"));
        if (size.isErr()) return size.error();
        for (int64_t index = 0; index < size.value(); ++index) {
            if (animations.size() >= kMaxSceneAnimations) {
                truncated = true;
                return {};
            }
            auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (index_value.isErr()) return index_value.error();
            auto name_value = callVariant(list.value(), "get", {&index_value.value()});
            if (name_value.isErr()) return name_value.error();
            auto name = text(name_value.value());
            if (name.isErr()) return name.error();
            auto key = makeStringName(name.value());
            if (key.isErr()) return key.error();
            auto animation_value = callObject(library, "AnimationLibrary", "get_animation", 2933122410LL, {&key.value()});
            if (animation_value.isErr()) return animation_value.error();
            auto animation = objectFromVariant(animation_value.value());
            if (animation.isErr() || !animation.value()) continue;
            auto mode = integer(callObject(animation.value(), "Animation", "get_loop_mode", 1988889481LL));
            if (mode.isErr()) return mode.error();
            animations[library_name.empty() ? name.value() : library_name + "/" + name.value()] = mode.value();
        }
        return {};
    };
    for (int64_t node = 0; node < node_count.value() && node < kMaxPackedSceneNodes; ++node) {
        auto node_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, node);
        if (node_index.isErr()) return node_index.error();
        auto property_count = integer(callObject(state.value(), "SceneState", "get_node_property_count",
                                                 923996154LL, {&node_index.value()}));
        if (property_count.isErr()) return property_count.error();
        bool mixer = false;
        for (int64_t property = 0; property < property_count.value(); ++property) {
            auto property_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, property);
            if (property_index.isErr()) return property_index.error();
            auto name_value = callObject(state.value(), "SceneState", "get_node_property_name", 351665558LL,
                                         {&node_index.value(), &property_index.value()});
            if (name_value.isErr()) return name_value.error();
            auto name = text(name_value.value());
            if (name.isErr()) return name.error();
            const bool one_dictionary = name.value() == "libraries";
            if (!one_dictionary && name.value().rfind("libraries/", 0) != 0) continue;
            auto value = callObject(state.value(), "SceneState", "get_node_property_value", 678354945LL,
                                    {&node_index.value(), &property_index.value()});
            if (value.isErr()) return value.error();
            mixer = true;
            const auto type = GodotApi::instance().variant_get_type(value.value().ptr());
            if (one_dictionary) {
                if (type != GDEXTENSION_VARIANT_TYPE_DICTIONARY) continue;
                auto keys = callVariant(value.value(), "keys");
                auto values = callVariant(value.value(), "values");
                if (keys.isErr()) return keys.error();
                if (values.isErr()) return values.error();
                auto size = integer(callVariant(keys.value(), "size"));
                if (size.isErr()) return size.error();
                for (int64_t index = 0; index < size.value(); ++index) {
                    auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
                    if (index_value.isErr()) return index_value.error();
                    auto library_key = callVariant(keys.value(), "get", {&index_value.value()});
                    auto library_value = callVariant(values.value(), "get", {&index_value.value()});
                    if (library_key.isErr()) return library_key.error();
                    if (library_value.isErr()) return library_value.error();
                    auto library_name = text(library_key.value());
                    auto library = objectFromVariant(library_value.value());
                    if (library_name.isErr() || library.isErr() || !library.value()) continue;
                    if (auto read = readLibrary(library.value(), library_name.value()); read.isErr()) {
                        return read.error();
                    }
                }
            } else {
                if (type != GDEXTENSION_VARIANT_TYPE_OBJECT) continue;
                auto library = objectFromVariant(value.value());
                if (library.isErr() || !library.value()) continue;
                if (auto read = readLibrary(library.value(), name.value().substr(10)); read.isErr()) {
                    return read.error();
                }
            }
        }
        if (mixer) ++mixers;
    }
    json read = {{"animations", std::move(animations)}, {"animation_mixers", mixers}};
    if (truncated) read["truncated"] = true;
    return read;
}

} // namespace

json GodotBridge::bridgeResourceRefreshCached(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    // Asked after the server has written a file. The editor keeps what it
    // has loaded and does not re-read a file that changed underneath it, so
    // a reader in this process answers from the old copy until something
    // reloads it. One path, or up to 256 in `paths` for a writer that
    // changed several files. ResourceLoader.has_cached is 2323990056 on
    // 4.5.1, 4.6.2 and 4.7.2.
    //
    // A path open in a tab also has its tab reloaded, which
    // reloadOpenSceneTab describes. discard_unsaved says the caller accepts
    // losing a tab's unsaved changes to it. A path the editor's index does
    // not list yet is indexed, which indexUnlistedWrite describes.
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    if (params.contains("discard_unsaved") && !params["discard_unsaved"].is_boolean()) {
        return errorJson(400, "discard_unsaved must be a boolean");
    }
    const bool discard_unsaved = params.value("discard_unsaved", false);
    std::vector<std::string> paths;
    const bool batch = params.contains("paths");
    if (batch) {
        const auto& listed = params["paths"];
        if (!listed.is_array() || listed.empty() || listed.size() > 256) {
            return errorJson(400, "paths must be an array of 1 to 256 res:// paths");
        }
        for (const auto& entry : listed) {
            if (!entry.is_string()) return errorJson(400, "paths must hold only strings");
            paths.push_back(entry.get<std::string>());
        }
    } else {
        paths.push_back(params.value("path", ""));
    }
    for (const auto& path : paths) {
        if (validateResPath(path, "").isErr()) {
            return errorJson(400, "path must be a normalized res:// path: " + path);
        }
    }
    for (const auto& bind : {std::make_tuple("ResourceLoader", "has_cached", 2323990056LL),
                             std::make_tuple("ResourceLoader", "load", 3358495409LL),
                             std::make_tuple("Object", "is_class", 3927539163LL),
                             std::make_tuple("Script", "get_source_code", 201670096LL),
                             std::make_tuple("Script", "set_source_code", 83702148LL),
                             std::make_tuple("Script", "reload", 1633102583LL),
                             std::make_tuple("EditorInterface", "get_open_scenes", 1139954409LL),
                             std::make_tuple("EditorInterface", "get_open_scene_roots", 3995934104LL),
                             std::make_tuple("EditorInterface", "reload_scene_from_path", 83702148LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return errorJson(loader.error());
    auto editor = editorInterface();
    if (editor.isErr()) return errorJson(editor.error());
    auto open = openScenePaths(editor.value());
    if (open.isErr()) return errorJson(open.error());
    const std::set<std::string> open_scenes(open.value().begin(), open.value().end());
    const json unsaved = open_scenes.empty() ? json::object() : unsavedScenesReport(editor.value());
    bool reloaded_one = false;
    json results = json::array();
    for (const auto& path : paths) {
        auto refreshed = refreshCachedCopy(loader.value(), path);
        if (refreshed.isErr()) return errorJson(500, refreshed.error().message);
        indexUnlistedWrite(loader.value(), path, refreshed.value());
        if (open_scenes.count(path)) {
            reloadOpenSceneTab(editor.value(), path, discard_unsaved, unsaved, reloaded_one,
                               refreshed.value());
        }
        results.push_back(std::move(refreshed.value()));
    }
    return batch ? liveResult({{"results", std::move(results)}}) : liveResult(results[0]);
}

json GodotBridge::bridgeAssetReadImportedStream(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/) {
    // What asset_configure_import checks a value against and reads back
    // after the reimport: the imported stream, loaded from disk. The cache
    // is ignored, because the question is what the file now loads as; the
    // editor's cached copy is updated in place by the reimport and is not
    // this read's business (#958). The properties are read through
    // Object.get by name, and AudioStream.get_length is the one new bind:
    // 1740695150 on 4.5.1, 4.6.2 and 4.7.2. An imported scene answers the
    // loop mode of each animation it holds instead (#1425).
    if (!hasOnlyKeys(params, {"path"}) || !params.contains("path") || !params["path"].is_string()) {
        return errorJson(400, "asset.readImportedStream takes path, a res:// asset");
    }
    const auto path = params["path"].get<std::string>();
    auto valid_path = validateResPath(path, "");
    if (valid_path.isErr()) return errorJson(valid_path.error());
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return errorJson(loader.error());
    auto godot_path = makeString(path);
    auto type_hint = makeString("");
    auto ignore_cache = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (godot_path.isErr() || type_hint.isErr() || ignore_cache.isErr()) {
        return errorJson(500, "Failed to build the load request");
    }
    // Asked first, because loading a file that is not there prints an
    // ERROR. ResourceLoader.exists is 4185558881 on all three lines.
    auto exists = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                             {&godot_path.value(), &type_hint.value()});
    auto found = exists.isOk() ? scalarFromVariant<GDExtensionBool>(exists.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                               : Result<GDExtensionBool>(exists.error());
    if (found.isErr() || found.value() == 0) {
        return errorJson(404, "Godot finds nothing to load at " + path);
    }
    auto loaded = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                             {&godot_path.value(), &type_hint.value(), &ignore_cache.value()});
    if (loaded.isErr()) return errorJson(loaded.error());
    auto object = objectFromVariant(loaded.value());
    if (object.isErr() || !object.value()) {
        return errorJson(422, "Godot could not load " + path, json{{"code", "load_failed"}});
    }
    const auto class_name = nodeClassName(object.value());
    json properties = json::object();
    std::vector<const char*> names;
    if (class_name == "AudioStreamWAV") names = {"mix_rate", "loop_mode", "loop_begin", "loop_end"};
    if (class_name == "AudioStreamOggVorbis" || class_name == "AudioStreamMP3") {
        names = {"loop", "loop_offset"};
    }
    for (const auto* name : names) {
        auto property = makeStringName(name);
        if (property.isErr()) continue;
        auto value = callObject(object.value(), "Object", "get", 2760726917LL, {&property.value()});
        if (value.isErr()) continue;
        auto converted = variantToJson(value.value(), 0, true);
        if (converted.isOk()) properties[name] = converted.value();
    }
    json answer = {{"path", path}, {"class", class_name}, {"properties", std::move(properties)}};
    if (class_name == "PackedScene") {
        auto read = packedSceneAnimations(object.value());
        if (read.isErr()) return errorJson(read.error());
        for (auto& [key, value] : read.value().items()) answer[key] = std::move(value);
    }
    if (!names.empty()) {
        auto length = callObject(object.value(), "AudioStream", "get_length", 1740695150LL);
        if (length.isOk()) {
            auto seconds = scalarFromVariant<double>(length.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
            if (seconds.isOk()) answer["length_seconds"] = seconds.value();
        }
    }
    return liveResult(std::move(answer));
}



json GodotBridge::bridgeScriptAttachment(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr editor) {
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto node = resolveNode(root.value(), params.value("target_node", ""));
    if (node.isErr()) return errorJson(node.error());
    if (auto refused = refuseUnsavableEdit(root.value(), node.value(),
                                           params.value("target_node", ""),
                                           SceneEdit::Property)) {
        return *refused;
    }
    auto old_script = callObject(node.value(), "Object", "get_script", 1214101251LL);
    if (old_script.isErr()) return errorJson(old_script.error());
    auto old_type = GodotApi::instance().variant_get_type(old_script.value().ptr());
    GDExtensionObjectPtr old_object = nullptr;
    if (old_type == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto converted = objectFromVariant(old_script.value());
        if (converted.isErr()) return errorJson(converted.error());
        old_object = converted.value();
    }

    VariantValue new_script;
    GDExtensionObjectPtr requested_script_object = nullptr;
    std::string script_path;
    const bool attaching = method == "script.attachToNode";
    if (attaching) {
        script_path = params.value("script_path", "");
        auto attachable = loadAttachableScript(node.value(), script_path);
        if (attachable.isErr()) {
            const auto& failure = attachable.error();
            return errorJson(failure);
        }
        requested_script_object = attachable.value().object;
        new_script = std::move(attachable.value().script);
    } else if (!old_object) {
        return errorJson(409, "Target node has no script to detach",
                              {{"code", "no_script_attached"}});
    }

    auto manager = undoManager(editor);
    if (manager.isErr()) return errorJson(manager.error());
    auto preflight = preflightUndoManagerBindings();
    auto method_bind = requireMethodBind("Object", "set_script", 1114965689LL);
    if (preflight.isErr()) return errorJson(preflight.error());
    if (method_bind.isErr()) return errorJson(method_bind.error());
    auto action = createAction(manager.value(), attaching ? "Didi: attach script" : "Didi: detach script", root.value());
    if (action.isErr()) return errorJson(action.error());
    auto apply = managerMethod(manager.value(), "add_do_method", node.value(), "set_script", {&new_script});
    auto revert = managerMethod(manager.value(), "add_undo_method", node.value(), "set_script", {&old_script.value()});
    if (apply.isErr() || revert.isErr()) {
        // Closed, so the next mutation does not merge into it (#1152).
        abandonAction(manager.value());
        return errorJson(500, "Failed to register script UndoRedo transaction");
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(committed.error());
    auto observed_script = callObject(node.value(), "Object", "get_script", 1214101251LL);
    if (observed_script.isErr()) return errorJson(observed_script.error());
    auto observed_type = GodotApi::instance().variant_get_type(observed_script.value().ptr());
    GDExtensionObjectPtr observed_object = nullptr;
    if (observed_type == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto converted = objectFromVariant(observed_script.value());
        if (converted.isErr()) return errorJson(converted.error());
        observed_object = converted.value();
    }
    if ((attaching && observed_object != requested_script_object) || (!attaching && observed_object)) {
        // The action is already committed, so the node and the undo stack
        // may both carry the change. Roll it back the way signal connect
        // does, and if that also fails say the outcome is unknown rather
        // than a bare 422, which reads as "nothing happened".
        const std::string reason = attaching
            ? "Godot rejected the script assignment; its native base may be incompatible with the target node"
            : "Godot did not detach the target node's script";
        auto reverted = undoLastAction(manager.value(), root.value());
        if (reverted.isErr()) {
            json failure = errorJson(500, reason + "; the committed action could not be undone: " +
                                              reverted.error().message);
            failure["error"]["data"] = {{"outcome", "unknown"}, {"rolled_back", false}};
            return failure;
        }
        return errorJson(422, reason,
                         {{"code", "script_assignment_rejected"},
                          {"outcome", "rolled_back"}, {"rolled_back", true}});
    }
    // The script the node held before, read before the commit, so the
    // change journal can say what was replaced (#1151).
    const json before_script = old_object ? json(resourcePathOf(old_object)) : json(nullptr);
    return liveSceneMutation({{"status", "success"}, {"target_node", params.value("target_node", "")},
                              {"script_path", script_path}, {"attached", attaching}, {"detached", !attaching},
                              {"before", {{"script", before_script}}},
                              {"undo_redo_registered", true}});
}


json GodotBridge::bridgeEngineClassExists(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    // The class list of the engine that will load the file. resource_create
    // checked its type guard against the pinned 4.7 dump and wrote
    // resources a 4.5.1 editor then refused to load at all -- not one
    // dropped property, the whole file -- while reporting the version
    // mismatch in the same payload (#766).
    //
    // ClassDB.class_exists is 2619796661 on 4.5.1, 4.6.2 and 4.7.2, and its
    // answers were measured on all three: DrawableTexture2D and
    // BlitMaterial exist only on 4.7, JointLimitation3D arrived in 4.6.
    //
    // A class_name script type is not in ClassDB -- it lives in the script
    // server's global list -- so it answers false here exactly as it is
    // absent from the dump. allow_unknown_type stays the route for those.
    if (!params.contains("class_names") || !params["class_names"].is_array()) {
        return errorJson(400, "engine.classExists requires a class_names array");
    }
    const auto& names = params["class_names"];
    // 65 because resource_create asks about one root type and up to 64
    // sub-resource types, and a cap below what the caller can legitimately
    // ask would send it back to the pinned reference in silence, which is
    // the bug this route exists to close.
    if (names.empty() || names.size() > 65) {
        return errorJson(400, "engine.classExists accepts 1 to 65 class names");
    }
    auto required = requireMethodBind("ClassDB", "class_exists", 2619796661LL);
    if (required.isErr()) return errorJson(501, required.error().message);
    auto class_db = singleton("ClassDB");
    if (class_db.isErr()) return errorJson(class_db.error());

    json classes = json::array();
    for (const auto& value : names) {
        if (!value.is_string()) {
            return errorJson(400, "engine.classExists class_names must be strings");
        }
        const std::string name = value.get<std::string>();
        auto class_name = makeStringName(name);
        if (class_name.isErr()) return errorJson(500, class_name.error().message);
        auto answer = callObject(class_db.value(), "ClassDB", "class_exists", 2619796661LL,
                                 {&class_name.value()});
        if (answer.isErr()) return errorJson(500, answer.error().message);
        auto known =
            scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (known.isErr()) return errorJson(500, known.error().message);
        classes.push_back({{"name", name}, {"exists", known.value() != 0}});
    }
    return liveResult({{"status", "success"}, {"classes", std::move(classes)}});
}


} // namespace didi::godot
