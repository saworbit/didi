#include "bridge_internal.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/json_bounds.hpp"
#include "didi/common/project_path.hpp"
#include "didi/runtime/animation_requests.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <tuple>

namespace didi::godot::bridge_internal {
namespace {
Result<GDExtensionObjectPtr> resolveAnimationPlayer(const std::string& path, const std::string& session_kind) {
    Result<GDExtensionObjectPtr> root = Error::internal("unresolved");
    if (session_kind == "editor") {
        auto editor = editorInterface();
        if (editor.isErr()) return editor.error();
        root = editedSceneRoot(editor.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return tree.error();
        root = liveSceneTreeRoot(tree.value());
    }
    if (root.isErr()) return root.error();
    auto node = resolveNode(root.value(), path);
    if (node.isErr()) return Error::notFound("No node at " + path);
    if (!node.value()) return Error::notFound("No node at " + path);
    auto class_name = makeString("AnimationPlayer");
    if (class_name.isErr()) return class_name.error();
    auto is_player = callObject(node.value(), "Object", "is_class", 3927539163LL, {&class_name.value()});
    if (is_player.isErr()) return is_player.error();
    auto flag = scalarFromVariant<GDExtensionBool>(is_player.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (flag.isErr() || !flag.value()) return Error::notFound("Node at " + path + " is not an AnimationPlayer");
    return node.value();
}


} // namespace

json animListTracks(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseAnimListRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    for (const auto& bind : {std::make_tuple("AnimationMixer", "get_animation_list", 1139954409LL),
                             std::make_tuple("AnimationMixer", "get_animation", 2933122410LL),
                             std::make_tuple("Animation", "get_length", 1740695150LL),
                             std::make_tuple("Animation", "get_loop_mode", 1988889481LL),
                             std::make_tuple("Animation", "get_track_count", 3905245786LL),
                             std::make_tuple("Animation", "track_get_type", 3445944217LL),
                             std::make_tuple("Animation", "track_get_path", 408788394LL),
                             std::make_tuple("Animation", "track_get_key_count", 923996154LL),
                             std::make_tuple("Animation", "track_get_key_time", 3085491603LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }
    auto player = resolveAnimationPlayer(parsed.value().animation_player_path, session_kind);
    if (player.isErr()) return errorJson(player.error());

    auto names = callObject(player.value(), "AnimationMixer", "get_animation_list", 1139954409LL);
    if (names.isErr()) return errorJson(500, names.error().message);
    auto names_size = callVariant(names.value(), "size");
    if (names_size.isErr()) return errorJson(500, names_size.error().message);
    auto name_count = scalarFromVariant<int64_t>(names_size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (name_count.isErr()) return errorJson(500, name_count.error().message);

    auto int_call = [&](GDExtensionObjectPtr object, const char* klass, const char* method, int64_t hash,
                        std::vector<const VariantValue*> args) -> Result<int64_t> {
        auto result = callObject(object, klass, method, hash, args);
        if (result.isErr()) return result.error();
        return scalarFromVariant<int64_t>(result.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    auto float_call = [&](GDExtensionObjectPtr object, const char* klass, const char* method, int64_t hash,
                          std::vector<const VariantValue*> args) -> Result<double> {
        auto result = callObject(object, klass, method, hash, args);
        if (result.isErr()) return result.error();
        return scalarFromVariant<double>(result.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
    };

    std::vector<runtime::AnimationInfo> animations;
    // One past the cap is enough for the builder to report the cut.
    const int64_t animation_limit = static_cast<int64_t>(runtime::kMaxAnimations) + 1;
    for (int64_t index = 0; index < name_count.value() && index < animation_limit; ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return errorJson(500, index_value.error().message);
        auto name_value = callVariant(names.value(), "get", {&index_value.value()});
        if (name_value.isErr()) return errorJson(500, name_value.error().message);
        auto name_text = stringFromVariant(name_value.value(), GodotApi::instance().variant_get_type(name_value.value().ptr()));
        if (name_text.isErr()) return errorJson(500, name_text.error().message);
        runtime::AnimationInfo info;
        info.name = boundUtf8(name_text.value(), 256).value;

        auto name_key = makeStringName(name_text.value());
        if (name_key.isErr()) return errorJson(500, name_key.error().message);
        auto animation_value = callObject(player.value(), "AnimationMixer", "get_animation", 2933122410LL, {&name_key.value()});
        if (animation_value.isErr()) return errorJson(500, animation_value.error().message);
        auto animation = objectFromVariant(animation_value.value());
        if (animation.isErr() || !animation.value()) return errorJson(500, "Animation resource could not be read: " + name_text.value());

        auto length = float_call(animation.value(), "Animation", "get_length", 1740695150LL, {});
        auto loop_mode = int_call(animation.value(), "Animation", "get_loop_mode", 1988889481LL, {});
        auto track_count = int_call(animation.value(), "Animation", "get_track_count", 3905245786LL, {});
        if (length.isErr() || loop_mode.isErr() || track_count.isErr()) {
            return errorJson(500, "Animation metadata could not be read: " + name_text.value());
        }
        info.length = std::max(0.0, length.value());
        info.loop_mode_id = loop_mode.value();

        const int64_t track_limit = static_cast<int64_t>(runtime::kMaxTracksPerAnimation);
        for (int64_t track_index = 0; track_index < track_count.value(); ++track_index) {
            if (track_index >= track_limit) { info.tracks_cut = true; break; }
            auto track_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, track_index);
            if (track_value.isErr()) return errorJson(500, track_value.error().message);
            runtime::AnimationTrackInfo track;
            track.index = track_index;
            auto type = int_call(animation.value(), "Animation", "track_get_type", 3445944217LL, {&track_value.value()});
            if (type.isErr()) return errorJson(500, type.error().message);
            track.type_id = type.value();
            auto path_value = callObject(animation.value(), "Animation", "track_get_path", 408788394LL, {&track_value.value()});
            if (path_value.isErr()) return errorJson(500, path_value.error().message);
            auto path_text = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
            if (path_text.isErr()) return errorJson(500, path_text.error().message);
            track.path = boundUtf8(path_text.value(), 1024).value;
            auto key_count = int_call(animation.value(), "Animation", "track_get_key_count", 923996154LL, {&track_value.value()});
            if (key_count.isErr()) return errorJson(500, key_count.error().message);
            const int64_t key_limit = static_cast<int64_t>(runtime::kMaxKeysPerTrack);
            for (int64_t key_index = 0; key_index < key_count.value(); ++key_index) {
                if (key_index >= key_limit) { track.key_times_cut = true; break; }
                auto key_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, key_index);
                if (key_value.isErr()) return errorJson(500, key_value.error().message);
                auto time = float_call(animation.value(), "Animation", "track_get_key_time", 3085491603LL,
                                       {&track_value.value(), &key_value.value()});
                if (time.isErr()) return errorJson(500, time.error().message);
                track.key_times.push_back(time.value());
            }
            info.tracks.push_back(std::move(track));
        }
        animations.push_back(std::move(info));
    }
    return liveResult(runtime::buildAnimationCatalog(std::move(animations)));
}


json animPlayTrack(const json& params, const std::string& session_kind) {
    if (session_kind != "game") return bridgeError(409, "session_kind_rejected");
    auto parsed = runtime::parseAnimPlayRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    for (const auto& bind : {std::make_tuple("AnimationMixer", "has_animation", 2619796661LL),
                             std::make_tuple("AnimationPlayer", "play", 3118260607LL),
                             std::make_tuple("AnimationPlayer", "is_playing", 36873697LL),
                             std::make_tuple("Object", "get", 2760726917LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }
    const auto& request = parsed.value();
    auto player = resolveAnimationPlayer(request.animation_player_path, session_kind);
    if (player.isErr()) return errorJson(player.error());

    auto name = makeStringName(request.animation_name);
    if (name.isErr()) return errorJson(500, name.error().message);
    auto has = callObject(player.value(), "AnimationMixer", "has_animation", 2619796661LL, {&name.value()});
    if (has.isErr()) return errorJson(500, has.error().message);
    auto has_flag = scalarFromVariant<GDExtensionBool>(has.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (has_flag.isErr() || !has_flag.value()) {
        return errorJson(404, "AnimationPlayer has no animation named " + request.animation_name);
    }

    auto blend = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, -1.0);
    auto speed = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, request.custom_speed);
    auto from_end = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(request.from_end));
    if (blend.isErr() || speed.isErr() || from_end.isErr()) return errorJson(500, "Failed to construct play arguments");
    auto played = callObject(player.value(), "AnimationPlayer", "play", 3118260607LL,
                             {&name.value(), &blend.value(), &speed.value(), &from_end.value()});
    if (played.isErr()) {
        return errorJson(504, played.error().message, {{"outcome", "unknown_outcome"}, {"retryable", false}});
    }
    auto playing = callObject(player.value(), "AnimationPlayer", "is_playing", 36873697LL);
    if (playing.isErr()) return errorJson(500, playing.error().message);
    auto playing_flag = scalarFromVariant<GDExtensionBool>(playing.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (playing_flag.isErr()) return errorJson(500, playing_flag.error().message);
    auto property = makeStringName("current_animation");
    if (property.isErr()) return errorJson(500, property.error().message);
    auto current = callObject(player.value(), "Object", "get", 2760726917LL, {&property.value()});
    if (current.isErr()) return errorJson(500, current.error().message);
    auto current_text = stringFromVariant(current.value(), GodotApi::instance().variant_get_type(current.value().ptr()));
    return liveResult({{"dispatched", true},
                       {"animation_name", current_text.isOk() ? current_text.value() : request.animation_name},
                       {"custom_speed", request.custom_speed}, {"from_end", request.from_end},
                       {"playing", playing_flag.value() != 0}, {"outcome", "completed"},
                       {"rollback", "not_available"}, {"session_kind", session_kind}});
}

namespace {

// The names in a TypedArray<StringName>, in engine order, up to a cap. `total`
// is the whole count, so a caller can tell a short list from a cut one.
struct NameList {
    std::vector<std::string> names;
    int64_t total{0};
};

Result<NameList> readNameList(VariantValue& list, size_t cap) {
    auto size_value = callVariant(list, "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    NameList result;
    result.total = size.value();
    for (int64_t index = 0; index < size.value() && result.names.size() < cap; ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto entry = callVariant(list, "get", {&index_value.value()});
        if (entry.isErr()) return entry.error();
        auto text = stringFromVariant(entry.value(), GodotApi::instance().variant_get_type(entry.value().ptr()));
        if (text.isErr()) return text.error();
        result.names.push_back(boundUtf8(text.value(), 256).value);
    }
    return result;
}

// How the file a res:// path names is spelled on disk, when that differs from
// the argument only in letter case. Empty when the spelling matches, or when
// the file cannot be resolved to one inside the project.
//
// A case-insensitive filesystem opens res://MENU.tres for a file stored as
// res://menu.tres, and the loader then caches a second copy of the resource
// under the spelling it was given. The scene saved afterwards references the
// wrong spelling, which a case-sensitive platform and an exported pack cannot
// open. Godot 4.7.2 says so in its log ("This file will not open when exported
// to other case-sensitive platforms"); 4.5.1 and 4.6.2 say nothing, measured
// in vibe session seventeen.
std::optional<std::string> resPathCaseMismatch(const std::string& res_path) {
    namespace fs = std::filesystem;
    auto directory = projectDirectoryOnDisk();
    if (directory.isErr()) return std::nullopt;
    std::error_code error;
    fs::path root;
    fs::path target;
    try {
        root = fs::canonical(paths::projectPathFromUtf8(directory.value()), error);
        if (error) return std::nullopt;
        target = fs::canonical(root / paths::projectPathFromUtf8(res_path.substr(6)), error);
        if (error) return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    const auto relative = target.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") return std::nullopt;
    const std::string spelled = "res://" + paths::projectPathToUtf8(relative);
    if (spelled == res_path || spelled.size() != res_path.size()) return std::nullopt;
    // Case only: a symlink would change the path rather than its letters, and
    // is left alone. Non-ASCII letters are compared byte for byte at the same
    // length, which is as far as a case fold without a Unicode table goes.
    for (size_t index = 0; index < spelled.size(); ++index) {
        const auto a = static_cast<unsigned char>(spelled[index]);
        const auto b = static_cast<unsigned char>(res_path[index]);
        if (a == b) continue;
        if (a >= 0x80 && b >= 0x80) continue;
        if (std::tolower(a) != std::tolower(b)) return std::nullopt;
    }
    return spelled;
}

// What a library holds, as far as a player would show it: each animation's
// length and loop mode, and each track's type, path and key count. Two copies
// of the same file that differ here are two different libraries to a caller.
// Key times and values are not compared, which bounds the work at the catalog
// caps anim_list_tracks already applies.
Result<json> libraryFingerprint(GDExtensionObjectPtr library) {
    auto list = callObject(library, "AnimationLibrary", "get_animation_list", 3995934104LL);
    if (list.isErr()) return list.error();
    auto names = readNameList(list.value(), runtime::kMaxAnimations);
    if (names.isErr()) return names.error();
    json animations = json::object();
    for (const auto& name : names.value().names) {
        auto key = makeStringName(name);
        if (key.isErr()) return key.error();
        auto value = callObject(library, "AnimationLibrary", "get_animation", 2933122410LL, {&key.value()});
        if (value.isErr()) return value.error();
        auto animation = objectFromVariant(value.value());
        if (animation.isErr()) return animation.error();
        if (!animation.value()) {
            animations[name] = nullptr;
            continue;
        }
        auto number = [&](const char* method, int64_t hash, GDExtensionVariantType type,
                          std::vector<const VariantValue*> args = {}) -> json {
            auto result = callObject(animation.value(), "Animation", method, hash, args);
            if (result.isErr()) return nullptr;
            if (type == GDEXTENSION_VARIANT_TYPE_FLOAT) {
                auto read = scalarFromVariant<double>(result.value(), type);
                return read.isOk() ? json(read.value()) : json(nullptr);
            }
            auto read = scalarFromVariant<int64_t>(result.value(), type);
            return read.isOk() ? json(read.value()) : json(nullptr);
        };
        json entry = {{"length", number("get_length", 1740695150LL, GDEXTENSION_VARIANT_TYPE_FLOAT)},
                      {"loop_mode", number("get_loop_mode", 1988889481LL, GDEXTENSION_VARIANT_TYPE_INT)}};
        const json track_count = number("get_track_count", 3905245786LL, GDEXTENSION_VARIANT_TYPE_INT);
        json tracks = json::array();
        const int64_t limit = std::min<int64_t>(track_count.is_number_integer() ? track_count.get<int64_t>() : 0,
                                                static_cast<int64_t>(runtime::kMaxTracksPerAnimation));
        for (int64_t index = 0; index < limit; ++index) {
            auto track = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (track.isErr()) return track.error();
            std::string path;
            auto path_value = callObject(animation.value(), "Animation", "track_get_path", 408788394LL,
                                         {&track.value()});
            if (path_value.isOk()) {
                auto text = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
                if (text.isOk()) path = text.value();
            }
            tracks.push_back({number("track_get_type", 3445944217LL, GDEXTENSION_VARIANT_TYPE_INT, {&track.value()}),
                              path,
                              number("track_get_key_count", 923996154LL, GDEXTENSION_VARIANT_TYPE_INT,
                                     {&track.value()})});
        }
        entry["track_count"] = track_count;
        entry["tracks"] = std::move(tracks);
        animations[name] = std::move(entry);
    }
    return json{{"animation_count", names.value().total}, {"animations", std::move(animations)}};
}

// Gives an AnimationPlayer in the edited scene an AnimationLibrary loaded from
// a res:// file, through the editor UndoRedo stack (#770). Nothing else on the
// surface could, so anim_list_tracks and anim_play_track had nothing to find
// on any player the surface built.
//
// add_animation_library answers a bad name, a name in use and a library that
// is already on the player with an error code and an ERROR line in the
// editor's log, on 4.5.1, 4.6.2 and 4.7.2 alike. None of those is a sentence a
// caller can act on, so each is checked here first and refused by name.
//
// A preview runs every check and stops before the undo action, so a dry run
// refuses what the call would refuse and reports the libraries it would add
// to.

} // namespace

json animAddLibrary(const json& params, const std::string& session_kind) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");
    auto parsed = runtime::parseAnimAddLibraryRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error());
    const auto& request = parsed.value();
    // Hash-identical on 4.5.1, 4.6.2 and 4.7.2; see the amendment.
    for (const auto& bind : {std::make_tuple("AnimationMixer", "add_animation_library", 618909818LL),
                             std::make_tuple("AnimationMixer", "remove_animation_library", 3304788590LL),
                             std::make_tuple("AnimationMixer", "has_animation_library", 2619796661LL),
                             std::make_tuple("AnimationMixer", "get_animation_library", 147342321LL),
                             std::make_tuple("AnimationMixer", "get_animation_library_list", 3995934104LL),
                             std::make_tuple("AnimationLibrary", "get_animation_list", 3995934104LL),
                             std::make_tuple("AnimationLibrary", "get_animation", 2933122410LL),
                             std::make_tuple("Animation", "get_length", 1740695150LL),
                             std::make_tuple("Animation", "get_loop_mode", 1988889481LL),
                             std::make_tuple("Animation", "get_track_count", 3905245786LL),
                             std::make_tuple("Animation", "track_get_type", 3445944217LL),
                             std::make_tuple("Animation", "track_get_path", 408788394LL),
                             std::make_tuple("Animation", "track_get_key_count", 923996154LL),
                             std::make_tuple("Resource", "get_path", 201670096LL),
                             std::make_tuple("ResourceLoader", "exists", 4185558881LL),
                             std::make_tuple("ResourceLoader", "load", 3358495409LL)}) {
        if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
    if (preflightUndoManagerBindings().isErr() || preflightUndoRollbackBindings().isErr()) {
        return bridgeError(501, "required_bind_unavailable");
    }

    auto editor = editorInterface();
    if (editor.isErr()) return errorJson(editor.error());
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return errorJson(root.error());
    auto player = resolveAnimationPlayer(request.animation_player_path, session_kind);
    if (player.isErr()) return errorJson(player.error());
    // A library added to a player inside an instance the edited scene does not
    // own is dropped by the save, having been reported as added.
    if (auto refused = refuseUnsavableEdit(root.value(), player.value(),
                                           request.animation_player_path, SceneEdit::Property)) {
        return *refused;
    }

    // The same normalisation rule every other res:// argument answers to, so
    // '..', a doubled slash or a backslash is refused before the loader sees it.
    const std::string suffix = strings::endsWith(request.library_path, ".tres") ? ".tres" : ".res";
    auto valid_path = validateResPath(request.library_path, suffix);
    if (valid_path.isErr()) {
        return errorJson(400, "library_path must be a normalized res:// path ending in .tres or .res");
    }
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return errorJson(loader.error());
    auto path_value = makeString(request.library_path);
    auto hint = makeString("");
    auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (path_value.isErr() || hint.isErr() || cache_mode.isErr()) {
        return errorJson(500, "Failed to construct library load arguments");
    }
    auto exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                   {&path_value.value(), &hint.value()});
    if (exists_value.isErr()) return errorJson(500, exists_value.error().message);
    auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr()) return errorJson(500, exists.error().message);
    if (!exists.value()) {
        return errorJson(404, "No resource at " + request.library_path +
                                  ". Write the AnimationLibrary with resource_create first.");
    }
    if (auto spelled = resPathCaseMismatch(request.library_path)) {
        return bridgeError(400, "library_path_case_mismatch",
                           {{"library_path", request.library_path}, {"library_path_on_disk", *spelled},
                            {"retry_with", {{"library_path", *spelled}}}},
                           "The file is " + *spelled + " on disk. A scene saved with " +
                               request.library_path + " references a path a case-sensitive "
                               "platform and an exported pack cannot open.");
    }
    // Through the loader's cache, so the object added is the one the editor
    // already holds for that file and the scene saves a reference to it.
    auto loaded_value = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                   {&path_value.value(), &hint.value(), &cache_mode.value()});
    if (loaded_value.isErr()) return errorJson(500, loaded_value.error().message);
    auto library = objectFromVariant(loaded_value.value());
    if (library.isErr()) return errorJson(500, library.error().message);
    if (!library.value()) {
        return bridgeError(422, "animation_library_unloadable", {{"library_path", request.library_path}});
    }
    auto is_library = objectIsClass(library.value(), "AnimationLibrary");
    if (is_library.isErr()) return errorJson(500, is_library.error().message);
    if (!is_library.value()) {
        const auto actual = nodeClassName(library.value());
        std::string detail = request.library_path + " holds " + (actual.empty() ? "another type" : "a " + actual) + ".";
        auto is_animation = objectIsClass(library.value(), "Animation");
        if (is_animation.isOk() && is_animation.value()) {
            // The likeliest mistake, and the fix is one more resource_create.
            detail += " A player takes animations inside a library: write an AnimationLibrary whose "
                      "_data maps a name to this Animation, and add that.";
        }
        return bridgeError(422, "not_an_animation_library",
                           {{"library_path", request.library_path}, {"resource_class", actual}}, detail);
    }

    auto list_value = callObject(player.value(), "AnimationMixer", "get_animation_library_list", 3995934104LL);
    if (list_value.isErr()) return errorJson(500, list_value.error().message);
    auto existing = readNameList(list_value.value(), std::numeric_limits<size_t>::max());
    if (existing.isErr()) return errorJson(500, existing.error().message);
    const auto describe_name = [](const std::string& name) {
        return name.empty() ? std::string("the default library \"\"") : "\"" + name + "\"";
    };
    // The name first: it is what the caller chose, so it is the refusal they
    // can act on even when the library is also already there.
    for (const auto& name : existing.value().names) {
        if (name != request.library_name) continue;
        json data = {{"library_name", name}, {"library_names", existing.value().names}};
        std::string held_path;
        auto name_value = makeStringName(name);
        if (name_value.isOk()) {
            auto held = callObject(player.value(), "AnimationMixer", "get_animation_library", 147342321LL,
                                   {&name_value.value()});
            auto held_object = held.isOk() ? objectFromVariant(held.value()) : Result<GDExtensionObjectPtr>(held.error());
            if (held_object.isOk() && held_object.value()) {
                auto held_path_value = callObject(held_object.value(), "Resource", "get_path", 201670096LL);
                if (held_path_value.isOk()) {
                    auto text = stringFromVariant(held_path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                    if (text.isOk()) held_path = text.value();
                }
            }
        }
        // A library built into the scene has no file, and one whose path is
        // inside a scene file is spelled "res://x.tscn::id"; neither is a path
        // the caller could pass back, so only a real file is named.
        const bool file_backed = !held_path.empty() && held_path.find("::") == std::string::npos;
        data["existing_library_path"] = file_backed ? json(held_path) : json(nullptr);
        std::string detail = "The player already has " + describe_name(name) +
                             (file_backed ? ", from " + held_path : std::string(", built into the scene")) +
                             ". This tool adds and never replaces; choose another library_name.";
        return bridgeError(409, "animation_library_name_taken", std::move(data), detail);
    }
    for (const auto& name : existing.value().names) {
        auto name_value = makeStringName(name);
        if (name_value.isErr()) return errorJson(500, name_value.error().message);
        auto held = callObject(player.value(), "AnimationMixer", "get_animation_library", 147342321LL,
                               {&name_value.value()});
        if (held.isErr()) return errorJson(500, held.error().message);
        auto held_object = objectFromVariant(held.value());
        if (held_object.isErr()) return errorJson(500, held_object.error().message);
        if (held_object.value() != library.value()) continue;
        return bridgeError(409, "animation_library_already_added",
                           {{"library_name", name}, {"library_path", request.library_path}},
                           request.library_path + " is on this player as " + describe_name(name) + ".");
    }

    // Whether the editor's copy is still the file. The editor keeps every
    // resource it has loaded, and nothing an unattended editor does re-reads a
    // file that changed underneath it: not editor_reload_project, and not
    // EditorFileSystem.update_file or scan either, measured in vibe session
    // seventeen. So a library rewritten by resource_create or by an agent's own
    // file tools was added as the old copy and reported by its old names, while
    // the saved scene referenced the file and the game loaded the new one.
    //
    // IGNORE_DEEP (3) reads the file and the animation files it references
    // without touching the cache; REPLACE_DEEP (4) reloads the cached objects
    // in place, keeping their identity, which is what the editor itself does
    // when it notices a change. Both measured on 4.5.1, 4.6.2 and 4.7.2.
    auto ignore_deep = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(3));
    if (ignore_deep.isErr()) return errorJson(500, ignore_deep.error().message);
    auto disk_value = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                 {&path_value.value(), &hint.value(), &ignore_deep.value()});
    auto disk = disk_value.isOk() ? objectFromVariant(disk_value.value())
                                  : Result<GDExtensionObjectPtr>(disk_value.error());
    if (disk.isErr() || !disk.value()) {
        return bridgeError(422, "animation_library_unloadable", {{"library_path", request.library_path}},
                           "The editor holds a copy it loaded earlier, and the file itself no longer "
                           "loads, so a scene saved with it would not either.");
    }
    auto held_print = libraryFingerprint(library.value());
    auto disk_print = libraryFingerprint(disk.value());
    if (held_print.isErr()) return errorJson(500, held_print.error().message);
    if (disk_print.isErr()) return errorJson(500, disk_print.error().message);
    const bool stale = held_print.value() != disk_print.value();
    if (stale && !request.reload_from_disk) {
        const auto names_of = [](const json& print) {
            json names = json::array();
            for (auto it = print["animations"].begin(); it != print["animations"].end(); ++it) {
                names.push_back(it.key());
            }
            return names;
        };
        const json editor_names = names_of(held_print.value());
        const json file_names = names_of(disk_print.value());
        std::string detail = "The editor holds a copy of " + request.library_path +
                             " that no longer matches the file: ";
        detail += editor_names == file_names
                      ? "the same animations, " + editor_names.dump() + ", with different lengths or tracks."
                      : editor_names.dump() + " in the editor, " + file_names.dump() + " in the file.";
        detail += " Send reload_from_disk: true to take the file's version; that also updates every "
                  "player already holding this library, and discards any change made to the editor's "
                  "copy and not saved.";
        // Names rather than the whole fingerprint: a library at the catalog
        // caps would put this refusal over the response limit, and an answer
        // over the limit costs the caller the route.
        return bridgeError(409, "animation_library_differs_from_disk",
                           {{"library_path", request.library_path},
                            {"editor_copy_animations", editor_names},
                            {"file_animations", file_names},
                            {"retry_with", {{"reload_from_disk", true}}}},
                           detail);
    }
    // In a preview the reload does not happen, so the names come from the file
    // the real call would reload.
    const GDExtensionObjectPtr named_from = stale && request.preview ? disk.value() : library.value();
    if (stale && !request.preview) {
        auto replace_deep = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(4));
        if (replace_deep.isErr()) return errorJson(500, replace_deep.error().message);
        auto reloaded = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                   {&path_value.value(), &hint.value(), &replace_deep.value()});
        auto reloaded_object = reloaded.isOk() ? objectFromVariant(reloaded.value())
                                               : Result<GDExtensionObjectPtr>(reloaded.error());
        if (reloaded_object.isErr() || reloaded_object.value() != library.value()) {
            return bridgeError(500, "animation_library_reload_failed", {{"library_path", request.library_path}});
        }
    }

    // What the player will answer to once the library is on it: a named
    // library's animations are "name/animation", the default library's are
    // their own names. These are the names anim_play_track takes.
    auto animation_list = callObject(named_from, "AnimationLibrary", "get_animation_list", 3995934104LL);
    if (animation_list.isErr()) return errorJson(500, animation_list.error().message);
    auto contained = readNameList(animation_list.value(), runtime::kMaxAnimations);
    if (contained.isErr()) return errorJson(500, contained.error().message);
    json animations = json::array();
    for (const auto& animation : contained.value().names) {
        animations.push_back(request.library_name.empty() ? animation : request.library_name + "/" + animation);
    }
    const bool animations_truncated =
        contained.value().total > static_cast<int64_t>(contained.value().names.size());

    if (request.preview) {
        return liveResult({{"preview", true},
                           {"animation_player_path", request.animation_player_path},
                           {"library_name", request.library_name},
                           {"library_path", request.library_path},
                           {"library_names", existing.value().names},
                           {"animations", animations},
                           {"animation_count", contained.value().total},
                           {"animations_truncated", animations_truncated},
                           {"editor_copy_matches_file", !stale}});
    }

    auto name_value = makeStringName(request.library_name);
    auto library_value = makeObject(library.value());
    if (name_value.isErr() || library_value.isErr()) {
        return errorJson(500, "Failed to construct library arguments");
    }
    auto manager = undoManager(editor.value());
    if (manager.isErr()) return errorJson(manager.error());
    auto action = createAction(manager.value(),
                               "Didi: add animation library " + describe_name(request.library_name),
                               root.value());
    if (action.isErr()) return errorJson(500, action.error().message);
    auto apply = managerMethod(manager.value(), "add_do_method", player.value(), "add_animation_library",
                               {&name_value.value(), &library_value.value()});
    auto revert = apply.isOk()
        ? managerMethod(manager.value(), "add_undo_method", player.value(), "remove_animation_library",
                        {&name_value.value()})
        : Result<void>(apply.error());
    if (apply.isErr() || revert.isErr()) {
        abandonAction(manager.value());
        return bridgeError(500, "animation_library_undo_registration_failed");
    }
    auto committed = commitAction(manager.value());
    if (committed.isErr()) return errorJson(committed.error());

    // The engine returns an Error from add_animation_library and the undo
    // manager drops it, so the only evidence the add happened is the player
    // holding this exact object under this name afterwards.
    bool held_now = false;
    auto has = callObject(player.value(), "AnimationMixer", "has_animation_library", 2619796661LL,
                          {&name_value.value()});
    auto has_flag = has.isOk() ? scalarFromVariant<GDExtensionBool>(has.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                               : Result<GDExtensionBool>(has.error());
    if (has_flag.isOk() && has_flag.value()) {
        auto held = callObject(player.value(), "AnimationMixer", "get_animation_library", 147342321LL,
                               {&name_value.value()});
        auto held_object = held.isOk() ? objectFromVariant(held.value()) : Result<GDExtensionObjectPtr>(held.error());
        held_now = held_object.isOk() && held_object.value() == library.value();
    }
    if (!held_now) {
        auto reverted = undoLastAction(manager.value(), root.value());
        if (reverted.isErr()) {
            return bridgeError(500, "animation_library_postcondition_mismatch",
                               {{"outcome", "unknown"}, {"rolled_back", false}},
                               "The committed action could not be undone: " + reverted.error().message);
        }
        return bridgeError(500, "animation_library_postcondition_mismatch",
                           {{"outcome", "rolled_back"}, {"rolled_back", true}});
    }

    auto after_value = callObject(player.value(), "AnimationMixer", "get_animation_library_list", 3995934104LL);
    auto after = after_value.isOk() ? readNameList(after_value.value(), std::numeric_limits<size_t>::max())
                                    : Result<NameList>(after_value.error());
    return liveSceneMutation({{"status", "success"},
                              {"animation_player_path", request.animation_player_path},
                              {"library_name", request.library_name},
                              {"library_path", request.library_path},
                              {"library_names", after.isOk() ? json(after.value().names) : json(nullptr)},
                              {"animations", animations},
                              {"animation_count", contained.value().total},
                              {"animations_truncated", animations_truncated},
                              // Not part of the undo action: the file is what
                              // the scene references, and undoing the add
                              // does not bring an out-of-date copy back.
                              {"reloaded_from_disk", stale},
                              {"undo_redo_registered", true}});
}

} // namespace didi::godot::bridge_internal
