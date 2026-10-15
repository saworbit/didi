#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/common/config_file_syntax.hpp"
#include "didi/common/godot_error.hpp"
#include "didi/common/project_path.hpp"
#include "didi/offline/input_actions.hpp"
#include "didi/offline/project_settings_file.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>

namespace didi::godot {
using namespace bridge_internal;
namespace {
Result<void> validateSettingName(const std::string& setting) {
    if (setting.empty() || setting.front() == '/' || setting.back() == '/' ||
        setting.find('/') == std::string::npos || setting.find("//") != std::string::npos ||
        setting.find('\\') != std::string::npos) {
        return Error::invalidArgument("setting must be a non-empty slash-delimited ProjectSettings name");
    }
    return Result<void>::ok();
}

Result<void> validateGenericSettingName(const std::string& setting) {
    auto valid = validateSettingName(setting);
    if (valid.isErr()) return valid;
    if (strings::startsWith(setting, "autoload/") || strings::startsWith(setting, "input/")) {
        return Error::invalidArgument("Use the typed autoload or InputMap tools for this setting namespace");
    }
    return Result<void>::ok();
}

Result<void> validateIdentifier(const std::string& value, const std::string& label) {
    if (value.empty() || value.size() > 128 ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) {
        return Error::invalidArgument(label + " must be a valid identifier");
    }
    for (unsigned char character : value) {
        if (!std::isalnum(character) && character != '_') {
            return Error::invalidArgument(label + " must be a valid identifier");
        }
    }
    return Result<void>::ok();
}

// The event a descriptor names, built the way the editor's Input Map dialog
// builds one. What the descriptor may say, and every refusal of it, is shared
// with the offline route, which writes the same event into project.godot when
// no editor is attached (#1379).
Result<VariantValue> makeInputEvent(const json& descriptor) {
    auto parsed = didi::offline::parseInputEventDescriptor(descriptor);
    if (parsed.isErr()) return parsed.error();
    const auto& spec = parsed.value();
    const char* class_name = spec.class_name.c_str();

    NativeName native_class(class_name);
    auto object = constructObject(native_class.ptr());
    if (!object) return Error::internal("Godot could not construct " + spec.class_name);
    auto fail = [&](const Error& error) -> Result<VariantValue> {
        GodotApi::instance().object_destroy(object);
        return error;
    };
    auto set_int = [&](const char* owner, const char* method, int64_t hash, int64_t number) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, number);
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, hash, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    auto set_bool = [&](const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(object, "InputEventWithModifiers", method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };

    // An event with no device is bound to all of them (-1), as the editor's
    // Input Map dialog binds it. The engine's construction default is 0 for a
    // joypad, so a second controller never fired the action, and for a key it
    // is 0 on 4.5.1 and 16 on 4.7.2, so one call wrote different files on
    // different lines (#1295).
    if (auto set = set_int("InputEvent", "set_device", 1286410249LL, spec.device); set.isErr()) {
        return fail(set.error());
    }
    if (spec.type == "key") {
        const std::pair<const char*, const std::optional<int64_t>*> identities[] = {
            {"keycode", &spec.keycode}, {"physical_keycode", &spec.physical_keycode}, {"unicode", &spec.unicode}};
        for (const auto& [key, value] : identities) {
            if (!value->has_value()) continue;
            const int64_t hash = std::string(key) == "unicode" ? 1286410249LL : 888074362LL;
            auto set = set_int("InputEventKey", (std::string("set_") + key).c_str(), hash, **value);
            if (set.isErr()) return fail(set.error());
        }
        const std::pair<const char*, const std::optional<bool>*> modifiers[] = {
            {"set_shift_pressed", &spec.shift_pressed}, {"set_alt_pressed", &spec.alt_pressed},
            {"set_ctrl_pressed", &spec.ctrl_pressed}, {"set_meta_pressed", &spec.meta_pressed}};
        for (const auto& [setter, value] : modifiers) {
            if (!value->has_value()) continue;
            auto set = set_bool(setter, **value);
            if (set.isErr()) return fail(set.error());
        }
    } else if (spec.type == "mouse_button" || spec.type == "joypad_button") {
        auto set = set_int(class_name, "set_button_index", spec.type == "mouse_button" ? 3624991109LL : 1466368136LL,
                           *spec.button_index);
        if (set.isErr()) return fail(set.error());
    } else {
        auto axis = set_int(class_name, "set_axis", 1332685170LL, *spec.axis);
        if (axis.isErr()) return fail(axis.error());
        auto axis_value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, *spec.axis_value);
        if (axis_value.isErr()) return fail(axis_value.error());
        auto set = callObject(object, class_name, "set_axis_value", 373806689LL, {&axis_value.value()});
        if (set.isErr()) return fail(set.error());
    }
    auto result = makeObject(object);
    if (result.isErr()) return fail(result.error());
    return result;
}

Result<json> inputEventToJson(VariantValue& event_value) {
    auto event = objectFromVariant(event_value);
    if (event.isErr() || !event.value()) return Error::invalidArgument("InputMap contains a null input event");
    auto is_class = [&](const char* name) -> Result<bool> {
        auto class_name = makeString(name);
        if (class_name.isErr()) return class_name.error();
        auto result = callObject(event.value(), "Object", "is_class", 3927539163LL, {&class_name.value()});
        if (result.isErr()) return result.error();
        auto value = scalarFromVariant<GDExtensionBool>(result.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        return value.isOk() ? Result<bool>(value.value() != 0) : Result<bool>(value.error());
    };
    auto get_int = [&](const char* owner, const char* method, int64_t hash) -> Result<int64_t> {
        auto result = callObject(event.value(), owner, method, hash);
        if (result.isErr()) return result.error();
        return scalarFromVariant<int64_t>(result.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    auto device = get_int("InputEvent", "get_device", 3905245786LL);
    if (device.isErr()) return device.error();
    json output = {{"device", device.value()}};

    auto key = is_class("InputEventKey");
    auto mouse = is_class("InputEventMouseButton");
    auto joy_button = is_class("InputEventJoypadButton");
    auto joy_motion = is_class("InputEventJoypadMotion");
    if (key.isErr() || mouse.isErr() || joy_button.isErr() || joy_motion.isErr()) return Error::internal("Failed to identify InputEvent type");
    if (key.value()) {
        output["type"] = "key";
        for (const auto& field : std::array<std::pair<const char*, int64_t>, 3>{{
                 {"keycode", 1585896689LL}, {"physical_keycode", 1585896689LL}, {"unicode", 3905245786LL}}}) {
            auto value = get_int("InputEventKey", (std::string("get_") + field.first).c_str(), field.second);
            if (value.isErr()) return value.error();
            output[field.first] = value.value();
        }
        for (const auto* modifier : {"shift", "alt", "ctrl", "meta"}) {
            auto value = callObject(event.value(), "InputEventWithModifiers",
                                    (std::string("is_") + modifier + "_pressed").c_str(), 36873697LL);
            if (value.isErr()) return value.error();
            auto enabled = scalarFromVariant<GDExtensionBool>(value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (enabled.isErr()) return enabled.error();
            // Both names. Godot's own property is shift_pressed, which is what
            // this tool writes into project.godot and what runtime_inject_input
            // takes, and reporting only the short form left the engine's name
            // as the one spelling that appeared nowhere a caller could see
            // (#737). The short one stays because it is published.
            output[modifier] = enabled.value() != 0;
            output[std::string(modifier) + "_pressed"] = enabled.value() != 0;
        }
    } else if (mouse.value() || joy_button.value()) {
        output["type"] = mouse.value() ? "mouse_button" : "joypad_button";
        auto button = get_int(mouse.value() ? "InputEventMouseButton" : "InputEventJoypadButton", "get_button_index",
                              mouse.value() ? 1132662608LL : 595588182LL);
        if (button.isErr()) return button.error();
        output["button_index"] = button.value();
    } else if (joy_motion.value()) {
        output["type"] = "joypad_motion";
        auto axis = get_int("InputEventJoypadMotion", "get_axis", 4019121683LL);
        auto axis_value_variant = callObject(event.value(), "InputEventJoypadMotion", "get_axis_value", 1740695150LL);
        if (axis.isErr()) return axis.error();
        if (axis_value_variant.isErr()) return axis_value_variant.error();
        auto axis_value = scalarFromVariant<double>(axis_value_variant.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (axis_value.isErr()) return axis_value.error();
        output["axis"] = axis.value();
        output["axis_value"] = axis_value.value();
    } else {
        return Error::invalidArgument("InputMap contains an unsupported InputEvent class");
    }
    return output;
}

// project.godot's modified time and size, which any write moves. Read from the
// directory entry, so a file the editor cannot open still answers.
struct ProjectFileStamp {
    std::filesystem::file_time_type written;
    std::uintmax_t bytes{0};
    bool operator==(const ProjectFileStamp&) const = default;
};

std::optional<ProjectFileStamp> projectFileStamp() {
    auto directory = projectDirectoryOnDisk();
    if (directory.isErr()) return std::nullopt;
    const auto file = didi::paths::projectPathFromUtf8(directory.value()) / "project.godot";
    std::error_code time_error;
    std::error_code size_error;
    const auto written = std::filesystem::last_write_time(file, time_error);
    const auto bytes = std::filesystem::file_size(file, size_error);
    if (time_error || size_error) return std::nullopt;
    return ProjectFileStamp{written, bytes};
}

// Puts a setting back after ProjectSettings.save failed. file_before is
// project.godot as it stood before that save.
Result<void> restoreProjectSetting(GDExtensionObjectPtr project_settings, VariantValue& name,
                                   VariantValue& previous,
                                   const std::optional<ProjectFileStamp>& file_before) {
    auto restored = callObject(project_settings, "ProjectSettings", "set_setting", 402577236LL,
                               {&name, &previous});
    if (restored.isErr()) return restored.error();
    // A save that failed and left the file as it was needs nothing more: the
    // old value back in memory is the whole rollback. Saving again into a file
    // the engine has just failed to write fails the same way, so the engine
    // printed its error a second time and a rollback that had worked answered
    // "rollback failed". Measured on 4.6.2 with project.godot and its folder
    // denied to the editor: the setting was gone from memory and the file
    // unchanged. Only a save that did move the file is written over.
    if (file_before.has_value() && projectFileStamp() == file_before) return Result<void>::ok();
    auto saved = callObject(project_settings, "ProjectSettings", "save", 166280745LL);
    if (saved.isErr()) return saved.error();
    auto code = scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (code.isErr()) return code.error();
    if (code.value() != 0) {
        return Error::internal("Rollback ProjectSettings.save failed with " +
                               ::didi::godot::describeGodotError(code.value()));
    }
    return Result<void>::ok();
}

// A save the engine refused, with the change rolled back so nothing moved.
// The usual cause is a project.godot the editor cannot write: read-only, held
// by another program, or in a folder that refuses new files. That is the
// caller's to fix, not a fault in the server (#1153). A failed rollback stays
// a 500.
json projectFileNotWritable(const std::string& change, const std::string& detail) {
    json project_file = nullptr;
    bool exists = false;
    bool read_only = false;
    auto directory = projectDirectoryOnDisk();
    if (directory.isOk()) {
        const auto file = didi::paths::projectPathFromUtf8(directory.value()) / "project.godot";
        std::error_code error;
        exists = std::filesystem::exists(file, error);
        const auto permissions = exists ? std::filesystem::status(file, error).permissions()
                                        : std::filesystem::perms::unknown;
        project_file = didi::paths::nativePathToUtf8(file);
        read_only = exists && !error &&
                    (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    }
    return errorJson(409,
                     "project.godot could not be saved (" + detail + "), so the " + change +
                         " was rolled back and nothing changed. The editor cannot write project.godot: "
                         "it is read-only, held by another program, or in a folder that refuses new files.",
                     {{"code", "project_file_not_writable"},
                      {"project_file", std::move(project_file)},
                      {"exists", exists},
                      {"read_only", read_only},
                      {"rolled_back", true},
                      {"engine_error", detail}});
}

// The input actions project.godot itself declares.
//
// ProjectSettings.has_setting answers true for an engine default such as
// ui_accept, because the engine registers the built-in map as settings. That is
// the right answer to "does this action exist" and the wrong answer to "did
// this project define it". Removal needs the second one, because an action the
// file does not contain cannot be removed by writing the file (#485), and so
// does a listing asked for the project's own actions (#775).
struct ProjectInputActions {
    std::set<std::string> names;
    // Set when the file cannot answer, in which case names means nothing.
    std::optional<Error> failure;
};

ProjectInputActions projectFileInputActions() {
    ProjectInputActions declared;
    auto directory = projectDirectoryOnDisk();
    if (directory.isErr()) {
        declared.failure = directory.error();
        return declared;
    }
    std::ifstream file(std::filesystem::path(directory.value()) / "project.godot");
    if (!file.is_open()) return declared;

    std::ostringstream contents;
    contents << file.rdbuf();
    const auto scanned = didi::config_file::scan(contents.str());
    // A manifest Godot will not load cannot answer this. The parse stops where
    // it breaks, so an action below that point is absent from the scan and
    // "the project does not define it" is a claim with nothing behind it --
    // and the removal it gates rewrites the whole file from the editor's own
    // ProjectSettings, which would take the hand edit that broke the file with
    // it (#826).
    if (const auto failure = didi::config_file::loadFailure(scanned)) {
        std::string where = failure->unterminated
            ? std::string("project.godot ends part-way through a value")
            : "project.godot line " + std::to_string(failure->line) + " sets " +
                  (failure->section.empty() ? failure->key
                                            : failure->section + "/" + failure->key) +
                  " to a value Godot's parser refuses, because " + failure->value_reason;
        declared.failure = Error(409, where +
                                          ". The engine answers ERR_PARSE_ERROR for the whole "
                                          "file, so the project does not open and what it "
                                          "declares cannot be read.",
                                 {{"code", "project_file_unparseable"}});
        return declared;
    }
    // The action names the engine registers. Godot writes the name bare, or
    // quoted when it needs the spaces, and the value is a dictionary spread
    // over the four lines below it -- none of which a line-at-a-time reader
    // gets right on its own (#813).
    for (const auto& entry : scanned.entries) {
        if (entry.section == "input") declared.names.insert(entry.key);
    }
    return declared;
}

// What project.godot holds after a save, read back from the file. The three
// project writers answered with the request and the save's return code, and
// ProjectSettings holds whatever was set whether or not the save wrote it, so
// nothing in the answer had been read after the write (#1019). The file is
// what the next start loads, so it is the state to report.

// The literal the file holds for one setting, or null when it has no line for
// it: after a removal, or for a built-in set to its default, which Godot does
// not write.
Result<json> projectFileSettingLiteral(const std::string& setting) {
    auto directory = projectDirectoryOnDisk();
    if (directory.isErr()) return directory.error();
    auto read = didi::offline::readProjectSetting(std::filesystem::path(directory.value()), setting);
    if (read.isErr()) return read.error();
    if (!read.value().existed) return json(nullptr);
    return json(read.value().literal);
}

// The autoload the file declares under this name, or null when it declares none.
Result<json> projectFileAutoload(const std::string& name) {
    auto directory = projectDirectoryOnDisk();
    if (directory.isErr()) return directory.error();
    auto read = didi::offline::readProjectAutoloads(std::filesystem::path(directory.value()));
    if (read.isErr()) return read.error();
    for (const auto& autoload : read.value()) {
        if (autoload.name != name) continue;
        return json{{"name", autoload.name}, {"path", autoload.path},
                    {"singleton", autoload.singleton}};
    }
    return json(nullptr);
}

Result<bool> projectFileDefinesInputAction(const std::string& action) {
    auto declared = projectFileInputActions();
    if (declared.failure) {
        auto failure = *declared.failure;
        if (failure.code == 409) {
            failure.message += " Repair the file before removing an input action from it: saving "
                               "the settings would write this editor's whole map over it.";
        }
        return failure;
    }
    return declared.names.count(action) != 0;
}

const char* variantTypeName(GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_NIL: return "null";
        case GDEXTENSION_VARIANT_TYPE_BOOL: return "bool";
        case GDEXTENSION_VARIANT_TYPE_INT: return "int";
        case GDEXTENSION_VARIANT_TYPE_FLOAT: return "float";
        case GDEXTENSION_VARIANT_TYPE_STRING: return "String";
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME: return "StringName";
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH: return "NodePath";
        case GDEXTENSION_VARIANT_TYPE_ARRAY: return "Array";
        case GDEXTENSION_VARIANT_TYPE_DICTIONARY: return "Dictionary";
        case GDEXTENSION_VARIANT_TYPE_COLOR: return "Color";
        case GDEXTENSION_VARIANT_TYPE_VECTOR2: return "Vector2";
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I: return "Vector2i";
        case GDEXTENSION_VARIANT_TYPE_VECTOR3: return "Vector3";
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I: return "Vector3i";
        case GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY: return "PackedStringArray";
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY: return "PackedInt32Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY: return "PackedInt64Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY: return "PackedFloat32Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY: return "PackedFloat64Array";
        default: break;
    }
    return "another type";
}

// Whether a value the caller sent can stand in for the type a setting holds.
//
// JSON carries fewer types than Variant does, so the conversions the engine
// does anyway are allowed: an integer into a float setting, a whole number into
// an int setting, a string into a StringName or a NodePath, a list into any of
// the packed arrays. Everything else is a different setting wearing the same
// name, which is what made the project name an integer (#490).
// The settings whose value JSON has no type of its own for: a Color, a
// vector, a compound such as Rect2, or a packed array.
bool incomingTypeFitsSetting(GDExtensionVariantType current, GDExtensionVariantType incoming,
                             bool incoming_is_whole_number) {
    if (current == incoming) return true;
    // Nothing is known about a setting the engine holds as null, so nothing is
    // refused on its account.
    if (current == GDEXTENSION_VARIANT_TYPE_NIL) return true;
    if (current == GDEXTENSION_VARIANT_TYPE_FLOAT &&
        incoming == GDEXTENSION_VARIANT_TYPE_INT) {
        return true;
    }
    if (current == GDEXTENSION_VARIANT_TYPE_INT &&
        incoming == GDEXTENSION_VARIANT_TYPE_FLOAT) {
        return incoming_is_whole_number;
    }
    if ((current == GDEXTENSION_VARIANT_TYPE_STRING_NAME ||
         current == GDEXTENSION_VARIANT_TYPE_NODE_PATH) &&
        incoming == GDEXTENSION_VARIANT_TYPE_STRING) {
        return true;
    }
    if (incoming == GDEXTENSION_VARIANT_TYPE_ARRAY) {
        switch (current) {
            case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR2_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY:
            case GDEXTENSION_VARIANT_TYPE_PACKED_COLOR_ARRAY:
                return true;
            default: break;
        }
    }
    return false;
}

// Whether a res:// path the caller gave points at something that is there.
// project_set_autoload already refuses a script that is not on disk; a setting
// that names a scene had no such check, so run/main_scene could be pointed at a
// file that does not exist and the project stopped running (#490).
Result<bool> resourcePathExistsOnDisk(const std::string& resource_path) {
    auto directory = projectDirectoryOnDisk();
    if (directory.isErr()) return directory.error();
    const std::string relative = resource_path.substr(6);
    std::error_code error;
    const auto target = std::filesystem::path(directory.value()) /
                        std::filesystem::path(paths::projectPathFromUtf8(relative));
    return std::filesystem::exists(target, error) && !error;
}

} // namespace

json GodotBridge::bridgeProjectResolveUids(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    // ResourceUID is the engine's own UID table. Didi reads it instead of
    // .godot/uid_cache.bin, which is an undocumented binary cache written on
    // the editor's schedule: absent on a fresh clone, stale between saves,
    // and readable mid-write. Every bind below carries the same hash on
    // Godot 4.5.1, 4.6.2 and 4.7.2, so there is no version gate here.
    if (!params.contains("queries") || !params["queries"].is_array()) {
        return errorJson(400, "project.resolveUids requires a queries array");
    }
    const auto& queries = params["queries"];
    if (queries.empty() || queries.size() > 256) {
        return errorJson(400, "project.resolveUids accepts 1 to 256 queries");
    }
    for (const auto& bind : {std::make_tuple("ResourceUID", "text_to_id", 1321353865LL),
                             std::make_tuple("ResourceUID", "has_id", 1116898809LL),
                             std::make_tuple("ResourceUID", "get_id_path", 844755477LL),
                             std::make_tuple("ResourceUID", "path_to_uid", 1703090593LL),
                             std::make_tuple("ResourceLoader", "exists", 4185558881LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }
    auto uid_table = singleton("ResourceUID");
    if (uid_table.isErr()) return errorJson(uid_table.error());
    auto resource_loader = singleton("ResourceLoader");
    if (resource_loader.isErr()) return errorJson(resource_loader.error());

    // Whether Godot can load a path, which is the question a reference asks.
    // It is not the same question as whether a file scan indexed the path:
    // a remap, a type the scan does not index, or a scan that hit its own
    // cap all read as absent on disk and load perfectly well here.
    auto engineCanLoad = [&](const std::string& res_path) -> Result<bool> {
        auto path_arg = makeString(res_path);
        if (path_arg.isErr()) return path_arg.error();
        auto hint = makeString(std::string{});
        if (hint.isErr()) return hint.error();
        auto answer = callObject(resource_loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                 {&path_arg.value(), &hint.value()});
        if (answer.isErr()) return answer.error();
        auto loadable = scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (loadable.isErr()) return loadable.error();
        return loadable.value() != 0;
    };

    json entries = json::array();
    for (const auto& value : queries) {
        if (!value.is_string()) return errorJson(400, "project.resolveUids queries must be strings");
        const std::string query = value.get<std::string>();
        json entry{{"query", query}, {"found", false}, {"uid", ""}, {"path", ""}};
        if (query.rfind("uid://", 0) == 0) {
            auto text = makeString(query);
            if (text.isErr()) return errorJson(500, text.error().message);
            auto id_value = callObject(uid_table.value(), "ResourceUID", "text_to_id", 1321353865LL,
                                       {&text.value()});
            if (id_value.isErr()) return errorJson(500, id_value.error().message);
            auto id = scalarFromVariant<int64_t>(id_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (id.isErr()) return errorJson(500, id.error().message);
            if (id.value() < 0) {
                // ResourceUID answers INVALID_ID for text that is not a UID
                // at all, which is a different fact from a UID it has never
                // been told about.
                entry["reason"] = "malformed_uid";
            } else {
                auto id_arg = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, id.value());
                if (id_arg.isErr()) return errorJson(500, id_arg.error().message);
                auto known = callObject(uid_table.value(), "ResourceUID", "has_id", 1116898809LL,
                                        {&id_arg.value()});
                if (known.isErr()) return errorJson(500, known.error().message);
                auto is_known = scalarFromVariant<GDExtensionBool>(known.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (is_known.isErr()) return errorJson(500, is_known.error().message);
                if (!is_known.value()) {
                    entry["reason"] = "unknown_to_engine";
                } else {
                    auto path_value = callObject(uid_table.value(), "ResourceUID", "get_id_path", 844755477LL,
                                                 {&id_arg.value()});
                    if (path_value.isErr()) return errorJson(500, path_value.error().message);
                    auto resolved = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                    if (resolved.isErr()) return errorJson(500, resolved.error().message);
                    entry["found"] = true;
                    entry["uid"] = query;
                    entry["path"] = resolved.value();
                    // A UID the engine still holds can point at a file that
                    // is gone. Registered is not the same as loadable.
                    auto loadable = engineCanLoad(resolved.value());
                    if (loadable.isErr()) return errorJson(500, loadable.error().message);
                    entry["exists"] = loadable.value();
                }
            }
        } else if (query.rfind("res://", 0) == 0) {
            auto path_arg = makeString(query);
            if (path_arg.isErr()) return errorJson(500, path_arg.error().message);
            auto uid_value = callObject(uid_table.value(), "ResourceUID", "path_to_uid", 1703090593LL,
                                        {&path_arg.value()});
            if (uid_value.isErr()) return errorJson(500, uid_value.error().message);
            auto uid_text = stringFromVariant(uid_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            if (uid_text.isErr()) return errorJson(500, uid_text.error().message);
            // path_to_uid hands back what it was given when the engine holds
            // no UID for that path, so the prefix is what separates a hit
            // from a miss rather than an empty string.
            // A path can load without any UID registered for it, so
            // existence is answered separately from resolution rather than
            // inferred from it.
            auto loadable = engineCanLoad(query);
            if (loadable.isErr()) return errorJson(500, loadable.error().message);
            entry["exists"] = loadable.value();
            if (uid_text.value().rfind("uid://", 0) == 0) {
                entry["found"] = true;
                entry["uid"] = uid_text.value();
                entry["path"] = query;
            } else {
                entry["reason"] = "unknown_to_engine";
            }
        } else {
            entry["reason"] = "unsupported_query";
        }
        entries.push_back(std::move(entry));
    }
    return liveResult({{"status", "success"}, {"entries", std::move(entries)}});
}


json GodotBridge::bridgeProjectSettings(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    const std::string setting = params.value("setting", "");
    auto valid_name = method == "project.setSetting"
        ? validateGenericSettingName(setting)
        : validateSettingName(setting);
    if (valid_name.isErr()) return errorJson(valid_name.error());

    auto project_settings = singleton("ProjectSettings");
    if (project_settings.isErr()) {
        return errorJson(project_settings.error());
    }
    auto name = makeStringName(setting);
    if (name.isErr()) return errorJson(name.error());
    auto exists_value = callObject(project_settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                                   {&name.value()});
    if (exists_value.isErr()) return errorJson(exists_value.error());
    auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr()) return errorJson(exists.error());

    if (method == "project.getSetting") {
        if (!exists.value()) {
            return errorJson(404, "Project setting not found: " + setting +
                                      ". The attached editor does not define it.",
                             {{"code", "setting_not_found"}});
        }
        VariantValue default_value;
        auto current = callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                  {&name.value(), &default_value});
        if (current.isErr()) return errorJson(current.error());
        auto value = variantToJson(current.value());
        if (value.isErr()) return errorJson(value.error());
        return liveResult({{"status", "success"}, {"setting", setting}, {"value", value.value()}});
    }

    const bool remove = params.value("remove", false);
    if (remove && params.contains("value")) {
        return errorJson(400, "Specify either value or remove: true, not both");
    }
    if (!remove && !params.contains("value")) {
        return errorJson(400, "value is required unless remove is true");
    }
    if (remove && !exists.value()) {
        return errorJson(404, "Project setting not found: " + setting + ". There is nothing to remove.",
                         {{"code", "setting_not_found"}});
    }

    // Godot does support custom project settings, so writing a name the
    // engine does not define is a legitimate mode. It is also
    // indistinguishable from the overwhelmingly more common case, a typo in
    // a real setting name, and the two were answered identically: status
    // success, persisted true, and a key nothing in the engine will ever
    // read. project_get_setting then returned it happily, so reading back
    // did not catch it either (#464).
    //
    // has_setting is the same question project_get_setting already answers
    // with a clean 404. The setter has it available and did not use it.
    const bool create = params.value("create", false);
    if (!remove && !exists.value() && !create) {
        return errorJson(
            404,
            "Project setting not found: " + setting +
                ". The engine does not define this name, so writing it would add a key "
                "nothing reads and report it as persisted. Check the spelling against "
                "project_get_setting. To add a custom setting on purpose, pass "
                "create: true.",
            {{"code", "setting_not_found"}});
    }

    VariantValue default_value;
    auto previous = exists.value()
        ? callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                     {&name.value(), &default_value})
        : Result<VariantValue>(VariantValue{});
    if (previous.isErr()) return errorJson(previous.error());
    // A setting the engine holds as a Color, a vector or a packed array is
    // built as that type, the way scene_set_property builds a property, so
    // the {r, g, b} project_get_setting answers can be written back. Built as
    // plain JSON it was a Dictionary, refused as a type mismatch whatever the
    // spelling (#1293), and a plugin list landed as an Array (#1304).
    const auto held_type = exists.value() ? GodotApi::instance().variant_get_type(previous.value().ptr())
                                          : GDEXTENSION_VARIANT_TYPE_NIL;
    const bool typed = !remove && settingTakesTypedValue(held_type);
    if (typed) {
        auto valid = validateJsonForPropertyType(setting, params["value"], held_type);
        if (valid.isErr()) {
            return errorJson(409, valid.error().message,
                             {{"code", "setting_type_mismatch"},
                              {"setting", setting},
                              {"expected_type", variantTypeName(held_type)},
                              {"retryable", false}});
        }
    }
    auto replacement = remove ? Result<VariantValue>(VariantValue{})
                       : typed ? makeJsonVariantForProperty(params["value"], held_type)
                               : makeJsonVariant(params["value"]);
    if (replacement.isErr()) return errorJson(replacement.error());
    // Removing a setting the engine defines puts its default back. Null
    // erases it from ProjectSettings, and the engine then warned "Property
    // not found" every frame for rendering/environment/defaults/default_clear_color.
    if (remove && exists.value()) {
        auto can_revert = callObject(project_settings.value(), "Object", "property_can_revert", 2619796661LL, {&name.value()});
        auto flag = can_revert.isOk() ? scalarFromVariant<GDExtensionBool>(can_revert.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                                      : Result<GDExtensionBool>(can_revert.error());
        if (flag.isOk() && flag.value()) {
            replacement = callObject(project_settings.value(), "Object", "property_get_revert", 2760726917LL, {&name.value()});
            if (replacement.isErr()) return errorJson(replacement.error());
        }
    }

    // Having established that the engine defines this setting, the tool
    // used to write whatever it was handed. The lookup that answered
    // defined_by_engine is holding the engine's own value, so its type is
    // available at exactly the point where the check belongs (#490).
    if (!remove && exists.value()) {
        const auto current_type = GodotApi::instance().variant_get_type(previous.value().ptr());
        const auto incoming_type = GodotApi::instance().variant_get_type(replacement.value().ptr());
        const bool whole_number = params["value"].is_number_float() &&
                                  params["value"].get<double>() ==
                                      std::floor(params["value"].get<double>());
        if (!incomingTypeFitsSetting(current_type, incoming_type, whole_number)) {
            return errorJson(
                409,
                std::string("Project setting type mismatch: ") + setting + " holds a " +
                    variantTypeName(current_type) + " and the value given is a " +
                    variantTypeName(incoming_type) +
                    ". Writing it would persist a value the engine cannot read back as the "
                    "setting it names.",
                {{"code", "setting_type_mismatch"},
                 {"setting", setting},
                 {"expected_type", variantTypeName(current_type)},
                 {"given_type", variantTypeName(incoming_type)},
                 {"retryable", false}});
        }
    }

    // A res:// value names a file, and a setting pointed at a file that is
    // not there is a project that no longer runs. project_set_autoload has
    // refused this since it was written; this tool did not (#490).
    if (!remove && params["value"].is_string()) {
        const auto text = params["value"].get<std::string>();
        if (strings::startsWith(text, "res://")) {
            auto present = resourcePathExistsOnDisk(text);
            if (present.isErr()) return errorJson(present.error());
            if (!present.value()) {
                return errorJson(404,
                                 "Project setting resource not found: " + text +
                                     ". Nothing is at that path, so " + setting +
                                     " would name a file the project cannot load.",
                                 {{"setting", setting},
                                  {"resource_path", text},
                                  {"resource_exists", false},
                                  {"retryable", false}});
            }
        }
    }

    // What the file held for it before the write, read the way value_written
    // is read after it: the literal, or null for no line (#1151).
    auto literal_before = projectFileSettingLiteral(setting);
    auto applied = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                              {&name.value(), &replacement.value()});
    if (applied.isErr()) return errorJson(applied.error());
    const auto file_before = projectFileStamp();
    auto saved = callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL);
    auto save_code = saved.isOk()
        ? scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT)
        : Result<int64_t>(saved.error());
    if (save_code.isErr() || save_code.value() != 0) {
        auto rollback_save = restoreProjectSetting(project_settings.value(), name.value(), previous.value(),
                                                   file_before);
        const std::string detail = save_code.isErr()
            ? save_code.error().message
            : ::didi::godot::describeGodotError(save_code.value());
        if (rollback_save.isErr()) {
            return errorJson(500, "ProjectSettings.save failed (" + detail + ") and rollback failed: " +
                                  rollback_save.error().message);
        }
        return projectFileNotWritable("setting", detail);
    }
    auto stored = projectFileSettingLiteral(setting);
    if (stored.isErr()) {
        return errorJson(500, "ProjectSettings.save reported success, and project.godot could not "
                              "be read back to confirm it: " + stored.error().message);
    }
    json result = {{"status", "success"}, {"setting", setting}, {"persisted", true},
                   {"removed", remove},
                   // What the file holds now, read back after the save
                   // (#1019). Null when it has no line for the setting.
                   {"value_written", stored.value()},
                   // Whether the engine knew this name before the write. A
                   // caller that passed create: true gets to see which of the
                   // two things it did.
                   {"defined_by_engine", static_cast<bool>(exists.value())}};
    // The literal it replaced, under the name the offline route already
    // uses (#1151): null when the file had no line, an engine default.
    // Absent for a setting the engine did not have.
    if (exists.value() && literal_before.isOk()) result["previous_value"] = literal_before.value();
    // The editor's Audio panel read this setting when it was built and saves
    // the bus layout to that file until the editor restarts. Moving it here
    // left the editor writing the old file while the next start loaded the
    // new one, which was empty, so every bus the project had was gone from
    // the editor after a restart. Measured on 4.7.2 in vibe session
    // nineteen; editor_reload_project is a filesystem scan and does not
    // change it.
    // The editor loads its plugins when it starts, so a plugin this list
    // enables is not loaded, and one it drops keeps running, until the next
    // start. project_set_autoload says the same about its own change (#1304).
    const bool plugins_changed = !(literal_before.isOk() && stored.isOk() &&
                                   literal_before.value() == stored.value());
    if (setting == "editor_plugins/enabled" && !remove && plugins_changed) {
        result["requires_editor_restart"] = true;
        result["limitation"] =
            "The attached editor loaded its plugins when it started. A plugin this list now "
            "enables loads, and one it no longer names stops, when the editor next starts.";
    }
    if (setting == "audio/buses/default_bus_layout") {
        const auto editor_path = startupBusLayoutPath();
        const auto project_path = busLayoutPathFrom(projectSettingString(setting));
        if (editor_path.has_value() && *editor_path != project_path) {
            result["requires_editor_restart"] = true;
            result["editor_layout_path"] = *editor_path;
            result["limitation"] =
                "The attached editor opened the project with " + *editor_path +
                " as its bus layout and keeps saving every bus change there until it "
                "restarts. The next start loads " + project_path + " instead, and the "
                "buses the editor holds now are in the other file, so copy the layout "
                "across or restart the editor before changing any bus.";
        }
    }
    return liveResult(std::move(result));
}


json GodotBridge::bridgeProjectAutoloads(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    auto project_settings = singleton("ProjectSettings");
    if (project_settings.isErr()) return errorJson(project_settings.error());

    if (method == "project.listAutoloads") {
        auto properties = callObject(project_settings.value(), "Object", "get_property_list", 3995934104LL);
        if (properties.isErr()) return errorJson(properties.error());
        auto size_value = callVariant(properties.value(), "size");
        if (size_value.isErr()) return errorJson(size_value.error());
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return errorJson(size.error());
        auto name_key = makeString("name");
        if (name_key.isErr()) return errorJson(name_key.error());
        std::vector<json> entries;
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return errorJson(index.error());
            auto descriptor = callVariant(properties.value(), "get", {&index.value()});
            if (descriptor.isErr()) return errorJson(descriptor.error());
            auto property_name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
            if (property_name_value.isErr()) return errorJson(property_name_value.error());
            auto property_type = GodotApi::instance().variant_get_type(property_name_value.value().ptr());
            if (property_type != GDEXTENSION_VARIANT_TYPE_STRING && property_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) continue;
            auto property_name = stringFromVariant(property_name_value.value(), property_type);
            if (property_name.isErr()) return errorJson(property_name.error());
            if (!strings::startsWith(property_name.value(), "autoload/") || property_name.value().size() <= 9) continue;
            auto setting_name = makeStringName(property_name.value());
            VariantValue default_value;
            if (setting_name.isErr()) return errorJson(setting_name.error());
            auto setting = callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                      {&setting_name.value(), &default_value});
            if (setting.isErr()) return errorJson(setting.error());
            auto setting_type = GodotApi::instance().variant_get_type(setting.value().ptr());
            auto encoded = stringFromVariant(setting.value(), setting_type);
            if (encoded.isErr()) return errorJson(encoded.error());
            const bool autoload_singleton = strings::startsWith(encoded.value(), "*");
            entries.push_back({{"name", property_name.value().substr(9)},
                               {"path", autoload_singleton ? encoded.value().substr(1) : encoded.value()},
                               {"singleton", autoload_singleton}});
        }
        std::sort(entries.begin(), entries.end(), [](const json& left, const json& right) {
            return left["name"].get<std::string>() < right["name"].get<std::string>();
        });
        return liveResult({{"status", "success"}, {"autoloads", entries}});
    }

    const std::string autoload_name = params.value("name", "");
    auto valid_name = validateIdentifier(autoload_name, "autoload name");
    if (valid_name.isErr()) return errorJson(valid_name.error());
    const std::string setting_path = "autoload/" + autoload_name;
    auto setting_name = makeStringName(setting_path);
    if (setting_name.isErr()) return errorJson(setting_name.error());
    auto exists_value = callObject(project_settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                                   {&setting_name.value()});
    if (exists_value.isErr()) return errorJson(exists_value.error());
    auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr()) return errorJson(exists.error());
    const bool removing = method == "project.removeAutoload";
    if (removing && !exists.value()) return errorJson(404, "Autoload not found: " + autoload_name);
    if (!removing && exists.value() && !params.value("replace", false)) {
        return errorJson(409, "Autoload already exists; pass replace: true to update it",
                              {{"code", "already_exists"}, {"retry_with", {{"replace", true}}}});
    }

    VariantValue default_value;
    auto previous = exists.value()
        ? callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                     {&setting_name.value(), &default_value})
        : Result<VariantValue>(VariantValue{});
    if (previous.isErr()) return errorJson(previous.error());
    Result<VariantValue> replacement(VariantValue{});
    std::string resource_path;
    bool autoload_singleton = true;
    if (!removing) {
        resource_path = params.value("path", "");
        auto valid_path = strings::endsWith(resource_path, ".gd") || strings::endsWith(resource_path, ".cs")
            ? validateScriptPath(resource_path)
            : validateResPath(resource_path, ".tscn");
        if (valid_path.isErr()) return errorJson(valid_path.error());
        auto loader = singleton("ResourceLoader");
        if (loader.isErr()) return errorJson(loader.error());
        auto path = makeString(resource_path);
        auto hint = makeString("");
        if (path.isErr() || hint.isErr()) return errorJson(500, "Failed to construct resource existence arguments");
        auto resource_exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                                {&path.value(), &hint.value()});
        if (resource_exists_value.isErr()) return errorJson(resource_exists_value.error());
        auto resource_exists = scalarFromVariant<GDExtensionBool>(resource_exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (resource_exists.isErr()) return errorJson(resource_exists.error());
        if (!resource_exists.value()) return errorJson(404, "Autoload resource not found: " + resource_path);
        autoload_singleton = params.value("singleton", true);
        replacement = makeString((autoload_singleton ? "*" : "") + resource_path);
        if (replacement.isErr()) return errorJson(replacement.error());
    }

    auto applied = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                              {&setting_name.value(), &replacement.value()});
    if (applied.isErr()) return errorJson(applied.error());
    const auto file_before = projectFileStamp();
    auto saved = callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL);
    auto save_code = saved.isOk()
        ? scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT)
        : Result<int64_t>(saved.error());
    if (save_code.isErr() || save_code.value() != 0) {
        auto rollback = restoreProjectSetting(project_settings.value(), setting_name.value(), previous.value(),
                                              file_before);
        if (rollback.isErr()) return errorJson(500, "Autoload save failed and rollback failed: " + rollback.error().message);
        return projectFileNotWritable("autoload", save_code.isErr()
                                                      ? save_code.error().message
                                                      : ::didi::godot::describeGodotError(save_code.value()));
    }
    auto stored = projectFileAutoload(autoload_name);
    if (stored.isErr()) {
        return errorJson(500, "ProjectSettings.save reported success, and project.godot could not "
                              "be read back to confirm it: " + stored.error().message);
    }
    // Say what this did and did not do. Writing the setting is not the same
    // as the attached editor knowing about it: Godot registers an autoload's
    // global name through editor-internal paths a GDExtension cannot reach,
    // so scripts referring to the singleton keep failing to compile in this
    // editor session until it restarts. `editor_reload_project` does not
    // clear it either. Reporting only `persisted: true` left callers
    // debugging their own scripts for a state this call had created.
    return liveResult({{"status", "success"}, {"name", autoload_name}, {"path", resource_path},
                       {"singleton", autoload_singleton}, {"removed", removing}, {"persisted", true},
                       // The entry project.godot declares after the save,
                       // or null once it declares none (#1019).
                       {"autoload", stored.value()},
                       {"registered_in_attached_editor", false},
                       {"requires_editor_restart", true},
                       {"limitation",
                        removing
                            ? std::string(
                                  "The setting is removed from project.godot, but this editor "
                                  "session keeps resolving the singleton until it is restarted. "
                                  "editor_reload_project does not change that.")
                            : std::string(
                                  "The setting is written to project.godot, but this editor "
                                  "session will not resolve the singleton until it is restarted. "
                                  "Scripts referencing it report 'Identifier not found' until "
                                  "then, and editor_reload_project does not change that.")}});
}


json GodotBridge::bridgeProjectInputActions(const std::string& method, const json& params, const std::string& /*session_kind*/, GDExtensionObjectPtr /*editor*/) {
    auto project_settings = singleton("ProjectSettings");
    if (project_settings.isErr()) return errorJson(project_settings.error());

    if (method == "project.listInputActions") {
        // Ninety actions came back on every call, eighty-five of them the
        // engine's ui_* map, and there was no smaller question to ask: not
        // the project's own, not one by name (#775). A listing now keeps to
        // what the project declares unless asked for the engine's too, as
        // P4 says (#1108). An action named by the caller is that request,
        // wherever the action comes from.
        const std::string only_action = params.value("action", "");
        const bool asked = params.contains("include_engine_defaults");
        bool include_engine_defaults = params.value("include_engine_defaults", !only_action.empty());
        const auto declared = projectFileInputActions();
        if (declared.failure && !include_engine_defaults) {
            if (asked) {
                return errorJson(declared.failure->code,
                                 declared.failure->message +
                                     " Which actions are the project's own cannot be told from "
                                     "the engine's until the file reads.",
                                 declared.failure->data.is_null() ? json::object()
                                                                  : declared.failure->data);
            }
            // Nobody asked to leave the engine's out, and they cannot be
            // told apart, so it lists every action and says why below.
            include_engine_defaults = true;
        }
        size_t omitted_engine_defaults = 0;
        bool named_action_omitted = false;
        auto properties = callObject(project_settings.value(), "Object", "get_property_list", 3995934104LL);
        if (properties.isErr()) return errorJson(properties.error());
        auto size_value = callVariant(properties.value(), "size");
        if (size_value.isErr()) return errorJson(size_value.error());
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return errorJson(size.error());
        auto name_key = makeString("name");
        auto deadzone_key = makeString("deadzone");
        auto events_key = makeString("events");
        if (name_key.isErr() || deadzone_key.isErr() || events_key.isErr()) return errorJson(500, "Failed to construct InputMap keys");
        std::vector<json> actions;
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return errorJson(index.error());
            auto descriptor = callVariant(properties.value(), "get", {&index.value()});
            if (descriptor.isErr()) return errorJson(descriptor.error());
            auto property_name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
            if (property_name_value.isErr()) return errorJson(property_name_value.error());
            auto property_type = GodotApi::instance().variant_get_type(property_name_value.value().ptr());
            if (property_type != GDEXTENSION_VARIANT_TYPE_STRING && property_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) continue;
            auto property_name = stringFromVariant(property_name_value.value(), property_type);
            if (property_name.isErr()) return errorJson(property_name.error());
            if (!strings::startsWith(property_name.value(), "input/") || property_name.value().size() <= 6) continue;
            const std::string action_name = property_name.value().substr(6);
            if (!only_action.empty() && action_name != only_action) continue;
            const bool defined_by_project = declared.names.count(action_name) != 0;
            if (!include_engine_defaults && !defined_by_project) {
                ++omitted_engine_defaults;
                named_action_omitted = !only_action.empty();
                continue;
            }
            auto setting_name = makeStringName(property_name.value());
            VariantValue default_value;
            if (setting_name.isErr()) return errorJson(setting_name.error());
            auto setting = callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                      {&setting_name.value(), &default_value});
            if (setting.isErr()) return errorJson(setting.error());
            if (GodotApi::instance().variant_get_type(setting.value().ptr()) != GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
                return errorJson(422, "InputMap setting is not a Dictionary: " + property_name.value(),
                                      {{"code", "malformed_input_map_entry"}});
            }
            auto deadzone_value = callVariant(setting.value(), "get", {&deadzone_key.value()});
            auto events_value = callVariant(setting.value(), "get", {&events_key.value()});
            if (deadzone_value.isErr() || events_value.isErr()) {
                return errorJson(422, "InputMap setting is missing deadzone or events",
                                 {{"code", "malformed_input_map_entry"}});
            }
            auto deadzone = scalarFromVariant<double>(deadzone_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
            if (deadzone.isErr()) return errorJson(deadzone.error());
            auto event_count_value = callVariant(events_value.value(), "size");
            if (event_count_value.isErr()) return errorJson(event_count_value.error());
            auto event_count = scalarFromVariant<int64_t>(event_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (event_count.isErr()) return errorJson(event_count.error());
            json events = json::array();
            for (int64_t event_index = 0; event_index < event_count.value(); ++event_index) {
                auto native_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, event_index);
                if (native_index.isErr()) return errorJson(native_index.error());
                auto event = callVariant(events_value.value(), "get", {&native_index.value()});
                if (event.isErr()) return errorJson(event.error());
                auto normalized = inputEventToJson(event.value());
                if (normalized.isErr()) return errorJson(normalized.error());
                events.push_back(normalized.value());
            }
            json entry = {{"action", action_name}, {"deadzone", deadzone.value()}, {"events", events}};
            if (!declared.failure) entry["defined_by_project"] = defined_by_project;
            actions.push_back(std::move(entry));
        }
        if (!only_action.empty() && actions.empty()) {
            if (named_action_omitted) {
                return errorJson(404,
                                 "Input action '" + only_action + "' is an engine default, and "
                                 "include_engine_defaults is false.",
                                 {{"code", "engine_default_action"},
                                  {"action", only_action},
                                  {"engine_default", true},
                                  {"retry_with", {{"include_engine_defaults", true}}}});
            }
            return errorJson(404, "Input action not found: " + only_action,
                             {{"code", "input_action_not_found"}, {"action", only_action}});
        }
        std::sort(actions.begin(), actions.end(), [](const json& left, const json& right) {
            return left["action"].get<std::string>() < right["action"].get<std::string>();
        });
        json listing = {{"status", "success"}, {"actions", actions}};
        if (!include_engine_defaults) listing["omitted_engine_default_count"] = omitted_engine_defaults;
        // Counted and said rather than guessed: an unreadable file leaves
        // every action's origin unknown, not every action the engine's.
        if (declared.failure) listing["defined_by_project_unavailable"] = declared.failure->message;
        return liveResult(listing);
    }

    const std::string action = params.value("action", "");
    auto valid_action = didi::offline::validateInputActionName(action);
    if (valid_action.isErr()) return errorJson(valid_action.error());
    const std::string setting_path = "input/" + action;
    auto setting_name = makeStringName(setting_path);
    if (setting_name.isErr()) return errorJson(setting_name.error());
    auto exists_value = callObject(project_settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                                   {&setting_name.value()});
    if (exists_value.isErr()) return errorJson(exists_value.error());
    auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr()) return errorJson(exists.error());
    const bool removing = method == "project.removeInputAction";
    if (removing && !exists.value()) return errorJson(404, "Input action not found: " + action);
    if (removing) {
        // has_setting says yes for ui_accept because the engine registers
        // the built-in map as settings. Removing one wrote nothing to
        // project.godot, left the running editor's InputMap without the
        // action, and reported persisted: true, which is the opposite of
        // what persisted promises: the removal is gone on the next load
        // (#485).
        auto defined = projectFileDefinesInputAction(action);
        if (defined.isErr()) return errorJson(defined.error());
        if (!defined.value()) {
            return errorJson(
                409,
                "Input action '" + action +
                    "' is an engine default rather than something this project defines. "
                    "It is not in project.godot, so there is nothing to remove there: the "
                    "removal would exist only in this editor session and be gone on the "
                    "next load, while UI navigation broke in anything run from it. Use "
                    "project_set_input_action to give the project its own events for this "
                    "name.",
                {{"code", "engine_default_action"},
                 {"action", action},
                 {"engine_default", true},
                 {"defined_by_project", false},
                 {"retryable", false}});
        }
    }
    if (!removing && exists.value() && !params.value("replace", false)) {
        return errorJson(409, "Input action already exists; pass replace: true to update it",
                              {{"code", "already_exists"}, {"retry_with", {{"replace", true}}}});
    }

    VariantValue default_value;
    auto previous = exists.value()
        ? callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                     {&setting_name.value(), &default_value})
        : Result<VariantValue>(VariantValue{});
    if (previous.isErr()) return errorJson(previous.error());
    Result<VariantValue> replacement(VariantValue{});
    double deadzone = 0.2;
    size_t event_count = 0;
    if (!removing) {
        deadzone = params.value("deadzone", 0.2);
        if (!std::isfinite(deadzone) || deadzone < 0.0 || deadzone > 1.0) {
            return errorJson(400, "deadzone must be finite and within 0.0..1.0");
        }
        json event_descriptors = params.value("events", json::array());
        if (!event_descriptors.is_array()) return errorJson(400, "events must be an array");
        auto dictionary = makeJsonVariant(json::object());
        auto events = makeJsonVariant(json::array());
        auto deadzone_key = makeString("deadzone");
        auto events_key = makeString("events");
        auto deadzone_value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, deadzone);
        if (dictionary.isErr() || events.isErr() || deadzone_key.isErr() || events_key.isErr() || deadzone_value.isErr()) {
            return errorJson(500, "Failed to construct InputMap setting containers");
        }
        size_t event_index = 0;
        for (const auto& descriptor : event_descriptors) {
            auto event = makeInputEvent(descriptor);
            if (event.isErr()) {
                // Which entry, because events takes many and a message about
                // one of them that does not say which is a message a caller
                // has to bisect (#737).
                return errorJson(event.error().code,
                                 "Argument 'events' entry " + std::to_string(event_index) +
                                     ": " + event.error().message);
            }
            ++event_index;
            auto appended = callVariant(events.value(), "append", {&event.value()});
            if (appended.isErr()) return errorJson(appended.error());
        }
        auto set_deadzone = callVariant(dictionary.value(), "set", {&deadzone_key.value(), &deadzone_value.value()});
        auto set_events = callVariant(dictionary.value(), "set", {&events_key.value(), &events.value()});
        if (set_deadzone.isErr() || set_events.isErr()) return errorJson(500, "Failed to construct InputMap setting");
        event_count = event_descriptors.size();
        replacement = std::move(dictionary.value());
    }

    // The editor's own InputMap is left as it is. An editor never loads a
    // project's input actions into its map -- the project's actions are
    // absent from it at startup on 4.5.1, 4.6.2 and 4.7.2 -- and Godot's
    // Project Settings dialog writes ProjectSettings and nothing else. This
    // used to finish with InputMap.load_from_project_settings(), which on
    // 4.5.1 erased the 3D viewport's own navigation actions, so every mouse
    // move over it printed "The InputMap action ... doesn't exist" and
    // Shift-pan stopped working until a restart; on 4.6.2 and 4.7.2 it kept
    // those and pulled the project's twenty actions, its ui_* overrides
    // included, into the editor's map (#925). Measured with
    // tools/vibe/probes/input_map_reload.py. A game loads the project's
    // actions when it starts, which is where they take effect.
    auto applied = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                              {&setting_name.value(), &replacement.value()});
    if (applied.isErr()) return errorJson(applied.error());
    const auto file_before = projectFileStamp();
    auto saved = callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL);
    auto save_code = saved.isOk()
        ? scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT)
        : Result<int64_t>(saved.error());
    if (save_code.isErr() || save_code.value() != 0) {
        auto rollback = restoreProjectSetting(project_settings.value(), setting_name.value(), previous.value(),
                                              file_before);
        if (rollback.isErr()) return errorJson(500, "InputMap save failed and rollback failed: " + rollback.error().message);
        return projectFileNotWritable("input action", save_code.isErr()
                                                          ? save_code.error().message
                                                          : ::didi::godot::describeGodotError(save_code.value()));
    }
    auto stored = projectFileDefinesInputAction(action);
    if (stored.isErr()) {
        return errorJson(500, "ProjectSettings.save reported success, and project.godot could not "
                              "be read back to confirm it: " + stored.error().message);
    }
    if (removing) {
        // The remove path never filled these in, so it echoed the defaults
        // rather than what the action had. Read them off the value that was
        // there a moment ago.
        auto deadzone_key = makeString("deadzone");
        auto events_key = makeString("events");
        if (deadzone_key.isOk() && events_key.isOk() &&
            GodotApi::instance().variant_get_type(previous.value().ptr()) ==
                GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
            auto had_deadzone = callVariant(previous.value(), "get", {&deadzone_key.value()});
            if (had_deadzone.isOk()) {
                auto value = scalarFromVariant<double>(had_deadzone.value(),
                                                       GDEXTENSION_VARIANT_TYPE_FLOAT);
                if (value.isOk()) deadzone = value.value();
            }
            auto had_events = callVariant(previous.value(), "get", {&events_key.value()});
            if (had_events.isOk()) {
                auto count = callVariant(had_events.value(), "size");
                if (count.isOk()) {
                    auto size = scalarFromVariant<int64_t>(count.value(),
                                                           GDEXTENSION_VARIANT_TYPE_INT);
                    if (size.isOk() && size.value() >= 0) {
                        event_count = static_cast<size_t>(size.value());
                    }
                }
            }
        }
    }
    json result = {{"status", "success"}, {"action", action}, {"deadzone", deadzone},
                   {"event_count", event_count}, {"removed", removing}, {"persisted", true},
                   // Whether project.godot declares the action after the
                   // save, read back from the file (#1019).
                   {"defined_by_project", stored.value()},
                   // Kept for callers that read it, and false because the
                   // editor's InputMap is no longer reloaded (#925).
                   {"runtime_reloaded", false},
                   {"takes_effect",
                    "when a game starts, which loads the project's input actions. The "
                    "editor does not load them into its own InputMap, so this editor "
                    "session is unchanged, the same as after an edit in Godot's Project "
                    "Settings dialog."}};
    return liveResult(result);
}


} // namespace didi::godot
