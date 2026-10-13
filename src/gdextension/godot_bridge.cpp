#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/gdextension/expression_sandbox.hpp"
#include "didi/gdextension/property_paths.hpp"
#include "didi/runtime/godot_arguments.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/undo_ledger.hpp"
#include "didi/gdextension/godot_object.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/config_file_syntax.hpp"
#include "didi/common/connection_flags.hpp"
#include "didi/common/godot_error.hpp"
#include "didi/common/json_bounds.hpp"
#include "didi/common/cell_records.hpp"
#include "didi/common/project_path.hpp"
#include "didi/common/scene_node_path.hpp"
#include "didi/offline/project_settings_file.hpp"
#include "didi/runtime/input_injection.hpp"
#include "didi/runtime/ghost_preview.hpp"
#include "didi/runtime/spatial_queries.hpp"
#include "didi/runtime/segmentation.hpp"
#include "didi/runtime/animation_requests.hpp"
#include "didi/runtime/audio_requests.hpp"
#include "didi/gdextension/godot_values.hpp"
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <unordered_map>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace didi {
namespace godot {
using namespace bridge_internal;
namespace bridge_internal {


// A semantic failure, said in a sentence, with the identifier kept as a stable
// code under `data.code`.
//
// These used to answer with the identifier as the whole message. That is the
// string a client shows a person, and `target_method_not_found` does not say
// which method was looked for or on which node, so the one thing the caller
// needed was the one thing missing. #406 and #424 fixed the argument
// rejections; this is the same treatment for the failures underneath them.
//
// The sentence comes from bridgeErrorSentence, so every identifier has one in a
// single place rather than at 120 call sites, and a site with something useful
// to add passes it through `detail` and `data`.


// One sentence per identifier, in one place. A call site that has something
// useful to add -- which node, which method, which type it found instead --
// passes it as `detail`, and it is appended to the sentence.
const std::map<std::string, std::string>& bridgeErrorSentences() {
    static const std::map<std::string, std::string> sentences = {
    {"node_not_owned",
     "The edited scene does not own this node, so the file cannot hold the change and a save "
     "would report saved having written none of it."},
    {"make_unique_refused",
     "The node kept its own resource when it was given a copy, so the write has nowhere it can be read back from."},
    {"node_inherited",
     "This node is inherited from the base scene, and a scene file has no way to record "
     "removing, moving or duplicating an inherited node."},
    {"cyclic_instance",
     "The scene to instance contains the edited scene, so the result could never be saved: "
     "Godot refuses that save and reports nothing to the caller."},
    {"input_queue_full",
     "Too many input events are held for the paused game. Resume it, or advance it a frame, "
     "so they are delivered first."},
    {"animation_library_already_added",
     "That library is already on this player under another name, and Godot holds one library "
     "under one name only."},
    {"animation_library_name_taken",
     "The player already has a library by that name."},
    {"animation_library_differs_from_disk",
     "The editor's copy of this library is not what the file now holds."},
    {"animation_library_slot",
     "An AnimationPlayer's libraries are not set as a property."},
    {"property_path_invalid",
     "The property path is not one Godot can resolve."},
    {"property_not_found",
     "No property by that name is on the object the path reached."},
    {"property_slot_empty",
     "The path steps into a property that holds nothing."},
    {"property_path_not_resource",
     "The path steps into something that is not a resource."},
    {"property_type_unsupported",
     "The property holds a type this tool cannot spell in JSON."},
    {"property_write_excluded",
     "A property write does not make this change."},
    {"subresource_not_saved",
     "The write would change a resource the edited scene's save does not keep, so it would "
     "read back as applied and then be lost."},
    {"batch_writes_overlap",
     "Two writes in the batch overlap, so one undo step could not restore both."},
    {"animation_library_postcondition_mismatch",
     "The player did not hold the library after the change was committed."},
    {"animation_library_reload_failed",
     "The editor's copy of the library could not be reloaded from the file, so nothing was added."},
    {"animation_library_undo_registration_failed",
     "The change could not be registered with the editor's undo history, so it was not made."},
    {"animation_library_unloadable",
     "A file is there, and the engine could not load a resource from it, which is what a file "
     "that does not parse looks like."},
    {"library_path_case_mismatch",
     "The path spells the file with different letter case from the file on disk."},
    {"camera_path_does_not_resolve_to_camera3d",
     "That path resolves to a node, but not to a Camera3D."},
    {"camera_postcondition_mismatch",
     "The camera's transform did not read back as written after the change was committed."},
    {"camera_postcondition_read_failed",
     "The camera's transform could not be read back after the change was committed."},
    {"camera_state_read_failed",
     "The camera's current transform could not be read, so there would have been nothing to undo back to."},
    {"camera_undo_registration_failed",
     "The change could not be registered with the editor's undo history, so it was not made."},
    {"debug_draw_postcondition_mismatch",
     "The viewport's debug draw mode did not read back as set."},
    {"debug_hint_read_failed",
     "The viewport's debug draw mode could not be read."},
    {"declared_signal_not_found",
     "The emitter node declares no signal by that name."},
    {"duplicate_gridmap_position",
     "The same cell position appears more than once in this batch, and only one of them could win."},
    {"duplicate_tilemap_coordinate",
     "The same cell coordinate appears more than once in this batch, and only one of them could win."},
    {"extension_protocol_error",
     "The GDExtension interface returned something this build cannot read, so the result would not have been trustworthy."},
    {"gridmap_item_not_found",
     "The MeshLibrary on that GridMap has no item with that id."},
    {"gridmap_postcondition_mismatch",
     "The cells did not read back as written after the change was committed."},
    {"gridmap_snapshot_failed",
     "The cells could not be read before the change, so there would have been nothing to undo back to."},
    {"gridmap_target_not_found",
     "No node was found at that gridmap path."},
    {"gridmap_target_wrong_type",
     "That path resolves to a node, but not to a GridMap."},
    {"gridmap_undo_registration_failed",
     "The change could not be registered with the editor's undo history, so it was not made."},
    {"invalid_phase7_signal_test_seam",
     "That is not a seam this build admits."},
    {"invalid_gridmap_set_cells_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_signal_connect_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_signal_disconnect_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_signal_emit_argument_encoding",
     "An argument is not encoded the way this method expects."},
    {"invalid_signal_emit_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_signal_list_connections_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_shader_get_visual_graph_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_shader_list_uniforms_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_shader_set_uniform_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_tilemap_get_used_rect_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_tilemap_set_cells_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_ui_list_controls_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_viewport_set_camera_transform_request",
     "The arguments do not match the published shape for this tool."},
    {"invalid_viewport_toggle_debug_draw_request",
     "The arguments do not match the published shape for this tool."},
    {"missing_or_ambiguous_signal_connection",
     "No single existing connection matches that emitter, signal, target and method."},
    {"not_an_animation_library",
     "The resource at that path is not an AnimationLibrary."},
    {"required_bind_unavailable",
     "A Godot method this call depends on is not available in the attached engine build, so the call was refused rather than half done."},
    {"response_limit",
     "The response grew past the size this method will send; ask for a narrower slice."},
    {"session_kind_rejected",
     "This method is not available on the attached session; it needs a different session kind."},
    {"signal_argument_metadata_work_limit",
     "Reading the signal argument metadata would cost more work than this method will spend; ask for a narrower slice."},
    {"signal_connection_already_exists",
     "That emitter, signal, target and method are already connected."},
    {"signal_connection_metadata_work_limit",
     "Reading the connection metadata would cost more work than this method will spend; ask for a narrower slice."},
    {"signal_connection_state_inconsistent",
     "The engine reported the connection in two states at once, so nothing was changed."},
    {"signal_emit_argument_count_exceeded",
     "More arguments were given than this method will emit."},
    {"signal_emit_argument_type_mismatch",
     "An argument is not the type that signal declares in that position."},
    {"signal_emit_arguments_too_large",
     "The arguments are larger than this method will emit."},
    {"signal_emit_arity_mismatch",
     "The number of arguments given does not match what that signal declares."},
    {"signal_emit_failed",
     "The engine refused the emit."},
    {"signal_metadata_work_limit",
     "Reading the signal metadata would cost more work than this method will spend; ask for a narrower slice."},
    {"signal_postcondition_mismatch",
     "The connection did not read back as made or removed after the change was committed."},
    {"signal_target_arity_incompatible",
     "The target method cannot accept the arguments that signal carries."},
    {"signal_undo_redo_registration_failed",
     "The change could not be registered with the editor's undo history, so it was not made."},
    {"target_method_not_found",
     "The target node has no method by that name."},
    {"target_script_not_compiled",
     "The script on the target node declares this method and the engine does not have it, which "
     "is what a script that failed to compile looks like. This is not a problem with the method "
     "name."},
    {"tilemap_layer_has_no_tileset",
     "That TileMapLayer has no TileSet, so no cell can be placed on it."},
    {"tilemap_postcondition_mismatch",
     "The cells did not read back as written after the change was committed."},
    {"tilemap_snapshot_failed",
     "The cells could not be read before the change, so there would have been nothing to undo back to."},
    {"tilemap_target_not_found",
     "No node was found at that tilemap path."},
    {"tilemap_target_wrong_type",
     "That path resolves to a node, but not to a TileMapLayer."},
    {"tilemap_tile_not_found",
     "The TileSet on that layer has no tile for the source id and atlas coordinates given."},
    {"tilemap_undo_registration_failed",
     "The change could not be registered with the editor's undo history, so it was not made."},
    {"unsupported_existing_connection_flags",
     "The existing connection is not one the scene file stores."},
    {"unsupported_signal_emit_argument",
     "An argument is of a type this method has no way to send."},
    };
    return sentences;
}

json bridgeError(int code, const std::string& identifier, json data, const std::string& detail) {
    const auto& sentences = bridgeErrorSentences();
    const auto found = sentences.find(identifier);
    // An identifier with no sentence is still better said than printed bare,
    // and a test pins that every identifier in this file has one.
    std::string message = found == sentences.end()
                              ? "The engine refused this call (" + identifier + ")."
                              : found->second;
    if (!detail.empty()) message += " " + detail;
    if (!data.is_object()) data = json::object();
    data["code"] = identifier;
    if (!data.contains("retryable")) data["retryable"] = false;
    return errorJson(code, message, std::move(data));
}

// The same refusal as a Result's error, for a helper that returns one.
Error bridgeRefusal(int code, const std::string& identifier) {
    auto answer = bridgeError(code, identifier);
    auto& error = answer["error"];
    return Error(code, error["message"].get<std::string>(), std::move(error["data"]));
}

#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
std::string g_phase7SignalTestSeam;

bool takePhase7SignalTestSeam(std::string_view expected) {
    if (g_phase7SignalTestSeam != expected) return false;
    g_phase7SignalTestSeam.clear();
    return true;
}
#endif


Result<VariantValue> makeColor(double r, double g, double b, double a) {
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_COLOR, 4);
    if (!constructor) return Error::internal("Godot Color constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_COLOR);
    const void* arguments[] = {&r, &g, &b, &a};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_COLOR, native.ptr());
}

Result<VariantValue> makeVector2(double x, double y) {
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_VECTOR2, 3);
    if (!constructor) return Error::internal("Godot Vector2 constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_VECTOR2);
    const void* arguments[] = {&x, &y};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR2, native.ptr());
}

Result<VariantValue> makeJsonVariant(const json& value, int depth) {
    if (depth > 16) return Error::invalidArgument("JSON value is nested more than 16 levels deep");
    if (value.is_null()) return VariantValue{};
    if (value.is_boolean()) return makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(value.get<bool>()));
    if (value.is_number_integer()) return makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(value.get<int64_t>()));
    if (value.is_number_unsigned()) {
        auto number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return Error::invalidArgument("Unsigned integer is outside Godot Variant int range");
        }
        return makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(number));
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number)) return Error::invalidArgument("JSON real must be finite");
        return makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, number);
    }
    if (value.is_string()) return makeString(value.get<std::string>());
    if (value.is_array() || value.is_object()) {
        const auto type = value.is_array() ? GDEXTENSION_VARIANT_TYPE_ARRAY : GDEXTENSION_VARIANT_TYPE_DICTIONARY;
        auto ctor = GodotApi::instance().variant_get_ptr_constructor(type, 0);
        if (!ctor) return Error::internal("Godot container constructor is unavailable");
        NativeValue native(type);
        ctor(native.ptr(), nullptr);
        native.markInitialized();
        auto container = variantFromNative(type, native.ptr());
        if (container.isErr()) return container.error();
        if (value.is_array()) {
            for (const auto& element : value) {
                auto item = makeJsonVariant(element, depth + 1);
                if (item.isErr()) return item.error();
                auto appended = callVariant(container.value(), "append", {&item.value()});
                if (appended.isErr()) return appended.error();
            }
        } else {
            for (auto it = value.begin(); it != value.end(); ++it) {
                auto key = makeString(it.key());
                auto item = makeJsonVariant(it.value(), depth + 1);
                if (key.isErr()) return key.error();
                if (item.isErr()) return item.error();
                auto assigned = callVariant(container.value(), "set", {&key.value(), &item.value()});
                if (assigned.isErr()) return assigned.error();
            }
        }
        return std::move(container.value());
    }
    return Error::invalidArgument("JSON value cannot be converted to a supported Godot Variant");
}

// A JSON real that names a whole number, inside the range an int64 holds
// exactly. JSON has one number type, so a client with a real in hand sends
// 3.0 where it means 3 and there is nothing else it can write. 2^63 is a
// double but not an int64, so the upper bound is exclusive.
bool isWholeNumberJsonReal(const json& value) {
    if (!value.is_number_float()) return false;
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::trunc(number) != number) return false;
    return number >= -9223372036854775808.0 && number < 9223372036854775808.0;
}

// A JSON object standing for a fixed set of numeric members, and nothing else.
// Extra keys are refused rather than dropped: a caller who writes "z" on a
// Vector2 has made a mistake worth seeing, and silently ignoring it writes a
// position they did not ask for.
bool isNumericMemberObject(const json& value, std::initializer_list<const char*> required,
                           std::initializer_list<const char*> optional, bool whole_numbers) {
    if (!value.is_object()) return false;
    size_t matched = 0;
    for (const auto* key : required) {
        if (!value.contains(key)) return false;
        const auto& member = value[key];
        if (!member.is_number()) return false;
        if (whole_numbers && !member.is_number_integer() && !member.is_number_unsigned() &&
            !isWholeNumberJsonReal(member)) {
            return false;
        }
        ++matched;
    }
    for (const auto* key : optional) {
        if (!value.contains(key)) continue;
        const auto& member = value[key];
        if (!member.is_number()) return false;
        if (whole_numbers && !member.is_number_integer() && !member.is_number_unsigned() &&
            !isWholeNumberJsonReal(member)) {
            return false;
        }
        ++matched;
    }
    return matched == value.size();
}

// #rrggbb or #rrggbbaa. Godot's own Color(String) accepts more spellings, but
// the ones it guesses at are the ones a caller most easily gets wrong, so the
// accepted set is the unambiguous pair.
bool isHexColorString(const json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text.size() != 7 && text.size() != 9) return false;
    if (text.front() != '#') return false;
    for (size_t index = 1; index < text.size(); ++index) {
        if (!std::isxdigit(static_cast<unsigned char>(text[index]))) return false;
    }
    return true;
}

// A resource slot is filled by naming the resource, which is how a .tscn names
// one and how script_attach_to_node already takes a Script.
bool isResourcePathString(const json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text.rfind("res://", 0) != 0 || text.size() <= 6) return false;
    if (text.find('\0') != std::string::npos) return false;
    if (text.find("..") != std::string::npos) return false;
    return true;
}

// The largest magnitude a Godot float property can hold.
//
// Godot stores a float property as real_t, which is 32-bit in a standard build,
// and Vector2, Vector3 and Color are made of the same. A JSON number above this
// becomes inf the moment it lands there: the scene file ends up holding
// Vector2(inf, 5), inf propagates through the transform to every child on the
// next frame, and the read back is a value JSON cannot spell.
constexpr double kGodotRealMaximum = 3.4028234663852886e38;

// Which member names carry reals, per property type. Empty for a type whose
// members are whole numbers or which is not made of reals at all.
std::vector<const char*> realMembersOfPropertyType(GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_VECTOR2: return {"x", "y"};
        case GDEXTENSION_VARIANT_TYPE_VECTOR3: return {"x", "y", "z"};
        case GDEXTENSION_VARIANT_TYPE_COLOR: return {"r", "g", "b", "a"};
        default: return {};
    }
}

// One member of the JSON object a built-in is spelled as, and its type.
struct BuiltinMember {
    const char* name;
    GDExtensionVariantType type;
};

// The members of each built-in made of other values (Q7), named as Godot names
// them and in the order its constructor takes them, so a Transform3D is
// {"basis": {"x", "y", "z"}, "origin"} and a Rect2 {"position", "size"}. A
// Basis and a Projection are their columns, which is what Godot's x, y and z
// are. The vectors and Color keep their own rules above; these are made of
// them.
const std::vector<BuiltinMember>* compoundMembers(GDExtensionVariantType type) {
    constexpr auto kFloat = GDEXTENSION_VARIANT_TYPE_FLOAT;
    constexpr auto kInt = GDEXTENSION_VARIANT_TYPE_INT;
    static const std::vector<BuiltinMember> vector4 = {{"x", kFloat}, {"y", kFloat}, {"z", kFloat}, {"w", kFloat}};
    static const std::vector<BuiltinMember> vector4i = {{"x", kInt}, {"y", kInt}, {"z", kInt}, {"w", kInt}};
    static const std::vector<BuiltinMember> rect2 = {{"position", GDEXTENSION_VARIANT_TYPE_VECTOR2},
                                                     {"size", GDEXTENSION_VARIANT_TYPE_VECTOR2}};
    static const std::vector<BuiltinMember> rect2i = {{"position", GDEXTENSION_VARIANT_TYPE_VECTOR2I},
                                                      {"size", GDEXTENSION_VARIANT_TYPE_VECTOR2I}};
    static const std::vector<BuiltinMember> aabb = {{"position", GDEXTENSION_VARIANT_TYPE_VECTOR3},
                                                    {"size", GDEXTENSION_VARIANT_TYPE_VECTOR3}};
    static const std::vector<BuiltinMember> plane = {{"normal", GDEXTENSION_VARIANT_TYPE_VECTOR3}, {"d", kFloat}};
    static const std::vector<BuiltinMember> transform2d = {{"x", GDEXTENSION_VARIANT_TYPE_VECTOR2},
                                                           {"y", GDEXTENSION_VARIANT_TYPE_VECTOR2},
                                                           {"origin", GDEXTENSION_VARIANT_TYPE_VECTOR2}};
    static const std::vector<BuiltinMember> basis = {{"x", GDEXTENSION_VARIANT_TYPE_VECTOR3},
                                                     {"y", GDEXTENSION_VARIANT_TYPE_VECTOR3},
                                                     {"z", GDEXTENSION_VARIANT_TYPE_VECTOR3}};
    static const std::vector<BuiltinMember> transform3d = {{"basis", GDEXTENSION_VARIANT_TYPE_BASIS},
                                                           {"origin", GDEXTENSION_VARIANT_TYPE_VECTOR3}};
    static const std::vector<BuiltinMember> projection = {{"x", GDEXTENSION_VARIANT_TYPE_VECTOR4},
                                                          {"y", GDEXTENSION_VARIANT_TYPE_VECTOR4},
                                                          {"z", GDEXTENSION_VARIANT_TYPE_VECTOR4},
                                                          {"w", GDEXTENSION_VARIANT_TYPE_VECTOR4}};
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_VECTOR4: return &vector4;
        case GDEXTENSION_VARIANT_TYPE_VECTOR4I: return &vector4i;
        case GDEXTENSION_VARIANT_TYPE_QUATERNION: return &vector4;
        case GDEXTENSION_VARIANT_TYPE_RECT2: return &rect2;
        case GDEXTENSION_VARIANT_TYPE_RECT2I: return &rect2i;
        case GDEXTENSION_VARIANT_TYPE_AABB: return &aabb;
        case GDEXTENSION_VARIANT_TYPE_PLANE: return &plane;
        case GDEXTENSION_VARIANT_TYPE_TRANSFORM2D: return &transform2d;
        case GDEXTENSION_VARIANT_TYPE_BASIS: return &basis;
        case GDEXTENSION_VARIANT_TYPE_TRANSFORM3D: return &transform3d;
        case GDEXTENSION_VARIANT_TYPE_PROJECTION: return &projection;
        default: return nullptr;
    }
}

// What each element of a packed array is, or NIL for any other type.
GDExtensionVariantType packedElementType(GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY:
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY:
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY: return GDEXTENSION_VARIANT_TYPE_INT;
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY:
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY: return GDEXTENSION_VARIANT_TYPE_FLOAT;
        case GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY: return GDEXTENSION_VARIANT_TYPE_STRING;
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR2_ARRAY: return GDEXTENSION_VARIANT_TYPE_VECTOR2;
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY: return GDEXTENSION_VARIANT_TYPE_VECTOR3;
        case GDEXTENSION_VARIANT_TYPE_PACKED_COLOR_ARRAY: return GDEXTENSION_VARIANT_TYPE_COLOR;
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY: return GDEXTENSION_VARIANT_TYPE_VECTOR4;
        default: return GDEXTENSION_VARIANT_TYPE_NIL;
    }
}

bool propertyTypeAcceptsJson(const json& value, GDExtensionVariantType type);

// A byte array and a 32-bit one wrap a number they cannot hold rather than
// refuse it, so 256 would land as 0 with nothing said.
bool packedElementAcceptsJson(const json& item, GDExtensionVariantType packed,
                              GDExtensionVariantType element) {
    if (!propertyTypeAcceptsJson(item, element)) return false;
    if (packed == GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY) {
        const double number = item.get<double>();
        return number >= 0 && number <= 255;
    }
    if (packed == GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY) {
        const double number = item.get<double>();
        return number >= -2147483648.0 && number <= 2147483647.0;
    }
    return true;
}

bool propertyTypeAcceptsJson(const json& value, GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_NIL:
            return value.is_null();
        case GDEXTENSION_VARIANT_TYPE_BOOL:
            return value.is_boolean();
        case GDEXTENSION_VARIANT_TYPE_INT:
            // The whole-number real belongs here for the same reason an int
            // belongs on a float property below: the client wrote the only
            // number JSON gave it, and Godot converts either way losslessly.
            return value.is_number_integer() || value.is_number_unsigned() ||
                   isWholeNumberJsonReal(value);
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
            return value.is_number();
        case GDEXTENSION_VARIANT_TYPE_STRING:
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
            return value.is_string();
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
            return isNumericMemberObject(value, {"x", "y"}, {}, false);
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
            return isNumericMemberObject(value, {"x", "y"}, {}, true);
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
            return isNumericMemberObject(value, {"x", "y", "z"}, {}, false);
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
            return isNumericMemberObject(value, {"x", "y", "z"}, {}, true);
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            return isHexColorString(value) ||
                   isNumericMemberObject(value, {"r", "g", "b"}, {"a"}, false);
        case GDEXTENSION_VARIANT_TYPE_OBJECT:
            // null clears the slot, which is the only way to empty one.
            return value.is_null() || isResourcePathString(value);
        case GDEXTENSION_VARIANT_TYPE_ARRAY:
            // What its elements may be depends on the array the property holds
            // now, typed or not, so they are checked when the value is built.
            return value.is_array();
        case GDEXTENSION_VARIANT_TYPE_DICTIONARY:
            // The same for a dictionary's keys and values (#1195).
            return value.is_object();
        default:
            break;
    }
    if (const auto* members = compoundMembers(type)) {
        if (!value.is_object() || value.size() != members->size()) return false;
        for (const auto& member : *members) {
            const auto found = value.find(member.name);
            if (found == value.end() || !propertyTypeAcceptsJson(*found, member.type)) return false;
        }
        return true;
    }
    const auto element = packedElementType(type);
    if (element == GDEXTENSION_VARIANT_TYPE_NIL || !value.is_array()) return false;
    for (const auto& item : value) {
        if (!packedElementAcceptsJson(item, type, element)) return false;
    }
    return true;
}

// Whether the property contract has a JSON spelling for this Godot type at all.
// Reported next to a shader uniform so a caller can tell a value it may write
// from one it may only read, and taken from the same decision scene_set_property
// makes rather than from a second list that would drift from it.
bool jsonTypeIsInsidePropertyContract(int godot_type) {
    return matchJsonToPropertyType(json(nullptr), godot_type) !=
               PropertyTypeMatch::UnsupportedPropertyType ||
           matchJsonToPropertyType(json(0), godot_type) !=
               PropertyTypeMatch::UnsupportedPropertyType;
}


// Refuses a number the property cannot hold.
//
// A float property is 32-bit, so anything above about 3.4e38 becomes inf on the
// way in. The write went through, the scene file ended up holding
// Vector2(inf, 5), and the value reported back was JSON null, which is what a
// caller reads as "unset". `applied: false` was the only signal, and it sits
// beside `status: "success"` where it also appears for a value the engine
// merely coerced. Refusing names the bound instead.
Result<void> validateRealRangeForPropertyType(const std::string& property_name, const json& value,
                                              GDExtensionVariantType type) {
    const auto refusal = describeRealRangeRefusal(property_name, value, static_cast<int>(type));
    if (refusal.has_value()) return Error::invalidArgument(*refusal);
    return Result<void>::ok();
}

// The property name is not decoration. A scene_instantiate_node call carries
// several properties, and a rejection that does not say which one leaves the
// caller guessing at the very moment they have nothing else to look at.
Result<void> validateJsonForPropertyType(const std::string& property_name, const json& value,
                                         GDExtensionVariantType type) {
    switch (matchJsonToPropertyType(value, static_cast<int>(type))) {
        case PropertyTypeMatch::Compatible:
            return validateRealRangeForPropertyType(property_name, value, type);
        case PropertyTypeMatch::UnsupportedPropertyType:
            return Error::invalidArgument("Property \"" + property_name + "\" is a " +
                                          godotVariantTypeName(static_cast<int>(type)) +
                                          ", which is outside the Phase 1 scalar property contract");
        case PropertyTypeMatch::Incompatible:
            break;
    }
    return Error::invalidArgument(
        describePropertyTypeMismatch(property_name, value, static_cast<int>(type)));
}

double jsonMember(const json& value, const char* key, double fallback) {
    return value.contains(key) ? value[key].get<double>() : fallback;
}

int64_t jsonWholeMember(const json& value, const char* key) {
    const auto& member = value[key];
    return member.is_number_float() ? static_cast<int64_t>(member.get<double>())
                                    : member.get<int64_t>();
}

Result<VariantValue> makeColorFromJson(const json& value) {
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        const auto channel = [&](size_t offset) {
            return static_cast<double>(std::stoi(text.substr(offset, 2), nullptr, 16)) / 255.0;
        };
        const double alpha = text.size() == 9 ? channel(7) : 1.0;
        return makeColor(channel(1), channel(3), channel(5), alpha);
    }
    return makeColor(jsonMember(value, "r", 0.0), jsonMember(value, "g", 0.0),
                     jsonMember(value, "b", 0.0), jsonMember(value, "a", 1.0));
}


Result<bool> objectIsClass(GDExtensionObjectPtr object, const char* class_name);
Result<std::string> stringFromVariant(VariantValue& value, GDExtensionVariantType type);
Result<VariantValue> makeVector3(double x, double y, double z);

// The class Godot gives a node, for saying what was found instead of what was
// wanted. Empty when the engine will not say, in which case the sentence simply
// omits the type rather than inventing one.
std::string nodeClassName(GDExtensionObjectPtr object) {
    auto value = callObject(object, "Object", "get_class", 201670096LL);
    if (value.isErr()) return {};
    auto text = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    return text.isErr() ? std::string() : text.value();
}

// "That path resolves to a node, but not to a TileMapLayer; it is a Node2D."
//
// A path that resolves to nothing and a path that resolves to the wrong kind of
// node are different problems with different fixes, and both used to answer
// with the same identifier. A caller that cannot tell them apart retries the
// path when it should be looking at the node, or the other way round.
json wrongNodeTypeError(const std::string& identifier, const std::string& path,
                        GDExtensionObjectPtr object) {
    const auto found = nodeClassName(object);
    json data = {{"path", path}};
    std::string detail = "Path " + path;
    if (found.empty()) {
        detail += " was searched.";
    } else {
        data["found_type"] = found;
        detail += " is a " + found + ".";
    }
    return bridgeError(404, identifier, std::move(data), detail);
}

Result<VariantValue> makeVector2i(int64_t x, int64_t y);
Result<VariantValue> makeVector3i(int64_t x, int64_t y, int64_t z);

// Splits a declared resource type into the classes it takes and the ones it
// rules out. Godot spells both in one hint_string, and get_property_list
// repeats it under class_name, so "BaseMaterial3D,ShaderMaterial" is two
// entries and "Texture2D,-AtlasTexture" is one of each.
ResourceTypeHint parseResourceTypeHintImpl(const std::string& declared_type) {
    ResourceTypeHint hint;
    size_t start = 0;
    while (start <= declared_type.size()) {
        const auto comma = declared_type.find(',', start);
        const auto end = comma == std::string::npos ? declared_type.size() : comma;
        std::string entry = declared_type.substr(start, end - start);
        const auto first = entry.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const auto last = entry.find_last_not_of(" \t");
            entry = entry.substr(first, last - first + 1);
            if (entry.front() == '-') {
                if (entry.size() > 1) hint.excluded.push_back(entry.substr(1));
            } else {
                hint.accepted.push_back(entry);
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return hint;
}

// The editor's rule, applied to one candidate. An exclusion is checked first
// because Godot erases the excluded name from the set that the accepted entries
// filled, so it beats the entry that put it there. A hint with nothing to accept
// constrains nothing: refusing on a rule that names no type would be worse than
// the missing check.
ResourceTypeVerdict resourceTypeVerdictImpl(
    const ResourceTypeHint& hint, const std::string& resource_class,
    const std::function<bool(const std::string&)>& inherits) {
    for (const auto& excluded : hint.excluded) {
        if (!resource_class.empty() && resource_class == excluded) {
            return ResourceTypeVerdict::Excluded;
        }
    }
    if (hint.accepted.empty()) return ResourceTypeVerdict::Accepted;
    for (const auto& accepted : hint.accepted) {
        if (inherits(accepted)) return ResourceTypeVerdict::Accepted;
    }
    return ResourceTypeVerdict::NotAccepted;
}

// "a BaseMaterial3D or a ShaderMaterial". One entry reads exactly as it did
// before this list was understood, because that case was always right.
std::string describeAcceptedTypes(const std::vector<std::string>& accepted) {
    std::string text;
    for (size_t index = 0; index < accepted.size(); ++index) {
        if (index > 0) text += index + 1 == accepted.size() ? " or a " : ", a ";
        text += accepted[index];
    }
    return text;
}

std::string describeResourceTypeRefusalImpl(const std::string& property_name,
                                            const ResourceTypeHint& hint,
                                            const std::string& resource_path,
                                            const std::string& resource_class,
                                            ResourceTypeVerdict verdict) {
    const std::string found = resource_class.empty() ? "an unrecognised type" : resource_class;
    std::string message = "Property \"" + property_name + "\"";
    // No shipped hint_string is exclusions alone, but a sentence with a hole
    // where the type should be is worse than one that says less.
    message += hint.accepted.empty() ? " takes no type this resource is"
                                     : " holds a " + describeAcceptedTypes(hint.accepted);
    message += "; " + resource_path + " is " + found;
    if (verdict == ResourceTypeVerdict::Excluded) {
        message += ", which that slot excludes";
    }
    return message;
}

// Loads the resource the caller named and refuses it if it is not what the
// property holds. Writing a Texture2D into a tile_set slot would either be
// dropped by Godot or leave a scene nobody can open, and neither is something
// to report as a successful write. Same check script_attach_to_node makes for a
// Script.
//
// The declared type is a list rather than a name, which is what parsing it
// through ResourceTypeHint is for. It used to be compared whole, so the 34
// properties whose declared type carries a comma -- every material slot in 2D
// and 3D among them -- refused the types they were named after (#783).
bool resourceFileExists(const std::string& path);

Result<VariantValue> makeResourceForProperty(const std::string& property_name,
                                             const json& value,
                                             const std::string& expected_class) {
    if (value.is_null()) return VariantValue{};
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    const auto path = value.get<std::string>();
    if (!resourceFileExists(path)) {
        return Error::notFound("Property \"" + property_name + "\": no resource could be loaded from " +
                               path + ", because there is no file there.");
    }
    auto path_value = makeString(path);
    if (path_value.isErr()) return path_value.error();
    auto type_hint = makeString("");
    if (type_hint.isErr()) return type_hint.error();
    auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (cache_mode.isErr()) return cache_mode.error();
    auto loaded_value = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                   {&path_value.value(), &type_hint.value(), &cache_mode.value()});
    if (loaded_value.isErr()) return loaded_value.error();
    auto loaded = objectFromVariant(loaded_value.value());
    if (loaded.isErr()) return loaded.error();
    if (!loaded.value()) {
        return Error::notFound("Property \"" + property_name + "\": no resource could be loaded from " +
                               path);
    }
    const auto hint = parseResourceTypeHintImpl(expected_class);
    if (!hint.accepted.empty() || !hint.excluded.empty()) {
        // Read once, before the verdict: an exclusion is decided on the class
        // the resource actually is, not on what it inherits from.
        const auto actual_name = nodeClassName(loaded.value());
        Error engine_failure;
        bool engine_failed = false;
        const auto inherits = [&](const std::string& base) {
            if (engine_failed) return false;
            auto matches = objectIsClass(loaded.value(), base.c_str());
            if (matches.isErr()) {
                engine_failure = matches.error();
                engine_failed = true;
                return false;
            }
            return matches.value();
        };
        const auto verdict = resourceTypeVerdictImpl(hint, actual_name, inherits);
        if (engine_failed) return engine_failure;
        if (verdict != ResourceTypeVerdict::Accepted) {
            return Error::invalidArgument(describeResourceTypeRefusalImpl(
                property_name, hint, path, actual_name, verdict));
        }
    }
    return makeObject(loaded.value());
}

// Godot narrows a real to an int on assignment, but converting the whole
// number here keeps the value that reaches the property the one the caller
// named rather than one the engine derived.
// A built-in made by the engine's own constructor for these arguments.
// variant_construct picks the constructor whose parameter types the arguments
// have, so a Transform2D made from three Vector2s is Transform2D(x, y, origin)
// on every line, with no constructor index to keep in step with the engine.
Result<VariantValue> constructBuiltin(GDExtensionVariantType type,
                                      const std::vector<const VariantValue*>& arguments) {
    auto& api = GodotApi::instance();
    if (!api.variant_construct) return Error::internal("Godot's Variant constructor is unavailable");
    std::vector<GDExtensionConstVariantPtr> pointers;
    for (const auto* argument : arguments) pointers.push_back(argument->ptr());
    VariantValue built(VariantValue::Uninitialized{});
    GDExtensionCallError error{};
    api.variant_construct(type, built.ptr(), pointers.empty() ? nullptr : pointers.data(),
                          static_cast<int32_t>(pointers.size()), &error);
    built.markInitialized();
    if (error.error != GDEXTENSION_CALL_OK) {
        return Error::internal("Godot could not make a " + godotVariantTypeName(static_cast<int>(type)) +
                               " from " + std::to_string(arguments.size()) + " values (call error " +
                               std::to_string(error.error) + ")");
    }
    return std::move(built);
}

Result<VariantValue> makeJsonVariantForProperty(const json& value, GDExtensionVariantType type);

// An untyped Array of each element made as the given type.
Result<VariantValue> makeArrayOfType(const json& value, GDExtensionVariantType element) {
    auto items = makeJsonVariant(json::array());
    if (items.isErr()) return items.error();
    for (const auto& item : value) {
        auto built = makeJsonVariantForProperty(item, element);
        if (built.isErr()) return built.error();
        auto appended = callVariant(items.value(), "append", {&built.value()});
        if (appended.isErr()) return appended.error();
    }
    return std::move(items.value());
}

Result<VariantValue> makeJsonVariantForProperty(const json& value, GDExtensionVariantType type) {
    if (const auto* members = compoundMembers(type)) {
        std::vector<VariantValue> built;
        built.reserve(members->size());
        for (const auto& member : *members) {
            auto part = makeJsonVariantForProperty(value[member.name], member.type);
            if (part.isErr()) return part.error();
            built.push_back(std::move(part.value()));
        }
        std::vector<const VariantValue*> arguments;
        for (const auto& part : built) arguments.push_back(&part);
        return constructBuiltin(type, arguments);
    }
    if (const auto element = packedElementType(type); element != GDEXTENSION_VARIANT_TYPE_NIL) {
        // Through an Array of elements already the packed type's own, because
        // Godot converts a Dictionary element to a zero vector and says
        // nothing (measured on 4.5.1, 4.6.2 and 4.7.2).
        auto items = makeArrayOfType(value, element);
        if (items.isErr()) return items.error();
        return constructBuiltin(type, {&items.value()});
    }
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_ARRAY:
            // An Array property is built by makeArrayForProperty, which knows
            // the array it replaces. One with nothing to take a type from, an
            // Array inside a typed Array, is refused rather than guessed at.
            return Error::invalidArgument(
                "An Array inside an Array has no element type this can take its values from.");
        case GDEXTENSION_VARIANT_TYPE_INT:
            if (isWholeNumberJsonReal(value)) {
                return makeScalar(GDEXTENSION_VARIANT_TYPE_INT,
                                  static_cast<int64_t>(value.get<double>()));
            }
            break;
        // The other direction, and it is the one that loses data. Object::set
        // coerces an int Variant to the float the property declares, so a
        // scene property survived this; ShaderMaterial::set_shader_parameter
        // stores the Variant as handed to it, so a float uniform ended up
        // holding an int and Godot dropped the mismatched parameter the next
        // time the material was serialised (#612). 1 is the ordinary JSON
        // spelling of a whole number, so this happened every time a caller set
        // a float uniform to 0 or 1.
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
            if (value.is_number()) {
                return makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, value.get<double>());
            }
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
            return makeVector2(value["x"].get<double>(), value["y"].get<double>());
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
            return makeVector2i(jsonWholeMember(value, "x"), jsonWholeMember(value, "y"));
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
            return makeVector3(value["x"].get<double>(), value["y"].get<double>(),
                               value["z"].get<double>());
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
            return makeVector3i(jsonWholeMember(value, "x"), jsonWholeMember(value, "y"),
                                jsonWholeMember(value, "z"));
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            return makeColorFromJson(value);
        default:
            break;
    }
    return makeJsonVariant(value);
}

Result<void> requireMethodBind(const char* class_name, const char* method_name, int64_t hash) {
    auto& api = GodotApi::instance();
    NativeName klass(class_name);
    NativeName method(method_name);
    if (!klass.valid() || !method.valid()) {
        return Error::internal("Failed to construct Godot method identifiers");
    }
    if (!api.classdb_get_method_bind(klass.ptr(), method.ptr(), hash)) {
        return Error::internal(std::string("Godot method binding unavailable: ") +
                               class_name + "." + method_name);
    }
    return Result<void>::ok();
}

// Whether a value holds an object that has been freed, which objectFromVariant
// answers as null. Reading the pointer only copies it out of the Variant.
bool holdsFreedObject(VariantValue& value) {
    const auto& api = GodotApi::instance();
    if (api.variant_get_type(value.ptr()) != GDEXTENSION_VARIANT_TYPE_OBJECT || !api.variant_booleanize) {
        return false;
    }
    auto object = scalarFromVariant<GDExtensionObjectPtr>(value, GDEXTENSION_VARIANT_TYPE_OBJECT);
    return object.isOk() && object.value() && !api.variant_booleanize(value.ptr());
}

Result<std::string> nativeStringToUtf8(const void* native_string) {
    auto& api = GodotApi::instance();
    auto length = api.string_to_utf8_chars(native_string, nullptr, 0);
    if (length < 0) return Error::internal("Godot String UTF-8 conversion failed");
    std::string text(static_cast<size_t>(length), '\0');
    if (length > 0) api.string_to_utf8_chars(native_string, text.data(), length);
    return text;
}

Result<std::string> stringFromVariant(VariantValue& value, GDExtensionVariantType type) {
    auto& api = GodotApi::instance();
    auto to_native = api.get_variant_to_type_constructor(type);
    if (!to_native) return Error::internal("Missing string-like Variant conversion");
    NativeValue native(type);
    to_native(native.ptr(), value.ptr());
    native.markInitialized();
    if (type == GDEXTENSION_VARIANT_TYPE_STRING) return nativeStringToUtf8(native.ptr());

    int string_ctor_index = type == GDEXTENSION_VARIANT_TYPE_STRING_NAME ? 2 : 3;
    auto string_ctor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_STRING, string_ctor_index);
    if (!string_ctor) return Error::internal("Missing String conversion constructor");
    NativeValue native_string(GDEXTENSION_VARIANT_TYPE_STRING);
    const void* args[] = {native.ptr()};
    string_ctor(native_string.ptr(), args);
    native_string.markInitialized();
    return nativeStringToUtf8(native_string.ptr());
}

// `lenient` substitutes null for a Variant type this cannot coerce, instead of
// failing the whole conversion. Only for callers that read structure and ignore
// values -- method arity metadata, where a single default argument of an
// uncoercible type would otherwise make an entire node's method list unreadable.
Result<json> realVectorToJson(VariantValue& value, int dimensions);
Result<json> wholeVectorToJson(VariantValue& value, int dimensions);
Result<json> colorToJson(VariantValue& value);
Result<json> resourcePathToJson(VariantValue& value);

// An Array property's new value. A typed one, Array[Vector2] or
// Array[Texture2D], keeps what it holds when handed an untyped array, with no
// error, and an element of another type is dropped the same way (measured on
// 4.5.1, 4.6.2 and 4.7.2). So the new array is made with the type, class and
// script of the one the property holds now, each element built as that type.
// An untyped one has no element type to turn a JSON object or list into: a
// vector sent there would land as a Dictionary that reads back the same. It
// takes scalars only.
Result<VariantValue> makeArrayForProperty(const std::string& property_name, const json& value,
                                          VariantValue& current,
                                          const std::function<Result<VariantValue>(const std::string&,
                                              const json&, const std::string&)>& object_builder) {
    if (GodotApi::instance().variant_get_type(current.ptr()) != GDEXTENSION_VARIANT_TYPE_ARRAY) {
        return Error::invalidArgument("Property \"" + property_name +
                                      "\" holds no Array to take an element type from.");
    }
    auto builtin = callVariant(current, "get_typed_builtin");
    if (builtin.isErr()) return builtin.error();
    auto element_type = scalarFromVariant<int64_t>(builtin.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (element_type.isErr()) return element_type.error();
    const auto element = static_cast<GDExtensionVariantType>(element_type.value());
    std::string class_text;
    if (element == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto class_name = callVariant(current, "get_typed_class_name");
        if (class_name.isErr()) return class_name.error();
        auto text = stringFromVariant(class_name.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
        if (text.isErr()) return text.error();
        class_text = text.value();
    }
    auto items = makeJsonVariant(json::array());
    if (items.isErr()) return items.error();
    for (size_t index = 0; index < value.size(); ++index) {
        const auto& item = value[index];
        const auto name = property_name + "[" + std::to_string(index) + "]";
        auto built = [&]() -> Result<VariantValue> {
            if (element == GDEXTENSION_VARIANT_TYPE_NIL) {
                if (!item.is_null() && !item.is_boolean() && !item.is_number() && !item.is_string()) {
                    return Error::invalidArgument(
                        "Property \"" + property_name + "\" is an untyped Array, so it has no element type "
                        "to turn the JSON " + jsonValueTypeName(item) + " at index " + std::to_string(index) +
                        " into, and Godot would keep it as a Dictionary or a nested Array rather than the "
                        "vector or colour it may stand for. Send numbers, strings, booleans or null, or "
                        "declare the property as a typed array such as Array[Vector2].");
                }
                return makeJsonVariant(item);
            }
            if (element == GDEXTENSION_VARIANT_TYPE_OBJECT) {
                if (object_builder) return object_builder(name, item, class_text);
                if (!propertyTypeAcceptsJson(item, element)) {
                    return Error::invalidArgument(describePropertyTypeMismatch(name, item, element));
                }
                return makeResourceForProperty(name, item, class_text);
            }
            auto fits = validateJsonForPropertyType(name, item, element);
            if (fits.isErr()) return fits.error();
            return makeJsonVariantForProperty(item, element);
        }();
        if (built.isErr()) return built.error();
        auto appended = callVariant(items.value(), "append", {&built.value()});
        if (appended.isErr()) return appended.error();
    }
    if (element == GDEXTENSION_VARIANT_TYPE_NIL) return std::move(items.value());
    auto class_name = callVariant(current, "get_typed_class_name");
    auto script = callVariant(current, "get_typed_script");
    if (class_name.isErr()) return class_name.error();
    if (script.isErr()) return script.error();
    return constructBuiltin(GDEXTENSION_VARIANT_TYPE_ARRAY,
                            {&items.value(), &builtin.value(), &class_name.value(), &script.value()});
}

// A Dictionary property's new value (#1195). A typed one, Dictionary[String,
// int], keeps what it holds when handed an untyped dictionary, with no error,
// as a typed Array does (measured on 4.5.1, 4.6.2 and 4.7.2). So the new one is
// made with the key and value types of the one the property holds, each value
// built as its type. JSON keys are strings: they stay a String, become a
// StringName for a StringName key, and a whole number for an int key. An
// untyped dictionary takes scalar values, for the reason an untyped Array does.
Result<VariantValue> makeDictionaryForProperty(const std::string& property_name, const json& value,
                                               VariantValue& current) {
    if (GodotApi::instance().variant_get_type(current.ptr()) != GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
        return Error::invalidArgument("Property \"" + property_name +
                                      "\" holds no Dictionary to take key and value types from.");
    }
    const auto typed = [&](const char* method) -> Result<GDExtensionVariantType> {
        auto builtin = callVariant(current, method);
        if (builtin.isErr()) return builtin.error();
        auto number = scalarFromVariant<int64_t>(builtin.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (number.isErr()) return number.error();
        return static_cast<GDExtensionVariantType>(number.value());
    };
    auto key_type = typed("get_typed_key_builtin");
    if (key_type.isErr()) return key_type.error();
    auto value_type = typed("get_typed_value_builtin");
    if (value_type.isErr()) return value_type.error();
    const auto key = key_type.value();
    const auto element = value_type.value();
    if (key != GDEXTENSION_VARIANT_TYPE_NIL && key != GDEXTENSION_VARIANT_TYPE_STRING &&
        key != GDEXTENSION_VARIANT_TYPE_STRING_NAME && key != GDEXTENSION_VARIANT_TYPE_INT) {
        return Error::invalidArgument(
            "Property \"" + property_name + "\" is a Dictionary keyed by " +
            godotVariantTypeName(static_cast<int>(key)) +
            ", and a JSON key is a string, which spells a String, StringName or int key only.");
    }
    std::string class_text;
    if (element == GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto class_name = callVariant(current, "get_typed_value_class_name");
        if (class_name.isErr()) return class_name.error();
        auto text = stringFromVariant(class_name.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
        if (text.isErr()) return text.error();
        class_text = text.value();
    }
    auto entries = makeJsonVariant(json::object());
    if (entries.isErr()) return entries.error();
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto& key_text = it.key();
        const auto& item = it.value();
        const auto name = property_name + "[\"" + key_text + "\"]";
        auto built_key = [&]() -> Result<VariantValue> {
            if (key == GDEXTENSION_VARIANT_TYPE_INT) {
                const auto number = dictionaryIntKey(key_text);
                if (!number) {
                    return Error::invalidArgument(
                        "Property \"" + property_name + "\" is a Dictionary keyed by int, and the key \"" +
                        key_text + "\" is not a whole number. Send each key as the number in a string, "
                        "such as \"3\" or \"-1\".");
                }
                return makeScalar(GDEXTENSION_VARIANT_TYPE_INT, *number);
            }
            if (key == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                return makeJsonVariantForProperty(json(key_text), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            }
            auto as_text = makeJsonVariant(json(key_text));
            // An untyped Dictionary keeps 1 and "1" apart, and an int key reads
            // back as its digits. One the property holds as an int, and not as
            // a String, is written back as that int, so a read sent back
            // unchanged leaves table[1] where it was (#1249).
            if (const auto number = dictionaryIntKey(key_text); number && key == GDEXTENSION_VARIANT_TYPE_NIL &&
                                                               as_text.isOk()) {
                auto as_int = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, *number);
                if (as_int.isErr()) return as_int.error();
                auto holds_int = callVariant(current, "has", {&as_int.value()});
                auto holds_text = callVariant(current, "has", {&as_text.value()});
                if (holds_int.isErr()) return holds_int.error();
                if (holds_text.isErr()) return holds_text.error();
                auto int_held = scalarFromVariant<GDExtensionBool>(holds_int.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                auto text_held = scalarFromVariant<GDExtensionBool>(holds_text.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (int_held.isErr()) return int_held.error();
                if (text_held.isErr()) return text_held.error();
                if (int_held.value() != 0 && text_held.value() == 0) return std::move(as_int.value());
            }
            return as_text;
        }();
        if (built_key.isErr()) return built_key.error();
        auto built = [&]() -> Result<VariantValue> {
            if (element == GDEXTENSION_VARIANT_TYPE_NIL) {
                if (!item.is_null() && !item.is_boolean() && !item.is_number() && !item.is_string()) {
                    return Error::invalidArgument(
                        "Property \"" + property_name + "\" is an untyped Dictionary, so it has no value "
                        "type to turn the JSON " + jsonValueTypeName(item) + " at key \"" + key_text +
                        "\" into, and Godot would keep it as a Dictionary or an Array rather than the "
                        "vector or colour it may stand for. Send numbers, strings, booleans or null, or "
                        "declare the property as a typed dictionary such as Dictionary[String, Vector2].");
                }
                return makeJsonVariant(item);
            }
            if (element == GDEXTENSION_VARIANT_TYPE_OBJECT) {
                if (!propertyTypeAcceptsJson(item, element)) {
                    return Error::invalidArgument(describePropertyTypeMismatch(name, item, element));
                }
                return makeResourceForProperty(name, item, class_text);
            }
            auto fits = validateJsonForPropertyType(name, item, element);
            if (fits.isErr()) return fits.error();
            return makeJsonVariantForProperty(item, element);
        }();
        if (built.isErr()) return built.error();
        auto stored = callVariant(entries.value(), "set", {&built_key.value(), &built.value()});
        if (stored.isErr()) return stored.error();
    }
    if (key == GDEXTENSION_VARIANT_TYPE_NIL && element == GDEXTENSION_VARIANT_TYPE_NIL) {
        return std::move(entries.value());
    }
    auto key_builtin = callVariant(current, "get_typed_key_builtin");
    auto key_class = callVariant(current, "get_typed_key_class_name");
    auto key_script = callVariant(current, "get_typed_key_script");
    auto value_builtin = callVariant(current, "get_typed_value_builtin");
    auto value_class = callVariant(current, "get_typed_value_class_name");
    auto value_script = callVariant(current, "get_typed_value_script");
    for (const auto* part : {&key_builtin, &key_class, &key_script, &value_builtin, &value_class, &value_script}) {
        if (part->isErr()) return part->error();
    }
    return constructBuiltin(GDEXTENSION_VARIANT_TYPE_DICTIONARY,
                            {&entries.value(), &key_builtin.value(), &key_class.value(), &key_script.value(),
                             &value_builtin.value(), &value_class.value(), &value_script.value()});
}

Result<json> variantToJson(VariantValue& value, int depth, bool lenient) {
    if (depth > 16) return Error::invalidArgument("Godot Variant is nested more than 16 levels deep");
    auto& api = GodotApi::instance();
    auto type = api.variant_get_type(value.ptr());
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_NIL: return json(nullptr);
        case GDEXTENSION_VARIANT_TYPE_BOOL: {
            auto result = scalarFromVariant<GDExtensionBool>(value, type);
            return result.isOk() ? Result<json>(json(result.value() != 0)) : Result<json>(result.error());
        }
        case GDEXTENSION_VARIANT_TYPE_INT: {
            auto result = scalarFromVariant<int64_t>(value, type);
            return result.isOk() ? Result<json>(json(result.value())) : Result<json>(result.error());
        }
        case GDEXTENSION_VARIANT_TYPE_FLOAT: {
            auto result = scalarFromVariant<double>(value, type);
            return result.isOk() ? Result<json>(realToJson(result.value()))
                                 : Result<json>(result.error());
        }
        case GDEXTENSION_VARIANT_TYPE_STRING:
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH: {
            auto result = stringFromVariant(value, type);
            return result.isOk() ? Result<json>(json(result.value())) : Result<json>(result.error());
        }
        case GDEXTENSION_VARIANT_TYPE_ARRAY: {
            auto size_result = callVariant(value, "size");
            if (size_result.isErr()) return size_result.error();
            auto size = scalarFromVariant<int64_t>(size_result.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return size.error();
            json output = json::array();
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return index.error();
                auto item = callVariant(value, "get", {&index.value()});
                if (item.isErr()) return item.error();
                auto converted = variantToJson(item.value(), depth + 1, lenient);
                if (converted.isErr()) return converted.error();
                output.push_back(converted.value());
            }
            return output;
        }
        case GDEXTENSION_VARIANT_TYPE_DICTIONARY: {
            auto keys = callVariant(value, "keys");
            if (keys.isErr()) return keys.error();
            auto size_result = callVariant(keys.value(), "size");
            if (size_result.isErr()) return size_result.error();
            auto size = scalarFromVariant<int64_t>(size_result.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return size.error();
            json output = json::object();
            std::map<std::string, GDExtensionVariantType> key_types;
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return index.error();
                auto key = callVariant(keys.value(), "get", {&index.value()});
                if (key.isErr()) return key.error();
                auto key_type = GodotApi::instance().variant_get_type(key.value().ptr());
                // An int key reads as the decimal string a write takes it as,
                // so an int-keyed dictionary round-trips (#1195).
                Result<std::string> key_text = std::string();
                if (key_type == GDEXTENSION_VARIANT_TYPE_INT) {
                    auto number = scalarFromVariant<int64_t>(key.value(), GDEXTENSION_VARIANT_TYPE_INT);
                    if (number.isErr()) return number.error();
                    key_text = std::to_string(number.value());
                } else if (key_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                           key_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                    key_text = stringFromVariant(key.value(), key_type);
                } else {
                    return Error::invalidArgument("Godot Dictionary contains a key that is not a string or an int");
                }
                if (key_text.isErr()) return key_text.error();
                if (const auto first = key_types.find(key_text.value()); first != key_types.end()) {
                    return Error::invalidArgument("Godot Dictionary holds the key \"" + key_text.value() +
                                                  "\" as both a " +
                                                  godotVariantTypeName(static_cast<int>(first->second)) +
                                                  " and a " + godotVariantTypeName(static_cast<int>(key_type)) +
                                                  ", which JSON cannot tell apart");
                }
                key_types.emplace(key_text.value(), key_type);
                auto item = callVariant(value, "get", {&key.value()});
                if (item.isErr()) return item.error();
                auto converted = variantToJson(item.value(), depth + 1, lenient);
                if (converted.isErr()) return converted.error();
                output[key_text.value()] = converted.value();
            }
            return output;
        }
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
            return realVectorToJson(value, type == GDEXTENSION_VARIANT_TYPE_VECTOR2 ? 2 : 3);
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
            return wholeVectorToJson(value, type == GDEXTENSION_VARIANT_TYPE_VECTOR2I ? 2 : 3);
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            return colorToJson(value);
        case GDEXTENSION_VARIANT_TYPE_OBJECT:
            // A resource is named by its path, which is what the write takes,
            // so the reread can be compared with the request. Anything else in
            // an object slot, a Node for instance, has no path to give.
            return resourcePathToJson(value);
        default:
            // The shapes a write takes, read member by member (Q7).
            if (const auto* members = compoundMembers(type)) {
                if (!api.variant_get_named) return Error::internal("Godot's Variant member read is unavailable");
                json output = json::object();
                for (const auto& member : *members) {
                    NativeName key(member.name);
                    VariantValue part(VariantValue::Uninitialized{});
                    GDExtensionBool valid = false;
                    api.variant_get_named(value.ptr(), key.ptr(), part.ptr(), &valid);
                    part.markInitialized();
                    if (!valid) {
                        return Error::internal("Godot gave no " + std::string(member.name) + " for a " +
                                               godotVariantTypeName(static_cast<int>(type)));
                    }
                    auto converted = variantToJson(part, depth + 1, lenient);
                    if (converted.isErr()) return converted.error();
                    output[member.name] = std::move(converted.value());
                }
                return output;
            }
            // A packed array through Array(from), the engine's own conversion,
            // so each element reads as it would in an Array.
            if (packedElementType(type) != GDEXTENSION_VARIANT_TYPE_NIL) {
                auto as_array = constructBuiltin(GDEXTENSION_VARIANT_TYPE_ARRAY, {&value});
                if (as_array.isErr()) return as_array.error();
                return variantToJson(as_array.value(), depth, lenient);
            }
            if (lenient) return json(nullptr);
            return Error::invalidArgument("Godot Variant type " + std::to_string(type) + " is not JSON-coercible");
    }
}

// One member read for every fixed-size built-in the property contract accepts.
// The axis list is the difference between them, so it is the only thing that
// varies.
Result<json> builtinMembersToJson(VariantValue& value, GDExtensionVariantType type,
                                  std::initializer_list<const char*> members, bool whole_numbers) {
    auto& api = GodotApi::instance();
    if (api.variant_get_type(value.ptr()) != type || !api.variant_get_ptr_getter) {
        return Error::internal("Godot value is not the built-in type it was read as");
    }
    auto converter = api.get_variant_to_type_constructor(type);
    if (!converter) return Error::internal("Godot built-in conversion is unavailable");
    NativeValue native(type);
    converter(native.ptr(), value.ptr());
    native.markInitialized();
    json output = json::object();
    for (const auto* member : members) {
        NativeName name(member);
        auto getter = name.valid() ? api.variant_get_ptr_getter(type, name.ptr()) : nullptr;
        if (!getter) return Error::internal("Godot built-in member getter is unavailable");
        if (whole_numbers) {
            int64_t component = 0;
            getter(native.ptr(), &component);
            output[member] = component;
        } else {
            // The GDExtension pointer ABI marshals float and real_t members as
            // double, which is why makeVector2 hands the constructor doubles
            // and why every existing reader here uses one.
            double component = 0.0;
            getter(native.ptr(), &component);
            output[member] = realToJson(component);
        }
    }
    return output;
}

Result<json> realVectorToJson(VariantValue& value, int dimensions) {
    return dimensions == 2
        ? builtinMembersToJson(value, GDEXTENSION_VARIANT_TYPE_VECTOR2, {"x", "y"}, false)
        : builtinMembersToJson(value, GDEXTENSION_VARIANT_TYPE_VECTOR3, {"x", "y", "z"}, false);
}

Result<json> wholeVectorToJson(VariantValue& value, int dimensions) {
    return dimensions == 2
        ? builtinMembersToJson(value, GDEXTENSION_VARIANT_TYPE_VECTOR2I, {"x", "y"}, true)
        : builtinMembersToJson(value, GDEXTENSION_VARIANT_TYPE_VECTOR3I, {"x", "y", "z"}, true);
}

Result<json> colorToJson(VariantValue& value) {
    return builtinMembersToJson(value, GDEXTENSION_VARIANT_TYPE_COLOR, {"r", "g", "b", "a"}, false);
}

Result<json> resourcePathToJson(VariantValue& value) {
    auto object = objectFromVariant(value);
    if (object.isErr()) return object.error();
    if (!object.value()) return json(nullptr);
    auto is_resource = objectIsClass(object.value(), "Resource");
    if (is_resource.isErr() || !is_resource.value()) return json(nullptr);
    auto path_name = makeStringName("resource_path");
    if (path_name.isErr()) return path_name.error();
    auto path_value = callObject(object.value(), "Object", "get", 2760726917LL, {&path_name.value()});
    if (path_value.isErr()) return path_value.error();
    auto path = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    if (path.isErr()) return path.error();
    // A resource built in memory or embedded in a scene has no path of its own.
    // Saying null is the honest answer; inventing one is not.
    return path.value().empty() ? json(nullptr) : json(path.value());
}

Result<json> nativeVector2ToJson(const void* native_vector) {
    auto& api = GodotApi::instance();
    if (!api.variant_get_ptr_getter) return Error::internal("Godot built-in member getter API is unavailable");
    NativeName x_name("x");
    NativeName y_name("y");
    if (!x_name.valid() || !y_name.valid()) return Error::internal("Failed to construct Vector2 member names");
    auto get_x = api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_VECTOR2, x_name.ptr());
    auto get_y = api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_VECTOR2, y_name.ptr());
    if (!get_x || !get_y) return Error::internal("Godot Vector2 member getters are unavailable");
    double x = 0.0;
    double y = 0.0;
    get_x(native_vector, &x);
    get_y(native_vector, &y);
    return json{{"x", x}, {"y", y}};
}

Result<json> vector2ToJson(VariantValue& value) {
    if (GodotApi::instance().variant_get_type(value.ptr()) != GDEXTENSION_VARIANT_TYPE_VECTOR2) {
        return Error::invalidArgument("Godot value is not a Vector2");
    }
    auto converter = GodotApi::instance().get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR2);
    if (!converter) return Error::internal("Godot Vector2 conversion is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_VECTOR2);
    converter(native.ptr(), value.ptr());
    native.markInitialized();
    return nativeVector2ToJson(native.ptr());
}

Result<json> rect2ToJson(VariantValue& value) {
    if (GodotApi::instance().variant_get_type(value.ptr()) != GDEXTENSION_VARIANT_TYPE_RECT2) {
        return Error::invalidArgument("Godot value is not a Rect2");
    }
    auto& api = GodotApi::instance();
    auto converter = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_RECT2);
    if (!converter || !api.variant_get_ptr_getter) {
        return Error::internal("Godot Rect2 conversion is unavailable");
    }
    NativeValue native_rect(GDEXTENSION_VARIANT_TYPE_RECT2);
    converter(native_rect.ptr(), value.ptr());
    native_rect.markInitialized();
    NativeName position_name("position");
    NativeName size_name("size");
    auto get_position = position_name.valid()
        ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_RECT2, position_name.ptr()) : nullptr;
    auto get_size = size_name.valid()
        ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_RECT2, size_name.ptr()) : nullptr;
    if (!get_position || !get_size) return Error::internal("Godot Rect2 member getters are unavailable");
    NativeValue native_position(GDEXTENSION_VARIANT_TYPE_VECTOR2);
    NativeValue native_size(GDEXTENSION_VARIANT_TYPE_VECTOR2);
    get_position(native_rect.ptr(), native_position.ptr());
    get_size(native_rect.ptr(), native_size.ptr());
    native_position.markInitialized();
    native_size.markInitialized();
    auto position = nativeVector2ToJson(native_position.ptr());
    auto size = nativeVector2ToJson(native_size.ptr());
    if (position.isErr()) return position.error();
    if (size.isErr()) return size.error();
    return json{{"position", position.value()}, {"size", size.value()}};
}

// Which display driver this engine came up on, as the engine names it.
//
// "headless" is the one every build machine, container and ssh session gets,
// and it selects the dummy rendering backend: there is no device behind the
// viewport, so nothing that ends in a picture can work for as long as the
// editor is running. Didi attached to such an editor happily and then answered
// every capture with `404 not_found`, which tells a caller to look again with a
// better argument (#676).
//
// Read once. The display driver is chosen at startup and does not change, and
// this is asked on paths that must not add an engine call per frame.
const std::string& displayServerName() {
    static const std::string name = [] {
        auto server = singleton("DisplayServer");
        if (server.isErr()) return std::string();
        // get_name() -> String. The hash is the signature's, so it is the same
        // on 4.5, 4.6 and 4.7; an engine that changes it simply answers nothing
        // here and the reports fall back to saying they do not know.
        auto value = callObject(server.value(), "DisplayServer", "get_name", 201670096LL);
        if (value.isErr()) return std::string();
        auto text = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        return text.isErr() ? std::string() : text.value();
    }();
    return name;
}

// Whether this engine has a rendering device behind its viewports.
bool engineCanRender() { return displayServerName() != "headless"; }


bool hasOnlyKeys(const json& value, std::initializer_list<const char*> allowed) {
    for (auto it = value.begin(); it != value.end(); ++it) {
        bool found = false;
        for (const auto* key : allowed) {
            if (it.key() == key) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

Result<void> validateResPath(const std::string& path, const std::string& expected_suffix) {
    const std::string remainder = strings::startsWith(path, "res://") ? path.substr(6) : std::string();
    if (remainder.empty() || remainder.front() == '/' || remainder.find("//") != std::string::npos ||
        remainder.find("./") == 0 || remainder.find("/./") != std::string::npos ||
        strings::endsWith(remainder, "/.") || remainder.find(':') != std::string::npos ||
        path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
        (!expected_suffix.empty() && !strings::endsWith(path, expected_suffix))) {
        return Error::invalidArgument("path must be a normalized res:// path ending in " + expected_suffix);
    }
    return Result<void>::ok();
}

Result<void> validateScriptPath(const std::string& path) {
    if (!strings::endsWith(path, ".gd") && !strings::endsWith(path, ".cs")) {
        return Error::invalidArgument("script_path must end in .gd or .cs");
    }
    return validateResPath(path, strings::endsWith(path, ".gd") ? ".gd" : ".cs");
}

bool pathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_value = root.lexically_normal().generic_string();
    auto candidate_value = candidate.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(root_value.begin(), root_value.end(), root_value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(candidate_value.begin(), candidate_value.end(), candidate_value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return candidate_value == root_value ||
           (candidate_value.size() > root_value.size() &&
            candidate_value.compare(0, root_value.size(), root_value) == 0 &&
            candidate_value[root_value.size()] == '/');
}

Result<GDExtensionObjectPtr> editorInterface() {
    auto& api = GodotApi::instance();
    if (!api.isLiveReady()) return Error::notConnected("Godot main-loop bridge is not ready");
    NativeName name("EditorInterface");
    auto editor = api.global_get_singleton(name.ptr());
    if (!editor) return Error::notConnected("Godot EditorInterface singleton is unavailable");
    return editor;
}

Result<GDExtensionObjectPtr> editedSceneRoot(GDExtensionObjectPtr editor) {
    auto result = callObject(editor, "EditorInterface", "get_edited_scene_root", 3160264692LL);
    if (result.isErr()) return result.error();
    auto root = objectFromVariant(result.value());
    if (root.isErr()) return root.error();
    if (!root.value()) return Error::notFound("No edited scene is open in Godot");
    return root.value();
}

Result<std::string> nodeString(GDExtensionObjectPtr node, const char* method, int64_t hash);

// The res:// path of the scene currently open in the editor, or empty for one
// that has never been saved.
//
// Every live scene answer was true of "the edited scene" and named no scene, so
// a caller had no way to notice the scene had moved under it: scene_create
// opens what it writes, and from that point every later scene_* call answered
// about a different file. The same is true of the node 404s, which said a node
// was not found without saying where they looked.
//
// Node.get_scene_file_path carries the same method hash on 4.5.1, 4.6.2 and
// 4.7.2, so pinning it costs nothing across the engines CI covers.
std::string editedScenePath(GDExtensionObjectPtr root) {
    if (!root) return {};
    auto value = callObject(root, "Node", "get_scene_file_path", 201670096LL);
    if (value.isErr()) return {};
    auto text = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    return text.isErr() ? std::string() : text.value();
}

// Adds the edited scene's identity to a live scene answer. `unsaved` rather
// than a missing field for a scene that has never been saved, because absent
// would read as "this build does not report it".
void addEditedSceneIdentity(json& payload, GDExtensionObjectPtr root) {
    const auto path = editedScenePath(root);
    payload["scene_file_path"] = path.empty() ? json(nullptr) : json(path);
    if (path.empty()) payload["scene_is_unsaved"] = true;
}


// SceneState spells a child of the edited root "./Child"; relativePathWithinEditedRoot,
// which asks Node.get_path_to, spells the same node "Child". Both spell the
// root ".". One spelling, so the two can be compared. Every comparison of a
// SceneState path against a relativePathWithinEditedRoot result goes through
// here, because the one that did not cost #660: a persistent group read as
// transient, so its undo put the membership back in a form the next save drops.
std::string sceneStatePathWithinEditedRoot(const std::string& state_path) {
    if (strings::startsWith(state_path, "./")) return state_path.substr(2);
    return state_path;
}

Result<std::string> relativePathWithinEditedRoot(GDExtensionObjectPtr root,
                                                 GDExtensionObjectPtr target) {
    if (root == target) return std::string(".");
    auto target_value = makeObject(target);
    auto use_unique_path = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                      static_cast<GDExtensionBool>(0));
    if (target_value.isErr()) return target_value.error();
    if (use_unique_path.isErr()) return use_unique_path.error();
    auto relative = callObject(root, "Node", "get_path_to", 498846349LL,
                               {&target_value.value(), &use_unique_path.value()});
    if (relative.isErr()) return relative.error();
    auto path = stringFromVariant(relative.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    if (path.isErr()) return path.error();
    if (path.value() == ".." || strings::startsWith(path.value(), "../") ||
        strings::startsWith(path.value(), "/")) {
        return Error::invalidArgument("Scene node path escapes the edited scene root");
    }
    return path.value();
}

Result<GDExtensionObjectPtr> resolveNode(GDExtensionObjectPtr root, const std::string& path) {
    if (path.empty() || path == "/root" || path == ".") return root;
    // The same rule the preview path runs, from the one place that states it.
    if (auto refused = paths::refuseParentRelativeNodePath(path)) return *refused;
    std::string relative = path;
    if (strings::startsWith(relative, "/root/")) relative = relative.substr(6);
    auto root_name = nodeString(root, "get_name", 2002593661LL);
    if (root_name.isErr()) return root_name.error();
    if (relative == root_name.value()) return root;
    if (strings::startsWith(relative, root_name.value() + "/")) {
        relative = relative.substr(root_name.value().size() + 1);
    }
    auto node_path = makeNodePath(relative);
    if (node_path.isErr()) return node_path.error();
    auto result = callObject(root, "Node", "get_node_or_null", 2734337346LL, {&node_path.value()});
    if (result.isErr()) return result.error();
    auto object = objectFromVariant(result.value());
    if (object.isErr()) return object.error();
    if (!object.value()) {
        // Which scene was searched. scene_create opens what it writes, so a
        // path that was there a call ago can be absent now because the edited
        // scene moved rather than because the node went away.
        const auto scene = editedScenePath(root);
        return Error::notFound("Scene node not found: " + path + " (searched " +
                               (scene.empty() ? std::string("the unsaved edited scene")
                                              : scene) + ")");
    }
    auto within_root = relativePathWithinEditedRoot(root, object.value());
    if (within_root.isErr()) return within_root.error();
    return object.value();
}

Result<std::string> nodeString(GDExtensionObjectPtr node, const char* method, int64_t hash) {
    auto result = callObject(node, std::string(method) == "get_class" ? "Object" : "Node", method, hash);
    if (result.isErr()) return result.error();
    auto type = GodotApi::instance().variant_get_type(result.value().ptr());
    return stringFromVariant(result.value(), type);
}

Result<std::string> logicalPathFromEditedRoot(GDExtensionObjectPtr root,
                                               GDExtensionObjectPtr target) {
    auto root_name = nodeString(root, "get_name", 2002593661LL);
    if (root_name.isErr()) return root_name.error();
    const std::string logical_root = "/root/" + root_name.value();
    if (root == target) return logical_root;

    auto relative_path = relativePathWithinEditedRoot(root, target);
    if (relative_path.isErr()) return relative_path.error();
    if (relative_path.value().empty() || relative_path.value() == ".") return logical_root;
    return logical_root + "/" + relative_path.value();
}

// ---------------------------------------------------------------------------
// Scene ownership: what the file can hold.
//
// SceneState::_parse_node keeps a node only when the edited root owns it, or
// when the instance that owns it is one the edited root marked editable, so a
// change to a node inside an instanced sub-scene was applied to the live tree,
// reported as applied, and dropped by the save. A node the edited scene
// inherits is owned by the edited root, but its existence is recorded in the
// base scene, and a .tscn has no marker for "this inherited node is gone", so
// removing, moving or duplicating one was dropped the same way. The editor
// refuses both before anything happens, in SceneTreeDock::_validate_no_foreign;
// the bridge now does too, on the real call and on the preview (#588, #589).
//
// Node.get_scene_inherited_state is not bound to GDExtension, so the base
// scene is read from the edited scene's own PackedScene: SceneState.
// get_node_instance(0) is the base when the file's root node carries instance=
// and null otherwise, and the base's own state lists the paths the edited scene
// inherits, base by base. Every bind below carries the same hash on 4.5.1,
// 4.6.2 and 4.7.2.


// A PackedScene through the loader's cache, which for the scene open in the
// editor is the editor's own copy. 404 when there is no such file.
Result<VariantValue> loadPackedSceneVariant(const std::string& scene_path) {
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    auto path_value = makeString(scene_path);
    auto hint = makeString("PackedScene");
    auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (path_value.isErr() || hint.isErr() || cache_mode.isErr()) {
        return Error::internal("Failed to construct scene resource arguments");
    }
    auto exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                   {&path_value.value(), &hint.value()});
    if (exists_value.isErr()) return exists_value.error();
    auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(),
                                                     GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr()) return exists.error();
    if (!exists.value()) return Error::notFound("PackedScene not found: " + scene_path);
    return callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                      {&path_value.value(), &hint.value(), &cache_mode.value()});
}

Result<InheritedSceneState> inheritedSceneState(GDExtensionObjectPtr root) {
    InheritedSceneState state;
    const auto edited_path = editedScenePath(root);
    // Never saved: there is no file for anything to inherit from.
    if (edited_path.empty()) return state;
    auto current = loadPackedSceneVariant(edited_path);
    if (current.isErr()) {
        // A path with no file behind it yet inherits nothing either.
        if (current.error().code == 404) return state;
        return current.error();
    }
    auto index_zero = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    auto for_parent = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (index_zero.isErr()) return index_zero.error();
    if (for_parent.isErr()) return for_parent.error();
    // Inheritance chains are short. The bound is so a file that names itself
    // as its own base cannot spin here.
    constexpr int kMaxInheritanceDepth = 16;
    for (int depth = 0; depth < kMaxInheritanceDepth; ++depth) {
        auto packed = objectFromVariant(current.value());
        if (packed.isErr()) return packed.error();
        if (!packed.value()) break;
        auto state_value = callObject(packed.value(), "PackedScene", "get_state", 3479783971LL);
        if (state_value.isErr()) return state_value.error();
        auto scene_state = objectFromVariant(state_value.value());
        if (scene_state.isErr()) return scene_state.error();
        if (!scene_state.value()) break;
        auto count_value = callObject(scene_state.value(), "SceneState", "get_node_count",
                                      3905245786LL);
        if (count_value.isErr()) return count_value.error();
        auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (count.isErr()) return count.error();
        if (count.value() <= 0) break;
        if (depth > 0) {
            for (int64_t i = 0; i < count.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return index.error();
                auto path_value = callObject(scene_state.value(), "SceneState", "get_node_path",
                                             2272487792LL, {&index.value(), &for_parent.value()});
                if (path_value.isErr()) return path_value.error();
                auto path = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
                if (path.isErr()) return path.error();
                state.node_paths.insert(sceneStatePathWithinEditedRoot(path.value()));
            }
        }
        auto base_value = callObject(scene_state.value(), "SceneState", "get_node_instance",
                                     511017218LL, {&index_zero.value()});
        if (base_value.isErr()) return base_value.error();
        auto base = objectFromVariant(base_value.value());
        if (base.isErr()) return base.error();
        if (!base.value()) break;
        if (depth == 0) {
            auto base_path_value = callObject(base.value(), "Resource", "get_path", 201670096LL);
            if (base_path_value.isErr()) return base_path_value.error();
            auto base_path = stringFromVariant(base_path_value.value(),
                                               GDEXTENSION_VARIANT_TYPE_STRING);
            if (base_path.isErr()) return base_path.error();
            state.base_scene = base_path.value();
        }
        current = std::move(base_value);
    }
    return state;
}


Result<NodeOwnership> classifyNodeOwnership(GDExtensionObjectPtr root, GDExtensionObjectPtr node,
                                            const InheritedSceneState& inherited) {
    NodeOwnership ownership;
    if (node == root) {
        ownership.owned_by_scene = true;
        return ownership;
    }
    auto owner_value = callObject(node, "Node", "get_owner", 3160264692LL);
    if (owner_value.isErr()) return owner_value.error();
    auto owner = objectFromVariant(owner_value.value());
    if (owner.isErr()) return owner.error();
    if (owner.value() == root) {
        ownership.owned_by_scene = true;
    } else if (!owner.value()) {
        ownership.foreign = true;
    } else {
        auto owner_arg = makeObject(owner.value());
        if (owner_arg.isErr()) return owner_arg.error();
        // is_editable_instance asserts its argument is a descendant and prints
        // when it is not, so that is asked first. An owner is always an
        // ancestor of what it owns, so anything not beneath the root sits
        // above it and cannot be an instance the root marked.
        auto beneath_value = callObject(root, "Node", "is_ancestor_of", 3093956946LL,
                                        {&owner_arg.value()});
        if (beneath_value.isErr()) return beneath_value.error();
        auto beneath = scalarFromVariant<GDExtensionBool>(beneath_value.value(),
                                                          GDEXTENSION_VARIANT_TYPE_BOOL);
        if (beneath.isErr()) return beneath.error();
        bool editable = false;
        if (beneath.value()) {
            auto editable_value = callObject(root, "Node", "is_editable_instance", 3093956946LL,
                                             {&owner_arg.value()});
            if (editable_value.isErr()) return editable_value.error();
            auto flag = scalarFromVariant<GDExtensionBool>(editable_value.value(),
                                                           GDEXTENSION_VARIANT_TYPE_BOOL);
            if (flag.isErr()) return flag.error();
            editable = flag.value() != 0;
        }
        if (!editable) {
            ownership.foreign = true;
            auto owner_scene = callObject(owner.value(), "Node", "get_scene_file_path", 201670096LL);
            if (owner_scene.isOk()) {
                auto text = stringFromVariant(owner_scene.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (text.isOk()) ownership.owner_scene = text.value();
            }
            if (beneath.value()) {
                auto instance_root = logicalPathFromEditedRoot(root, owner.value());
                if (instance_root.isOk()) ownership.instance_root = instance_root.value();
            }
        }
    }
    if (ownership.owned_by_scene && !inherited.base_scene.empty()) {
        auto relative = relativePathWithinEditedRoot(root, node);
        if (relative.isErr()) return relative.error();
        ownership.inherited = inherited.node_paths.count(relative.value()) > 0;
    }
    return ownership;
}

// The node and every descendant the edited root owns, which is the set a
// branch loses when it leaves the tree: Node::_propagate_after_exit_tree
// clears any owner that is no longer an ancestor. Internal children are
// skipped, as the packer skips them.

// The non-internal children of a node, in tree order. Internal children are
// skipped, as the packer skips them.
Result<std::vector<GDExtensionObjectPtr>> childNodes(GDExtensionObjectPtr node) {
    std::vector<GDExtensionObjectPtr> collected;
    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
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
        collected.push_back(child.value());
    }
    return collected;
}




// The refusal the file's rules give this edit, or nothing when it can be saved.
std::optional<json> refuseUnsavableEdit(GDExtensionObjectPtr root, GDExtensionObjectPtr node,
                                        const std::string& described_path, SceneEdit edit) {
    InheritedSceneState inherited;
    if (edit == SceneEdit::Structure) {
        auto loaded = inheritedSceneState(root);
        if (loaded.isErr()) return errorJson(loaded.error());
        inherited = std::move(loaded.value());
    }
    auto ownership = classifyNodeOwnership(root, node, inherited);
    if (ownership.isErr()) return errorJson(ownership.error());
    const auto& owned = ownership.value();
    if (owned.foreign) {
        const char* dropped = edit == SceneEdit::AddChild
            ? "a node added under it"
            : (edit == SceneEdit::Structure ? "removing, moving or duplicating it"
                                            : "a change to it");
        std::string detail = "`" + described_path + "` ";
        if (!owned.owner_scene.empty() && !owned.instance_root.empty()) {
            detail += "belongs to the instance of " + owned.owner_scene + " at " +
                      owned.instance_root + ", so a save would drop " + dropped + ". Edit " +
                      owned.owner_scene + " itself, or turn on Editable Children for " +
                      owned.instance_root + " in the editor first.";
        } else {
            detail += std::string("is not owned by the edited scene, so a save would drop ") + dropped + ".";
        }
        json data = {{"target_node", described_path}};
        if (!owned.owner_scene.empty()) data["owner_scene"] = owned.owner_scene;
        if (!owned.instance_root.empty()) data["instance_root"] = owned.instance_root;
        return bridgeError(409, "node_not_owned", std::move(data), detail);
    }
    if (owned.inherited && edit == SceneEdit::Structure) {
        const std::string detail = "`" + described_path + "` is inherited from " +
                                   inherited.base_scene + ". Make the change in " +
                                   inherited.base_scene + ", or override its properties here instead.";
        return bridgeError(409, "node_inherited",
                           {{"target_node", described_path}, {"base_scene", inherited.base_scene}},
                           detail);
    }
    return std::nullopt;
}

// One res:// path from a ResourceLoader.get_dependencies entry: the path on its
// own, or "uid::type::path" when the file recorded a uid, in which case the uid
// is asked for its current path and the recorded one is the fallback.
Result<std::string> dependencyResourcePath(const std::string& entry) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t separator = entry.find("::", start);
        parts.push_back(entry.substr(
            start, separator == std::string::npos ? std::string::npos : separator - start));
        if (separator == std::string::npos) break;
        start = separator + 2;
    }
    if (!parts.empty() && strings::startsWith(parts.front(), "uid://")) {
        auto uids = singleton("ResourceUID");
        if (uids.isErr()) return uids.error();
        auto text = makeString(parts.front());
        if (text.isErr()) return text.error();
        auto id_value = callObject(uids.value(), "ResourceUID", "text_to_id", 1321353865LL,
                                   {&text.value()});
        if (id_value.isErr()) return id_value.error();
        auto id = scalarFromVariant<int64_t>(id_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (id.isErr()) return id.error();
        if (id.value() >= 0) {
            auto id_arg = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, id.value());
            if (id_arg.isErr()) return id_arg.error();
            auto known_value = callObject(uids.value(), "ResourceUID", "has_id", 1116898809LL,
                                          {&id_arg.value()});
            if (known_value.isErr()) return known_value.error();
            auto known = scalarFromVariant<GDExtensionBool>(known_value.value(),
                                                            GDEXTENSION_VARIANT_TYPE_BOOL);
            if (known.isErr()) return known.error();
            if (known.value()) {
                auto path_value = callObject(uids.value(), "ResourceUID", "get_id_path",
                                             844755477LL, {&id_arg.value()});
                if (path_value.isErr()) return path_value.error();
                auto path = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (path.isErr()) return path.error();
                if (!path.value().empty()) return path.value();
            }
        }
    }
    for (const auto& part : parts) {
        if (strings::startsWith(part, "res://")) return part;
    }
    return std::string();
}

// The res:// file the editor saves the bus layout to, from the value of
// audio/buses/default_bus_layout. 4.6.2 and 4.7.2 hold that setting as a uid
// once the file exists, and write it to project.godot that way on the next
// ProjectSettings.save(); 4.5.1 keeps the path. Measured by
// tools/vibe/probes/audio_bus_engine.py. A uid the engine cannot resolve is
// returned as it is, because it names no file.
std::string busLayoutPathFrom(const std::optional<std::string>& setting) {
    const std::string value = setting.value_or("res://default_bus_layout.tres");
    if (strings::startsWith(value, "uid://")) {
        auto resolved = dependencyResourcePath(value);
        if (resolved.isOk() && !resolved.value().empty()) return resolved.value();
    }
    return value;
}

// What to add to a bus answer when the project names a different layout file
// from the one the editor opened. The editor keeps saving to the one it opened
// until it restarts, and the next start loads the one the project names, so the
// buses it holds now are not what that start will see.
std::string busLayoutMovedSentence(const std::string& editor_path, const std::string& project_path) {
    if (editor_path == project_path) return std::string();
    return " audio/buses/default_bus_layout has changed since the editor opened the project: "
           "it now names " + project_path + ", which the editor loads when it next starts, "
           "and until then the editor keeps writing " + editor_path + ", the layout it opened. "
           "Restart the editor, or put the setting back, before relying on either file.";
}

// Whether a PackedScene file is there to read. ResourceLoader.get_dependencies
// prints an engine ERROR for a path it cannot open, so nothing below asks it
// about a file this has not seen first.
Result<bool> packedSceneExists(const std::string& scene_path) {
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    auto path_value = makeString(scene_path);
    auto hint = makeString("PackedScene");
    if (path_value.isErr()) return path_value.error();
    if (hint.isErr()) return hint.error();
    auto exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                   {&path_value.value(), &hint.value()});
    if (exists_value.isErr()) return exists_value.error();
    auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(),
                                                     GDEXTENSION_VARIANT_TYPE_BOOL);
    if (exists.isErr()) return exists.error();
    return exists.value() != 0;
}

// The scene files from `scene_path` down to the edited scene, or empty when
// the edited scene is not among its dependencies. Instancing and inheritance
// are both ext_resources of type PackedScene, so both kinds of cycle are
// found by one walk over the scene files each scene depends on. This is the
// check SceneTreeDock makes before it instances a dropped scene and
// EditorNode makes before it saves; EditorInterface.save_scene returns OK
// either way, so the refusal has to come before the tree holds the cycle (#590).
Result<std::vector<std::string>> instanceCycleChain(const std::string& scene_path,
                                                    const std::string& edited_path) {
    if (scene_path == edited_path) return std::vector<std::string>{scene_path};
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    auto is_scene_file = [](const std::string& path) {
        return strings::endsWith(path, ".tscn") || strings::endsWith(path, ".scn");
    };
    // A project has far fewer scenes than this; the bound is so a walk over
    // one that does not cannot run on.
    constexpr size_t kMaxScenesWalked = 4096;
    std::vector<std::vector<std::string>> pending{{scene_path}};
    std::set<std::string> visited{scene_path};
    while (!pending.empty()) {
        auto chain = std::move(pending.back());
        pending.pop_back();
        auto present = packedSceneExists(chain.back());
        if (present.isErr()) return present.error();
        if (!present.value()) continue;
        auto path_value = makeString(chain.back());
        if (path_value.isErr()) return path_value.error();
        auto dependencies = callObject(loader.value(), "ResourceLoader", "get_dependencies",
                                       3538744774LL, {&path_value.value()});
        if (dependencies.isErr()) return dependencies.error();
        auto size_value = callVariant(dependencies.value(), "size");
        if (size_value.isErr()) return size_value.error();
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        for (int64_t i = 0; i < size.value(); ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return index.error();
            auto entry_value = callVariant(dependencies.value(), "get", {&index.value()});
            if (entry_value.isErr()) return entry_value.error();
            auto entry = stringFromVariant(
                entry_value.value(), GodotApi::instance().variant_get_type(entry_value.value().ptr()));
            if (entry.isErr()) return entry.error();
            auto dependency = dependencyResourcePath(entry.value());
            if (dependency.isErr()) return dependency.error();
            if (dependency.value().empty() || !is_scene_file(dependency.value())) continue;
            if (dependency.value() == edited_path) {
                chain.push_back(dependency.value());
                return chain;
            }
            if (visited.size() >= kMaxScenesWalked) continue;
            if (!visited.insert(dependency.value()).second) continue;
            auto next = chain;
            next.push_back(dependency.value());
            pending.push_back(std::move(next));
        }
    }
    return std::vector<std::string>{};
}

std::optional<json> refuseInstanceCycle(GDExtensionObjectPtr root, const std::string& scene_path) {
    const auto edited = editedScenePath(root);
    // An unsaved scene is in no file's dependencies.
    if (edited.empty()) return std::nullopt;
    auto chain = instanceCycleChain(scene_path, edited);
    if (chain.isErr()) return errorJson(chain.error());
    if (chain.value().empty()) return std::nullopt;
    std::string detail;
    if (chain.value().size() == 1) {
        detail = scene_path + " is the edited scene.";
    } else if (chain.value().size() == 2) {
        detail = scene_path + " instances or inherits " + edited + ", the edited scene.";
    } else {
        detail = scene_path + " reaches " + edited + ", the edited scene, through";
        for (size_t i = 1; i + 1 < chain.value().size(); ++i) {
            detail += (i == 1 ? " " : ", ") + chain.value()[i];
        }
        detail += ".";
    }
    return bridgeError(409, "cyclic_instance",
                       {{"scene_path", scene_path}, {"edited_scene", edited}, {"chain", chain.value()}},
                       detail);
}

std::string resourcePathOf(GDExtensionObjectPtr resource);



// The checks a mutation runs on its target before it does anything, asked by
// the preview through scene.getProperty with a `mutation` block naming the
// tool and its arguments. A dry run that had opened the node had everything
// the real call uses to refuse it, and previewed it as planned anyway: the
// '..' rule was #571, and these are the ownership, cycle and base-type rules
// (#588, #589, #590, #603).
// Whether the engine has a class by this name, asked before constructing one.
// classdb_construct_object answers null for an unknown name, and first prints
// "Cannot get class" to the editor's log, which the refusal that follows cannot
// take back. The harness had been producing that line on every run for a
// request whose whole purpose is to be refused cleanly (vibe session
// seventeen). ClassDB.class_exists is 2619796661 on 4.5.1, 4.6.2 and 4.7.2.
// Anything that stops the question being asked answers true, which leaves the
// construction and its own refusal exactly as they were.
bool engineHasClass(const std::string& name) {
    if (requireMethodBind("ClassDB", "class_exists", 2619796661LL).isErr()) return true;
    auto class_db = singleton("ClassDB");
    if (class_db.isErr()) return true;
    auto class_name = makeStringName(name);
    if (class_name.isErr()) return true;
    auto answer = callObject(class_db.value(), "ClassDB", "class_exists", 2619796661LL,
                             {&class_name.value()});
    if (answer.isErr()) return true;
    auto known = scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    return known.isErr() || known.value() != 0;
}

// Whether ResourceLoader can see a file at this path, asked before loading it.
// A load of a path with nothing behind it prints three ERROR lines to the
// editor's log before it answers null, for a refusal the caller was going to
// get anyway. Anything that stops the question being asked answers true.
bool resourceFileExists(const std::string& path) {
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return true;
    auto path_value = makeString(path);
    auto hint = makeString("");
    if (path_value.isErr() || hint.isErr()) return true;
    auto answer = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                             {&path_value.value(), &hint.value()});
    if (answer.isErr()) return true;
    auto found = scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    return found.isErr() || found.value() != 0;
}

// The slot anim_add_library fills, which scene_set_property cannot write. On
// 4.5 it is one Dictionary, outside the scalar contract; from 4.6 it is one
// libraries/<name> property per library, which a player without that library
// does not have. So the same request was refused as a Dictionary on one engine
// and as a property that does not exist on the next -- which reads as "this
// player has no libraries" -- and the 4.5 dry run previewed it as a planned
// change. None of the three named the tool that works (vibe session
// seventeen). One refusal now, on every engine, and on the dry run too.
std::optional<json> refuseAnimationLibrarySlot(GDExtensionObjectPtr node, const std::string& property) {
    if (property != "libraries" && property.rfind("libraries/", 0) != 0) return std::nullopt;
    auto mixer = objectIsClass(node, "AnimationMixer");
    if (mixer.isErr() || !mixer.value()) return std::nullopt;
    return bridgeError(400, "animation_library_slot",
                       {{"property_name", property}, {"use_tool", "anim_add_library"}},
                       "Use anim_add_library: it takes the player, a res:// library file and the "
                       "name to hold it under, and registers the add with the editor's undo history.");
}

// The typed tool for an engine method scene_call_method will not call, when
// there is one. The refusal already said "the typed tools cover those" and named
// none, so a caller who reached for add_animation_library by hand was left to
// search the surface for the tool that does it.
std::string typedToolForEngineMethod(const std::string& method) {
    static const std::map<std::string, std::string> tools = {
        {"add_animation_library", "anim_add_library"},
        {"add_to_group", "scene_add_to_group"},
        {"connect", "signal_connect"},
        {"disconnect", "signal_disconnect"},
        {"emit_signal", "signal_emit"},
        {"queue_free", "scene_remove_node"},
        {"remove_from_group", "scene_remove_from_group"},
        {"reparent", "scene_reparent_node"},
        {"set_cell", "tilemap_set_cells"},
        {"set_cell_item", "gridmap_set_cells"},
        {"set_script", "script_attach_to_node"},
        {"set_shader_parameter", "shader_set_uniform"},
    };
    const auto found = tools.find(method);
    return found == tools.end() ? std::string() : found->second;
}


// What the class says about a property, as opposed to what it currently holds.
// An empty resource slot reads back as nil, so the value alone cannot say the
// slot is a TileSet, and a resource write has nothing to validate against
// without the declared class.


Result<GDExtensionObjectPtr> undoManager(GDExtensionObjectPtr editor) {
    auto result = callObject(editor, "EditorInterface", "get_editor_undo_redo", 3819628421LL);
    if (result.isErr()) return result.error();
    auto manager = objectFromVariant(result.value());
    if (manager.isErr()) return manager.error();
    if (!manager.value()) return Error::notConnected("EditorUndoRedoManager is unavailable");
    return manager.value();
}

Result<void> preflightUndoManagerBindings() {
    static const std::array<std::pair<const char*, int64_t>, 8> required = {{
        {"create_action", 796197507LL},
        {"commit_action", 3216645846LL},
        {"add_do_method", 1517810467LL},
        {"add_undo_method", 1517810467LL},
        {"add_do_property", 1017172818LL},
        {"add_undo_property", 1017172818LL},
        {"add_do_reference", 3975164845LL},
        {"add_undo_reference", 3975164845LL}
    }};
    for (const auto& [method, hash] : required) {
        auto available = requireMethodBind("EditorUndoRedoManager", method, hash);
        if (available.isErr()) return available;
    }
    return Result<void>::ok();
}

Result<void> preflightUndoRollbackBindings() {
    for (const auto& bind : {
             std::make_tuple("EditorUndoRedoManager", "get_object_history_id", 1107568780LL),
             std::make_tuple("EditorUndoRedoManager", "get_history_undo_redo", 2417974513LL),
             std::make_tuple("UndoRedo", "has_undo", 36873697LL),
             std::make_tuple("UndoRedo", "undo", 2240911060LL)}) {
        auto available = requireMethodBind(std::get<0>(bind), std::get<1>(bind),
                                           std::get<2>(bind));
        if (available.isErr()) return available;
    }
    return Result<void>::ok();
}

Result<void> preflightNodeTransactionBindings() {
    static const std::array<std::pair<const char*, int64_t>, 6> required = {{
        {"add_child", 3863233950LL},
        {"remove_child", 1078189570LL},
        {"set_owner", 1078189570LL},
        {"move_child", 3315886247LL},
        {"reparent", 3685795103LL},
        {"is_ancestor_of", 3093956946LL}
    }};
    for (const auto& [method, hash] : required) {
        auto available = requireMethodBind("Node", method, hash);
        if (available.isErr()) return available;
    }
    return Result<void>::ok();
}

Result<void> preflightNodeUndoTransaction() {
    auto manager = preflightUndoManagerBindings();
    if (manager.isErr()) return manager;
    return preflightNodeTransactionBindings();
}

Result<void> createAction(GDExtensionObjectPtr manager, const std::string& name,
                          GDExtensionObjectPtr context_object) {
    auto action_name = makeString(name);
    auto merge_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    auto context = makeObject(context_object);
    auto backward = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    auto unsaved = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
    if (action_name.isErr()) return action_name.error();
    if (merge_mode.isErr()) return merge_mode.error();
    if (context.isErr()) return context.error();
    if (backward.isErr()) return backward.error();
    if (unsaved.isErr()) return unsaved.error();
    auto call = callObject(manager, "EditorUndoRedoManager", "create_action", 796197507LL,
                           {&action_name.value(), &merge_mode.value(), &context.value(), &backward.value(), &unsaved.value()});
    return call.isOk() ? Result<void>::ok() : Result<void>(call.error());
}

Result<void> managerMethod(GDExtensionObjectPtr manager, const char* operation,
                           GDExtensionObjectPtr object, const std::string& method,
                           const std::vector<const VariantValue*>& method_args) {
    auto object_value = makeObject(object);
    auto method_value = makeStringName(method);
    if (object_value.isErr()) return object_value.error();
    if (method_value.isErr()) return method_value.error();
    std::vector<const VariantValue*> args{&object_value.value(), &method_value.value()};
    args.insert(args.end(), method_args.begin(), method_args.end());
    auto result = callObject(manager, "EditorUndoRedoManager", operation, 1517810467LL, args);
    return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
}

Result<void> managerReference(GDExtensionObjectPtr manager, const char* operation,
                              GDExtensionObjectPtr object) {
    auto object_value = makeObject(object);
    if (object_value.isErr()) return object_value.error();
    auto result = callObject(manager, "EditorUndoRedoManager", operation, 3975164845LL, {&object_value.value()});
    return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
}

Result<void> commitAction(GDExtensionObjectPtr manager) {
    auto execute = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
    if (execute.isErr()) return execute.error();
    auto result = callObject(manager, "EditorUndoRedoManager", "commit_action", 3216645846LL, {&execute.value()});
    return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
}

// The UndoRedo behind one of the manager's histories. GLOBAL_HISTORY is 0.
Result<GDExtensionObjectPtr> historyUndoRedo(GDExtensionObjectPtr manager, VariantValue& history_id) {
    auto history = callObject(manager, "EditorUndoRedoManager", "get_history_undo_redo",
                              2417974513LL, {&history_id});
    if (history.isErr()) return history.error();
    auto undo_redo = objectFromVariant(history.value());
    if (undo_redo.isErr()) return undo_redo.error();
    if (!undo_redo.value()) return Error::notFound("No UndoRedo history exists for the edited scene");
    return undo_redo.value();
}

Result<int64_t> undoRedoVersion(GDExtensionObjectPtr undo_redo) {
    auto version = callObject(undo_redo, "UndoRedo", "get_version", 3905245786LL);
    if (version.isErr()) return version.error();
    return scalarFromVariant<int64_t>(version.value(), GDEXTENSION_VARIANT_TYPE_INT);
}

// Every node of one class under a node, as find_children finds them.
Result<std::vector<GDExtensionObjectPtr>> descendantsOfClass(GDExtensionObjectPtr node,
                                                             const char* class_name,
                                                             bool recursive) {
    auto pattern = makeString("*");
    auto type = makeString(class_name);
    auto deep = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(recursive));
    auto owned = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (pattern.isErr() || type.isErr() || deep.isErr() || owned.isErr()) {
        return Error::internal("Failed to build find_children arguments");
    }
    auto found = callObject(node, "Node", "find_children", 2560337219LL,
                            {&pattern.value(), &type.value(), &deep.value(), &owned.value()});
    if (found.isErr()) return found.error();
    auto size_value = callVariant(found.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    std::vector<GDExtensionObjectPtr> nodes;
    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) return index.error();
        auto entry = callVariant(found.value(), "get", {&index.value()});
        if (entry.isErr()) return entry.error();
        auto object = objectFromVariant(entry.value());
        if (object.isOk() && object.value()) nodes.push_back(object.value());
    }
    return nodes;
}

// The editor's own Undo or Redo: the item in its title bar menu that a person
// clicks, and that Ctrl+Z runs.
//
// EditorUndoRedoManager keeps a stack of actions per history beside the
// UndoRedo it wraps, and only its own undo() and redo() move an action from
// one stack to the other. Neither is bound for extensions. Stepping the
// UndoRedo directly moves the version and leaves the stacks where they were,
// so the manager then prints "Inconsistent redo history" and answers that a
// scene undone past its save is saved: the answer scene_close reads before it
// decides whether a close needs discard_unsaved (#913). The menu item calls
// the manager's own undo() and redo() and keeps both true.
//
// The item is found by its shortcut's resource name, "Undo" or "Redo". That is
// the engine's untranslated name for ui_undo and ui_redo on 4.5.1, 4.6.2 and
// 4.7.2, where the item ids differ (12 on 4.5, 13 after), the label is
// translated, and the keys can be remapped.
struct EditorHistoryCommand {
    GDExtensionObjectPtr menu = nullptr;
    int64_t id = -1;
};

Result<EditorHistoryCommand> editorHistoryCommand(GDExtensionObjectPtr editor, bool undo) {
    const std::string wanted = undo ? "Undo" : "Redo";
    const auto missing = [&wanted] {
        return Error(501, "This editor has no " + wanted + " item in its title bar menu, so Didi "
                          "cannot " + (wanted == "Undo" ? "undo" : "redo") + " through the editor's "
                          "own history. Stepping the scene's UndoRedo directly would leave that "
                          "history inconsistent (#913).");
    };
    auto base_value = callObject(editor, "EditorInterface", "get_base_control", 2783021301LL);
    if (base_value.isErr()) return base_value.error();
    auto base = objectFromVariant(base_value.value());
    if (base.isErr()) return base.error();
    if (!base.value()) return missing();
    auto bars = descendantsOfClass(base.value(), "MenuBar", true);
    if (bars.isErr()) return bars.error();
    for (GDExtensionObjectPtr bar : bars.value()) {
        auto menus = descendantsOfClass(bar, "PopupMenu", false);
        if (menus.isErr()) return menus.error();
        for (GDExtensionObjectPtr menu : menus.value()) {
            auto count_value = callObject(menu, "PopupMenu", "get_item_count", 3905245786LL);
            if (count_value.isErr()) return count_value.error();
            auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (count.isErr()) return count.error();
            for (int64_t item = 0; item < count.value(); ++item) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, item);
                if (index.isErr()) return index.error();
                auto shortcut_value = callObject(menu, "PopupMenu", "get_item_shortcut", 1449483325LL,
                                                 {&index.value()});
                if (shortcut_value.isErr()) continue;
                auto shortcut = objectFromVariant(shortcut_value.value());
                if (shortcut.isErr() || !shortcut.value()) continue;
                auto name_value = callObject(shortcut.value(), "Resource", "get_name", 201670096LL);
                if (name_value.isErr()) continue;
                auto name = stringFromVariant(name_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (name.isErr() || name.value() != wanted) continue;
                auto id_value = callObject(menu, "PopupMenu", "get_item_id", 923996154LL, {&index.value()});
                if (id_value.isErr()) return id_value.error();
                auto id = scalarFromVariant<int64_t>(id_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                if (id.isErr()) return id.error();
                return EditorHistoryCommand{menu, id.value()};
            }
        }
    }
    return missing();
}


// Runs the editor's own Undo or Redo item, synchronously.
Result<void> pressEditorHistoryItem(GDExtensionObjectPtr editor, bool undo) {
    auto command = editorHistoryCommand(editor, undo);
    if (command.isErr()) return command.error();
    auto signal = makeStringName("id_pressed");
    auto id = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, command.value().id);
    if (signal.isErr() || id.isErr()) return Error::internal("Failed to build the menu command");
    auto emitted = callObject(command.value().menu, "Object", "emit_signal", 4047867050LL,
                              {&signal.value(), &id.value()});
    if (emitted.isErr()) return emitted.error();
    auto emit_code = scalarFromVariant<int64_t>(emitted.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (emit_code.isErr()) return emit_code.error();
    if (emit_code.value() != 0) {
        return Error(501, std::string("The editor's ") + (undo ? "Undo" : "Redo") +
                              " item did not run: " +
                              ::didi::godot::describeGodotError(emit_code.value()));
    }
    return Result<void>::ok();
}

Result<HistoryMoved> runEditorHistoryCommand(GDExtensionObjectPtr editor,
                                             GDExtensionObjectPtr manager,
                                             GDExtensionObjectPtr scene_undo_redo, bool undo) {
    // Found before anything is read, so an editor with no such item refuses
    // without having touched a history.
    auto command = editorHistoryCommand(editor, undo);
    if (command.isErr()) return command.error();
    auto global_id = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (global_id.isErr()) return global_id.error();
    auto global = historyUndoRedo(manager, global_id.value());
    const GDExtensionObjectPtr global_undo_redo = global.isOk() ? global.value() : nullptr;
    auto scene_before = undoRedoVersion(scene_undo_redo);
    if (scene_before.isErr()) return scene_before.error();
    auto global_before = global_undo_redo ? undoRedoVersion(global_undo_redo) : Result<int64_t>(int64_t{0});
    if (global_before.isErr()) return global_before.error();

    auto pressed = pressEditorHistoryItem(editor, undo);
    if (pressed.isErr()) return pressed.error();

    auto scene_after = undoRedoVersion(scene_undo_redo);
    if (scene_after.isErr()) return scene_after.error();
    if (scene_after.value() != scene_before.value()) return HistoryMoved::Scene;
    if (global_undo_redo) {
        auto global_after = undoRedoVersion(global_undo_redo);
        if (global_after.isOk() && global_after.value() != global_before.value()) {
            return HistoryMoved::Global;
        }
    }
    return HistoryMoved::None;
}

// Undoes the action just committed for the edited scene. Used when a
// postcondition fails after the commit, so the tool does not report failure
// while the scene and the undo stack already carry the change.
//
// Through the editor's own Undo, so the manager's history stays true (#913).
// The editor's command undoes the newer of the scene's history and the global
// one, and the action just committed is the newest there is, so it is almost
// always the one taken. Which history moved says whether it was (#1152). When
// the command took the global history's action instead, which two actions in
// one tick of the clock it compares can do, the editor's own Redo puts that
// back, and the scene is put back directly. So it is too when the editor
// cannot be asked or moves nothing, as it does while a mouse button is held. A
// scene still holding a change the tool is about to call failed is worse than
// a history the editor calls inconsistent.
Result<void> undoLastAction(GDExtensionObjectPtr manager, GDExtensionObjectPtr root) {
    auto root_value = makeObject(root);
    if (root_value.isErr()) return root_value.error();
    auto history_id = callObject(manager, "EditorUndoRedoManager", "get_object_history_id",
                                 1107568780LL, {&root_value.value()});
    if (history_id.isErr()) return history_id.error();
    auto undo_redo = historyUndoRedo(manager, history_id.value());
    if (undo_redo.isErr()) return undo_redo.error();
    const auto has_undo = [](GDExtensionObjectPtr history) -> Result<bool> {
        auto available = callObject(history, "UndoRedo", "has_undo", 36873697LL);
        if (available.isErr()) return available.error();
        auto flag = scalarFromVariant<GDExtensionBool>(available.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (flag.isErr()) return flag.error();
        return flag.value() != 0;
    };
    auto scene_has = has_undo(undo_redo.value());
    if (scene_has.isErr()) return scene_has.error();
    if (!scene_has.value()) return Error(409, "Nothing to undo", {{"code", "nothing_to_undo"}});
    auto editor = editorInterface();
    if (editor.isOk()) {
        auto moved = runEditorHistoryCommand(editor.value(), manager, undo_redo.value(), true);
        if (moved.isOk() && moved.value() == HistoryMoved::Scene) return Result<void>::ok();
        if (moved.isOk() && moved.value() == HistoryMoved::Global) {
            // Our commit emptied the scene's redo stack, so the editor's Redo
            // takes the global action it just undid.
            auto restored = runEditorHistoryCommand(editor.value(), manager, undo_redo.value(), false);
            if (restored.isErr() || restored.value() != HistoryMoved::Global) {
                return Error::internal("The editor's Undo took an action from the global history in "
                                       "place of the one just committed, and its Redo did not put "
                                       "that back");
            }
        }
    }
    auto executed = callObject(undo_redo.value(), "UndoRedo", "undo", 2240911060LL);
    return executed.isOk() ? Result<void>::ok() : Result<void>(executed.error());
}

// Closes an action that was opened but could not be fully registered.
// EditorUndoRedoManager has no cancel, so the least bad move is to commit
// without executing: the manager stops being mid-action, nothing half
// registered is applied to the scene, and the next mutation cannot merge into
// our abandoned action or fail outright because one is still open.
void abandonAction(GDExtensionObjectPtr manager) {
    auto execute = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (execute.isErr()) return;
    (void)callObject(manager, "EditorUndoRedoManager", "commit_action", 3216645846LL,
                     {&execute.value()});
}

// One of the editor's histories as the change journal reads it (Q15). The
// manager answers null for a history it does not have, which is what a closed
// scene's history becomes, and prints an engine error saying so, so only the
// global history, which exists from the moment the plugin loads, and the
// edited scene's are ever asked for. get_history_count, get_current_action,
// get_action_name and get_current_action_name carry the same hashes on 4.5.1,
// 4.6.2 and 4.7.2.
Result<GDExtensionObjectPtr> undoRedoForHistory(GDExtensionObjectPtr manager, int64_t id) {
    auto id_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, id);
    if (id_value.isErr()) return id_value.error();
    return historyUndoRedo(manager, id_value.value());
}

Result<UndoHistoryState> readUndoHistory(GDExtensionObjectPtr manager, int64_t id,
                                         const std::string& scene_path) {
    auto undo_redo = undoRedoForHistory(manager, id);
    if (undo_redo.isErr()) return undo_redo.error();
    const auto integer = [&undo_redo](const char* method, int64_t hash) -> Result<int64_t> {
        auto value = callObject(undo_redo.value(), "UndoRedo", method, hash);
        if (value.isErr()) return value.error();
        return scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    UndoHistoryState state;
    state.id = id;
    state.scene_path = id == kGlobalUndoHistory ? std::string() : scene_path;
    auto version = integer("get_version", 3905245786LL);
    auto current = integer("get_current_action", 2455072627LL);
    auto count = integer("get_history_count", 2455072627LL);
    if (version.isErr()) return version.error();
    if (current.isErr()) return current.error();
    if (count.isErr()) return count.error();
    state.version = version.value();
    state.current = current.value();
    state.count = count.value();
    if (state.current >= 0) {
        auto name = callObject(undo_redo.value(), "UndoRedo", "get_current_action_name", 201670096LL);
        if (name.isErr()) return name.error();
        auto text = stringFromVariant(name.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (text.isErr()) return text.error();
        state.action = text.value();
    }
    return state;
}

// get_action_name for one index of a history, empty when it cannot be read.
std::string undoActionName(GDExtensionObjectPtr manager, int64_t id, int64_t index) {
    auto undo_redo = undoRedoForHistory(manager, id);
    if (undo_redo.isErr()) return {};
    auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
    if (index_value.isErr()) return {};
    auto name = callObject(undo_redo.value(), "UndoRedo", "get_action_name", 990163283LL,
                           {&index_value.value()});
    if (name.isErr()) return {};
    auto text = stringFromVariant(name.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    return text.isErr() ? std::string() : text.value();
}

// The commits this editor run has seen, for telling which of two histories
// holds the newer action. Read and written only on the main thread.
UndoLedger& undoLedger() {
    static UndoLedger ledger([] {
        std::random_device device;
        std::ostringstream run;
        run << std::hex << std::setfill('0') << std::setw(8) << device() << std::setw(8)
            << device();
        return run.str();
    }());
    return ledger;
}

// See the header for why the tolerance is sized to a float32 round trip.
bool jsonValuesEquivalentImpl(const json& observed, const json& requested) {
    if (observed.is_number() && requested.is_number()) {
        const double a = observed.get<double>();
        const double b = requested.get<double>();
        if (!std::isfinite(a) || !std::isfinite(b)) return observed == requested;
        const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
        // A shader float, a Color channel and a Vector component are all 32 bit
        // in a standard build, which carries about seven decimal digits.
        return std::fabs(a - b) <= 1e-6 * scale;
    }
    if (observed.is_object() && requested.is_object()) {
        if (observed.size() != requested.size()) return false;
        for (auto it = requested.begin(); it != requested.end(); ++it) {
            const auto found = observed.find(it.key());
            if (found == observed.end()) return false;
            if (!jsonValuesEquivalentImpl(*found, it.value())) return false;
        }
        return true;
    }
    if (observed.is_array() && requested.is_array()) {
        if (observed.size() != requested.size()) return false;
        for (size_t index = 0; index < observed.size(); ++index) {
            if (!jsonValuesEquivalentImpl(observed[index], requested[index])) return false;
        }
        return true;
    }
    return observed == requested;
}

json liveResult(const json& fields) {
    json result = fields;
    result["execution_mode"] = "live";
    result["is_live_engine"] = true;
    return result;
}

// Parses "min,max[,step][,flags...]". Anything that does not start with two
// numbers is not a range this can act on, and is passed over rather than
// guessed at.
std::optional<ShaderHintRange> parseShaderHintRangeImpl(const std::string& hint_string) {
    std::vector<std::string> fields;
    std::string current;
    for (const char character : hint_string) {
        if (character == ',') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    fields.push_back(current);

    const auto number = [](const std::string& text) -> std::optional<double> {
        std::string trimmed = text;
        const auto first = trimmed.find_first_not_of(" \t");
        if (first == std::string::npos) return std::nullopt;
        const auto last = trimmed.find_last_not_of(" \t");
        trimmed = trimmed.substr(first, last - first + 1);
        if (trimmed.empty()) return std::nullopt;
        try {
            size_t consumed = 0;
            const double parsed = std::stod(trimmed, &consumed);
            if (consumed != trimmed.size()) return std::nullopt;
            return parsed;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    if (fields.size() < 2) return std::nullopt;
    const auto minimum = number(fields[0]);
    const auto maximum = number(fields[1]);
    if (!minimum.has_value() || !maximum.has_value()) return std::nullopt;

    ShaderHintRange range;
    range.minimum = *minimum;
    range.maximum = *maximum;
    size_t flags_from = 2;
    if (fields.size() > 2) {
        if (const auto step = number(fields[2])) {
            range.step = *step;
            flags_from = 3;
        }
    }
    for (size_t index = flags_from; index < fields.size(); ++index) {
        if (fields[index] == "or_greater") range.or_greater = true;
        if (fields[index] == "or_less") range.or_less = true;
    }
    return range;
}

}  // namespace

bool jsonValuesEquivalent(const json& observed, const json& requested) {
    return jsonValuesEquivalentImpl(observed, requested);
}

// An account of a write that did not land, built only out of what this call
// saw.
//
// `applied: false` used to be the whole answer, and it covers two different
// things. Measured on 4.5.1, 4.6.2 and 4.7.2, which agree line for line:
//
//   Timer.wait_time      = -3.0   -> still 1.0     the property did not move
//   AudioStreamPlayer.bus = "Music" -> still "Master"  the property did not move
//   Control.anchors_preset = 15   -> still 0       the property did not move
//   ProgressBar.value    = 999.0  -> 100.0         Godot stored a third value
//
// The last row is the one the old shape hid. `value` comes back holding 100,
// which reads as a plausible result, and the caller had nothing saying the
// engine had substituted it. So the outcome is reported as two cases the server
// can actually tell apart by looking: unchanged, and replaced.
//
// Deliberately not reported: *why*. The first three rows are three different
// failures with three different remedies, and they are identical in everything
// this call can observe. The engine printed "Time should be greater than zero"
// for the first, to its own error stream, and there is no route from a
// GDExtension to that text. Guessing between them would put a confident wrong
// reason where there is now an honest short one.
//
// What the engine will say is what it declares about the property, which is a
// fact rather than a diagnosis. Measured through this call against a live
// 4.5.1 editor, all three of the unchanged rows carry one:
//
//   wait_time       range  "0.001,4096,0.001,or_greater,exp,suffix:s"
//   bus             enum   "Master"
//   anchors_preset  enum   "Custom:-1,Full Rect:15,Top Left:0,..."
//
// The first two are the remedy: 0.001 is the minimum the error stream was
// talking about, and the bus enum lists the buses that exist, which is how a
// caller learns there is no Music. The third is the opposite and is relayed
// anyway -- the enum contains 15, so it says the value was fine and the cause
// is elsewhere, which is the reason this field is not presented as the reason.
//
// The bus enum is filled in by AudioStreamPlayer under is_editor_hint, so it
// is empty in a bare SceneTree and populated here. Reading a hint outside the
// editor and concluding the field is useless is a mistake this comment exists
// to stop someone repeating.
//
// Only range and enum hints are relayed. They are the hints whose hint_string
// is a statement about which values the property takes; the rest are editor
// affordances (multiline, password, colour-no-alpha) or type declarations this
// tool already validates against. Their numbers have been 1, 2 and 3 since
// Godot 4.0 and are unchanged across every supported line, checked against
// --dump-extension-api on 4.5.1 and 4.7.2.
json notAppliedReport(const json& observed, const json& old_value, int hint,
                      const std::string& hint_string) {
    json report;
    if (jsonValuesEquivalent(observed, old_value)) {
        report["outcome"] = "unchanged";
        report["detail"] =
            "The commit ran and the property still holds the value it held before. Godot "
            "did not store the requested value, and reported nothing to this call about "
            "why. The usual causes are a sibling property that owns the write, a value "
            "naming something that does not exist, and a value outside what the property "
            "takes.";
    } else {
        report["outcome"] = "replaced";
        report["detail"] =
            "The commit ran and Godot stored a value of its own, which is neither the old "
            "value nor the requested one. value is that substitute, not a write that "
            "landed.";
    }

    const char* kind = nullptr;
    switch (hint) {
        case 1: kind = "range"; break;
        case 2: kind = "enum"; break;
        case 3: kind = "enum_suggestion"; break;
        default: break;
    }
    if (kind != nullptr && !hint_string.empty()) {
        // What the engine declares, not a verdict on this write. A constraint
        // the requested value satisfies is still worth handing back: it is the
        // caller's evidence that the value was not the problem.
        report["engine_constraint"] = {{"kind", kind}, {"hint_string", hint_string}};
    }
    return report;
}

ShaderCompileProbe shaderCompileProbe(const std::string& code, const std::string& shader_path) {
    ShaderCompileProbe probe;
    probe.uniform = "didi_compile_probe";
    while (code.find(probe.uniform) != std::string::npos) probe.uniform += '_';

    // The folder Godot resolves a relative include against, String::get_base_dir
    // of the shader's path. A shader embedded in a scene carries the scene's
    // path, so its includes resolve against the scene's folder.
    std::string folder;
    if (const auto scheme = shader_path.find("://"); scheme != std::string::npos) {
        const auto rest = shader_path.substr(scheme + 3);
        const auto slash = rest.find_last_of("/\\");
        folder = shader_path.substr(0, scheme + 3) +
                 (slash == std::string::npos ? std::string() : rest.substr(0, slash));
    }
    // String::is_absolute_path.
    const auto absolute = [](const std::string& path) {
        return path.front() == '/' || path.front() == '\\' || path.find(":/") != std::string::npos ||
               path.find(":\\") != std::string::npos;
    };
    std::string out;
    out.reserve(code.size() + 64);
    size_t start = 0;
    while (true) {
        const auto end = code.find('\n', start);
        auto line = code.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!folder.empty() && strings::startsWith(strings::trim(line), "#include")) {
            const auto open = line.find('"');
            const auto close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
            if (close != std::string::npos && close > open + 1) {
                const auto path = line.substr(open + 1, close - open - 1);
                if (!absolute(path)) {
                    line = line.substr(0, open + 1) + folder + (folder.back() == '/' ? "" : "/") + path +
                           line.substr(close);
                }
            }
        }
        out += line;
        if (end == std::string::npos) break;
        out += '\n';
        start = end + 1;
    }
    probe.code = out + "\nuniform float " + probe.uniform + ";\n";
    return probe;
}

std::optional<ShaderHintRange> parseShaderHintRange(const std::string& hint_string) {
    return parseShaderHintRangeImpl(hint_string);
}

ResourceTypeHint parseResourceTypeHint(const std::string& declared_type) {
    return parseResourceTypeHintImpl(declared_type);
}

ResourceTypeVerdict resourceTypeVerdict(const ResourceTypeHint& hint,
                                        const std::string& resource_class,
                                        const std::function<bool(const std::string&)>& inherits) {
    return resourceTypeVerdictImpl(hint, resource_class, inherits);
}

std::string describeResourceTypeRefusal(const std::string& property_name,
                                        const ResourceTypeHint& hint,
                                        const std::string& resource_path,
                                        const std::string& resource_class,
                                        ResourceTypeVerdict verdict) {
    return describeResourceTypeRefusalImpl(property_name, hint, resource_path, resource_class,
                                           verdict);
}

namespace bridge_internal {

// A committed edited-scene mutation lands in the editor's open scene and its
// undo history, and nowhere else. Every reply reported plain success, so an
// agent that mutated, read its change back and handed off left a scene one
// "don't save" away from never having happened, and nothing said so;
// undo_redo_registered: true read as reassurance that the change was real
// rather than as the warning that it was unsaved (#557). The offline setting
// writer set the precedent with its limitation paragraph for the mode where
// this matters less.
// The server turns scene_saved: false into a follow_up step naming
// editor_save_scene (src/mcp/follow_ups.cpp, Q6).
json liveSceneMutation(json fields) {
    fields["scene_saved"] = false;
    fields["limitation"] =
        "This change is in the editor's open scene and its undo history, not on disk. Call "
        "editor_save_scene to persist it; closing the editor without saving discards it.";
    return liveResult(fields);
}

// --- The typed object layer (Q7) --------------------------------------------
//
// scene_get_property and scene_set_property reach a property by path, in the
// grammar Godot's own get_indexed and set_indexed take, and take a batch. What
// can be decided from strings is in property_paths.cpp; this is the half that
// asks the engine.

// A bridgeError, carried as an Error so it can travel through a Result.
Error bridgeRefusal(int code, const std::string& identifier, json data, const std::string& detail) {
    auto composed = bridgeError(code, identifier, std::move(data), detail);
    return Error(code, composed["error"]["message"].get<std::string>(), composed["error"]["data"]);
}

// The other direction, for the refusal helpers that answer with a response.
Error errorOfResponse(const json& response) {
    const auto& error = response["error"];
    return Error(error.value("code", 500), error.value("message", std::string()),
                 error.contains("data") ? error["data"] : json());
}


// The names an object shows in the inspector, which are the ones worth
// offering a caller whose step named nothing. Groups and categories are rows
// in the same list and are not properties.

std::string resourcePathOf(GDExtensionObjectPtr resource) {
    auto name = makeStringName("resource_path");
    if (name.isErr()) return {};
    auto value = callObject(resource, "Object", "get", 2760726917LL, {&name.value()});
    if (value.isErr()) return {};
    auto text = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    return text.isOk() ? text.value() : std::string();
}

// Whether EditorInterface.get_unsaved_scenes exists on this engine, asked once.
//
// classdb_get_method_bind prints an ERROR when it answers null, and this was
// asked on every call, so every didi_control_room read on 4.5 and 4.6 put a
// red line in the editor's log and in the stream runtime_watch_invariants
// reads for engine errors (#600). ClassDB.class_has_method answers the same
// question without printing, and the answer cannot change for the life of the
// process, so it is asked once and the bind is only looked up where it exists.
// ClassDB.class_has_method is 3860701026 on 4.5.1, 4.6.2 and 4.7.2.
bool unsavedScenesBindAvailable() {
    static std::optional<bool> cached;
    if (cached.has_value()) return *cached;
    auto class_db = singleton("ClassDB");
    if (class_db.isErr()) return false;
    auto class_name = makeStringName("EditorInterface");
    auto method_name = makeStringName("get_unsaved_scenes");
    auto no_inheritance = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (class_name.isErr() || method_name.isErr() || no_inheritance.isErr()) return false;
    auto answer = callObject(class_db.value(), "ClassDB", "class_has_method", 3860701026LL,
                             {&class_name.value(), &method_name.value(), &no_inheritance.value()});
    if (answer.isErr()) return false;
    auto declared = scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (declared.isErr()) return false;
    const bool available = declared.value() != 0 &&
                           requireMethodBind("EditorInterface", "get_unsaved_scenes", 1139954409LL).isOk();
    cached = available;
    return available;
}

// The editor's own list of scenes with unsaved changes. The read side of dirty
// state, EditorInterface.get_unsaved_scenes, binds from Godot 4.7; 4.5 and 4.6
// expose only the write side, so on those engines the answer is null with
// readable false rather than an empty list that would read as clean (#557).
// Same probe scene.close uses, so the two cannot disagree about which engines
// can answer.
json unsavedScenesReport(GDExtensionObjectPtr editor) {
    json report = {{"unsaved_scenes_readable", false}, {"unsaved_scenes", nullptr}};
    if (!unsavedScenesBindAvailable()) {
        return report;
    }
    auto unsaved = callObject(editor, "EditorInterface", "get_unsaved_scenes", 1139954409LL);
    if (unsaved.isErr()) return report;
    auto unsaved_size = callVariant(unsaved.value(), "size");
    if (unsaved_size.isErr()) return report;
    auto unsaved_count = scalarFromVariant<int64_t>(unsaved_size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (unsaved_count.isErr()) return report;
    // One entry per open scene, so this bound sits far above anything an
    // editor produces; a report that had to stop early would be a lie.
    constexpr int64_t kMaxUnsavedScenesReported = 1024;
    if (unsaved_count.value() > kMaxUnsavedScenesReported) return report;
    json paths = json::array();
    for (int64_t index = 0; index < unsaved_count.value(); ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return report;
        auto entry = callVariant(unsaved.value(), "get", {&index_value.value()});
        if (entry.isErr()) return report;
        auto entry_path = stringFromVariant(
            entry.value(), GodotApi::instance().variant_get_type(entry.value().ptr()));
        if (entry_path.isErr()) return report;
        paths.push_back(entry_path.value());
    }
    report["unsaved_scenes_readable"] = true;
    report["unsaved_scenes"] = std::move(paths);
    return report;
}

// The res:// path of every scene open in an editor tab. The list has one entry
// per tab, so the same bound as the unsaved list applies.
// EditorInterface.get_open_scenes is 1139954409 on 4.5.1, 4.6.2 and 4.7.2.
Result<std::vector<std::string>> openScenePaths(GDExtensionObjectPtr editor) {
    auto listed = callObject(editor, "EditorInterface", "get_open_scenes", 1139954409LL);
    if (listed.isErr()) return listed.error();
    auto size = callVariant(listed.value(), "size");
    if (size.isErr()) return size.error();
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return count.error();
    constexpr int64_t kMaxOpenScenesRead = 1024;
    if (count.value() > kMaxOpenScenesRead) {
        return Error(409, "The editor has more scenes open than Didi will read (" +
                              std::to_string(count.value()) + ")",
                     {{"code", "too_many_open_scenes"}, {"open_scenes", count.value()},
                      {"most_read", kMaxOpenScenesRead}});
    }
    std::vector<std::string> paths;
    for (int64_t index = 0; index < count.value(); ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto entry = callVariant(listed.value(), "get", {&index_value.value()});
        if (entry.isErr()) return entry.error();
        auto path = stringFromVariant(entry.value(),
                                      GodotApi::instance().variant_get_type(entry.value().ptr()));
        if (path.isErr()) return path.error();
        paths.push_back(path.value());
    }
    return paths;
}

// The instance id of the root of the tab that holds path, or 0 when no tab
// does. EditorInterface.get_open_scene_roots is 3995934104 on all three lines.
Result<uint64_t> openSceneRootId(GDExtensionObjectPtr editor, const std::string& path) {
    auto roots = callObject(editor, "EditorInterface", "get_open_scene_roots", 3995934104LL);
    if (roots.isErr()) return roots.error();
    auto size = callVariant(roots.value(), "size");
    if (size.isErr()) return size.error();
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return count.error();
    for (int64_t index = 0; index < count.value(); ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto entry = callVariant(roots.value(), "get", {&index_value.value()});
        if (entry.isErr()) return entry.error();
        auto root = objectFromVariant(entry.value());
        if (root.isErr()) return root.error();
        if (!root.value()) continue;
        auto root_path = nodeString(root.value(), "get_scene_file_path", 201670096LL);
        if (root_path.isErr()) return root_path.error();
        if (root_path.value() == path) {
            return static_cast<uint64_t>(GodotApi::instance().object_get_instance_id(root.value()));
        }
    }
    return uint64_t{0};
}

// Brings the editor tab that holds a rewritten scene in line with the file.
//
// An open tab is a node tree the editor built from the file, not a cached
// PackedScene, so a cache refresh never reaches it, and the next save wrote
// the tree from before the write back over the file (#1068).
// EditorInterface.reload_scene_from_path rebuilds the tab from disk: measured
// on 4.5.1, 4.6.2 and 4.7.2, it returns with a new root holding the new
// contents and drops the tab's unsaved changes and its undo history. That last
// part is why a tab that has unsaved changes, or on 4.5 and 4.6 one that
// cannot be shown not to, is reloaded only when the caller said to discard
// them.
//
// On 4.7 the reload keeps whichever tab was current. On 4.5 and 4.6 it ends
// with TabBar.set_current_tab(previous index), which does nothing when the bar
// is already there, so a rebuilt tab left of a current last tab stays the
// edited scene (#1072). The switch back cannot happen in this frame, because
// the editor ignores open requests while it changes scenes, so the answer
// names the scene it moved from and the server opens it on a later one.
//
// Two more facts decide how it is called. On 4.5 and 4.6 a second reload in
// the same frame does nothing and says nothing, because the editor is still
// switching scenes from the first, so one tab is reloaded per request and the
// rest are answered pending; the server asks again. And on those lines a
// reload of a path no tab holds clears the current scene's undo history, so it
// is only ever asked for a path get_open_scenes lists. Whether it took is read
// from the root: a reload that happened replaced it.

} // namespace

// The admission rule the property tools apply, reading the JSON alone. It is
// the same call the bridge makes, so a test that asserts it here asserts what
// a live editor session will answer.
bool jsonValueFitsPropertyType(const json& value, int variant_type) {
    return propertyTypeAcceptsJson(value, static_cast<GDExtensionVariantType>(variant_type));
}

GodotBridge& GodotBridge::instance() {
    static GodotBridge bridge;
    return bridge;
}

Result<ReimportBatch> GodotBridge::resolveReimportPaths(
    const std::vector<std::string>& paths) {
    namespace fs = std::filesystem;
    if (paths.empty() || paths.size() > 256) {
        return Error::invalidArgument("paths must contain 1 to 256 source assets");
    }
    auto project_path = resolveGodotProjectPath();
    if (project_path.isErr()) return project_path.error();
    std::error_code ec;
    const auto root = fs::weakly_canonical(
        didi::paths::projectPathFromUtf8(project_path.value()), ec);
    if (ec || !fs::is_directory(root, ec)) return Error::internal("Godot project root is unavailable");

    std::set<std::string> unique;
    ReimportBatch batch;
    batch.paths.reserve(paths.size());
    for (const auto& path : paths) {
        if (path.size() < 7 || path.size() > 1024 || path.find('\0') != std::string::npos ||
            strings::startsWith(path, "res://.godot/") || strings::endsWith(path, ".import")) {
            return Error::invalidArgument("Every reimport path must identify a project source asset");
        }
        auto valid = validateResPath(path, "");
        if (valid.isErr()) return valid.error();
        const auto relative = didi::paths::projectPathFromUtf8(path.substr(6));
        const auto candidate = fs::canonical(root / relative, ec);
        if (ec || !pathWithin(root, candidate) || !fs::is_regular_file(candidate, ec) ||
            fs::is_symlink(fs::symlink_status(root / relative, ec))) {
            return Error::invalidArgument("Reimport path must be an existing regular file beneath the project root: " + path);
        }
        auto relative_canonical = fs::relative(candidate, root, ec);
        if (ec) return Error::invalidArgument("Unable to normalize reimport path: " + path);
        const auto resource_path = "res://" + didi::paths::projectPathToUtf8(relative_canonical);
        if (!unique.insert(resource_path).second) {
            return Error::invalidArgument("Reimport paths must be unique after normalization");
        }
        // Godot's import system owns exactly the files that carry a .import
        // sidecar. reimport_files reads the importer name out of that file, so
        // a path without one reaches the engine as "importer for type '' not
        // found" printed to the editor output while this tool reported success.
        // EditorFileSystem.update_file is the documented call for the rest:
        // "can be used to update text files saved by an external program".
        auto sidecar = candidate;
        sidecar += ".import";
        std::error_code sidecar_error;
        const bool imported = fs::is_regular_file(sidecar, sidecar_error) && !sidecar_error;
        batch.paths.push_back(resource_path);
        (imported ? batch.reimported : batch.refreshed).push_back(resource_path);
        if (!imported) batch.needs_scan = true;
    }

    return batch;
}

bool GodotBridge::assetIsImported(const std::string& resource_path) {
    namespace fs = std::filesystem;
    if (!strings::startsWith(resource_path, "res://")) return false;
    auto project_path = resolveGodotProjectPath();
    if (project_path.isErr()) return false;
    std::error_code ec;
    const auto root = fs::weakly_canonical(
        didi::paths::projectPathFromUtf8(project_path.value()), ec);
    if (ec) return false;
    auto sidecar = root / didi::paths::projectPathFromUtf8(resource_path.substr(6));
    sidecar += ".import";
    std::error_code sidecar_error;
    return fs::is_regular_file(sidecar, sidecar_error) && !sidecar_error;
}

// Godot writes the .import sidecar whether or not the import worked, and marks
// a failure with valid=false under [remap]. A sidecar on disk therefore said
// nothing about success, and asset_reimport listed a PNG whose import the
// engine had just refused under `imported`. The harness's own fixture was such
// a PNG -- every chunk CRC wrong -- and passed for as long as it existed,
// because the engine's "Error importing" line went to a log nothing read (vibe
// session seventeen). `valid` is read the way the engine reads it, through the
// same conversion the offline audit uses (#853).
bool GodotBridge::assetImportFailed(const std::string& resource_path) {
    namespace fs = std::filesystem;
    if (!strings::startsWith(resource_path, "res://")) return false;
    auto project_path = resolveGodotProjectPath();
    if (project_path.isErr()) return false;
    std::error_code ec;
    const auto root = fs::weakly_canonical(
        didi::paths::projectPathFromUtf8(project_path.value()), ec);
    if (ec) return false;
    auto sidecar = root / didi::paths::projectPathFromUtf8(resource_path.substr(6));
    sidecar += ".import";
    std::ifstream file(sidecar, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream contents;
    contents << file.rdbuf();
    for (const auto& entry : didi::config_file::scan(contents.str()).entries) {
        if (entry.section == "remap" && entry.key == "valid") {
            return !didi::config_file::booleanize(entry.value_text);
        }
    }
    return false;
}

std::vector<std::string> GodotBridge::unindexedAssets(const std::vector<std::string>& resource_paths) {
    std::vector<std::string> missing;
    auto editor = editorInterface();
    if (editor.isErr()) return resource_paths;
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return resource_paths;
    auto filesystem_object = objectFromVariant(filesystem.value());
    if (filesystem_object.isErr() || !filesystem_object.value()) return resource_paths;
    for (const auto& resource_path : resource_paths) {
        const auto slash = resource_path.find_last_of('/');
        bool listed = false;
        if (slash != std::string::npos && slash + 1 < resource_path.size()) {
            auto directory_value = makeString(resource_path.substr(0, slash + 1));
            auto file_value = makeString(resource_path.substr(slash + 1));
            if (directory_value.isOk() && file_value.isOk()) {
                auto found = callObject(filesystem_object.value(), "EditorFileSystem", "get_filesystem_path",
                                        3188521125LL, {&directory_value.value()});
                auto directory_object = found.isOk() ? objectFromVariant(found.value())
                                                     : Result<GDExtensionObjectPtr>(found.error());
                if (directory_object.isOk() && directory_object.value()) {
                    auto index = callObject(directory_object.value(), "EditorFileSystemDirectory",
                                            "find_file_index", 1321353865LL, {&file_value.value()});
                    auto position = index.isOk()
                                        ? scalarFromVariant<int64_t>(index.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                        : Result<int64_t>(index.error());
                    listed = position.isOk() && position.value() >= 0;
                }
            }
        }
        if (!listed) missing.push_back(resource_path);
    }
    return missing;
}

namespace bridge_internal {

// The editor's ProgressDialog, found once. On 4.5.1, 4.6.2 and 4.7.2 it is a
// Control and a direct child of the root window whether or not a task is open,
// and visible exactly while one is (tools/vibe/probes/scan_reimport_engine.py,
// the burst part). Main thread only, like every caller.
GDObjectInstanceID g_progress_dialog_id = 0;
bool g_progress_dialog_missing_reported = false;

} // namespace

bool GodotBridge::editorProgressOpen() {
    auto& api = GodotApi::instance();
    if (!api.object_get_instance_from_id || !api.object_get_instance_id) return false;
    GDExtensionObjectPtr dialog = g_progress_dialog_id != 0
                                      ? api.object_get_instance_from_id(g_progress_dialog_id)
                                      : nullptr;
    if (!dialog) {
        g_progress_dialog_id = 0;
        auto tree = liveSceneTree();
        if (tree.isErr()) return false;
        auto root = liveSceneTreeRoot(tree.value());
        if (root.isErr() || !root.value()) return false;
        auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
        if (include_internal.isErr()) return false;
        auto children = callObject(root.value(), "Node", "get_children", 873284517LL, {&include_internal.value()});
        if (children.isErr()) return false;
        auto size_value = callVariant(children.value(), "size");
        auto size = size_value.isOk() ? scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                      : Result<int64_t>(size_value.error());
        if (size.isErr()) return false;
        for (int64_t i = 0; i < size.value() && !dialog; ++i) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return false;
            auto child_variant = callVariant(children.value(), "get", {&index.value()});
            if (child_variant.isErr()) continue;
            auto child = objectFromVariant(child_variant.value());
            if (child.isErr() || !child.value()) continue;
            auto class_name = nodeString(child.value(), "get_class", 201670096LL);
            if (class_name.isOk() && class_name.value() == "ProgressDialog") dialog = child.value();
        }
        if (!dialog) {
            if (!g_progress_dialog_missing_reported) {
                g_progress_dialog_missing_reported = true;
                DIDI_LOG_WARN("GODOT_BRIDGE", "The editor's ProgressDialog is not a child of the root "
                              "window, so a reimport cannot wait out the editor's own progress tasks.");
            }
            return false;
        }
        g_progress_dialog_id = api.object_get_instance_id(dialog);
    }
    auto visible = callObject(dialog, "CanvasItem", "is_visible_in_tree", 36873697LL);
    if (visible.isErr()) return false;
    auto flag = scalarFromVariant<GDExtensionBool>(visible.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    return flag.isOk() && flag.value() != 0;
}

// The editor opens the scenes it restores from its layout, and the project's
// main scene on a first open, in its handler for the first scan's
// sources_changed (EditorNode::_sources_changed while waiting_for_first_scan,
// 4.5.1, 4.6.2 and 4.7.2). Nothing binds that flag. The file index says the
// same thing: EditorFileSystem starts with an empty root and swaps the scanned
// one in just before it emits sources_changed, and a project that loads this
// extension holds at least the file that declares it. The frames the editor
// runs between the swap and the scenes opening are inside progress tasks.
// get_filesystem is 842323275 and both counts 3905245786 on all three lines.
void GodotBridge::reportEditedSceneSaved(json& result) {
    auto editor = editorInterface();
    if (editor.isErr()) return;
    auto root = editedSceneRoot(editor.value());
    if (root.isErr() || !root.value()) return;
    const auto unsaved = unsavedScenesReport(editor.value());
    if (!unsaved.value("unsaved_scenes_readable", false) || !unsaved["unsaved_scenes"].is_array()) {
        const std::string limitation =
            "Godot before 4.7 cannot say whether this left the edited scene with unsaved "
            "changes. Save it with editor_save_scene if it should be on disk.";
        const auto existing = result.find("limitation");
        result["limitation"] = existing != result.end() && existing->is_string()
                                   ? existing->get<std::string>() + " " + limitation
                                   : limitation;
        return;
    }
    // A scene never saved has no path, and nothing on disk to match.
    const std::string path = editedScenePath(root.value());
    bool saved = !path.empty();
    for (const auto& entry : unsaved["unsaved_scenes"]) {
        if (entry.is_string() && entry.get<std::string>() == path) saved = false;
    }
    result["scene_saved"] = saved;
}

namespace bridge_internal {

// The edited scene's history, or nothing when no scene is open.
std::optional<UndoHistoryState> editedSceneHistory(GDExtensionObjectPtr editor,
                                                   GDExtensionObjectPtr manager) {
    auto root = editedSceneRoot(editor);
    if (root.isErr() || !root.value()) return std::nullopt;
    auto root_value = makeObject(root.value());
    if (root_value.isErr()) return std::nullopt;
    auto id_value = callObject(manager, "EditorUndoRedoManager", "get_object_history_id",
                               1107568780LL, {&root_value.value()});
    if (id_value.isErr()) return std::nullopt;
    auto id = scalarFromVariant<int64_t>(id_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (id.isErr() || id.value() == kGlobalUndoHistory) return std::nullopt;
    auto state = readUndoHistory(manager, id.value(), editedScenePath(root.value()));
    if (state.isErr()) return std::nullopt;
    return state.value();
}

// One reference judged against the editor as it is now. own_out, when given,
// receives its history as read, for the caller that goes on to undo it.
UndoVerdict judgeUndoReference(GDExtensionObjectPtr editor, GDExtensionObjectPtr manager,
                               const UndoCommit& ref,
                               std::optional<UndoHistoryState>* own_out = nullptr,
                               std::optional<UndoHistoryState>* other_out = nullptr,
                               const Result<std::vector<std::string>>* open_scenes = nullptr) {
    auto& ledger = undoLedger();
    if (ref.run != ledger.run()) {
        // Judged by the ledger, whose run is the only thing it compares first.
        return ledger.evaluate(ref, UndoHistoryState{}, false, std::nullopt, {});
    }
    const auto scene = editedSceneHistory(editor, manager);
    const bool global = ref.history_id == kGlobalUndoHistory;
    const bool candidate = global || (scene.has_value() && scene->id == ref.history_id);
    // Only the global history and the edited scene's are read. The manager
    // prints an engine error for a history it does not have, a scene's history
    // goes when the scene closes, and nothing says which ids remain without
    // asking for one. Those two are also the only ones the editor's Undo can
    // reach, so a reference into any other is judged by whether its scene is
    // still open.
    if (!candidate) {
        UndoVerdict verdict;
        const auto open = open_scenes ? *open_scenes : openScenePaths(editor);
        const bool still_open =
            ref.scene_path.empty() ||
            (open.isOk() && std::find(open.value().begin(), open.value().end(), ref.scene_path) !=
                                open.value().end());
        if (still_open) {
            verdict.state = UndoState::blocked;
            verdict.needs_scene_tab = true;
            verdict.reason = "Its scene is not the current tab. The editor's Undo reaches only the "
                             "current scene's history and the global one, so it is judged once its "
                             "scene is current.";
        } else {
            verdict.reason = "Its scene is closed, and closing a scene discards its history.";
        }
        return verdict;
    }
    auto own = global ? readUndoHistory(manager, kGlobalUndoHistory, {})
                      : Result<UndoHistoryState>(*scene);
    if (own.isErr()) {
        UndoVerdict verdict;
        verdict.reason = "Its history cannot be read: " + own.error().message;
        return verdict;
    }
    std::optional<UndoHistoryState> other;
    if (global) {
        other = scene;
    } else {
        auto read = readUndoHistory(manager, kGlobalUndoHistory, {});
        if (read.isOk()) other = read.value();
    }
    if (own_out) *own_out = own.value();
    if (other_out) *other_out = other;
    return ledger.evaluate(ref, own.value(), candidate, other,
                           [manager, &ref](int64_t index) {
                               return undoActionName(manager, ref.history_id, index);
                           });
}

json verdictJson(const UndoVerdict& verdict, const UndoCommit& ref) {
    json out = verdict.toJson();
    if (verdict.needs_scene_tab && !ref.scene_path.empty()) {
        out["next_call"] = {{"tool", "scene_open"},
                            {"arguments", {{"scene_path", ref.scene_path}}},
                            {"reason", "Makes its scene the current tab, where the editor's "
                                       "Undo can reach it."}};
    }
    return out;
}

// The refusal for a reference that cannot be undone on its own. Each code is
// spelled out rather than built from the state's name, so the refusal census
// in tests/test_refusal_remedies.py can see it.
json undoRefusalData(UndoState state) {
    switch (state) {
    case UndoState::blocked: return {{"code", "undo_entry_blocked"}};
    case UndoState::later_history: return {{"code", "undo_entry_later_history"}};
    case UndoState::undone: return {{"code", "undo_entry_undone"}};
    case UndoState::gone:
    case UndoState::available: break;
    }
    return {{"code", "undo_entry_gone"}};
}

// editor.undoStatus: whether each reference can be undone on its own now.
json undoStatusAnswer(const json& params) {
    const auto refs = params.find("refs");
    if (refs == params.end() || !refs->is_array()) {
        return errorJson(400, "refs must be an array of undo references",
                         {{"code", "invalid_undo_reference"}, {"field", "refs"}});
    }
    if (refs->size() > kMaxUndoStatusRefs) {
        return errorJson(400, "At most " + std::to_string(kMaxUndoStatusRefs) +
                                  " references can be judged in one call",
                         {{"code", "invalid_undo_reference"}, {"field", "refs"}});
    }
    auto editor = editorInterface();
    if (editor.isErr()) return errorJson(editor.error());
    auto manager = undoManager(editor.value());
    if (manager.isErr()) return errorJson(manager.error());
    json states = json::array();
    // Read once: every reference outside the current scene asks for it.
    const auto open_scenes = openScenePaths(editor.value());
    for (const auto& item : *refs) {
        std::string problem;
        const auto ref = UndoCommit::fromJson(item, &problem);
        if (!ref.has_value()) {
            states.push_back({{"state", "invalid"},
                              {"reason", "Not an undo reference: " + problem + " is missing or "
                                         "has the wrong type."}});
            continue;
        }
        states.push_back(verdictJson(
            judgeUndoReference(editor.value(), manager.value(), *ref, nullptr, nullptr, &open_scenes),
            *ref));
    }
    return liveResult({{"status", "success"}, {"run", undoLedger().run()}, {"states", std::move(states)}});
}

// editor.undo with expect: undoes exactly the action the reference names, or
// refuses without touching a history. The editor's own Undo is the only undo
// that keeps EditorUndoRedoManager's stacks true (#913), and it takes the newer
// of the edited scene's history and the global one, so the reference is
// judged first, and the histories are read again afterwards to show that the
// one it names moved back one step and the other did not move.
json undoReferencedAction(const json& params) {
    const json& expect = params["expect"];
    // The journal entry the server is undoing, named in every refusal, because
    // a live tool's refusal is the bridge's own and nothing is added to it on
    // the way back.
    const auto entry = params.value("journal_entry", json());
    const std::string about =
        entry.is_number_integer() ? "Journal entry " + std::to_string(entry.get<int64_t>()) + ": " : "";
    const auto refuse = [&entry, &about](int code, const std::string& message, json data) {
        if (entry.is_number_integer()) data["journal_entry"] = entry;
        return errorJson(code, about + message, std::move(data));
    };
    std::string problem;
    const auto ref = UndoCommit::fromJson(expect, &problem);
    if (!ref.has_value()) {
        return refuse(400, "expect is not an undo reference: " + problem +
                                  " is missing or has the wrong type",
                         {{"code", "invalid_undo_reference"}, {"field", problem}});
    }
    auto editor = editorInterface();
    if (editor.isErr()) return refuse(editor.error().code, editor.error().message, json::object());
    auto manager = undoManager(editor.value());
    if (manager.isErr()) return refuse(manager.error().code, manager.error().message, json::object());
    std::optional<UndoHistoryState> own;
    std::optional<UndoHistoryState> other;
    const auto verdict = judgeUndoReference(editor.value(), manager.value(), *ref, &own, &other);
    if (verdict.state != UndoState::available) {
        json data = undoRefusalData(verdict.state);
        data["undo"] = verdictJson(verdict, *ref);
        return refuse(409, verdict.reason, std::move(data));
    }
    auto pressed = pressEditorHistoryItem(editor.value(), true);
    if (pressed.isErr()) return refuse(pressed.error().code, pressed.error().message, json::object());
    auto own_after = readUndoHistory(manager.value(), ref->history_id, ref->scene_path);
    std::optional<UndoHistoryState> other_after;
    if (other.has_value()) {
        auto read = readUndoHistory(manager.value(), other->id, other->scene_path);
        if (read.isOk()) other_after = read.value();
    }
    const bool other_moved = other.has_value() &&
                             (!other_after.has_value() || other_after->version != other->version);
    const bool own_moved = own_after.isOk() && own_after.value().version != own->version;
    if (other_moved) {
        // The judgement said this cannot happen; say exactly what did.
        return refuse(500,
                         "The editor's Undo took an action in the other history (\"" +
                             other->action + "\") instead of this one. That action is undone; "
                             "editor_redo puts it back.",
                         {{"code", "undo_entry_misdirected"},
                          {"outcome", "unknown_outcome"},
                          {"undone_action", other->action},
                          {"undone_history", other->id == kGlobalUndoHistory ? "global" : "scene"}});
    }
    if (!own_moved) {
        return refuse(409,
                         "The editor ran its own Undo and no history moved. Godot refuses to "
                         "undo while a mouse button is held down in the editor.",
                         {{"code", "editor_declined"}, {"retryable", true}});
    }
    const auto& after = own_after.value();
    if (after.version != ref->version - 1 || after.current != ref->index - 1) {
        return refuse(500,
                         "The editor's Undo moved this history, but not to the state before "
                         "this action: version " + std::to_string(after.version) + ", action " +
                             std::to_string(after.current) + ".",
                         {{"code", "undo_entry_misdirected"},
                          {"outcome", "unknown_outcome"},
                          {"observed_version", after.version},
                          {"observed_index", after.current}});
    }
    json stepped = {{"status", "success"},
                    {"action", "undo"},
                    {"history", ref->history_id == kGlobalUndoHistory ? "global" : "scene"},
                    {"undone", ref->action},
                    {"observed", {{"version", after.version},
                                  {"current_action", after.action},
                                  {"redo_available", after.count > after.current + 1}}}};
    GodotBridge::instance().reportEditedSceneSaved(stepped);
    return liveResult(stepped);
}

} // namespace

GodotBridge::UndoSnapshot GodotBridge::snapshotUndoHistories() {
    UndoSnapshot snapshot;
    auto editor = editorInterface();
    if (editor.isErr()) return snapshot;
    auto manager = undoManager(editor.value());
    if (manager.isErr()) return snapshot;
    auto global = readUndoHistory(manager.value(), kGlobalUndoHistory, {});
    if (global.isOk()) snapshot.global = global.value();
    snapshot.scene = editedSceneHistory(editor.value(), manager.value());
    return snapshot;
}

void GodotBridge::attachUndoCommits(json& result, const UndoSnapshot& before) {
    if (!before.global.has_value() && !before.scene.has_value()) return;
    if (!result.is_object()) return;
    auto editor = editorInterface();
    if (editor.isErr()) return;
    auto manager = undoManager(editor.value());
    if (manager.isErr()) return;
    auto& ledger = undoLedger();
    std::vector<UndoCommit> commits;
    const auto observe = [&](const UndoHistoryState& was, const UndoHistoryState& now) {
        auto made = ledger.observe(was, now, [&](int64_t index) {
            return undoActionName(manager.value(), was.id, index);
        });
        commits.insert(commits.end(), made.begin(), made.end());
    };
    if (before.global.has_value()) {
        if (auto now = readUndoHistory(manager.value(), kGlobalUndoHistory, {}); now.isOk()) {
            observe(*before.global, now.value());
        }
    }
    // The scene that was current when the command started, and only while it
    // still is: reading a history the manager no longer has prints an engine
    // error, and a command that closed or switched the scene committed nothing
    // there that it was asked for. A scene saved for the first time inside the
    // command reads with its new path.
    if (before.scene.has_value()) {
        const auto now = editedSceneHistory(editor.value(), manager.value());
        if (now.has_value() && now->id == before.scene->id) observe(*before.scene, *now);
    }
    if (commits.empty()) return;
    std::sort(commits.begin(), commits.end(),
              [](const UndoCommit& a, const UndoCommit& b) { return a.serial < b.serial; });
    json steps = json::array();
    for (const auto& commit : commits) steps.push_back(commit.toJson());
    const auto error = result.find("error");
    if (error != result.end() && error->is_object()) {
        auto& data = (*error)["data"];
        if (!data.is_object()) data = json::object();
        data["undo_steps"] = std::move(steps);
        return;
    }
    result["undo_steps"] = std::move(steps);
}

bool GodotBridge::editorFirstScanApplied() {
    static bool applied = false;
    static bool unreadable_reported = false;
    if (applied) return true;
    // Not known is not a reason to hold anything, so it answers true, and it
    // is asked again on the next frame.
    const auto unreadable = [] {
        if (!unreadable_reported) {
            unreadable_reported = true;
            DIDI_LOG_WARN("GODOT_BRIDGE", "The editor's file index cannot be read, so scene_open "
                          "and scene_create do not wait for the editor to open its startup scenes.");
        }
        return true;
    };
    auto editor = editorInterface();
    if (editor.isErr()) return unreadable();
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    auto filesystem_object = filesystem.isOk() ? objectFromVariant(filesystem.value())
                                               : Result<GDExtensionObjectPtr>(filesystem.error());
    if (filesystem_object.isErr() || !filesystem_object.value()) return unreadable();
    auto root = callObject(filesystem_object.value(), "EditorFileSystem", "get_filesystem", 842323275LL);
    auto root_object = root.isOk() ? objectFromVariant(root.value())
                                   : Result<GDExtensionObjectPtr>(root.error());
    if (root_object.isErr()) return unreadable();
    // No root at all is an editor taking its filesystem down, not one starting.
    if (!root_object.value()) return true;
    for (const char* count : {"get_subdir_count", "get_file_count"}) {
        auto value = callObject(root_object.value(), "EditorFileSystemDirectory", count, 3905245786LL);
        auto number = value.isOk() ? scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                   : Result<int64_t>(value.error());
        if (number.isErr()) return unreadable();
        if (number.value() > 0) {
            applied = true;
            return true;
        }
    }
    return false;
}

bool GodotBridge::assetImportSettled(const std::string& resource_path) {
    const auto slash = resource_path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= resource_path.size()) return true;
    const std::string directory = resource_path.substr(0, slash + 1);
    const std::string file = resource_path.substr(slash + 1);

    auto editor = editorInterface();
    if (editor.isErr()) return true;
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return true;
    auto filesystem_object = objectFromVariant(filesystem.value());
    if (filesystem_object.isErr() || !filesystem_object.value()) return true;

    auto directory_value = makeString(directory);
    if (directory_value.isErr()) return true;
    auto found = callObject(filesystem_object.value(), "EditorFileSystem", "get_filesystem_path",
                            3188521125LL, {&directory_value.value()});
    if (found.isErr()) return true;
    auto directory_object = objectFromVariant(found.value());
    // The editor does not know the directory yet, which during a scan is work
    // outstanding rather than an answer.
    if (directory_object.isErr() || !directory_object.value()) return false;

    auto file_value = makeString(file);
    if (file_value.isErr()) return true;
    auto index = callObject(directory_object.value(), "EditorFileSystemDirectory",
                            "find_file_index", 1321353865LL, {&file_value.value()});
    if (index.isErr()) return true;
    auto position = scalarFromVariant<int64_t>(index.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (position.isErr() || position.value() < 0) return false;

    auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, position.value());
    if (index_value.isErr()) return true;
    auto valid = callObject(directory_object.value(), "EditorFileSystemDirectory",
                            "get_file_import_is_valid", 1116898809LL, {&index_value.value()});
    if (valid.isErr()) return true;
    auto flag = scalarFromVariant<GDExtensionBool>(valid.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (flag.isErr()) return true;
    if (flag.value() != 0) return true;

    // The flag alone is not enough for a file imported during this very pass.
    // It is computed when the file is indexed, and the import that follows does
    // not recompute it, so a .png the scan has just imported still reports
    // false until the next scan while its .import sidecar is already on disk.
    // The sidecar is the thing the answer is about, so it settles the question
    // the flag leaves open. Measured: a .gd reports the flag true and needs no
    // sidecar; a freshly imported .png reports it false and has one.
    return assetIsImported(resource_path);
}

namespace bridge_internal {

// The addon's count of the editor's import passes, made once in an editor
// session and kept for its life. Main thread only, like every caller.
std::optional<VariantValue> g_import_watch;
bool g_import_watch_tried = false;
// Didi's own reimport_files is on the stack. The frames the editor runs inside
// that call belong to a pass Didi started, and nothing may be answered from
// them either.
int g_reimport_call_depth = 0;

constexpr const char* kImportWatchPath = "res://addons/didi/didi_import_watch.gd";

struct ReimportCallScope {
    ReimportCallScope() { ++g_reimport_call_depth; }
    ~ReimportCallScope() { --g_reimport_call_depth; }
    ReimportCallScope(const ReimportCallScope&) = delete;
    ReimportCallScope& operator=(const ReimportCallScope&) = delete;
};

// Whether EditorFileSystem.is_importing exists on this engine, asked once. It
// binds from 4.7, and a bind lookup that answers null prints an ERROR (#600),
// so ClassDB.class_has_method is asked first. The bind is 36873697 on 4.7.2.
bool importingBindAvailable() {
    static std::optional<bool> cached;
    if (cached.has_value()) return *cached;
    auto class_db = singleton("ClassDB");
    if (class_db.isErr()) return false;
    auto class_name = makeStringName("EditorFileSystem");
    auto method_name = makeStringName("is_importing");
    auto no_inheritance = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (class_name.isErr() || method_name.isErr() || no_inheritance.isErr()) return false;
    auto answer = callObject(class_db.value(), "ClassDB", "class_has_method", 3860701026LL,
                             {&class_name.value(), &method_name.value(), &no_inheritance.value()});
    if (answer.isErr()) return false;
    auto declared = scalarFromVariant<GDExtensionBool>(answer.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (declared.isErr()) return false;
    const bool available = declared.value() != 0 &&
                           requireMethodBind("EditorFileSystem", "is_importing", 36873697LL).isOk();
    cached = available;
    return available;
}

std::optional<int64_t> importWatchCount(const char* property) {
    if (!g_import_watch.has_value()) return std::nullopt;
    auto object = objectFromVariant(*g_import_watch);
    if (object.isErr() || !object.value()) return std::nullopt;
    auto name = makeStringName(property);
    if (name.isErr()) return std::nullopt;
    auto value = callObject(object.value(), "Object", "get", 2760726917LL, {&name.value()});
    if (value.isErr()) return std::nullopt;
    auto count = scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return std::nullopt;
    return count.value();
}

// Makes the watch the first time it is wanted. A project whose copy of the
// addon predates the file gets one warning, and keeps the parts the engine can
// supply without it.
void ensureImportWatch(VariantValue& filesystem) {
    if (g_import_watch_tried) return;
    g_import_watch_tried = true;
    const auto unavailable = [](const std::string& why) {
        DIDI_LOG_WARN("GODOT_BRIDGE", "asset_reimport cannot see the editor's own import pass, so "
                      "it can answer from inside one (#914): ", why);
    };
    auto loader = singleton("ResourceLoader");
    auto path = makeString(kImportWatchPath);
    auto type_hint = makeString("");
    auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (loader.isErr() || path.isErr() || type_hint.isErr() || cache_mode.isErr()) {
        return unavailable("ResourceLoader is unavailable.");
    }
    // Asked before loading, because loading a file that is not there prints an
    // ERROR. ResourceLoader.exists is 4185558881 on 4.5.1, 4.6.2 and 4.7.2.
    auto exists = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                             {&path.value(), &type_hint.value()});
    auto found = exists.isOk() ? scalarFromVariant<GDExtensionBool>(exists.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                               : Result<GDExtensionBool>(exists.error());
    if (found.isErr() || found.value() == 0) {
        return unavailable(std::string("this project's copy of the addon has no ") + kImportWatchPath +
                           ". Copy the built addon folder in again.");
    }
    auto script = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                             {&path.value(), &type_hint.value(), &cache_mode.value()});
    auto script_object = script.isOk() ? objectFromVariant(script.value())
                                       : Result<GDExtensionObjectPtr>(script.error());
    if (script_object.isErr() || !script_object.value()) {
        return unavailable(std::string(kImportWatchPath) + " did not load.");
    }
    json empty = json::array();
    auto no_arguments = makeJsonVariant(empty);
    auto new_name = makeStringName("new");
    if (no_arguments.isErr() || new_name.isErr()) return unavailable("the watch could not be made.");
    auto watcher = callObject(script_object.value(), "Object", "callv", 1260104456LL,
                              {&new_name.value(), &no_arguments.value()});
    auto watcher_object = watcher.isOk() ? objectFromVariant(watcher.value())
                                         : Result<GDExtensionObjectPtr>(watcher.error());
    if (watcher_object.isErr() || !watcher_object.value()) {
        return unavailable("the watch could not be made.");
    }
    json watch_arguments = json::array();
    auto watch_array = makeJsonVariant(watch_arguments);
    auto watch_name = makeStringName("watch");
    if (watch_array.isErr() || watch_name.isErr() ||
        callVariant(watch_array.value(), "append", {&filesystem}).isErr() ||
        callObject(watcher_object.value(), "Object", "callv", 1260104456LL,
                   {&watch_name.value(), &watch_array.value()}).isErr()) {
        return unavailable("the watch could not connect to EditorFileSystem.");
    }
    g_import_watch = std::move(watcher.value());
    DIDI_LOG_INFO("GODOT_BRIDGE", "Watching the editor's import passes through ", kImportWatchPath);
}

} // namespace

ImportPassObservation GodotBridge::observeEditorImportPass() {
    ImportPassObservation seen;
    seen.inside_own_call = g_reimport_call_depth > 0;
    auto editor = editorInterface();
    if (editor.isErr()) return seen;
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return seen;
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr() || !object.value()) return seen;
    ensureImportWatch(filesystem.value());
    seen.started = importWatchCount("started");
    seen.finished = importWatchCount("finished");
    if (importingBindAvailable()) {
        auto importing = callObject(object.value(), "EditorFileSystem", "is_importing", 36873697LL);
        if (importing.isOk()) {
            auto flag = scalarFromVariant<GDExtensionBool>(importing.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (flag.isOk()) seen.importing = flag.value() != 0;
        }
    }
    return seen;
}

void GodotBridge::releaseImportWatch() {
    // While the engine still runs: a RefCounted the extension holds at exit is
    // reported as an ObjectDB leak, and a Variant destroyed after the engine
    // has gone calls into nothing. Nothing makes it again once released.
    g_import_watch_tried = true;
    g_import_watch.reset();
}

std::optional<ScanSettle> GodotBridge::beginScanSettle() {
    auto editor = editorInterface();
    if (editor.isErr()) return std::nullopt;
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return std::nullopt;
    ensureImportWatch(filesystem.value());
    const auto settled = importWatchCount("settled");
    if (!settled.has_value()) return std::nullopt;
    return ScanSettle{*settled, std::nullopt};
}

bool GodotBridge::scanSettled(const ScanSettle& wait) {
    return ::didi::godot::scanSettled(
        wait, ScanSettleObservation{importWatchCount("settled"), importWatchCount("settled_index")});
}

Result<ProjectScanWait> GodotBridge::beginProjectScan() {
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isErr()) return scanning.error();
    ProjectScanWait wait;
    if (scanning.value()) wait.prior = beginScanSettle();
    return wait;
}

Result<ProjectScanStep> GodotBridge::projectScanStepNow(const ProjectScanWait& wait) {
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isErr()) return scanning.error();
    return ::didi::godot::projectScanStep(
        wait, scanning.value(), editorProgressOpen(),
        ScanSettleObservation{importWatchCount("settled"), importWatchCount("settled_index")});
}

Result<void> GodotBridge::startProjectScan(ProjectScanWait& wait) {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return filesystem.error();
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr()) return object.error();
    if (!object.value()) return Error::notConnected("EditorFileSystem is unavailable");
    ensureImportWatch(filesystem.value());
    // Read before the call: without threads the editor applies a scan inside
    // it, and a scan builds a new index, so the emission that ends the wait is
    // one made with another index.
    const auto settled_before = importWatchCount("settled");
    const auto index_before = importWatchCount("index_id");
    auto scanned = callObject(object.value(), "EditorFileSystem", "scan", 3218959716LL);
    if (scanned.isErr()) return scanned.error();
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isErr()) return scanning.error();
    const auto settled_after = importWatchCount("settled");
    if (scanning.value() || !settled_before.has_value() ||
        (settled_after.has_value() && *settled_after > *settled_before)) {
        wait.own = ScanSettle{settled_before.value_or(0), index_before};
        wait.prior.reset();
        return Result<void>::ok();
    }
    // scan() started nothing and applied nothing. A finished scan still holds
    // the thread a new one would run on until a later frame applies it, so the
    // wait is for that, and then for a scan of Didi's own.
    wait.prior = ScanSettle{settled_after.value_or(*settled_before), std::nullopt};
    return Result<void>::ok();
}

Result<ReimportStart> GodotBridge::startAssetReimport(const ReimportBatch& batch,
                                                      const std::optional<ScanSettle>& seen_scan) {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return filesystem.error();
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr() || !object.value()) return Error::notConnected("EditorFileSystem is unavailable");
    // A scan is for what the editor does not have yet: a file it has never
    // indexed, or an asset whose import is outstanding. A script it already
    // lists needs update_file and no more, and a full scan asked for one was
    // the scan a reimport straight after it could not see through. Asked
    // before update_file, which can add a file to the index without importing
    // it.
    bool scan_needed = false;
    if (batch.needs_scan) {
        for (const auto& path : batch.refreshed) {
            if (!assetImportSettled(path)) {
                scan_needed = true;
                break;
            }
        }
    }
    // update_file is synchronous and starts no scan, so it goes first and the
    // scanning window the frame loop waits on belongs to the reimport alone.
    // The bind carries hash 83702148 on Godot 4.5.1, 4.6.2 and 4.7.2.
    for (const auto& path : batch.refreshed) {
        auto godot_path = makeString(path);
        if (godot_path.isErr()) return godot_path.error();
        auto updated = callObject(object.value(), "EditorFileSystem", "update_file", 83702148LL,
                                  {&godot_path.value()});
        if (updated.isErr()) return updated.error();
    }
    // A path with no sidecar may be a file the editor has never seen, and
    // update_file does not import one: no .import is written, nothing appears
    // under .godot/imported, and the asset stays unusable while the call
    // reports it refreshed and idle (#731). scan() is the walk that finds new
    // files and runs the importer over them; scan_sources compares folder
    // times in whole seconds and can miss a folder made since the last scan
    // (#1114). It is asynchronous and sets the scanning flag, which is the
    // window the frame loop already waits on.
    ReimportStart start;
    if (scan_needed) {
        // The index the scan will replace, read before it starts: without
        // threads the editor applies a scan inside the call. scan() starts
        // nothing while another scan is running, so the wait is for that one,
        // and a scan of changes builds no new index, so then any later
        // emission ends it. One that has finished and is not yet applied also
        // makes scan() start nothing, and replaces the index all the same.
        auto scanning_before = isEditorFilesystemScanning();
        if (scanning_before.isErr()) return scanning_before.error();
        start.settle = beginScanSettle();
        if (start.settle.has_value() && !scanning_before.value()) {
            start.settle->replaced_index = importWatchCount("index_id");
        }
        auto scanned = callObject(object.value(), "EditorFileSystem", "scan", 3218959716LL);
        if (scanned.isErr()) return scanned.error();
    }
    if (batch.reimported.empty()) return start;
    // reimport_files looks every file up in the editor's index, and while a
    // scan runs it cannot find one: the engine prints "Can't find file ...
    // during file reimport", skips it, and reimports nothing. Started straight
    // after the scan above, that was every batch mixing a script with an asset
    // in a project whose scan outlasts the call, and each was reported as
    // reimported; the harness met it only on a loaded machine. The editor's own
    // scans do the same to an asset named alone. Asked of the engine on 4.5.1,
    // 4.6.2 and 4.7.2 by tools/vibe/probes/scan_reimport_engine.py: a scan and
    // then a reimport at once failed every round with 800 scripts in the
    // project, and a reimport held until the scan had finished never did. So
    // the reimport waits, and the frame loop starts it once nothing is scanning
    // and the editor lists every asset.
    // The scanning flag clears on the scan's own thread, before a later frame
    // applies the results, and that work runs frames of its own under progress
    // tasks. A reimport started before it is over collides with it: the engine
    // printed hundreds of "Task ... already exists" lines and some rounds
    // reimported nothing (the same probe's burst part, on all three lines).
    // So the reimport also waits for sources_changed, which the editor emits
    // once the results are applied, and for no progress task.
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isErr()) return scanning.error();
    if (scanning.value() && !start.settle.has_value()) start.settle = beginScanSettle();
    // A scan that has finished and is not applied clears the flag, so only a
    // caller that saw it running knows it is there. The editor applies it in
    // its next frame, which reimport_files would run, and applying it imports
    // what the scan found by calling reimport_files again: the engine refuses
    // that as recursive and the asset stays unimported. Seen on 4.7.2 by
    // tools/vibe/probes/reimport_in_scan_tail.py (#995).
    if (!start.settle.has_value() && seen_scan.has_value()) start.settle = seen_scan;
    start.held = scanning.value() || editorProgressOpen() ||
                 (start.settle.has_value() && !scanSettled(*start.settle)) ||
                 !unindexedAssets(batch.reimported).empty();
    if (start.held) return start;
    auto started = reimportIndexedAssets(batch.reimported);
    if (started.isErr()) return started.error();
    return start;
}

Result<void> GodotBridge::reimportIndexedAssets(const std::vector<std::string>& reimported) {
    if (reimported.empty()) return Result<void>::ok();
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return filesystem.error();
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr() || !object.value()) return Error::notConnected("EditorFileSystem is unavailable");
    json path_array = reimported;
    auto godot_paths = makeJsonVariant(path_array);
    if (godot_paths.isErr()) return godot_paths.error();
    ensureImportWatch(filesystem.value());
    const auto passes_before = importWatchCount("started");
    {
        ReimportCallScope inside_own_call;
        auto started = callObject(object.value(), "EditorFileSystem", "reimport_files", 4015028928LL,
                                  {&godot_paths.value()});
        if (started.isErr()) return started.error();
    }
    // reimport_files refuses to start inside the editor's own pass while its
    // importing flag is set, prints an ERROR and returns nothing, and the
    // refused call emits no resources_reimporting of its own. On 4.5 and 4.6,
    // which bind no is_importing, that silence is the only way to know. It
    // used to reach the caller as reimported and idle.
    const auto passes_after = importWatchCount("started");
    if (passes_before.has_value() && passes_after.has_value() && *passes_after == *passes_before) {
        return Error(409,
                     "The editor was in the middle of its own import pass and Godot refused to "
                     "start another inside it, so nothing was reimported. Retry once the editor "
                     "is idle; the engine's own line is under engine_diagnostics.",
                     json{{"code", "editor_import_busy"},
                          {"outcome", "not_imported"},
                          {"retryable", true}});
    }
    return Result<void>::ok();
}

namespace bridge_internal {

// Paths written while the editor filesystem was scanning, waiting for it to be
// idle so they can be indexed. Bounded: a caller that writes thousands of
// scenes during one scan does not get an unbounded queue out of it.
struct DeferredReindex {
    std::string path;
    int attempts{0};
};
std::mutex g_deferred_reindex_mutex;
std::vector<DeferredReindex> g_deferred_reindex;
// sources_changed as counted when a retry last failed. The editor applies a
// scan on a later frame than the one that clears its scanning flag, and a scan
// can start and finish between two frames, so an attempt can still land
// before the results it needs. The next one waits for the editor to apply them.
std::optional<int64_t> g_deferred_reindex_after;
constexpr size_t kMaxDeferredReindex = 256;
constexpr int kMaxReindexAttempts = 3;

} // namespace

// Runs EditorFileSystem.update_file and reports whether the engine now resolves
// the uid the file carries, or nothing when the editor could not be asked.
//
// update_file is the call Godot documents for a file "saved by an external
// program", it is synchronous and it starts no scan. It is also a no-op while a
// scan is in progress, which is exactly when the save callback failed too.
std::optional<GodotBridge::WrittenResourceUid> GodotBridge::indexWrittenResource(
    const std::string& resource_path) {
    WrittenResourceUid state;
    auto editor = editorInterface();
    if (editor.isErr()) return std::nullopt;
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return std::nullopt;
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr() || !object.value()) return std::nullopt;
    // A retry waits for sources_changed, which only the watch counts.
    ensureImportWatch(filesystem.value());

    auto godot_path = makeString(resource_path);
    if (godot_path.isErr()) return std::nullopt;
    // The bind carries hash 83702148 on Godot 4.5.1, 4.6.2 and 4.7.2.
    auto updated = callObject(object.value(), "EditorFileSystem", "update_file", 83702148LL,
                              {&godot_path.value()});
    if (updated.isErr()) return std::nullopt;

    auto uid_table = singleton("ResourceUID");
    if (uid_table.isErr()) return std::nullopt;
    auto uid_value = callObject(uid_table.value(), "ResourceUID", "path_to_uid", 1703090593LL,
                                {&godot_path.value()});
    if (uid_value.isErr()) return std::nullopt;
    auto uid_text = stringFromVariant(uid_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    if (uid_text.isErr() || uid_text.value().rfind("uid://", 0) != 0) return std::nullopt;
    state.uid = uid_text.value();

    auto id_text = makeString(state.uid);
    if (id_text.isErr()) return std::nullopt;
    auto id_value = callObject(uid_table.value(), "ResourceUID", "text_to_id", 1321353865LL,
                               {&id_text.value()});
    if (id_value.isErr()) return std::nullopt;
    auto id = scalarFromVariant<int64_t>(id_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (id.isErr()) return std::nullopt;
    auto id_argument = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, id.value());
    if (id_argument.isErr()) return std::nullopt;
    auto known = callObject(uid_table.value(), "ResourceUID", "has_id", 1116898809LL,
                            {&id_argument.value()});
    if (known.isErr()) return std::nullopt;
    auto registered = scalarFromVariant<GDExtensionBool>(known.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (registered.isErr()) return std::nullopt;
    state.registered = registered.value() != 0;
    return state;
}

GodotBridge::WrittenResourceUid GodotBridge::registerWrittenResourceUid(
    const std::string& resource_path) {
    auto indexed = indexWrittenResource(resource_path);
    if (!indexed.has_value()) return WrittenResourceUid{};
    auto state = *indexed;
    if (state.registered) return state;

    // Not registered means a scan is running, or its results are not applied
    // yet, or the file is in a folder the editor does not list. Adding the id
    // here does not survive a scan: it replaces the table with what it found,
    // and it did not find this file. So the work waits for idle instead, and
    // the frame loop tries again.
    {
        std::lock_guard<std::mutex> lock(g_deferred_reindex_mutex);
        const bool queued = std::any_of(g_deferred_reindex.begin(), g_deferred_reindex.end(),
                                        [&](const DeferredReindex& entry) { return entry.path == resource_path; });
        if (!queued && g_deferred_reindex.size() < kMaxDeferredReindex) {
            g_deferred_reindex.push_back(DeferredReindex{resource_path, 0});
        }
    }
    state.deferred = true;
    // update_file never indexes a file in a folder the editor does not list,
    // however often it is called, and a scene written into a new folder is
    // exactly that. scan and scan_sources both list the folder and index the
    // file within a few frames (tools/vibe/probes/new_folder_uid_engine.py, on
    // 4.5.1, 4.6.2 and 4.7.2). Through scene_create on 4.5.1, scan_sources
    // missed a folder made moments after the last scan in two rounds of three
    // (tools/vibe/probes/deferred_scene_uid.py), so this asks for a full scan,
    // which walks every folder. A scan already running finds the file itself.
    const auto ask_for_scan = [&]() -> Result<void> {
        auto editor = editorInterface();
        if (editor.isErr()) return editor.error();
        auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
        if (filesystem.isErr()) return filesystem.error();
        auto object = objectFromVariant(filesystem.value());
        if (object.isErr()) return object.error();
        if (!object.value()) return Error::notConnected("EditorFileSystem is unavailable");
        auto scan = callObject(object.value(), "EditorFileSystem", "scan", 3218959716LL);
        if (scan.isErr()) return scan.error();
        return Result<void>::ok();
    };
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isOk() && !scanning.value()) {
        if (auto asked = ask_for_scan(); asked.isErr()) {
            DIDI_LOG_WARN("GODOT_BRIDGE", "Could not ask the editor to scan for ", resource_path, ": ",
                          asked.error().message);
        }
    }
    return state;
}

void GodotBridge::processDeferredReindexFrame() {
    {
        std::lock_guard<std::mutex> lock(g_deferred_reindex_mutex);
        if (g_deferred_reindex.empty()) return;
    }
    auto scanning = isEditorFilesystemScanning();
    if (scanning.isErr() || scanning.value()) return;
    const auto settled = importWatchCount("settled");

    std::vector<DeferredReindex> pending;
    {
        std::lock_guard<std::mutex> lock(g_deferred_reindex_mutex);
        if (g_deferred_reindex_after.has_value() && settled.has_value() &&
            *settled <= *g_deferred_reindex_after) {
            return;
        }
        pending.swap(g_deferred_reindex);
        g_deferred_reindex_after.reset();
    }
    // A single attempt used to be taken as done. One that ran before the
    // editor applied its scan indexed nothing, and the scene's uid then stayed
    // unknown until some later scan happened to find the file.
    std::vector<DeferredReindex> again;
    for (auto& entry : pending) {
        const auto indexed = indexWrittenResource(entry.path);
        if (indexed.has_value() && indexed->registered) {
            DIDI_LOG_INFO("GODOT_BRIDGE", "Indexed ", entry.path, " after the editor filesystem settled");
            continue;
        }
        DIDI_LOG_INFO("GODOT_BRIDGE", "Not indexed yet: ", entry.path, " (attempt ", entry.attempts + 1,
                      ", sources_changed ", settled.value_or(-1), ")");
        if (++entry.attempts >= kMaxReindexAttempts) {
            DIDI_LOG_WARN("GODOT_BRIDGE", "Could not index ", entry.path, " in ", entry.attempts,
                          " attempts; the editor's next scan of it will");
            continue;
        }
        again.push_back(std::move(entry));
    }
    if (again.empty()) return;
    std::lock_guard<std::mutex> lock(g_deferred_reindex_mutex);
    for (auto& entry : again) {
        if (g_deferred_reindex.size() < kMaxDeferredReindex) g_deferred_reindex.push_back(std::move(entry));
    }
    g_deferred_reindex_after = settled;
}

namespace bridge_internal {

// What Godot calls the main screen a given editor class belongs to.
//
// set_main_screen_editor takes the tab's name and there is no getter, so the
// only way back is the class of whichever child of the main screen container
// is visible. Godot wraps the Script and Game screens in a WindowWrapper so
// they can be torn off into their own window, which is why those two are found
// one level down.
const char* mainScreenNameForClass(const std::string& class_name) {
    if (class_name == "CanvasItemEditor") return "2D";
    if (class_name == "Node3DEditor") return "3D";
    if (class_name == "ScriptEditor") return "Script";
    if (class_name == "GameView") return "Game";
    if (class_name == "EditorAssetLibrary") return "AssetLib";
    return nullptr;
}

} // namespace

std::optional<std::string> GodotBridge::currentMainScreenName() {
    auto editor = editorInterface();
    if (editor.isErr()) return std::nullopt;
    auto container = callObject(editor.value(), "EditorInterface", "get_editor_main_screen", 1706218421LL);
    if (container.isErr()) return std::nullopt;
    auto container_object = objectFromVariant(container.value());
    if (container_object.isErr() || !container_object.value()) return std::nullopt;

    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return std::nullopt;
    auto children = callObject(container_object.value(), "Node", "get_children", 873284517LL,
                               {&include_internal.value()});
    if (children.isErr()) return std::nullopt;
    auto count_value = callVariant(children.value(), "size");
    if (count_value.isErr()) return std::nullopt;
    auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return std::nullopt;

    const auto classOf = [&](GDExtensionObjectPtr object) -> std::optional<std::string> {
        auto value = callObject(object, "Object", "get_class", 201670096LL);
        if (value.isErr()) return std::nullopt;
        auto text = stringFromVariant(value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (text.isErr()) return std::nullopt;
        return text.value();
    };

    for (int64_t index = 0; index < count.value(); ++index) {
        auto position = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (position.isErr()) continue;
        auto child_value = callVariant(children.value(), "get", {&position.value()});
        if (child_value.isErr()) continue;
        auto child = objectFromVariant(child_value.value());
        if (child.isErr() || !child.value()) continue;
        auto visible_value = callObject(child.value(), "CanvasItem", "is_visible", 36873697LL);
        if (visible_value.isErr()) continue;
        auto visible = scalarFromVariant<GDExtensionBool>(visible_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (visible.isErr() || visible.value() == 0) continue;

        const auto child_class = classOf(child.value());
        if (!child_class.has_value()) return std::nullopt;
        if (const char* name = mainScreenNameForClass(*child_class)) return std::string(name);
        // A wrapper holds the screen that was torn off; the class inside it is
        // the one that names the tab.
        auto inner = callObject(child.value(), "Node", "get_children", 873284517LL,
                                {&include_internal.value()});
        if (inner.isErr()) return std::nullopt;
        auto inner_count_value = callVariant(inner.value(), "size");
        if (inner_count_value.isErr()) return std::nullopt;
        auto inner_count = scalarFromVariant<int64_t>(inner_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (inner_count.isErr()) return std::nullopt;
        for (int64_t inner_index = 0; inner_index < inner_count.value(); ++inner_index) {
            auto inner_position = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, inner_index);
            if (inner_position.isErr()) continue;
            auto inner_value = callVariant(inner.value(), "get", {&inner_position.value()});
            if (inner_value.isErr()) continue;
            auto inner_child = objectFromVariant(inner_value.value());
            if (inner_child.isErr() || !inner_child.value()) continue;
            const auto inner_class = classOf(inner_child.value());
            if (!inner_class.has_value()) continue;
            if (const char* name = mainScreenNameForClass(*inner_class)) return std::string(name);
        }
        // Visible, and none of the classes Godot ships. An addon's main screen
        // is named by the addon, which nothing here can read back.
        return std::nullopt;
    }
    return std::nullopt;
}

Result<void> GodotBridge::selectMainScreen(const std::string& name) {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto screen = makeString(name);
    if (screen.isErr()) return screen.error();
    // The bind carries hash 83702148 on Godot 4.5.1, 4.6.2 and 4.7.2.
    auto selected = callObject(editor.value(), "EditorInterface", "set_main_screen_editor", 83702148LL,
                               {&screen.value()});
    if (selected.isErr()) return selected.error();
    return Result<void>::ok();
}

namespace bridge_internal {

// The coroutine waits scene_call_method has outstanding.
//
// Each entry holds the GDScript waiter object that is doing the awaiting. The
// Variant owns a reference to it, so erasing the entry drops that reference and
// the engine frees the RefCounted; nothing here is manually freed.
struct ScriptCallWait {
    VariantValue waiter;
    std::string method_name;
};

std::mutex g_script_call_mutex;
std::map<uint64_t, ScriptCallWait> g_script_calls;
uint64_t g_next_script_call_id = 1;
constexpr size_t kMaxPendingScriptCalls = 16;

} // namespace

std::optional<json> GodotBridge::collectScriptCall(uint64_t await_id) {
    std::lock_guard<std::mutex> lock(g_script_call_mutex);
    auto found = g_script_calls.find(await_id);
    if (found == g_script_calls.end()) return std::nullopt;

    const auto read = [&](const char* property) -> Result<VariantValue> {
        auto object = objectFromVariant(found->second.waiter);
        if (object.isErr() || !object.value()) return Error::internal("The coroutine waiter is gone");
        auto name = makeStringName(property);
        if (name.isErr()) return name.error();
        return callObject(object.value(), "Object", "get", 2760726917LL, {&name.value()});
    };

    auto finished_value = read("finished");
    if (finished_value.isErr()) {
        g_script_calls.erase(found);
        return json(nullptr);
    }
    auto finished = scalarFromVariant<GDExtensionBool>(finished_value.value(),
                                                       GDEXTENSION_VARIANT_TYPE_BOOL);
    if (finished.isErr() || finished.value() == 0) return std::nullopt;

    json value = nullptr;
    auto returned = read("value");
    if (returned.isOk()) {
        auto rendered = variantToJson(returned.value(), 0, true);
        if (rendered.isOk()) value = rendered.value();
    }
    // Erasing drops the last reference this side holds, and the waiter is a
    // RefCounted, so the engine frees it here rather than at some later scan.
    g_script_calls.erase(found);
    return value;
}

void GodotBridge::abandonScriptCall(uint64_t await_id) {
    std::lock_guard<std::mutex> lock(g_script_call_mutex);
    g_script_calls.erase(await_id);
}

json GodotBridge::callScriptMethod(const json& params,
                                   std::optional<PendingScriptCall>& pending) {
    pending.reset();
    const auto fail = [](int code, const std::string& message) {
        return json{{"error", {{"code", code}, {"message", message}}}};
    };

    if (!params.is_object() || !params.contains("target_node") ||
        !params["target_node"].is_string() ||
        params["target_node"].get<std::string>().empty() ||
        params["target_node"].get<std::string>().size() > 1024 ||
        !params.contains("method_name") || !params["method_name"].is_string()) {
        return fail(400, "scene_call_method needs target_node and method_name");
    }
    const auto method_name = params["method_name"].get<std::string>();
    if (method_name.empty() || method_name.size() > 128) {
        return fail(400, "method_name must be 1 to 128 characters");
    }
    // Godot uses a leading underscore for engine callbacks and for a script's
    // own private helpers. Calling _ready or _process by hand corrupts a node's
    // state in a way no caller intends, so the prefix is refused whatever the
    // script declares.
    if (method_name.front() == '_') {
        return errorJson(403,
                         "Refusing to call \"" + method_name +
                             "\". A leading underscore is Godot's mark for an engine callback or a "
                             "script's private helper, and calling one by hand corrupts node state. "
                             "Expose the behaviour under a name without the underscore.",
                         json{{"code", "method_private"}, {"field", "method_name"}});
    }
    const auto arguments = params.value("arguments", json::array());
    if (!arguments.is_array() || arguments.size() > 8) {
        return fail(400, "arguments must be an array of at most 8 values");
    }
    std::function<bool(const json&, int)> supported = [&](const json& value, int depth) {
        if (depth > 4) return false;
        if (value.is_null() || value.is_boolean() || value.is_number_integer()) return true;
        if (value.is_number_unsigned()) {
            return value.get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        }
        if (value.is_number_float()) return std::isfinite(value.get<double>());
        if (value.is_string()) return value.get_ref<const std::string&>().size() <= 4096;
        if (value.is_array()) {
            if (value.size() > 64) return false;
            for (const auto& element : value) if (!supported(element, depth + 1)) return false;
            return true;
        }
        if (value.is_object()) {
            if (value.size() > 64) return false;
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.key().size() > 4096 || !supported(it.value(), depth + 1)) return false;
            }
            return true;
        }
        return false;
    };
    for (const auto& argument : arguments) {
        if (!supported(argument, 0)) {
            return fail(400, "arguments hold a value scene_call_method cannot pass to Godot");
        }
    }
    try {
        if (arguments.dump().size() > 8u * 1024u) {
            return errorJson(413, "arguments exceed 8 KiB",
                             json{{"code", "arguments_too_large"}, {"field", "arguments"}});
        }
    } catch (const json::exception&) {
        return fail(400, "arguments are not valid JSON text");
    }

    auto editor = editorInterface();
    if (editor.isErr()) return fail(editor.error().code, editor.error().message);
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return fail(root.error().code, root.error().message);
    auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
    if (target.isErr()) return fail(target.error().code, target.error().message);

    // The allowlist is the node's own script, and nothing else. Every engine
    // method is out of reach by construction rather than by enumeration: free,
    // queue_free, set_script and the rest are declared by ClassDB, so none of
    // them appears here.
    auto script_value = callObject(target.value(), "Object", "get_script", 1214101251LL);
    if (script_value.isErr()) return fail(500, script_value.error().message);
    auto script = objectFromVariant(script_value.value());
    if (script.isErr() || !script.value()) {
        const auto typed = typedToolForEngineMethod(method_name);
        return errorJson(422,
                         "The node has no script, so it declares no methods to call. Engine methods "
                         "are deliberately out of reach here; the typed tools cover those." +
                             (typed.empty() ? std::string()
                                            : " For " + method_name + ", use " + typed + "."),
                         typed.empty() ? json{{"code", "node_has_no_script"}}
                                       : json{{"code", "node_has_no_script"}, {"use_tool", typed}});
    }
    // The editor only creates a script instance for a @tool script. Without
    // one the node carries the script resource, has_method answers true, and
    // the call returns nil having run nothing. That silence is the worst
    // possible answer, so it is turned into a refusal that names the cause.
    auto tool_value = callObject(script.value(), "Script", "is_tool", 36873697LL);
    if (tool_value.isErr()) return fail(500, tool_value.error().message);
    auto is_tool = scalarFromVariant<GDExtensionBool>(tool_value.value(),
                                                      GDEXTENSION_VARIANT_TYPE_BOOL);
    if (is_tool.isErr()) return fail(500, is_tool.error().message);
    if (is_tool.value() == 0) {
        return errorJson(422,
                         "The node's script is not a @tool script, so the editor has not created an "
                         "instance of it and there is nothing to run. Calling it would return nothing "
                         "having done nothing. Add @tool to the script if it is meant to act in the "
                         "editor, or run the project and drive it there.",
                         {{"code", "script_not_tool"}});
    }

    // The leaf script's list already carries methods it inherits from a base
    // script, so a project that splits behaviour across scripts works without
    // this walking a chain.
    auto method_list = callObject(script.value(), "Script", "get_script_method_list", 2915620761LL);
    if (method_list.isErr()) return fail(500, method_list.error().message);
    auto count_value = callVariant(method_list.value(), "size");
    if (count_value.isErr()) return fail(500, count_value.error().message);
    auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return fail(500, count.error().message);

    json declared = json::array();
    std::optional<json> chosen;
    for (int64_t index = 0; index < count.value(); ++index) {
        auto position = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (position.isErr()) continue;
        auto entry = callVariant(method_list.value(), "get", {&position.value()});
        if (entry.isErr()) continue;
        auto rendered = variantToJson(entry.value(), 0, true);
        if (rendered.isErr() || !rendered.value().is_object()) continue;
        const auto name = rendered.value().value("name", std::string());
        if (name.empty() || name.front() == '_') continue;
        declared.push_back(name);
        if (name == method_name) chosen = rendered.value();
    }
    if (!chosen.has_value()) {
        return errorJson(404,
                         "\"" + method_name + "\" is not a method this node's script declares. "
                         "scene_call_method calls project methods only. Declared: " +
                             declared.dump() +
                             (typedToolForEngineMethod(method_name).empty()
                                  ? std::string()
                                  : ". For the engine's " + method_name + ", use " +
                                        typedToolForEngineMethod(method_name) + "."),
                         json{{"code", "method_not_declared"},
                              {"field", "method_name"},
                              {"declared", declared}});
    }

    const auto declared_arguments = chosen->value("args", json::array());
    const auto default_arguments = chosen->value("default_args", json::array());
    if (!declared_arguments.is_array() || !default_arguments.is_array() ||
        default_arguments.size() > declared_arguments.size()) {
        return fail(500, "Godot returned an unreadable method list");
    }
    // The trailing parameters that declare a default may be left out, and the
    // engine fills them in, as GDScript does for any call. Measured on 4.5.1,
    // 4.6.2 and 4.7.2: default_args lists them, and a call with only the
    // required ones runs with the defaults (#1251).
    const size_t required_arguments = declared_arguments.size() - default_arguments.size();
    if (arguments.size() < required_arguments || arguments.size() > declared_arguments.size()) {
        const auto takes = required_arguments == declared_arguments.size()
            ? std::to_string(declared_arguments.size())
            : std::to_string(required_arguments) + " to " + std::to_string(declared_arguments.size());
        return errorJson(409, "\"" + method_name + "\" takes " + takes + " argument(s) and " +
                                  std::to_string(arguments.size()) + " were given",
                         json{{"code", "argument_count_mismatch"},
                              {"field", "arguments"},
                              {"expected", declared_arguments.size()},
                              {"required", required_arguments},
                              {"given", arguments.size()}});
    }
    // Each argument has to fit the parameter Godot declared, checked before
    // anything runs. A mismatch inside the engine is a crash or a silent
    // conversion, and neither is an answer.
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto& declared_argument = declared_arguments[index];
        if (!declared_argument.is_object()) continue;
        const auto type = declared_argument.value("type", static_cast<int64_t>(0));
        const auto& given = arguments[index];
        bool compatible = true;
        switch (type) {
            case GDEXTENSION_VARIANT_TYPE_NIL: compatible = true; break;
            case GDEXTENSION_VARIANT_TYPE_BOOL: compatible = given.is_boolean(); break;
            case GDEXTENSION_VARIANT_TYPE_INT:
                compatible = given.is_number_integer() || given.is_number_unsigned();
                break;
            case GDEXTENSION_VARIANT_TYPE_FLOAT: compatible = given.is_number(); break;
            case GDEXTENSION_VARIANT_TYPE_STRING:
            case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
            case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
                compatible = given.is_string();
                break;
            case GDEXTENSION_VARIANT_TYPE_ARRAY: compatible = given.is_array(); break;
            case GDEXTENSION_VARIANT_TYPE_DICTIONARY: compatible = given.is_object(); break;
            default:
                // An untyped parameter, or one whose type this cannot construct
                // from JSON. Untyped is common in GDScript and is allowed;
                // anything else is refused rather than guessed at.
                compatible = declared_argument.value("type", static_cast<int64_t>(0)) ==
                                 GDEXTENSION_VARIANT_TYPE_NIL ||
                             given.is_null();
                break;
        }
        if (!compatible) {
            return errorJson(409, "argument " + std::to_string(index + 1) + " of \"" +
                                      method_name +
                                      "\" does not fit the parameter type the script declares",
                             json{{"code", "argument_type_mismatch"},
                                  {"field", "arguments"},
                                  {"argument_index", index}});
        }
    }

    // Everything that decides whether this call can run has now been read off
    // the target: the node resolved, it carries a script, the script is a
    // @tool script so the editor has an instance of it, the method is one the
    // script declares, and the arguments fit what it declares. A preview stops
    // here and reports those facts.
    //
    // The dry run used to report the node's `name` property, a constant that
    // came back for every method name and every argument list, and then minted
    // a token for a call it had all the evidence to know would 422. The gate's
    // own 428 promises the preview reads the target where it can; it claimed
    // target_state and answered about something else (#463).
    if (params.value("preview", false)) {
        return json{{"preview", true},
                    {"target_node", params["target_node"]},
                    {"method_name", method_name},
                    {"method_exists", true},
                    {"script_is_tool", true},
                    {"declared_argument_count", declared_arguments.size()},
                    {"signature", *chosen},
                    {"would_run", true}};
    }

    json argument_array = arguments;
    auto godot_arguments = makeJsonVariant(argument_array);
    if (godot_arguments.isErr()) return fail(400, godot_arguments.error().message);
    auto godot_name = makeStringName(method_name);
    if (godot_name.isErr()) return fail(500, godot_name.error().message);

    auto returned = callObject(target.value(), "Object", "callv", 1260104456LL,
                               {&godot_name.value(), &godot_arguments.value()});
    if (returned.isErr()) return fail(returned.error().code, returned.error().message);

    // A GDScript function containing await hands back a GDScriptFunctionState
    // instead of the value. The class is not in the GDExtension class list, so
    // it is identified by name; Object.is_class would have been the obvious
    // check and its hash differs on 4.7.2.
    bool is_coroutine = false;
    if (GodotApi::instance().variant_get_type(returned.value().ptr()) ==
        GDEXTENSION_VARIANT_TYPE_OBJECT) {
        auto returned_object = objectFromVariant(returned.value());
        if (returned_object.isOk() && returned_object.value()) {
            auto class_value = callObject(returned_object.value(), "Object", "get_class", 201670096LL);
            if (class_value.isOk()) {
                auto class_name = stringFromVariant(class_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                is_coroutine = class_name.isOk() && class_name.value() == "GDScriptFunctionState";
            }
            if (is_coroutine) {
                // The value arrives on the state's completed signal, and the
                // extension has no way to receive one: a custom Callable is
                // built and invoked correctly but never reached by the signal,
                // and a normal Callable needs an Object with a registered
                // method. GDScript's own await is the mechanism for this, so
                // the addon does the waiting and this polls it.
                auto loader = singleton("ResourceLoader");
                if (loader.isErr()) return fail(loader.error().code, loader.error().message);
                auto waiter_path = makeString("res://addons/didi/didi_await.gd");
                auto type_hint = makeString("");
                auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
                if (waiter_path.isErr() || type_hint.isErr() || cache_mode.isErr()) {
                    return fail(500, "Failed to construct the coroutine waiter request");
                }
                auto script_value = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                               {&waiter_path.value(), &type_hint.value(),
                                                &cache_mode.value()});
                auto waiter_script = script_value.isOk() ? objectFromVariant(script_value.value())
                                                         : Result<GDExtensionObjectPtr>(script_value.error());
                if (waiter_script.isErr() || !waiter_script.value()) {
                    return fail(501,
                                "\"" + method_name + "\" is a coroutine, and this project's copy of "
                                "the addon has no res://addons/didi/didi_await.gd to await it with. "
                                "The method has started and will finish on its own. Copy the built "
                                "addon folder in again.");
                }
                json empty = json::array();
                auto no_arguments = makeJsonVariant(empty);
                if (no_arguments.isErr()) return fail(500, no_arguments.error().message);
                auto new_name = makeStringName("new");
                if (new_name.isErr()) return fail(500, new_name.error().message);
                auto waiter = callObject(waiter_script.value(), "Object", "callv", 1260104456LL,
                                         {&new_name.value(), &no_arguments.value()});
                if (waiter.isErr()) return fail(waiter.error().code, waiter.error().message);
                auto waiter_object = objectFromVariant(waiter.value());
                if (waiter_object.isErr() || !waiter_object.value()) {
                    return fail(500, "The coroutine waiter could not be constructed");
                }

                json watch_arguments = json::array();
                auto watch_array = makeJsonVariant(watch_arguments);
                if (watch_array.isErr()) return fail(500, watch_array.error().message);
                auto appended = callVariant(watch_array.value(), "append", {&returned.value()});
                if (appended.isErr()) return fail(500, appended.error().message);
                auto watch_name = makeStringName("watch");
                if (watch_name.isErr()) return fail(500, watch_name.error().message);
                auto watching = callObject(waiter_object.value(), "Object", "callv", 1260104456LL,
                                           {&watch_name.value(), &watch_array.value()});
                if (watching.isErr()) return fail(watching.error().code, watching.error().message);

                uint64_t await_id = 0;
                {
                    std::lock_guard<std::mutex> lock(g_script_call_mutex);
                    if (g_script_calls.size() >= kMaxPendingScriptCalls) {
                        return errorJson(429, "Too many coroutine calls are already being awaited",
                                         json{{"code", "await_limit_reached"},
                                              {"retryable", true}});
                    }
                    await_id = g_next_script_call_id++;
                    ScriptCallWait entry;
                    entry.waiter = std::move(waiter.value());
                    entry.method_name = method_name;
                    g_script_calls.emplace(await_id, std::move(entry));
                }
                PendingScriptCall wait;
                wait.await_id = await_id;
                wait.target_node = params["target_node"].get<std::string>();
                wait.method_name = method_name;
                pending = wait;
                return json::object();
            }
        }
    }

    auto value = variantToJson(returned.value(), 0, true);
    if (value.isErr()) return fail(value.error().code, value.error().message);
    // Project code, which can change the scene with or without an undo entry
    // (#1049).
    json called = {{"status", "success"},
                   {"target_node", params["target_node"]},
                   {"method_name", method_name},
                   {"awaited", false},
                   {"returned", value.value()}};
    reportEditedSceneSaved(called);
    return called;
}

Result<bool> GodotBridge::isEditorFilesystemScanning() {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return filesystem.error();
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr() || !object.value()) return Error::notConnected("EditorFileSystem is unavailable");
    auto scanning = callObject(object.value(), "EditorFileSystem", "is_scanning", 36873697LL);
    if (scanning.isErr()) return scanning.error();
    auto value = scalarFromVariant<GDExtensionBool>(scanning.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (value.isErr()) return value.error();
    return value.value() != 0;
}

void GodotBridge::rememberStartupBusLayoutSetting() {
    m_startupBusLayoutSetting = projectSettingString("audio/buses/default_bus_layout");
    m_startupBusLayoutRemembered = true;
}

std::optional<std::string> GodotBridge::startupBusLayoutPath() {
    if (!m_startupBusLayoutRemembered) return std::nullopt;
    // Resolved when asked rather than at initialization, because a uid is
    // resolved through ResourceUID, which may not have read its cache yet then.
    return busLayoutPathFrom(m_startupBusLayoutSetting);
}

std::optional<std::string> GodotBridge::projectSettingString(const std::string& name) {
    auto settings = singleton("ProjectSettings");
    if (settings.isErr()) return std::nullopt;
    auto setting_name = makeString(name);
    if (setting_name.isErr()) return std::nullopt;
    // ProjectSettings.has_setting is 3927539163 and get_setting is 223050753
    // on 4.5.1, 4.6.2 and 4.7.2.
    auto present = callObject(settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                              {&setting_name.value()});
    if (present.isErr()) return std::nullopt;
    auto has = scalarFromVariant<GDExtensionBool>(present.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (has.isErr() || !has.value()) return std::nullopt;
    VariantValue fallback;
    auto value = callObject(settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                            {&setting_name.value(), &fallback});
    if (value.isErr()) return std::nullopt;
    const auto type = GodotApi::instance().variant_get_type(value.value().ptr());
    if (type != GDEXTENSION_VARIANT_TYPE_STRING && type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
        return std::nullopt;
    }
    auto text = stringFromVariant(value.value(), type);
    if (text.isErr()) return std::nullopt;
    return text.value();
}



// Phase 7B spatial reads. Both use only the attached session root viewport's
// existing World2D/World3D, so nothing here creates a world, a map or a body.
namespace bridge_internal {

Result<VariantValue> makeVector3(double x, double y, double z) {
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_VECTOR3, 3);
    if (!constructor) return Error::internal("Godot Vector3 constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    const void* arguments[] = {&x, &y, &z};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR3, native.ptr());
}

Result<VariantValue> makeVector2i(int64_t x, int64_t y) {
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_VECTOR2I, 3);
    if (!constructor) return Error::internal("Godot Vector2i constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_VECTOR2I);
    const void* arguments[] = {&x, &y};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR2I, native.ptr());
}

Result<VariantValue> makeVector3i(int64_t x, int64_t y, int64_t z) {
    auto constructor = GodotApi::instance().variant_get_ptr_constructor(
        GDEXTENSION_VARIANT_TYPE_VECTOR3I, 3);
    if (!constructor) return Error::internal("Godot Vector3i constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_VECTOR3I);
    const void* arguments[] = {&x, &y, &z};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR3I, native.ptr());
}

Result<json> integerVectorToJson(VariantValue& value, int dimensions) {
    const auto type = dimensions == 2 ? GDEXTENSION_VARIANT_TYPE_VECTOR2I
                                      : GDEXTENSION_VARIANT_TYPE_VECTOR3I;
    auto& api = GodotApi::instance();
    if (api.variant_get_type(value.ptr()) != type || !api.variant_get_ptr_getter) {
        return Error::internal("Godot returned an invalid integer vector");
    }
    auto converter = api.get_variant_to_type_constructor(type);
    if (!converter) return Error::internal("Godot integer vector conversion is unavailable");
    NativeValue native(type);
    converter(native.ptr(), value.ptr());
    native.markInitialized();
    json output = json::object();
    for (const auto* axis : {"x", "y", "z"}) {
        if (dimensions == 2 && axis[0] == 'z') break;
        NativeName name(axis);
        auto getter = name.valid() ? api.variant_get_ptr_getter(type, name.ptr()) : nullptr;
        if (!getter) return Error::internal("Godot integer vector member getter is unavailable");
        int64_t component = 0;
        getter(native.ptr(), &component);
        output[axis] = component;
    }
    return output;
}

bool isNilVariant(VariantValue& value) {
    return GodotApi::instance().variant_get_type(value.ptr()) == GDEXTENSION_VARIANT_TYPE_NIL;
}

Result<bool> objectIsClass(GDExtensionObjectPtr object, const char* class_name) {
    auto name = makeStringName(class_name);
    if (name.isErr()) return name.error();
    auto result = callObject(object, "Object", "is_class", 3927539163LL, {&name.value()});
    if (result.isErr()) return result.error();
    auto flag = scalarFromVariant<GDExtensionBool>(result.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    return flag.isOk() ? Result<bool>(flag.value() != 0) : Result<bool>(flag.error());
}

Result<std::set<int64_t>> packedIntegerSet(VariantValue& packed) {
    auto size_value = callVariant(packed, "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    std::set<int64_t> output;
    for (int64_t index = 0; index < size.value(); ++index) {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto item_value = callVariant(packed, "get", {&index_value.value()});
        if (item_value.isErr()) return item_value.error();
        auto item = scalarFromVariant<int64_t>(item_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (item.isErr()) return item.error();
        output.insert(item.value());
    }
    return output;
}


Result<json> pointVariantToJson(VariantValue& value, int dimension) {
    auto& api = GodotApi::instance();
    const auto type = dimension == 2 ? GDEXTENSION_VARIANT_TYPE_VECTOR2 : GDEXTENSION_VARIANT_TYPE_VECTOR3;
    if (api.variant_get_type(value.ptr()) != type) {
        return Error::internal("Godot returned a point of the wrong dimension");
    }
    if (!api.variant_get_ptr_getter) return Error::internal("Godot built-in member getter API is unavailable");
    auto to_native = api.get_variant_to_type_constructor(type);
    if (!to_native) return Error::internal("Missing Variant-to-native vector conversion");
    NativeValue native(type);
    to_native(native.ptr(), value.ptr());
    native.markInitialized();
    json output = json::object();
    for (const auto* axis : {"x", "y", "z"}) {
        if (dimension == 2 && axis[0] == 'z') break;
        NativeName name(axis);
        if (!name.valid()) return Error::internal("Failed to construct vector member name");
        auto getter = api.variant_get_ptr_getter(type, name.ptr());
        if (!getter) return Error::internal("Godot vector member getter is unavailable");
        double component = 0.0;
        getter(native.ptr(), &component);
        output[axis] = component;
    }
    return output;
}


} // namespace


// Phase 7B animation. The list reads an AnimationPlayer's library through the
// pinned AnimationMixer and Animation binds and never touches a key. The play
// is game-only, calls AnimationPlayer.play once, and rereads state through
// Object.get because get_current_animation changes hash between 4.5 and 4.7.
namespace bridge_internal {


} // namespace

namespace bridge_internal {

// The project directory on disk. project.godot is the only record of what this
// project defines rather than what the engine defaults to, and two checks below
// need to read it.
Result<std::string> projectDirectoryOnDisk() {
    auto settings = singleton("ProjectSettings");
    if (settings.isErr()) return settings.error();
    auto resource_root = makeString("res://");
    if (resource_root.isErr()) return resource_root.error();
    auto globalized = callObject(settings.value(), "ProjectSettings", "globalize_path", 3135753539LL,
                                 {&resource_root.value()});
    if (globalized.isErr()) return globalized.error();
    return stringFromVariant(globalized.value(), GDEXTENSION_VARIANT_TYPE_STRING);
}

bool settingTakesTypedValue(GDExtensionVariantType held) {
    switch (held) {
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            return true;
        default:
            return compoundMembers(held) != nullptr ||
                   packedElementType(held) != GDEXTENSION_VARIANT_TYPE_NIL;
    }
}

} // namespace

// Every method execute() serves, and whether it runs before the editor is
// looked up: a game has no EditorInterface, so the methods a game can serve,
// and the ones that look the editor up themselves, run before that (#1255).
const std::unordered_map<std::string_view, GodotBridge::BridgeMethod>& GodotBridge::methodTable() {
    static const std::unordered_map<std::string_view, BridgeMethod> table = {
        {"runtime.evalGdscript", {&GodotBridge::bridgeRuntimeEvalGdscript, nullptr}},
        {"runtime.getTree", {&GodotBridge::bridgeRuntimeTreeAndPause, nullptr}},
        {"runtime.setPaused", {&GodotBridge::bridgeRuntimeTreeAndPause, nullptr}},
        {"runtime.stop", {&GodotBridge::bridgeRuntimeTreeAndPause, nullptr}},
        {"runtime.injectInput", {&GodotBridge::bridgeRuntimeInjectInput, nullptr}},
        {"runtime.missingInputActions", {&GodotBridge::bridgeRuntimeMissingInputActions, nullptr}},
        {"physics.raycast", {&GodotBridge::bridgePhysicsRaycast, nullptr}},
        {"physics.raycastBatch", {&GodotBridge::bridgePhysicsRaycastBatch, nullptr}},
        {"physics.clearance", {&GodotBridge::bridgePhysicsClearance, nullptr}},
        {"vision.frustumQuery", {&GodotBridge::bridgeVisionFrustumQuery, nullptr}},
        {"preview.renderGhost", {&GodotBridge::bridgePreviewRenderGhost, nullptr}},
        {"preview.clearGhosts", {&GodotBridge::bridgePreviewClearGhosts, nullptr}},
        {"nav.queryPath", {&GodotBridge::bridgeNavQueryPath, nullptr}},
        {"ui.listControls", {&GodotBridge::bridgeUiListControls, nullptr}},
        {"ui.hitTest", {&GodotBridge::bridgeUiHitTest, nullptr}},
        {"anim.listTracks", {&GodotBridge::bridgeAnimListTracks, nullptr}},
        {"anim.playTrack", {&GodotBridge::bridgeAnimPlayTrack, nullptr}},
        {"anim.addLibrary", {&GodotBridge::bridgeAnimAddLibrary, nullptr}},
        {"export.reloadPresets", {&GodotBridge::bridgeExportReloadPresets, nullptr}},
        {"editor.getProtocolServers", {&GodotBridge::bridgeEditorGetProtocolServers, nullptr}},
        {"editor.openScenes", {&GodotBridge::bridgeEditorOpenScenes, nullptr}},
        {"resource.refreshCached", {&GodotBridge::bridgeResourceRefreshCached, nullptr}},
        {"asset.readImportedStream", {&GodotBridge::bridgeAssetReadImportedStream, nullptr}},
        {"audio.listBuses", {&GodotBridge::bridgeAudioListBuses, nullptr}},
        {"audio.configureBus", {&GodotBridge::bridgeAudioConfigureBus, nullptr}},
        {"editor.getRecoveryState", {nullptr, &GodotBridge::bridgeEditorGetRecoveryState}},
        {"tilemap.getUsedRect", {nullptr, &GodotBridge::bridgeTilemapGetUsedRect}},
        {"tilemap.setCells", {nullptr, &GodotBridge::bridgeTilemapSetCells}},
        {"gridmap.setCells", {nullptr, &GodotBridge::bridgeGridmapSetCells}},
        {"vision.setCameraTransform", {nullptr, &GodotBridge::bridgeVisionSetCameraTransform}},
        {"vision.toggleDebugDraw", {nullptr, &GodotBridge::bridgeVisionToggleDebugDraw}},
        {"signal.listConnections", {nullptr, &GodotBridge::bridgeSignals}},
        {"signal.connect", {nullptr, &GodotBridge::bridgeSignals}},
        {"signal.disconnect", {nullptr, &GodotBridge::bridgeSignals}},
        {"signal.emit", {nullptr, &GodotBridge::bridgeSignals}},
        {"project.resolveUids", {nullptr, &GodotBridge::bridgeProjectResolveUids}},
        {"project.getSetting", {nullptr, &GodotBridge::bridgeProjectSettings}},
        {"project.setSetting", {nullptr, &GodotBridge::bridgeProjectSettings}},
        {"project.listAutoloads", {nullptr, &GodotBridge::bridgeProjectAutoloads}},
        {"project.setAutoload", {nullptr, &GodotBridge::bridgeProjectAutoloads}},
        {"project.removeAutoload", {nullptr, &GodotBridge::bridgeProjectAutoloads}},
        {"project.listInputActions", {nullptr, &GodotBridge::bridgeProjectInputActions}},
        {"project.setInputAction", {nullptr, &GodotBridge::bridgeProjectInputActions}},
        {"project.removeInputAction", {nullptr, &GodotBridge::bridgeProjectInputActions}},
        {"script.attachToNode", {nullptr, &GodotBridge::bridgeScriptAttachment}},
        {"script.detachFromNode", {nullptr, &GodotBridge::bridgeScriptAttachment}},
        {"audio.addBus", {nullptr, &GodotBridge::bridgeAudioAddBus}},
        {"engine.classExists", {nullptr, &GodotBridge::bridgeEngineClassExists}},
        {"scene.listGroups", {nullptr, &GodotBridge::bridgeSceneGroups}},
        {"scene.addToGroup", {nullptr, &GodotBridge::bridgeSceneGroups}},
        {"scene.removeFromGroup", {nullptr, &GodotBridge::bridgeSceneGroups}},
        {"scene.getGroupMembers", {nullptr, &GodotBridge::bridgeSceneGroups}},
        {"scene.create", {nullptr, &GodotBridge::bridgeSceneFiles}},
        {"scene.open", {nullptr, &GodotBridge::bridgeSceneFiles}},
        {"scene.close", {nullptr, &GodotBridge::bridgeSceneFiles}},
        {"scene.packBranch", {nullptr, &GodotBridge::bridgeSceneFiles}},
        {"editor.getSelection", {nullptr, &GodotBridge::bridgeEditorGetSelection}},
        {"editor.getState", {nullptr, &GodotBridge::bridgeEditorState}},
        {"scene.getHierarchy", {nullptr, &GodotBridge::bridgeEditorState}},
        {"shader.listUniforms", {nullptr, &GodotBridge::bridgeShaders}},
        {"shader.setUniform", {nullptr, &GodotBridge::bridgeShaders}},
        {"shader.getVisualGraph", {nullptr, &GodotBridge::bridgeShaders}},
        {"scene.getProperty", {nullptr, &GodotBridge::bridgeSceneProperties}},
        {"scene.setProperty", {nullptr, &GodotBridge::bridgeSceneProperties}},
        {"scene.instantiateNode", {nullptr, &GodotBridge::bridgeSceneInstantiateNode}},
        {"scene.removeNode", {nullptr, &GodotBridge::bridgeSceneNodeEdits}},
        {"scene.duplicateNode", {nullptr, &GodotBridge::bridgeSceneNodeEdits}},
        {"scene.reparentNode", {nullptr, &GodotBridge::bridgeSceneNodeEdits}},
        {"editor.undoStatus", {nullptr, &GodotBridge::bridgeEditorUndoStatus}},
        {"editor.undo", {nullptr, &GodotBridge::bridgeEditorUndoRedo}},
        {"editor.redo", {nullptr, &GodotBridge::bridgeEditorUndoRedo}},
        {"editor.saveScene", {nullptr, &GodotBridge::bridgeEditorSaveScene}},
    };
    return table;
}

std::vector<std::string> GodotBridge::methodNames() {
    std::vector<std::string> names;
    for (const auto& entry : methodTable()) names.emplace_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

json GodotBridge::execute(const std::string& method, const json& params,
                          const std::string& session_kind) {
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
    if (method == "phase7SignalTest.configure") {
        static const std::set<std::string> admitted = {
            "malformed_metadata", "missing_required_api", "conversion_failure",
            "connect_postcondition_mismatch", "disconnect_postcondition_mismatch",
            "missing_destination_float_constructor",
            "connect_postcondition_mismatch_rollback_failure",
            "group_undo_registration_failure"};
        if (!hasOnlyKeys(params, {"seam"}) || !params.contains("seam") ||
            !params["seam"].is_string() || !admitted.count(params["seam"].get<std::string>())) {
            return bridgeError(400, "invalid_phase7_signal_test_seam");
        }
        g_phase7SignalTestSeam = params["seam"].get<std::string>();
        return liveResult({{"status", "configured"}});
    }
#endif
    const auto& table = methodTable();
    const auto found = table.find(method);
    if (found != table.end() && found->second.before_editor) {
        return (this->*found->second.before_editor)(method, params, session_kind);
    }
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return errorJson(editor_result.error());
    auto editor = editor_result.value();
    if (found != table.end()) {
        return (this->*found->second.with_editor)(method, params, session_kind, editor);
    }
    return errorJson(501, "No trustworthy live implementation for method: " + method);
}

json GodotBridge::bridgeRuntimeEvalGdscript(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return executeExpression(params, session_kind);
}

json GodotBridge::bridgeRuntimeTreeAndPause(const std::string& method, const json& params, const std::string& session_kind) {
    return executeRuntimeBridge(method, params, session_kind);
}

// A game has no EditorInterface, so this is one of the methods methodTable()
// runs before the editor lookup the editor-only methods depend on.
json GodotBridge::bridgeRuntimeInjectInput(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return injectInput(params, session_kind);
}

json GodotBridge::bridgeRuntimeMissingInputActions(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return inputMapMissingActions(params, session_kind);
}

json GodotBridge::bridgePhysicsRaycast(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return physicsRaycast(params, session_kind);
}

json GodotBridge::bridgePhysicsRaycastBatch(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return physicsRaycastBatch(params, session_kind);
}

json GodotBridge::bridgePhysicsClearance(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return physicsClearance(params, session_kind);
}

json GodotBridge::bridgeVisionFrustumQuery(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return visionFrustumQuery(params, session_kind);
}

json GodotBridge::bridgePreviewRenderGhost(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/) {
    return ghostPreviewRender(params);
}

json GodotBridge::bridgePreviewClearGhosts(const std::string& /*method*/, const json& params, const std::string& /*session_kind*/) {
    return ghostPreviewClear(params);
}

json GodotBridge::bridgeNavQueryPath(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return navQueryPath(params, session_kind);
}

json GodotBridge::bridgeUiListControls(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return uiListControls(params, session_kind);
}

json GodotBridge::bridgeUiHitTest(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return uiHitTest(params, session_kind);
}

json GodotBridge::bridgeAnimListTracks(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return animListTracks(params, session_kind);
}

json GodotBridge::bridgeAnimPlayTrack(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return animPlayTrack(params, session_kind);
}

json GodotBridge::bridgeAnimAddLibrary(const std::string& /*method*/, const json& params, const std::string& session_kind) {
    return animAddLibrary(params, session_kind);
}










namespace bridge_internal {


}  // namespace













namespace bridge_internal {

// The editor has two main-screen viewports and several spellings for each. Both
// lists live here and nowhere else. Field trial 03 found the cost of a second
// copy: `2d` and `canvas_item` were absent from the 2D branch, so with a Node2D
// scene open and the editor on the 3D main screen they returned a full size
// picture of the 3D grid, labelled as a capture of '2d'. For a 2D project that
// reads as an empty scene rather than as the wrong viewport.
const char* const kEditor2dIdentifiers[] = {"editor_2d", "active_editor_view_2d", "2d",
                                            "canvas_item"};
const char* const kEditor3dIdentifiers[] = {"active_editor_view", "editor_3d",
                                            "active_editor_view_3d", "3d"};

bool namesViewport(const std::string& value, const char* const* names, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (value == names[index]) return true;
    }
    return false;
}

} // namespace

std::optional<EditorViewport> selectEditorViewport(const std::string& camera_identifier) {
    if (namesViewport(camera_identifier, kEditor2dIdentifiers,
                      std::size(kEditor2dIdentifiers))) {
        return EditorViewport::TwoD;
    }
    if (namesViewport(camera_identifier, kEditor3dIdentifiers,
                      std::size(kEditor3dIdentifiers))) {
        return EditorViewport::ThreeD;
    }
    return std::nullopt;
}

std::string editorViewportIdentifierList() {
    std::string list;
    for (const auto* name : kEditor2dIdentifiers) {
        if (!list.empty()) list += ", ";
        list += name;
    }
    for (const auto* name : kEditor3dIdentifiers) {
        list += ", ";
        list += name;
    }
    return list;
}

Result<std::string> resolveGodotProjectPath() {
    auto settings = singleton("ProjectSettings");
    if (settings.isErr()) return settings.error();
    auto resource_root = makeString("res://");
    if (resource_root.isErr()) return resource_root.error();
    auto globalized = callObject(settings.value(), "ProjectSettings", "globalize_path", 3135753539LL,
                                 {&resource_root.value()});
    if (globalized.isErr()) return globalized.error();
    return stringFromVariant(globalized.value(), GDEXTENSION_VARIANT_TYPE_STRING);
}

// The property-type rejection, in words.
//
// A caller that sent the wrong JSON type has exactly one thing to look at: the
// error. Field trial 02 sent the string "1.0" for a float property, read
// "JSON value is incompatible with Godot property type 3", and concluded Didi
// rejected whole numbers for floats. The type number is knowable only from the
// GDExtension headers, the property was never named, and the one fact that
// would have ended it in a turn, that a JSON string arrived where a number was
// wanted, was the fact left out.
//
// These four are pure functions of a JSON value and a variant type number, so
// the decision and the sentence built from it are both testable without an
// engine. The set of accepted values lives in matchJsonToPropertyType alone:
// the message is built from the same answer that rejects, and cannot drift
// into describing a rule the check does not apply.

json realToJson(double value) {
    if (std::isfinite(value)) return json(value);
    if (std::isnan(value)) return json("nan");
    return json(value > 0 ? "inf" : "-inf");
}

std::optional<std::string> describeRealRangeRefusal(const std::string& property_name,
                                                    const json& value, int godot_type) {
    const auto type = static_cast<GDExtensionVariantType>(godot_type);
    const auto out_of_range = [](const json& number) {
        return number.is_number() && std::fabs(number.get<double>()) > kGodotRealMaximum;
    };
    const auto refuse = [&](const std::string& what, const json& number) {
        return "Property \"" + property_name + "\"" + what +
               " is stored as a 32-bit float, which cannot hold " + number.dump() +
               ". The largest magnitude it can hold is about 3.4e38; anything beyond that "
               "becomes inf, which would propagate through the scene and read back as a "
               "value JSON cannot spell.";
    };
    // The first real too big for the slot it lands in, and where it is: a
    // member name, a path of them through a compound value such as
    // basis.x.y, or an index into a packed array. A PackedFloat64Array holds
    // doubles, and is the one place a real is not real_t.
    std::function<std::optional<std::pair<std::string, json>>(const json&, GDExtensionVariantType,
                                                              const std::string&)>
        first_out_of_range = [&](const json& at, GDExtensionVariantType at_type,
                                 const std::string& where) -> std::optional<std::pair<std::string, json>> {
        const auto named = [&](const std::string& member) {
            return where.empty() ? member : where + "." + member;
        };
        if (at_type == GDEXTENSION_VARIANT_TYPE_FLOAT) {
            if (out_of_range(at)) return std::make_pair(where, at);
            return std::nullopt;
        }
        if (at.is_object()) {
            for (const auto* member : realMembersOfPropertyType(at_type)) {
                const auto found = at.find(member);
                if (found != at.end() && out_of_range(*found)) return std::make_pair(named(member), *found);
            }
            if (const auto* members = compoundMembers(at_type)) {
                for (const auto& member : *members) {
                    const auto found = at.find(member.name);
                    if (found == at.end()) continue;
                    if (auto inside = first_out_of_range(*found, member.type, named(member.name))) return inside;
                }
            }
        }
        const auto element = packedElementType(at_type);
        if (at.is_array() && element != GDEXTENSION_VARIANT_TYPE_NIL &&
            at_type != GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY) {
            for (size_t index = 0; index < at.size(); ++index) {
                if (auto inside = first_out_of_range(at[index], element, where + "[" + std::to_string(index) + "]")) {
                    return inside;
                }
            }
        }
        return std::nullopt;
    };
    const auto found = first_out_of_range(value, type, "");
    if (!found.has_value()) return std::nullopt;
    if (found->first.empty()) return refuse("", found->second);
    return refuse(" component \"" + found->first + "\"", found->second);
}

const std::map<std::string, std::string>& bridgeErrorSentenceTable() {
    return bridgeErrorSentences();
}

std::optional<int64_t> dictionaryIntKey(std::string_view text) {
    const bool negative = !text.empty() && text.front() == '-';
    const auto digits = negative ? text.substr(1) : text;
    if (digits.empty() || digits.size() > 19) return std::nullopt;
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;
    if (negative && digits == "0") return std::nullopt;
    uint64_t magnitude = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<uint64_t>(c - '0');
        if (magnitude > (UINT64_MAX - digit) / 10) return std::nullopt;
        magnitude = magnitude * 10 + digit;
    }
    const auto limit = static_cast<uint64_t>(INT64_MAX) + (negative ? 1u : 0u);
    if (magnitude > limit) return std::nullopt;
    if (negative) {
        return magnitude == static_cast<uint64_t>(INT64_MAX) + 1u ? INT64_MIN
                                                                 : -static_cast<int64_t>(magnitude);
    }
    return static_cast<int64_t>(magnitude);
}

PropertyTypeMatch matchJsonToPropertyType(const json& value, int godot_type) {
    // Delegates the accept/reject decision to propertyTypeAcceptsJson rather
    // than repeating it. Two copies of this rule already disagreed once: the
    // rewrite that produced this function was written before an int property
    // learned to take a whole-number real such as 9.0, and merging the two
    // without noticing would have quietly reverted that while every unit test
    // kept passing, because those exercise the helper and not this path. One
    // source of truth is the only version of this that stays true.
    switch (godot_type) {
        case GDEXTENSION_VARIANT_TYPE_NIL:
        case GDEXTENSION_VARIANT_TYPE_BOOL:
        case GDEXTENSION_VARIANT_TYPE_INT:
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
        case GDEXTENSION_VARIANT_TYPE_STRING:
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
        case GDEXTENSION_VARIANT_TYPE_COLOR:
        case GDEXTENSION_VARIANT_TYPE_OBJECT:
        case GDEXTENSION_VARIANT_TYPE_ARRAY:
        case GDEXTENSION_VARIANT_TYPE_DICTIONARY:
            break;
        default:
            if (compoundMembers(static_cast<GDExtensionVariantType>(godot_type)) == nullptr &&
                packedElementType(static_cast<GDExtensionVariantType>(godot_type)) ==
                    GDEXTENSION_VARIANT_TYPE_NIL) {
                return PropertyTypeMatch::UnsupportedPropertyType;
            }
            break;
    }
    return propertyTypeAcceptsJson(value, static_cast<GDExtensionVariantType>(godot_type))
               ? PropertyTypeMatch::Compatible
               : PropertyTypeMatch::Incompatible;
}

std::string godotVariantTypeName(int godot_type) {
    switch (godot_type) {
        case GDEXTENSION_VARIANT_TYPE_NIL: return "Nil";
        case GDEXTENSION_VARIANT_TYPE_BOOL: return "bool";
        case GDEXTENSION_VARIANT_TYPE_INT: return "int";
        case GDEXTENSION_VARIANT_TYPE_FLOAT: return "float";
        case GDEXTENSION_VARIANT_TYPE_STRING: return "String";
        case GDEXTENSION_VARIANT_TYPE_VECTOR2: return "Vector2";
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I: return "Vector2i";
        case GDEXTENSION_VARIANT_TYPE_RECT2: return "Rect2";
        case GDEXTENSION_VARIANT_TYPE_RECT2I: return "Rect2i";
        case GDEXTENSION_VARIANT_TYPE_VECTOR3: return "Vector3";
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I: return "Vector3i";
        case GDEXTENSION_VARIANT_TYPE_TRANSFORM2D: return "Transform2D";
        case GDEXTENSION_VARIANT_TYPE_VECTOR4: return "Vector4";
        case GDEXTENSION_VARIANT_TYPE_VECTOR4I: return "Vector4i";
        case GDEXTENSION_VARIANT_TYPE_PLANE: return "Plane";
        case GDEXTENSION_VARIANT_TYPE_QUATERNION: return "Quaternion";
        case GDEXTENSION_VARIANT_TYPE_AABB: return "AABB";
        case GDEXTENSION_VARIANT_TYPE_BASIS: return "Basis";
        case GDEXTENSION_VARIANT_TYPE_TRANSFORM3D: return "Transform3D";
        case GDEXTENSION_VARIANT_TYPE_PROJECTION: return "Projection";
        case GDEXTENSION_VARIANT_TYPE_COLOR: return "Color";
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME: return "StringName";
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH: return "NodePath";
        case GDEXTENSION_VARIANT_TYPE_RID: return "RID";
        case GDEXTENSION_VARIANT_TYPE_OBJECT: return "Object";
        case GDEXTENSION_VARIANT_TYPE_CALLABLE: return "Callable";
        case GDEXTENSION_VARIANT_TYPE_SIGNAL: return "Signal";
        case GDEXTENSION_VARIANT_TYPE_DICTIONARY: return "Dictionary";
        case GDEXTENSION_VARIANT_TYPE_ARRAY: return "Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY: return "PackedByteArray";
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY: return "PackedInt32Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY: return "PackedInt64Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY: return "PackedFloat32Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY: return "PackedFloat64Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY: return "PackedStringArray";
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR2_ARRAY: return "PackedVector2Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY: return "PackedVector3Array";
        case GDEXTENSION_VARIANT_TYPE_PACKED_COLOR_ARRAY: return "PackedColorArray";
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY: return "PackedVector4Array";
        default: break;
    }
    // A type this build has no name for still gets said out loud rather than
    // printed as a bare integer with no clue what kind of number it is.
    return "unnamed Godot type " + std::to_string(godot_type);
}

std::string jsonValueTypeName(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    if (value.is_object()) return "object";
    return "value";
}

std::string describePropertyTypeMismatch(const std::string& property_name, const json& value,
                                         int godot_type) {
    // What to send instead, phrased as the caller would type it. Coercing the
    // value here would be a false success of exactly the kind #213 to #217 were
    // about, so the advice is all this adds: the rejection itself stands.
    std::string remedy;
    switch (godot_type) {
        case GDEXTENSION_VARIANT_TYPE_NIL:
            remedy = "Send null.";
            break;
        case GDEXTENSION_VARIANT_TYPE_BOOL:
            remedy = "Send true or false, unquoted.";
            break;
        case GDEXTENSION_VARIANT_TYPE_INT:
            remedy = "Send a whole number, for example 12, not a quoted string.";
            break;
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
            remedy = "Send a number, for example 1.0, not a quoted string. "
                     "Whole numbers such as 1 are accepted for float properties.";
            break;
        case GDEXTENSION_VARIANT_TYPE_STRING:
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
            remedy = "Send a quoted string, for example \"text\".";
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR2:
            remedy = "Send an object with x and y numbers, for example {\"x\": 480, \"y\": 270}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR2I:
            remedy = "Send an object with whole-number x and y, for example {\"x\": 32, \"y\": 32}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR3:
            remedy = "Send an object with x, y and z numbers, for example "
                     "{\"x\": 0, \"y\": 1.5, \"z\": -2}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR3I:
            remedy = "Send an object with whole-number x, y and z, for example "
                     "{\"x\": 1, \"y\": 0, \"z\": 3}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_COLOR:
            remedy = "Send an object with r, g and b numbers and an optional a, for example "
                     "{\"r\": 1, \"g\": 0.5, \"b\": 0}, or a \"#rrggbb\" or \"#rrggbbaa\" string.";
            break;
        case GDEXTENSION_VARIANT_TYPE_OBJECT:
            remedy = "Send a res:// path to the resource, for example "
                     "\"res://tiles/arena_tileset.tres\", or null to clear the slot.";
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR4:
        case GDEXTENSION_VARIANT_TYPE_QUATERNION:
            remedy = "Send an object with x, y, z and w numbers, for example "
                     "{\"x\": 0, \"y\": 0, \"z\": 0, \"w\": 1}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_VECTOR4I:
            remedy = "Send an object with whole-number x, y, z and w, for example "
                     "{\"x\": 1, \"y\": 2, \"z\": 3, \"w\": 4}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_RECT2:
        case GDEXTENSION_VARIANT_TYPE_RECT2I:
            remedy = "Send an object with position and size, each an x and y, for example "
                     "{\"position\": {\"x\": 0, \"y\": 0}, \"size\": {\"x\": 64, \"y\": 32}}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_AABB:
            remedy = "Send an object with position and size, each an x, y and z, for example "
                     "{\"position\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"size\": {\"x\": 1, \"y\": 1, \"z\": 1}}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PLANE:
            remedy = "Send an object with a normal and a distance, for example "
                     "{\"normal\": {\"x\": 0, \"y\": 1, \"z\": 0}, \"d\": 0}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_TRANSFORM2D:
            remedy = "Send an object with the axes x and y and the origin, each an x and y. The "
                     "identity is {\"x\": {\"x\": 1, \"y\": 0}, \"y\": {\"x\": 0, \"y\": 1}, "
                     "\"origin\": {\"x\": 0, \"y\": 0}}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_BASIS:
            remedy = "Send an object with the columns x, y and z, each an x, y and z. The identity is "
                     "{\"x\": {\"x\": 1, \"y\": 0, \"z\": 0}, \"y\": {\"x\": 0, \"y\": 1, \"z\": 0}, "
                     "\"z\": {\"x\": 0, \"y\": 0, \"z\": 1}}.";
            break;
        case GDEXTENSION_VARIANT_TYPE_TRANSFORM3D:
            remedy = "Send an object with a basis, its columns x, y and z each an x, y and z, and an "
                     "origin with x, y and z: the shape scene_get_property reads it in.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PROJECTION:
            remedy = "Send an object with the columns x, y, z and w, each an x, y, z and w.";
            break;
        case GDEXTENSION_VARIANT_TYPE_ARRAY:
            remedy = "Send a JSON array.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY:
            remedy = "Send an array of whole numbers from 0 to 255.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT32_ARRAY:
            remedy = "Send an array of whole numbers that fit in 32 bits.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_INT64_ARRAY:
            remedy = "Send an array of whole numbers.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT32_ARRAY:
        case GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY:
            remedy = "Send an array of numbers.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_STRING_ARRAY:
            remedy = "Send an array of strings.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR2_ARRAY:
            remedy = "Send an array of objects with x and y numbers, for example "
                     "[{\"x\": 0, \"y\": 0}, {\"x\": 32, \"y\": 0}].";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY:
            remedy = "Send an array of objects with x, y and z numbers.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY:
            remedy = "Send an array of objects with x, y, z and w numbers.";
            break;
        case GDEXTENSION_VARIANT_TYPE_PACKED_COLOR_ARRAY:
            remedy = "Send an array of colours, each an object with r, g and b numbers and an "
                     "optional a, or a \"#rrggbb\" string.";
            break;
        default:
            remedy = "Send a value of that type.";
            break;
    }

    // Quoting the value back is what makes a JSON string visible as one: the
    // tester who sent "1.0" saw a message that could equally have described the
    // number 1.0, which is why they read it as a rejection of whole numbers.
    std::string received = value.dump();
    constexpr size_t kMaxReceivedChars = 60;
    if (received.size() > kMaxReceivedChars) {
        received = received.substr(0, kMaxReceivedChars) + "...";
    }

    const std::string type_name = godotVariantTypeName(godot_type);
    const std::string article =
        (!type_name.empty() && std::strchr("AEIOUaeiou", type_name[0]) != nullptr) ? "an " : "a ";
    return "Property \"" + property_name + "\" is " + article + type_name + "; received a JSON " +
           jsonValueTypeName(value) + " (" + received + "). " + remedy;
}

} // namespace godot
} // namespace didi
