#include "bridge_internal.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/common/connection_flags.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <tuple>
#include <utility>

namespace didi::godot {
using namespace bridge_internal;
namespace {
// The sentence script_check_syntax already carries for this condition, so the
// same fact reads the same way whichever tool the caller met it through.
const char* const kAutoloadRestartNote =
    "A newly registered autoload does not exist in the editor that registered it until that "
    "editor restarts, and a script naming one cannot compile until then, so its methods are not "
    "on the node yet. The engine resolves the identifier at run time. Do not rewrite the script "
    "for this: restart the editor. editor_reload_project is a file rescan and does not register "
    "it.";

// Why a method the caller can see in the file is not on the node.
//
// A GDScript that will not compile still gets assigned to the node -- the
// scene loader sets the Script resource whether or not it parsed -- but no
// script instance is created, so Object.has_method answers false for every
// method the file declares. The refusal that produced named the method and
// pointed the caller at a name that was already right (#729).
//
// The commonest cause by far is the one project_set_autoload creates: the
// singleton is registered and persisted, and the editor does not know it until
// it restarts, so every script that names it fails to compile in the meantime.
// script_check_syntax already carries a note for this exact condition; the
// connect path carried nothing.
struct UncompiledScript {
    bool found{false};
    std::string path;
    std::vector<std::string> unresolved_autoloads;
};

// The autoload names this project has registered, read from ProjectSettings.
// Empty when the settings cannot be read, which makes the caller fall back to
// the plain "did not compile" answer rather than inventing a cause.
std::vector<std::string> registeredAutoloadNames() {
    std::vector<std::string> names;
    auto project_settings = singleton("ProjectSettings");
    if (project_settings.isErr()) return names;
    auto properties = callObject(project_settings.value(), "Object", "get_property_list", 3995934104LL);
    if (properties.isErr()) return names;
    auto size_value = callVariant(properties.value(), "size");
    if (size_value.isErr()) return names;
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return names;
    auto name_key = makeString("name");
    if (name_key.isErr()) return names;
    for (int64_t index = 0; index < size.value(); ++index) {
        auto position = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (position.isErr()) continue;
        auto descriptor = callVariant(properties.value(), "get", {&position.value()});
        if (descriptor.isErr()) continue;
        auto property_name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
        if (property_name_value.isErr()) continue;
        const auto type = GodotApi::instance().variant_get_type(property_name_value.value().ptr());
        if (type != GDEXTENSION_VARIANT_TYPE_STRING && type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto property_name = stringFromVariant(property_name_value.value(), type);
        if (property_name.isErr()) continue;
        if (!strings::startsWith(property_name.value(), "autoload/") ||
            property_name.value().size() <= 9) {
            continue;
        }
        names.push_back(property_name.value().substr(9));
    }
    return names;
}

// Whether `name` appears in `source` as an identifier rather than inside a
// longer word. Deliberately textual: it is used to name a likely cause in a
// refusal, not to decide anything.
bool sourceNamesIdentifier(const std::string& source, const std::string& name) {
    if (name.empty()) return false;
    const auto is_word = [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    };
    size_t at = source.find(name);
    while (at != std::string::npos) {
        const bool left_clear = at == 0 || !is_word(static_cast<unsigned char>(source[at - 1]));
        const size_t after = at + name.size();
        const bool right_clear =
            after >= source.size() || !is_word(static_cast<unsigned char>(source[after]));
        if (left_clear && right_clear) return true;
        at = source.find(name, at + 1);
    }
    return false;
}

// Whether `source` declares `func <name>(`, allowing `static func` and any
// spacing. Textual, like the autoload scan above, and used for the same
// purpose: to say which of two causes a refusal has.
bool sourceDeclaresFunction(const std::string& source, const std::string& name) {
    if (name.empty()) return false;
    size_t at = source.find("func");
    while (at != std::string::npos) {
        const bool left_clear =
            at == 0 || !(std::isalnum(static_cast<unsigned char>(source[at - 1])) ||
                         source[at - 1] == '_');
        size_t cursor = at + 4;
        if (left_clear && cursor < source.size() &&
            (source[cursor] == ' ' || source[cursor] == '\t')) {
            while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t')) {
                ++cursor;
            }
            if (source.compare(cursor, name.size(), name) == 0) {
                cursor += name.size();
                while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t')) {
                    ++cursor;
                }
                if (cursor < source.size() && source[cursor] == '(') return true;
            }
        }
        at = source.find("func", at + 4);
    }
    return false;
}

// Whether the node's script declares this method while the engine does not have
// it, which is what a script that did not compile looks like from outside.
//
// can_instantiate is not the test, and is the obvious wrong one: in the editor
// it is false for every script that is not @tool, because editor scripting is
// off, so it would call a perfectly healthy script uncompiled. Script has no
// bound is_valid either. The file is the witness -- which is exactly the
// finding: the method is right there in the source, and the engine does not
// have it.
UncompiledScript uncompiledScriptOn(GDExtensionObjectPtr node, const std::string& method) {
    UncompiledScript report;
    auto script_value = callObject(node, "Object", "get_script", 1214101251LL);
    if (script_value.isErr()) return report;
    auto script = objectFromVariant(script_value.value());
    if (script.isErr() || !script.value()) return report;

    auto source_value = callObject(script.value(), "Script", "get_source_code", 201670096LL);
    if (source_value.isErr()) return report;
    auto source = stringFromVariant(source_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
    if (source.isErr()) return report;
    if (!sourceDeclaresFunction(source.value(), method)) return report;

    report.found = true;
    auto path = callObject(script.value(), "Resource", "get_path", 201670096LL);
    if (path.isOk()) {
        auto text = stringFromVariant(path.value(), GDEXTENSION_VARIANT_TYPE_STRING);
        if (text.isOk()) report.path = text.value();
    }
    for (const auto& autoload : registeredAutoloadNames()) {
        if (sourceNamesIdentifier(source.value(), autoload)) {
            report.unresolved_autoloads.push_back(autoload);
        }
    }
    return report;
}

} // namespace

json GodotBridge::bridgeSignals(const std::string& method, const json& params, const std::string& session_kind, GDExtensionObjectPtr editor) {
    if (session_kind != "editor") return bridgeError(409, "session_kind_rejected");

    auto bounded_string = [](const json& value, size_t minimum, size_t maximum) {
        if (!value.is_string()) return false;
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() < minimum || text.size() > maximum) return false;
        try {
            (void)json(text).dump();
            return true;
        } catch (const json::exception&) {
            return false;
        }
    };
    std::function<bool(const json&, int)> valid_emit_value =
        [&](const json& value, int depth) {
            if (depth > 8) return false;
            if (value.is_null() || value.is_boolean()) return true;
            if (value.is_number_unsigned()) {
                return value.get<uint64_t>() <=
                       static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            }
            if (value.is_number_integer()) return true;
            if (value.is_number_float()) return std::isfinite(value.get<double>());
            if (value.is_string()) return bounded_string(value, 0, 4096);
            if (value.is_array()) {
                if (value.size() > 64) return false;
                for (const auto& element : value) {
                    if (!valid_emit_value(element, depth + 1)) return false;
                }
                return true;
            }
            if (value.is_object()) {
                if (value.size() > 64) return false;
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (!bounded_string(json(it.key()), 0, 4096) ||
                        !valid_emit_value(it.value(), depth + 1)) {
                        return false;
                    }
                }
                return true;
            }
            return false;
        };
    auto utf8_prefix = [](const std::string& text, size_t maximum) -> Result<std::string> {
        size_t index = 0;
        size_t accepted = 0;
        while (index < text.size()) {
            const auto first = static_cast<unsigned char>(text[index]);
            size_t length = 0;
            if (first <= 0x7f) {
                length = 1;
            } else if (first >= 0xc2 && first <= 0xdf) {
                length = 2;
            } else if (first >= 0xe0 && first <= 0xef) {
                length = 3;
            } else if (first >= 0xf0 && first <= 0xf4) {
                length = 4;
            } else {
                return Error::internal("Godot returned invalid UTF-8 signal metadata");
            }
            if (index + length > text.size()) {
                return Error::internal("Godot returned truncated UTF-8 signal metadata");
            }
            for (size_t continuation = 1; continuation < length; ++continuation) {
                const auto byte = static_cast<unsigned char>(text[index + continuation]);
                if ((byte & 0xc0) != 0x80) {
                    return Error::internal("Godot returned invalid UTF-8 signal metadata");
                }
            }
            if (length == 3) {
                const auto second = static_cast<unsigned char>(text[index + 1]);
                if ((first == 0xe0 && second < 0xa0) ||
                    (first == 0xed && second >= 0xa0)) {
                    return Error::internal("Godot returned invalid UTF-8 signal metadata");
                }
            }
            if (length == 4) {
                const auto second = static_cast<unsigned char>(text[index + 1]);
                if ((first == 0xf0 && second < 0x90) ||
                    (first == 0xf4 && second >= 0x90)) {
                    return Error::internal("Godot returned invalid UTF-8 signal metadata");
                }
            }
            if (index + length > maximum) break;
            index += length;
            accepted = index;
        }
        return text.substr(0, accepted);
    };
    auto array_size = [](VariantValue& value) -> Result<int64_t> {
        auto result = callVariant(value, "size");
        if (result.isErr()) return result.error();
        return scalarFromVariant<int64_t>(result.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    auto array_at = [](VariantValue& value, int64_t index) -> Result<VariantValue> {
        auto native_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (native_index.isErr()) return native_index.error();
        return callVariant(value, "get", {&native_index.value()});
    };
    auto dictionary_field = [](VariantValue& value, const char* key) -> Result<VariantValue> {
        auto native_key = makeString(key);
        if (native_key.isErr()) return native_key.error();
        return callVariant(value, "get", {&native_key.value()});
    };
    auto make_callable = [](GDExtensionObjectPtr object,
                            const std::string& method_name) -> Result<VariantValue> {
        auto& api = GodotApi::instance();
        auto constructor = api.variant_get_ptr_constructor(
            GDEXTENSION_VARIANT_TYPE_CALLABLE, 2);
        if (!constructor) return Error::internal("Normal Callable constructor is unavailable");
        NativeName native_method(method_name);
        if (!native_method.valid()) return Error::internal("Callable method name is unavailable");
        NativeValue native_callable(GDEXTENSION_VARIANT_TYPE_CALLABLE);
        const void* arguments[] = {&object, native_method.ptr()};
        constructor(native_callable.ptr(), arguments);
        native_callable.markInitialized();
        return variantFromNative(GDEXTENSION_VARIANT_TYPE_CALLABLE,
                                 native_callable.ptr());
    };
    auto type_name = [](int64_t type) -> const char* {
        static constexpr std::array<const char*, 39> names = {{
            "nil", "bool", "int", "float", "string", "vector2", "vector2i",
            "rect2", "rect2i", "vector3", "vector3i", "transform2d", "vector4",
            "vector4i", "plane", "quaternion", "aabb", "basis", "transform3d",
            "projection", "color", "string_name", "node_path", "rid", "object",
            "callable", "signal", "dictionary", "array", "packed_byte_array",
            "packed_int32_array", "packed_int64_array", "packed_float32_array",
            "packed_float64_array", "packed_string_array", "packed_vector2_array",
            "packed_vector3_array", "packed_color_array", "packed_vector4_array"}};
        return type >= 0 && type < static_cast<int64_t>(names.size())
                   ? names[static_cast<size_t>(type)]
                   : "unknown";
    };

    constexpr int64_t kSignalNativeWorkCeiling = 1024;
    constexpr int64_t kSignalArgumentWorkCeiling = 64;
    auto utf8_less = [](const std::string& left, const std::string& right) {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](char a, char b) {
                return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
            });
    };
    struct SignalArgumentMetadata {
        std::string name;
        int64_t type{GDEXTENSION_VARIANT_TYPE_NIL};
        int64_t hint{0};
        std::string hint_string;
        int64_t usage{0};
        std::string class_name;
    };
    auto parse_argument_metadata = [](const json& argument)
        -> Result<SignalArgumentMetadata> {
        if (!argument.is_object() || !argument.contains("name") ||
            !argument["name"].is_string() || !argument.contains("type") ||
            !argument["type"].is_number_integer() || !argument.contains("hint") ||
            !argument["hint"].is_number_integer() || !argument.contains("hint_string") ||
            !argument["hint_string"].is_string() || !argument.contains("usage") ||
            !argument["usage"].is_number_integer() || !argument.contains("class_name") ||
            !argument["class_name"].is_string()) {
            return Error::internal("Godot PropertyInfo metadata is malformed");
        }
        const auto type = argument["type"].get<int64_t>();
        if (type < GDEXTENSION_VARIANT_TYPE_NIL ||
            type > GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR4_ARRAY) {
            return Error::internal("Godot PropertyInfo Variant type is invalid");
        }
        return SignalArgumentMetadata{
            argument["name"].get<std::string>(), type,
            argument["hint"].get<int64_t>(),
            argument["hint_string"].get<std::string>(),
            argument["usage"].get<int64_t>(),
            argument["class_name"].get<std::string>()};
    };
    struct SignalMetadata {
        std::vector<SignalArgumentMetadata> arguments;
    };
    auto signal_metadata = [&](GDExtensionObjectPtr object,
                               const std::string& signal_name) -> Result<SignalMetadata> {
        auto signals = callObject(object, "Object", "get_signal_list", 3995934104LL);
        if (signals.isErr()) return signals.error();
        auto signal_count = array_size(signals.value());
        if (signal_count.isErr() || signal_count.value() < 0) {
            return Error::internal("Godot signal metadata is malformed");
        }
        if (signal_count.value() > kSignalNativeWorkCeiling) {
            return bridgeRefusal(413, "signal_metadata_work_limit");
        }
        for (int64_t index = 0; index < signal_count.value(); ++index) {
            auto native_descriptor = array_at(signals.value(), index);
            if (native_descriptor.isErr()) return native_descriptor.error();
            auto serialized = variantToJson(native_descriptor.value());
            if (serialized.isErr()) {
                return Error::internal("Godot signal metadata is malformed");
            }
            const auto& descriptor = serialized.value();
            if (!descriptor.is_object() || !descriptor.contains("name") ||
                !descriptor["name"].is_string()) {
                return Error::internal("Godot signal descriptor is malformed");
            }
            if (descriptor["name"].get<std::string>() != signal_name) continue;
            const auto arguments = descriptor.value("args", json::array());
            if (!arguments.is_array()) {
                return Error::internal("Godot signal arguments are malformed");
            }
            if (arguments.size() > static_cast<size_t>(kSignalArgumentWorkCeiling)) {
                return bridgeRefusal(413, "signal_argument_metadata_work_limit");
            }
            SignalMetadata metadata;
            metadata.arguments.reserve(arguments.size());
            for (const auto& argument : arguments) {
                auto parsed = parse_argument_metadata(argument);
                if (parsed.isErr()) return parsed.error();
                metadata.arguments.push_back(std::move(parsed.value()));
            }
            return metadata;
        }
        return Error::notFound("Declared signal not found: " + signal_name);
    };
    struct MethodMetadata {
        int64_t required_arguments{0};
        int64_t total_arguments{0};
        bool vararg{false};
    };
    auto method_metadata = [&](GDExtensionObjectPtr object,
                               const std::string& method_name) -> Result<MethodMetadata> {
        auto methods = callObject(object, "Object", "get_method_list", 3995934104LL);
        if (methods.isErr()) return methods.error();
        auto serialized = variantToJson(methods.value(), 0, true);
        if (serialized.isErr()) return serialized.error();
        if (!serialized.value().is_array()) {
            return Error::internal("Godot method metadata is malformed");
        }
        for (const auto& descriptor : serialized.value()) {
            if (!descriptor.is_object() || !descriptor.contains("name") ||
                !descriptor["name"].is_string()) {
                return Error::internal("Godot method descriptor is malformed");
            }
            if (descriptor["name"].get<std::string>() != method_name) continue;
            const auto arguments = descriptor.value("args", json::array());
            const auto defaults = descriptor.value("default_args", json::array());
            if (!arguments.is_array() || !defaults.is_array() ||
                !descriptor.contains("flags") ||
                !descriptor["flags"].is_number_integer() ||
                defaults.size() > arguments.size()) {
                return Error::internal("Godot method arity metadata is malformed");
            }
            MethodMetadata metadata;
            metadata.total_arguments = static_cast<int64_t>(arguments.size());
            metadata.required_arguments = static_cast<int64_t>(
                arguments.size() - defaults.size());
            metadata.vararg =
                (descriptor["flags"].get<int64_t>() & GDEXTENSION_METHOD_FLAG_VARARG) != 0;
            return metadata;
        }
        return Error::notFound("Target method not found: " + method_name);
    };
    auto preflight_object_binds = [&](std::initializer_list<std::pair<const char*, int64_t>> binds) {
        for (const auto& [name, hash] : binds) {
            if (requireMethodBind("Object", name, hash).isErr()) return false;
        }
        return true;
    };

    if (method == "signal.listConnections") {
        if (!hasOnlyKeys(params, {"target_node"}) ||
            !params.contains("target_node") ||
            !bounded_string(params["target_node"], 1, 1024)) {
            return bridgeError(400, "invalid_signal_list_connections_request");
        }
        if (!preflight_object_binds({
                {"get_signal_list", 3995934104LL},
                {"get_signal_connection_list", 3147814860LL},
                {"is_class", 3927539163LL}})) {
            return bridgeError(501, "required_bind_unavailable");
        }
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error());
        auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
        if (target.isErr()) return errorJson(target.error());
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
        if (takePhase7SignalTestSeam("malformed_metadata")) {
            return bridgeError(500, "extension_protocol_error");
        }
#endif
        auto signals = callObject(target.value(), "Object", "get_signal_list", 3995934104LL);
        if (signals.isErr()) return errorJson(500, signals.error().message);
        auto native_signal_count = array_size(signals.value());
        if (native_signal_count.isErr() || native_signal_count.value() < 0) {
            return bridgeError(500, "extension_protocol_error");
        }
        if (native_signal_count.value() > kSignalNativeWorkCeiling) {
            return bridgeError(413, "signal_metadata_work_limit");
        }
        struct SignalRecord { std::string name; json arguments; };
        std::vector<SignalRecord> descriptors;
        descriptors.reserve(static_cast<size_t>(native_signal_count.value()));
        for (int64_t index = 0; index < native_signal_count.value(); ++index) {
            auto native_descriptor = array_at(signals.value(), index);
            if (native_descriptor.isErr()) return bridgeError(500, "extension_protocol_error");
            auto serialized = variantToJson(native_descriptor.value());
            if (serialized.isErr() || !serialized.value().is_object() ||
                !serialized.value().contains("name") ||
                !serialized.value()["name"].is_string()) {
                return bridgeError(500, "extension_protocol_error");
            }
            auto arguments = serialized.value().value("args", json::array());
            if (!arguments.is_array()) return bridgeError(500, "extension_protocol_error");
            if (arguments.size() > static_cast<size_t>(kSignalArgumentWorkCeiling)) {
                return bridgeError(413, "signal_argument_metadata_work_limit");
            }
            const auto name = serialized.value()["name"].get<std::string>();
            auto validated = utf8_prefix(name, name.size());
            if (validated.isErr() || validated.value().size() != name.size()) {
                return bridgeError(500, "extension_protocol_error");
            }
            descriptors.push_back({name, std::move(arguments)});
        }
        std::sort(descriptors.begin(), descriptors.end(),
                  [&](const SignalRecord& left, const SignalRecord& right) {
                      return utf8_less(left.name, right.name);
                  });

        bool truncated = false;
        json truncated_at = nullptr;
        // How many of the reported connections belong to the editor rather
        // than to the scene, so a caller can tell an empty answer from one
        // that is entirely the scene dock without walking the list (#461).
        size_t editor_connection_count = 0;
        size_t engine_connection_count = 0;
        auto mark_truncated = [&](const std::string& location) {
            truncated = true;
            if (truncated_at.is_null()) truncated_at = location;
        };
        json output_signals = json::array();
        const size_t signal_count = std::min<size_t>(descriptors.size(), 256);
        if (descriptors.size() > signal_count) mark_truncated("signals");
        for (size_t signal_index = 0; signal_index < signal_count; ++signal_index) {
            const auto& descriptor = descriptors[signal_index];
            const auto& raw_name = descriptor.name;
            auto name = utf8_prefix(raw_name, 256);
            if (name.isErr()) return bridgeError(500, "extension_protocol_error");
            if (name.value().size() != raw_name.size()) {
                mark_truncated("bytes");
            }
            const auto& raw_arguments = descriptor.arguments;
            json arguments = json::array();
            const size_t argument_count = std::min<size_t>(raw_arguments.size(), 16);
            if (raw_arguments.size() > argument_count) {
                mark_truncated("arguments");
            }
            for (size_t argument_index = 0; argument_index < argument_count; ++argument_index) {
                auto parsed = parse_argument_metadata(raw_arguments[argument_index]);
                if (parsed.isErr()) return bridgeError(500, "extension_protocol_error");
                const auto& raw_argument_name = parsed.value().name;
                auto validated = utf8_prefix(raw_argument_name, raw_argument_name.size());
                if (validated.isErr() || validated.value().size() != raw_argument_name.size()) {
                    return bridgeError(500, "extension_protocol_error");
                }
                auto argument_name = utf8_prefix(raw_argument_name, 256);
                if (argument_name.isErr()) return bridgeError(500, "extension_protocol_error");
                if (argument_name.value().size() != raw_argument_name.size()) {
                    mark_truncated("bytes");
                }
                arguments.push_back({{"name", argument_name.value()},
                                     {"type_id", parsed.value().type},
                                     {"type_name", type_name(parsed.value().type)}});
            }

            auto signal_name_value = makeStringName(raw_name);
            if (signal_name_value.isErr()) return errorJson(500, signal_name_value.error().message);
            auto connection_values = callObject(
                target.value(), "Object", "get_signal_connection_list", 3147814860LL,
                {&signal_name_value.value()});
            if (connection_values.isErr()) return errorJson(500, connection_values.error().message);
            auto connection_count_result = array_size(connection_values.value());
            if (connection_count_result.isErr() || connection_count_result.value() < 0) {
                return bridgeError(500, "extension_protocol_error");
            }
            if (connection_count_result.value() > kSignalNativeWorkCeiling) {
                return bridgeError(413, "signal_connection_metadata_work_limit");
            }
            struct ConnectionRecord {
                std::optional<std::string> target_node;
                std::string target_method;
                int64_t flags{0};
            };
            std::vector<ConnectionRecord> normalized_connections;
            normalized_connections.reserve(
                static_cast<size_t>(connection_count_result.value()));
            for (int64_t connection_index = 0;
                 connection_index < connection_count_result.value(); ++connection_index) {
                auto connection = array_at(connection_values.value(), connection_index);
                if (connection.isErr()) return bridgeError(500, "extension_protocol_error");
                auto callable = dictionary_field(connection.value(), "callable");
                auto flags_value = dictionary_field(connection.value(), "flags");
                if (callable.isErr() || flags_value.isErr() ||
                    GodotApi::instance().variant_get_type(callable.value().ptr()) !=
                        GDEXTENSION_VARIANT_TYPE_CALLABLE) {
                    return bridgeError(500, "extension_protocol_error");
                }
                auto flags = scalarFromVariant<int64_t>(
                    flags_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                auto callable_object_value = callVariant(callable.value(), "get_object");
                auto callable_method_value = callVariant(callable.value(), "get_method");
                if (flags.isErr() || callable_object_value.isErr() ||
                    callable_method_value.isErr()) {
                    return bridgeError(500, "extension_protocol_error");
                }
                if (flags.value() < 0 || flags.value() > 15) {
                    return bridgeError(500, "extension_protocol_error");
                }
                auto callable_object = objectFromVariant(callable_object_value.value());
                const auto callable_method_type = GodotApi::instance().variant_get_type(
                    callable_method_value.value().ptr());
                auto callable_method = stringFromVariant(
                    callable_method_value.value(), callable_method_type);
                if (callable_object.isErr() || callable_method.isErr()) {
                    return bridgeError(500, "extension_protocol_error");
                }
                auto validated_method = utf8_prefix(
                    callable_method.value(), callable_method.value().size());
                if (validated_method.isErr() ||
                    validated_method.value().size() != callable_method.value().size()) {
                    return bridgeError(500, "extension_protocol_error");
                }
                std::optional<std::string> target_path;
                if (callable_object.value()) {
                    auto node_name = makeString("Node");
                    if (node_name.isErr()) return errorJson(500, node_name.error().message);
                    auto is_node_value = callObject(
                        callable_object.value(), "Object", "is_class", 3927539163LL,
                        {&node_name.value()});
                    if (is_node_value.isErr()) return errorJson(500, is_node_value.error().message);
                    auto is_node = scalarFromVariant<GDExtensionBool>(
                        is_node_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                    if (is_node.isErr()) return errorJson(500, is_node.error().message);
                    if (is_node.value()) {
                        auto path = logicalPathFromEditedRoot(root.value(), callable_object.value());
                        if (path.isOk()) {
                            auto validated_path = utf8_prefix(path.value(), path.value().size());
                            if (validated_path.isErr() ||
                                validated_path.value().size() != path.value().size()) {
                                return bridgeError(500, "extension_protocol_error");
                            }
                            target_path = path.value();
                        }
                    }
                }
                normalized_connections.push_back(
                    {std::move(target_path), callable_method.value(), flags.value()});
            }
            std::sort(normalized_connections.begin(), normalized_connections.end(),
                      [&](const ConnectionRecord& left, const ConnectionRecord& right) {
                          if (left.target_node.has_value() != right.target_node.has_value()) {
                              return !left.target_node.has_value();
                          }
                          if (left.target_node != right.target_node) {
                              return left.target_node.has_value() &&
                                     utf8_less(*left.target_node, *right.target_node);
                          }
                          if (left.target_method != right.target_method) {
                              return utf8_less(left.target_method, right.target_method);
                          }
                          return left.flags < right.flags;
                      });
            json connections = json::array();
            const size_t connection_count = std::min<size_t>(
                normalized_connections.size(), 256);
            if (normalized_connections.size() > connection_count) {
                mark_truncated("connections");
            }
            for (size_t connection_index = 0;
                 connection_index < connection_count; ++connection_index) {
                const auto& record = normalized_connections[connection_index];
                auto bounded_method = utf8_prefix(record.target_method, 128);
                if (bounded_method.isErr()) return bridgeError(500, "extension_protocol_error");
                if (bounded_method.value().size() != record.target_method.size()) {
                    mark_truncated("bytes");
                }
                json target_path = nullptr;
                if (record.target_node.has_value()) {
                    auto bounded_path = utf8_prefix(*record.target_node, 1024);
                    if (bounded_path.isErr()) return bridgeError(500, "extension_protocol_error");
                    if (bounded_path.value().size() != record.target_node->size()) {
                        mark_truncated("bytes");
                    }
                    target_path = bounded_path.value();
                }
                // A freshly created Sprite2D with no user connections at all
                // reported five: the scene dock's own listeners, alive only
                // while the editor has this scene open, in none of the saved
                // .tscn and in nothing at runtime. An agent asking what is
                // wired to this node got five false positives and one true
                // one, at a ratio that gets worse the emptier the scene.
                // Marked rather than filtered, because a caller debugging the
                // editor itself has no other way to see them (#461).
                //
                // That fix keyed `scene` on where the receiver lives, which is
                // a different question from the one the field is read for, and
                // the two part company on any UI node. `Container::add_child`
                // wires the container to its own children to keep the layout
                // in order, and the receiver of those is inside the edited
                // scene, so a Button in a VBoxContainer reported four
                // connections where the saved .tscn carries one. They are not
                // what signal_connect makes and disconnecting one breaks the
                // layout (#768).
                //
                // CONNECT_PERSIST is the engine's own answer to "is this
                // stored in the scene", it is the only flag signal_connect
                // accepts, and it was already in this payload. So `scene`
                // means persistent, and the structural fact keeps its own
                // value rather than being thrown away:
                //
                //   scene   persistent, so it is in the .tscn and a caller
                //           can act on it
                //   engine  the receiver is in this scene and the connection
                //           is not saved, so the engine remade it and will
                //           remake it again
                //   editor  the receiver is not in this scene at all
                //
                // A filter of `origin != "editor"` selects the same set it
                // always did. A filter of `origin == "scene"` now selects what
                // the documentation always said it did.
                constexpr int64_t kConnectPersist = 2;
                const bool persistent = (record.flags & kConnectPersist) != 0;
                const bool receiver_in_scene = record.target_node.has_value();
                const char* origin = persistent          ? "scene"
                                     : receiver_in_scene ? "engine"
                                                         : "editor";
                connections.push_back({{"target_node", std::move(target_path)},
                                       {"target_method", bounded_method.value()},
                                       {"flags", record.flags},
                                       {"origin", origin}});
                if (!persistent && !receiver_in_scene) ++editor_connection_count;
                if (!persistent && receiver_in_scene) ++engine_connection_count;
            }
            json signal = {{"name", name.value()}, {"arguments", std::move(arguments)},
                           {"connections", std::move(connections)}};
            output_signals.push_back(std::move(signal));
            json candidate = {{"target_node", params["target_node"]},
                              {"signals", output_signals},
                              {"editor_connections", editor_connection_count},
                              // The same service the count above performs,
                              // for the bucket that used to be counted as
                              // authored: an answer that is entirely the
                              // engine's own layout plumbing is recognisable
                              // without walking the list.
                              {"engine_connections", engine_connection_count},
                              {"truncated", truncated},
                              {"truncated_at", truncated_at}};
            if (liveResult(candidate).dump().size() > 63u * 1024u) {
                output_signals.erase(output_signals.end() - 1);
                mark_truncated("bytes");
                break;
            }
        }
        json response = {{"target_node", params["target_node"]},
                         {"signals", std::move(output_signals)},
                         {"editor_connections", editor_connection_count},
                         {"engine_connections", engine_connection_count},
                         {"truncated", truncated},
                         {"truncated_at", truncated_at}};
        auto live = liveResult(response);
        if (live.dump().size() > 64u * 1024u) {
            return bridgeError(413, "response_limit");
        }
        return live;
    }

    const bool is_connect = method == "signal.connect";
    const bool is_disconnect = method == "signal.disconnect";
    if (is_connect || is_disconnect) {
        if (!hasOnlyKeys(params,
                is_connect
                    ? std::initializer_list<const char*>{
                          "emitter_node", "signal_name", "target_node", "target_method", "flags"}
                    : std::initializer_list<const char*>{
                          "emitter_node", "signal_name", "target_node", "target_method"}) ||
            !params.contains("emitter_node") ||
            !bounded_string(params["emitter_node"], 1, 1024) ||
            !params.contains("signal_name") ||
            !bounded_string(params["signal_name"], 1, 128) ||
            !params.contains("target_node") ||
            !bounded_string(params["target_node"], 1, 1024) ||
            !params.contains("target_method") ||
            !bounded_string(params["target_method"], 1, 128) ||
            (is_connect && params.contains("flags") &&
             (!(params["flags"].is_number_integer() ||
                params["flags"].is_number_unsigned()) ||
              // Read as int64 below, so a value that does not fit is a
              // shape problem rather than a flag this tool will not write.
              (params["flags"].is_number_unsigned() &&
               params["flags"].get<uint64_t>() >
                   static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))))) {
            return bridgeError(400, is_connect ? "invalid_signal_connect_request"
                                               : "invalid_signal_disconnect_request");
        }
        // The flags this call will write. The server refuses an
        // unacceptable value first and with the same rule, so reaching
        // this means a client spoke to the bridge directly.
        const int64_t requested_flags =
            is_connect && params.contains("flags")
                ? connection_flags::authored(params["flags"].get<int64_t>())
                : connection_flags::kPersist;
        if (is_connect && !connection_flags::isConnectable(requested_flags)) {
            return bridgeError(
                400, "invalid_signal_connect_request",
                {{"flags", params["flags"]}},
                *connection_flags::refuseConnectFlags(params["flags"].get<int64_t>()));
        }
        if (!preflight_object_binds({
                {"get_signal_list", 3995934104LL},
                {"get_method_list", 3995934104LL},
                {"get_signal_connection_list", 3147814860LL},
                {"has_signal", 2619796661LL}, {"has_method", 2619796661LL},
                {"connect", 1518946055LL}, {"disconnect", 1874754934LL},
                {"is_connected", 768136979LL}}) ||
            preflightUndoManagerBindings().isErr() ||
            !GodotApi::instance().variant_get_ptr_constructor(
                GDEXTENSION_VARIANT_TYPE_CALLABLE, 2) ||
            !GodotApi::instance().variant_call) {
            return bridgeError(501, "required_bind_unavailable");
        }
        if (requireMethodBind("EditorUndoRedoManager", "get_object_history_id",
                              1107568780LL).isErr() ||
            requireMethodBind("EditorUndoRedoManager", "get_history_undo_redo",
                              2417974513LL).isErr() ||
            requireMethodBind("UndoRedo", "has_undo", 36873697LL).isErr() ||
            requireMethodBind("UndoRedo", "undo", 2240911060LL).isErr()) {
            return bridgeError(501, "required_bind_unavailable");
        }

        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error());
        auto emitter = resolveNode(root.value(), params["emitter_node"].get<std::string>());
        auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
        if (emitter.isErr()) return errorJson(emitter.error());
        if (target.isErr()) return errorJson(target.error());
        const auto signal_name = params["signal_name"].get<std::string>();
        const auto target_method = params["target_method"].get<std::string>();
        auto signal_name_value = makeStringName(signal_name);
        auto method_name_value = makeStringName(target_method);
        if (signal_name_value.isErr() || method_name_value.isErr()) {
            return bridgeError(500, "extension_protocol_error");
        }
        auto has_signal_value = callObject(
            emitter.value(), "Object", "has_signal", 2619796661LL,
            {&signal_name_value.value()});
        auto has_method_value = callObject(
            target.value(), "Object", "has_method", 2619796661LL,
            {&method_name_value.value()});
        if (has_signal_value.isErr() || has_method_value.isErr()) {
            return bridgeError(500, "extension_protocol_error");
        }
        auto has_signal = scalarFromVariant<GDExtensionBool>(
            has_signal_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        auto has_method = scalarFromVariant<GDExtensionBool>(
            has_method_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (has_signal.isErr() || has_method.isErr()) {
            return bridgeError(500, "extension_protocol_error");
        }
        if (!has_signal.value()) {
            return bridgeError(
                404, "declared_signal_not_found",
                {{"emitter_node", params["emitter_node"].get<std::string>()},
                 {"signal_name", signal_name}},
                "Looked for signal '" + signal_name + "' on " +
                    params["emitter_node"].get<std::string>() + ".");
        }
        if (!has_method.value()) {
            // Before blaming the name. A script that did not compile is
            // still assigned to the node and has no instance behind it, so
            // has_method answers false for every method the file declares
            // and the refusal sent the caller to rename something that was
            // already right (#729).
            const auto uncompiled = uncompiledScriptOn(target.value(), target_method);
            if (uncompiled.found) {
                json data = {{"target_node", params["target_node"].get<std::string>()},
                             {"target_method", target_method},
                             {"script_path",
                              uncompiled.path.empty() ? json(nullptr) : json(uncompiled.path)},
                             {"unresolved_autoloads", uncompiled.unresolved_autoloads}};
                std::string detail = "The script is ";
                detail += uncompiled.path.empty() ? "attached to " + params["target_node"].get<std::string>()
                                                  : uncompiled.path;
                detail += ", it declares '" + target_method +
                          "', and the engine does not have that method on the node.";
                if (!uncompiled.unresolved_autoloads.empty()) {
                    detail += " It names ";
                    for (size_t index = 0; index < uncompiled.unresolved_autoloads.size(); ++index) {
                        if (index > 0) detail += ", ";
                        detail += "'" + uncompiled.unresolved_autoloads[index] + "'";
                    }
                    detail += uncompiled.unresolved_autoloads.size() == 1
                                  ? ", which is an autoload registered in this project. "
                                  : ", which are autoloads registered in this project. ";
                    detail += kAutoloadRestartNote;
                    data["note"] = kAutoloadRestartNote;
                } else {
                    detail += " Check it with script_check_syntax.";
                }
                return bridgeError(409, "target_script_not_compiled", std::move(data), detail);
            }
            return bridgeError(
                404, "target_method_not_found",
                {{"target_node", params["target_node"].get<std::string>()},
                 {"target_method", target_method}},
                "Looked for method '" + target_method + "' on " +
                    params["target_node"].get<std::string>() + ".");
        }
        auto signal = signal_metadata(emitter.value(), signal_name);
        auto target_metadata = method_metadata(target.value(), target_method);
        if (signal.isErr()) return errorJson(signal.error());
        if (target_metadata.isErr()) {
            return errorJson(target_metadata.error());
        }
        const int64_t signal_arity = static_cast<int64_t>(
            signal.value().arguments.size());
        // Connect only. Arity compatibility is a precondition for making a
        // connection and cannot be one for removing it: a disconnect never
        // calls the method, so whether the method could accept the signal's
        // arguments is not a fact about whether the disconnect can proceed.
        // Running it here told an agent tearing down the connections it made
        // that its signature was wrong, by the call whose whole purpose is
        // to stop the connection existing -- and the true answer, which the
        // same tool already gives for a compatible pair that was never
        // connected, is that no such connection exists (#714).
        if (is_connect &&
            (signal_arity < target_metadata.value().required_arguments ||
             (!target_metadata.value().vararg &&
              signal_arity > target_metadata.value().total_arguments))) {
            return bridgeError(409, "signal_target_arity_incompatible");
        }
        auto callable = make_callable(target.value(), target_method);
        if (callable.isErr()) return bridgeError(501, "required_bind_unavailable");

        struct ExactConnection {
            VariantValue callable;
            int64_t flags{0};
        };
        auto exact_connections = [&]() -> Result<std::vector<ExactConnection>> {
            auto values = callObject(
                emitter.value(), "Object", "get_signal_connection_list", 3147814860LL,
                {&signal_name_value.value()});
            if (values.isErr()) return values.error();
            auto count = array_size(values.value());
            if (count.isErr() || count.value() < 0 || count.value() > 4096) {
                return Error::internal("Signal connection metadata exceeds the preflight cap");
            }
            std::vector<ExactConnection> matches;
            for (int64_t index = 0; index < count.value(); ++index) {
                auto connection = array_at(values.value(), index);
                if (connection.isErr()) return connection.error();
                auto candidate = dictionary_field(connection.value(), "callable");
                auto flags_value = dictionary_field(connection.value(), "flags");
                if (candidate.isErr() || flags_value.isErr() ||
                    GodotApi::instance().variant_get_type(candidate.value().ptr()) !=
                        GDEXTENSION_VARIANT_TYPE_CALLABLE) {
                    return Error::internal("Signal connection descriptor is malformed");
                }
                auto object_value = callVariant(candidate.value(), "get_object");
                auto method_value = callVariant(candidate.value(), "get_method");
                auto bound_count_value = callVariant(
                    candidate.value(), "get_bound_arguments_count");
                if (object_value.isErr() || method_value.isErr() ||
                    bound_count_value.isErr()) {
                    return Error::internal("Signal Callable metadata is unavailable");
                }
                auto object = objectFromVariant(object_value.value());
                auto bound_count = scalarFromVariant<int64_t>(
                    bound_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                const auto method_type = GodotApi::instance().variant_get_type(
                    method_value.value().ptr());
                auto candidate_method = stringFromVariant(method_value.value(), method_type);
                auto flags = scalarFromVariant<int64_t>(
                    flags_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                if (object.isErr() || bound_count.isErr() || candidate_method.isErr() ||
                    flags.isErr()) {
                    return Error::internal("Signal Callable metadata is malformed");
                }
                if (object.value() == target.value() &&
                    candidate_method.value() == target_method &&
                    bound_count.value() == 0) {
                    matches.push_back(
                        {std::move(candidate.value()), flags.value()});
                }
            }
            return matches;
        };
        auto connected_value = callObject(
            emitter.value(), "Object", "is_connected", 768136979LL,
            {&signal_name_value.value(), &callable.value()});
        if (connected_value.isErr()) return errorJson(500, connected_value.error().message);
        auto connected = scalarFromVariant<GDExtensionBool>(
            connected_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        auto matches = exact_connections();
        if (connected.isErr() || matches.isErr()) {
            return bridgeError(500, "extension_protocol_error");
        }
        if ((connected.value() != 0) != !matches.value().empty()) {
            return bridgeError(500, "signal_connection_state_inconsistent");
        }
        if (is_connect && !matches.value().empty()) {
            return bridgeError(409, "signal_connection_already_exists");
        }
        if (is_disconnect && matches.value().size() != 1) {
            return bridgeError(409, "missing_or_ambiguous_signal_connection");
        }
        // Only a connection the scene file stores is the caller's to
        // remove. This used to demand exactly 2, which refused every
        // deferred or one-shot connection the editor's Connect dialog
        // writes, and every connection at all inside an instanced scene,
        // where the engine adds CONNECT_INHERITED to the value it reports
        // (#852). An engine or editor connection is still refused: it is
        // not in the .tscn, the engine makes it again, and removing one
        // breaks the layout it was keeping in order.
        if (is_disconnect &&
            !connection_flags::isPersistent(matches.value().front().flags)) {
            return bridgeError(
                409, "unsupported_existing_connection_flags",
                {{"flags", matches.value().front().flags}},
                "The connection's flags are " +
                    std::to_string(matches.value().front().flags) +
                    ", which does not include 2, CONNECT_PERSIST, so it is not stored in "
                    "the scene file. signal_list_connections reports it as origin 'engine' "
                    "or 'editor': the engine or the editor made it and will make it again, "
                    "and removing one breaks what it was keeping in order.");
        }

        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error());
        // A connect writes what the caller asked for. A disconnect's undo
        // restores what was there, verbatim, including CONNECT_INHERITED:
        // undo that put a deferred connection back as a plain one would be
        // its own silent behaviour change (#852).
        const int64_t applied_flags =
            is_connect ? requested_flags : matches.value().front().flags;
        auto flags_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, applied_flags);
        if (flags_value.isErr()) return errorJson(500, flags_value.error().message);
        auto action = createAction(manager.value(),
                                   is_connect ? "Connect signal" : "Disconnect signal",
                                   emitter.value());
        if (action.isErr()) return errorJson(500, action.error().message);
        Result<void> do_method = Result<void>::ok();
        Result<void> undo_method = Result<void>::ok();
        if (is_connect) {
            do_method = managerMethod(
                manager.value(), "add_do_method", emitter.value(), "connect",
                {&signal_name_value.value(), &callable.value(), &flags_value.value()});
            undo_method = managerMethod(
                manager.value(), "add_undo_method", emitter.value(), "disconnect",
                {&signal_name_value.value(), &callable.value()});
        } else {
            do_method = managerMethod(
                manager.value(), "add_do_method", emitter.value(), "disconnect",
                {&signal_name_value.value(), &matches.value().front().callable});
            undo_method = managerMethod(
                manager.value(), "add_undo_method", emitter.value(), "connect",
                {&signal_name_value.value(), &matches.value().front().callable,
                 &flags_value.value()});
        }
        if (do_method.isErr() || undo_method.isErr()) {
            // Closed, so the next mutation does not merge into it (#1152).
            abandonAction(manager.value());
            return bridgeError(500, "signal_undo_redo_registration_failed");
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(500, committed.error().message);

        bool force_postcondition_mismatch = false;
        bool force_rollback_failure = false;
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
        if (is_connect) {
            force_postcondition_mismatch =
                takePhase7SignalTestSeam("connect_postcondition_mismatch");
            if (!force_postcondition_mismatch &&
                takePhase7SignalTestSeam(
                    "connect_postcondition_mismatch_rollback_failure")) {
                force_postcondition_mismatch = true;
                force_rollback_failure = true;
            }
        } else {
            force_postcondition_mismatch =
                takePhase7SignalTestSeam("disconnect_postcondition_mismatch");
        }
#endif

        auto observed_connected_value = callObject(
            emitter.value(), "Object", "is_connected", 768136979LL,
            {&signal_name_value.value(), &callable.value()});
        auto observed_matches = exact_connections();
        bool postcondition_ok = false;
        if (observed_connected_value.isOk() && observed_matches.isOk()) {
            auto observed_connected = scalarFromVariant<GDExtensionBool>(
                observed_connected_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (observed_connected.isOk()) {
                postcondition_ok = is_connect
                    ? observed_connected.value() && observed_matches.value().size() == 1 &&
                          connection_flags::authored(
                              observed_matches.value().front().flags) == requested_flags
                    : !observed_connected.value() && observed_matches.value().empty();
            }
        }
        if (force_postcondition_mismatch) postcondition_ok = false;
        if (!postcondition_ok) {
            const Result<void> rolled_back = undoLastAction(manager.value(), root.value());
            bool restored = false;
            if (rolled_back.isOk()) {
                auto restored_connected_value = callObject(
                    emitter.value(), "Object", "is_connected", 768136979LL,
                    {&signal_name_value.value(), &callable.value()});
                auto restored_matches = exact_connections();
                if (restored_connected_value.isOk() && restored_matches.isOk()) {
                    auto restored_connected = scalarFromVariant<GDExtensionBool>(
                        restored_connected_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                    if (restored_connected.isOk()) {
                        restored = is_connect
                            ? !restored_connected.value() && restored_matches.value().empty()
                            : restored_connected.value() &&
                                  restored_matches.value().size() == 1 &&
                                  restored_matches.value().front().flags ==
                                      matches.value().front().flags;
                    }
                }
            }
            if (force_rollback_failure) restored = false;
            return bridgeError(
                500, "signal_postcondition_mismatch",
                {{"retryable", false},
                 {"rollback", restored ? "completed" : "failed"},
                 {"outcome", restored ? "rolled_back" : "unknown"},
                 {"restoration_observed", restored}});
        }
        // The flags the connection now has, not the flags this tool used to
        // be able to write. A caller that asked for a deferred connection
        // and was told `flags: 2` could not tell whether it got one.
        // The connection as it stood before the commit (#1151): a connect
        // is refused when one exists, and a disconnect removes the one it
        // found, with the flags it had.
        if (is_connect) {
            return liveSceneMutation({{"connected", true}, {"flags", applied_flags},
                                      {"before", {{"connected", false}}},
                                      {"undo_redo_registered", true},
                                      {"outcome", "completed"}, {"rollback", "undo_redo"}});
        }
        return liveSceneMutation({{"disconnected", true}, {"flags", applied_flags},
                                  {"before", {{"connected", true},
                                              {"flags", matches.value().front().flags}}},
                                  {"undo_redo_registered", true},
                                  {"outcome", "completed"}, {"rollback", "undo_redo"}});
    }

    if (!hasOnlyKeys(params, {"target_node", "signal_name", "arguments"}) ||
        !params.contains("target_node") ||
        !bounded_string(params["target_node"], 1, 1024) ||
        !params.contains("signal_name") ||
        !bounded_string(params["signal_name"], 1, 128) ||
        (params.contains("arguments") && !params["arguments"].is_array())) {
        return bridgeError(400, "invalid_signal_emit_request");
    }
    const auto emit_arguments = params.value("arguments", json::array());
    if (emit_arguments.size() > 16) {
        return bridgeError(400, "signal_emit_argument_count_exceeded");
    }
    for (const auto& argument : emit_arguments) {
        if (!valid_emit_value(argument, 0)) {
            return bridgeError(400, "unsupported_signal_emit_argument");
        }
    }
    try {
        if (emit_arguments.dump().size() > 32u * 1024u) {
            return bridgeError(413, "signal_emit_arguments_too_large");
        }
    } catch (const json::exception&) {
        return bridgeError(400, "invalid_signal_emit_argument_encoding");
    }
    if (!preflight_object_binds({{"get_signal_list", 3995934104LL},
                                 {"has_signal", 2619796661LL},
                                 {"emit_signal", 4047867050LL}})) {
        return bridgeError(501, "required_bind_unavailable");
    }
    auto root = editedSceneRoot(editor);
    if (root.isErr()) return errorJson(root.error());
    auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
    if (target.isErr()) return errorJson(target.error());
    const auto signal_name = params["signal_name"].get<std::string>();
    auto signal_name_value = makeStringName(signal_name);
    if (signal_name_value.isErr()) return errorJson(500, signal_name_value.error().message);
    auto has_signal_value = callObject(
        target.value(), "Object", "has_signal", 2619796661LL,
        {&signal_name_value.value()});
    if (has_signal_value.isErr()) return errorJson(500, has_signal_value.error().message);
    auto has_signal = scalarFromVariant<GDExtensionBool>(
        has_signal_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
    if (has_signal.isErr()) return errorJson(500, has_signal.error().message);
    if (!has_signal.value()) return bridgeError(404, "declared_signal_not_found");
    auto metadata = signal_metadata(target.value(), signal_name);
    if (metadata.isErr()) return errorJson(metadata.error());
    if (metadata.value().arguments.size() != emit_arguments.size()) {
        return bridgeError(409, "signal_emit_arity_mismatch");
    }
    auto compatible_argument = [](const json& argument,
                                  const SignalArgumentMetadata& metadata) {
        const bool typed_container =
            (metadata.type == GDEXTENSION_VARIANT_TYPE_ARRAY ||
             metadata.type == GDEXTENSION_VARIANT_TYPE_DICTIONARY) &&
            (metadata.hint != 0 || !metadata.hint_string.empty());
        if (typed_container) return false;
        switch (metadata.type) {
            case GDEXTENSION_VARIANT_TYPE_NIL: return true;
            case GDEXTENSION_VARIANT_TYPE_BOOL: return argument.is_boolean();
            case GDEXTENSION_VARIANT_TYPE_INT:
                return argument.is_number_integer() || argument.is_number_unsigned();
            case GDEXTENSION_VARIANT_TYPE_FLOAT:
                return argument.is_number() &&
                       std::isfinite(argument.get<double>());
            case GDEXTENSION_VARIANT_TYPE_STRING: return argument.is_string();
            case GDEXTENSION_VARIANT_TYPE_DICTIONARY: return argument.is_object();
            case GDEXTENSION_VARIANT_TYPE_ARRAY: return argument.is_array();
            case GDEXTENSION_VARIANT_TYPE_OBJECT:
                return argument.is_null() &&
                       (!metadata.class_name.empty() || !metadata.hint_string.empty());
            default: return false;
        }
    };
    bool missing_destination_float_constructor = false;
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
    missing_destination_float_constructor =
        takePhase7SignalTestSeam("missing_destination_float_constructor");
#endif
    std::function<bool(const json&, std::optional<GDExtensionVariantType>)>
        preflight_json_variant;
    preflight_json_variant = [&](const json& value,
                                 std::optional<GDExtensionVariantType> destination_type) {
        auto& api = GodotApi::instance();
        if (!api.variant_new_nil || !api.variant_destroy ||
            !api.get_variant_from_type_constructor) return false;
        if (value.is_null()) return true;
        GDExtensionVariantType type = GDEXTENSION_VARIANT_TYPE_NIL;
        if (destination_type.has_value() &&
            *destination_type != GDEXTENSION_VARIANT_TYPE_NIL) {
            type = *destination_type;
        } else if (value.is_boolean()) type = GDEXTENSION_VARIANT_TYPE_BOOL;
        else if (value.is_number_integer() || value.is_number_unsigned()) {
            type = GDEXTENSION_VARIANT_TYPE_INT;
        } else if (value.is_number_float()) type = GDEXTENSION_VARIANT_TYPE_FLOAT;
        else if (value.is_string()) type = GDEXTENSION_VARIANT_TYPE_STRING;
        else if (value.is_array()) type = GDEXTENSION_VARIANT_TYPE_ARRAY;
        else if (value.is_object()) type = GDEXTENSION_VARIANT_TYPE_DICTIONARY;
        else return false;

        if (type == GDEXTENSION_VARIANT_TYPE_FLOAT &&
            missing_destination_float_constructor) return false;
        if (type == GDEXTENSION_VARIANT_TYPE_STRING &&
            !api.string_new_with_utf8_chars) return false;
        if (type == GDEXTENSION_VARIANT_TYPE_ARRAY) {
            if (!value.is_array()) return false;
            if (!api.variant_get_ptr_constructor(type, 0) || !api.variant_call) return false;
            for (const auto& child : value) {
                if (!preflight_json_variant(child, std::nullopt)) return false;
            }
        } else if (type == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
            if (!value.is_object()) return false;
            if (!api.variant_get_ptr_constructor(type, 0) || !api.variant_call ||
                !api.string_new_with_utf8_chars ||
                !api.get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_STRING)) {
                return false;
            }
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!preflight_json_variant(it.value(), std::nullopt)) return false;
            }
        }
        return api.get_variant_from_type_constructor(type) != nullptr;
    };
    for (size_t index = 0; index < emit_arguments.size(); ++index) {
        if (!compatible_argument(emit_arguments[index], metadata.value().arguments[index])) {
            return bridgeError(400, "signal_emit_argument_type_mismatch");
        }
    }
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
    if (takePhase7SignalTestSeam("missing_required_api")) {
        return bridgeError(501, "required_bind_unavailable");
    }
#endif
    for (size_t index = 0; index < emit_arguments.size(); ++index) {
        if (!preflight_json_variant(
                emit_arguments[index],
                static_cast<GDExtensionVariantType>(metadata.value().arguments[index].type))) {
            return bridgeError(501, "required_bind_unavailable");
        }
    }
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
    if (takePhase7SignalTestSeam("conversion_failure")) {
        return bridgeError(500, "extension_protocol_error");
    }
#endif
    std::vector<VariantValue> native_arguments;
    native_arguments.reserve(emit_arguments.size());
    for (size_t index = 0; index < emit_arguments.size(); ++index) {
        const auto& argument_metadata = metadata.value().arguments[index];
        Result<VariantValue> converted =
            argument_metadata.type == GDEXTENSION_VARIANT_TYPE_FLOAT &&
                    (emit_arguments[index].is_number_integer() ||
                     emit_arguments[index].is_number_unsigned())
                ? makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT,
                             static_cast<double>(emit_arguments[index].get<int64_t>()))
                : (argument_metadata.type == GDEXTENSION_VARIANT_TYPE_OBJECT &&
                           emit_arguments[index].is_null()
                       ? Result<VariantValue>(VariantValue{})
                       : makeJsonVariant(emit_arguments[index]));
        if (converted.isErr()) return bridgeError(500, "extension_protocol_error");
        native_arguments.push_back(std::move(converted.value()));
    }
    std::vector<const VariantValue*> call_arguments{&signal_name_value.value()};
    call_arguments.reserve(native_arguments.size() + 1);
    for (auto& argument : native_arguments) call_arguments.push_back(&argument);
    // How many listeners this emit can reach, read before the emit rather
    // than after it, because a one-shot connection disconnects itself on
    // delivery. It is also what tells a refusal apart from a no-op below.
    int64_t connection_count = 0;
    if (requireMethodBind("Object", "get_signal_connection_list", 3147814860LL).isOk()) {
        auto connections = callObject(target.value(), "Object", "get_signal_connection_list",
                                      3147814860LL, {&signal_name_value.value()});
        if (connections.isOk()) {
            auto size_value = callVariant(connections.value(), "size");
            if (size_value.isOk()) {
                auto size = scalarFromVariant<int64_t>(size_value.value(),
                                                       GDEXTENSION_VARIANT_TYPE_INT);
                if (size.isOk()) connection_count = size.value();
            }
        }
    }
    auto emitted = callObject(target.value(), "Object", "emit_signal", 4047867050LL,
                              call_arguments);
    if (emitted.isErr()) return errorJson(500, emitted.error().message);
    auto emit_code = scalarFromVariant<int64_t>(
        emitted.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (emit_code.isErr()) return errorJson(500, emit_code.error().message);
    // ERR_UNAVAILABLE with nothing connected is not a failure. Godot keeps
    // a signal in Object::signal_map only once it has a connection, so
    // emitting a built-in signal nobody is listening to returns
    // ERR_UNAVAILABLE and the bridge reported "The engine refused the
    // emit." That is the state a caller is most likely to be in -- driving
    // a signal by hand during bring-up, before the connection exists -- and
    // in GDScript the same call is an unremarkable no-op (#624). The signal
    // is known to be declared by this point: the arity check above read it
    // out of the object's own signal list.
    constexpr int64_t kErrUnavailable = 2;
    if (emit_code.value() == kErrUnavailable && connection_count == 0) {
        return liveResult({{"emitted", false},
                           {"connection_count", 0},
                           {"argument_count", emit_arguments.size()},
                           {"note", "Nothing is connected to this signal, so the emit reached "
                                    "no listeners. Connect something with signal_connect to "
                                    "observe it."},
                           {"outcome", "completed"},
                           {"rollback", "not_available"}});
    }
    if (emit_code.value() != 0) return bridgeError(500, "signal_emit_failed");
    // The connections ran project code, which can change the scene with
    // or without an undo entry (#1049).
    json emitted_answer = {{"emitted", true},
                           {"connection_count", connection_count},
                           {"argument_count", emit_arguments.size()},
                           {"outcome", "completed"},
                           {"rollback", "not_available"}};
    reportEditedSceneSaved(emitted_answer);
    return liveResult(emitted_answer);
}


} // namespace didi::godot
