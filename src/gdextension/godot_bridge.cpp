#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/expression_sandbox.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/project_path.hpp"
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <functional>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace didi {
namespace godot {
namespace {

constexpr size_t kOpaqueBytes = 64;
using Opaque = std::array<std::byte, kOpaqueBytes>;

json errorJson(int code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

json errorJson(int code, const std::string& message, json data) {
    return {{"error", {{"code", code}, {"message", message}, {"data", std::move(data)}}}};
}

#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
std::string g_phase7SignalTestSeam;

bool takePhase7SignalTestSeam(std::string_view expected) {
    if (g_phase7SignalTestSeam != expected) return false;
    g_phase7SignalTestSeam.clear();
    return true;
}
#endif

class NativeValue {
public:
    NativeValue(GDExtensionVariantType type, bool initialized = false)
        : m_type(type), m_initialized(initialized) {}

    NativeValue(const NativeValue&) = delete;
    NativeValue& operator=(const NativeValue&) = delete;
    NativeValue(NativeValue&& other) noexcept
        : m_storage(other.m_storage), m_type(other.m_type), m_initialized(other.m_initialized) {
        other.m_initialized = false;
    }
    ~NativeValue() {
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(m_type);
            if (destructor) destructor(ptr());
        }
    }

    void* ptr() { return m_storage.data(); }
    const void* ptr() const { return m_storage.data(); }
    void markInitialized() { m_initialized = true; }

private:
    alignas(16) Opaque m_storage{};
    GDExtensionVariantType m_type;
    bool m_initialized{false};
};

class VariantValue {
public:
    struct Uninitialized {};

    VariantValue() {
        auto& api = GodotApi::instance();
        if (api.variant_new_nil) {
            api.variant_new_nil(ptr());
            m_initialized = true;
        }
    }
    explicit VariantValue(Uninitialized) {}
    VariantValue(const VariantValue&) = delete;
    VariantValue& operator=(const VariantValue&) = delete;
    VariantValue(VariantValue&& other) noexcept
        : m_storage(other.m_storage), m_initialized(other.m_initialized) {
        other.m_initialized = false;
    }
    VariantValue& operator=(VariantValue&& other) noexcept {
        if (this == &other) return *this;
        destroy();
        m_storage = other.m_storage;
        m_initialized = other.m_initialized;
        other.m_initialized = false;
        return *this;
    }
    ~VariantValue() { destroy(); }

    void* ptr() { return m_storage.data(); }
    const void* ptr() const { return m_storage.data(); }
    void markInitialized() { m_initialized = true; }
    bool initialized() const { return m_initialized; }

private:
    void destroy() {
        if (m_initialized && GodotApi::instance().variant_destroy) {
            GodotApi::instance().variant_destroy(ptr());
        }
        m_initialized = false;
    }

    alignas(16) Opaque m_storage{};
    bool m_initialized{false};
};

class NativeName {
public:
    explicit NativeName(const std::string& value) {
        auto& api = GodotApi::instance();
        if (api.string_name_new_with_utf8_chars) {
            api.string_name_new_with_utf8_chars(m_storage.data(), value.c_str());
            m_initialized = true;
        }
    }
    NativeName(const NativeName&) = delete;
    ~NativeName() {
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (destructor) destructor(m_storage.data());
        }
    }
    const void* ptr() const { return m_storage.data(); }
    bool valid() const { return m_initialized; }

private:
    alignas(16) Opaque m_storage{};
    bool m_initialized{false};
};

Result<VariantValue> callVariant(VariantValue& target, const std::string& method_name,
                                 const std::vector<const VariantValue*>& arguments = {});

Result<VariantValue> variantFromNative(GDExtensionVariantType type, void* native) {
    auto ctor = GodotApi::instance().get_variant_from_type_constructor(type);
    if (!ctor) return Error::internal("Missing Variant constructor for type " + std::to_string(type));
    VariantValue value(VariantValue::Uninitialized{});
    ctor(value.ptr(), native);
    value.markInitialized();
    return std::move(value);
}

Result<VariantValue> makeString(const std::string& text) {
    auto& api = GodotApi::instance();
    if (!api.string_new_with_utf8_chars) return Error::internal("Godot String constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_STRING);
    api.string_new_with_utf8_chars(native.ptr(), text.c_str());
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_STRING, native.ptr());
}

Result<VariantValue> makeStringName(const std::string& text) {
    auto& api = GodotApi::instance();
    if (!api.string_name_new_with_utf8_chars) return Error::internal("Godot StringName constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    api.string_name_new_with_utf8_chars(native.ptr(), text.c_str());
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_STRING_NAME, native.ptr());
}

Result<VariantValue> makeNodePath(const std::string& text) {
    auto& api = GodotApi::instance();
    NativeValue native_string(GDEXTENSION_VARIANT_TYPE_STRING);
    api.string_new_with_utf8_chars(native_string.ptr(), text.c_str());
    native_string.markInitialized();

    auto ctor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_NODE_PATH, 2);
    if (!ctor) return Error::internal("Godot NodePath(String) constructor is unavailable");
    NativeValue native_path(GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    const void* args[] = {native_string.ptr()};
    ctor(native_path.ptr(), args);
    native_path.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_NODE_PATH, native_path.ptr());
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

template <typename T>
Result<VariantValue> makeScalar(GDExtensionVariantType type, T value) {
    return variantFromNative(type, &value);
}

Result<VariantValue> makeObject(GDExtensionObjectPtr object) {
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_OBJECT, &object);
}

Result<VariantValue> makeJsonVariant(const json& value, int depth = 0) {
    if (depth > 16) return Error::invalidArgument("JSON nesting exceeds the Phase 2 limit of 16 levels");
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

Result<void> validateJsonForPropertyType(const json& value, GDExtensionVariantType type) {
    bool compatible = false;
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_NIL:
            compatible = value.is_null();
            break;
        case GDEXTENSION_VARIANT_TYPE_BOOL:
            compatible = value.is_boolean();
            break;
        case GDEXTENSION_VARIANT_TYPE_INT:
            compatible = value.is_number_integer() || value.is_number_unsigned();
            break;
        case GDEXTENSION_VARIANT_TYPE_FLOAT:
            compatible = value.is_number();
            break;
        case GDEXTENSION_VARIANT_TYPE_STRING:
        case GDEXTENSION_VARIANT_TYPE_STRING_NAME:
        case GDEXTENSION_VARIANT_TYPE_NODE_PATH:
            compatible = value.is_string();
            break;
        default:
            return Error::invalidArgument("Property type " + std::to_string(type) +
                                          " is outside the Phase 1 scalar property contract");
    }
    if (!compatible) {
        return Error::invalidArgument("JSON value is incompatible with Godot property type " +
                                      std::to_string(type));
    }
    return Result<void>::ok();
}

Result<VariantValue> callObject(GDExtensionObjectPtr object, const char* class_name,
                                const char* method_name, int64_t hash,
                                const std::vector<const VariantValue*>& arguments = {}) {
    if (!object) return Error::notFound(std::string("Cannot call ") + method_name + " on a null Godot object");
    auto& api = GodotApi::instance();
    NativeName klass(class_name);
    NativeName method(method_name);
    if (!klass.valid() || !method.valid()) return Error::internal("Failed to construct Godot method identifiers");
    auto bind = api.classdb_get_method_bind(klass.ptr(), method.ptr(), hash);
    if (!bind) return Error::internal(std::string("Godot method binding unavailable: ") + class_name + "." + method_name);

    std::vector<const void*> raw_args;
    raw_args.reserve(arguments.size());
    for (const auto* argument : arguments) raw_args.push_back(argument->ptr());

    VariantValue result(VariantValue::Uninitialized{});
    GDExtensionCallError error{};
    api.object_method_bind_call(bind, object, raw_args.empty() ? nullptr : raw_args.data(),
                                static_cast<GDExtensionInt>(raw_args.size()), result.ptr(), &error);
    result.markInitialized();
    if (error.error != GDEXTENSION_CALL_OK) {
        return Error::internal(std::string("Godot call failed: ") + class_name + "." + method_name +
                               " (call error " + std::to_string(error.error) + ")");
    }
    return std::move(result);
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

Result<VariantValue> callVariant(VariantValue& target, const std::string& method_name,
                                 const std::vector<const VariantValue*>& arguments) {
    auto& api = GodotApi::instance();
    NativeName method(method_name);
    if (!method.valid() || !api.variant_call) return Error::internal("Variant call API is unavailable");
    std::vector<const void*> raw_args;
    for (const auto* argument : arguments) raw_args.push_back(argument->ptr());
    VariantValue result(VariantValue::Uninitialized{});
    GDExtensionCallError error{};
    api.variant_call(target.ptr(), method.ptr(), raw_args.empty() ? nullptr : raw_args.data(),
                     static_cast<GDExtensionInt>(raw_args.size()), result.ptr(), &error);
    result.markInitialized();
    if (error.error != GDEXTENSION_CALL_OK) {
        return Error::internal("Godot Variant." + method_name + " failed (call error " + std::to_string(error.error) + ")");
    }
    return std::move(result);
}

template <typename T>
Result<T> scalarFromVariant(VariantValue& value, GDExtensionVariantType type) {
    auto ctor = GodotApi::instance().get_variant_to_type_constructor(type);
    if (!ctor) return Error::internal("Missing Variant-to-native constructor for type " + std::to_string(type));
    T result{};
    ctor(&result, value.ptr());
    return result;
}

Result<GDExtensionObjectPtr> objectFromVariant(VariantValue& value) {
    return scalarFromVariant<GDExtensionObjectPtr>(value, GDEXTENSION_VARIANT_TYPE_OBJECT);
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
Result<json> variantToJson(VariantValue& value, int depth = 0, bool lenient = false) {
    if (depth > 16) return Error::invalidArgument("Godot Variant nesting exceeds the Phase 2 limit of 16 levels");
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
            return result.isOk() ? Result<json>(json(result.value())) : Result<json>(result.error());
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
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return index.error();
                auto key = callVariant(keys.value(), "get", {&index.value()});
                if (key.isErr()) return key.error();
                auto key_type = GodotApi::instance().variant_get_type(key.value().ptr());
                if (key_type != GDEXTENSION_VARIANT_TYPE_STRING && key_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                    return Error::invalidArgument("Godot Dictionary contains a non-string key");
                }
                auto key_text = stringFromVariant(key.value(), key_type);
                if (key_text.isErr()) return key_text.error();
                auto item = callVariant(value, "get", {&key.value()});
                if (item.isErr()) return item.error();
                auto converted = variantToJson(item.value(), depth + 1, lenient);
                if (converted.isErr()) return converted.error();
                output[key_text.value()] = converted.value();
            }
            return output;
        }
        default:
            if (lenient) return json(nullptr);
            return Error::invalidArgument("Godot Variant type " + std::to_string(type) + " is not JSON-coercible");
    }
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

Result<GDExtensionObjectPtr> singleton(const std::string& name) {
    auto& api = GodotApi::instance();
    NativeName native_name(name);
    if (!native_name.valid()) return Error::internal("Failed to construct singleton name: " + name);
    auto object = api.global_get_singleton(native_name.ptr());
    if (!object) return Error::notConnected("Godot singleton is unavailable: " + name);
    return object;
}

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

Result<void> validateGroupName(const std::string& group) {
    if (group.empty() || group.size() > 128 || group.find('/') != std::string::npos ||
        group.find('\\') != std::string::npos) {
        return Error::invalidArgument("group must be a non-empty name without path separators");
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

Result<void> validateActionName(const std::string& action) {
    if (action.empty() || action.size() > 128) return Error::invalidArgument("action must be a non-empty name");
    for (unsigned char character : action) {
        if (std::iscntrl(character) || std::isspace(character)) {
            return Error::invalidArgument("action may not contain whitespace or control characters");
        }
    }
    return Result<void>::ok();
}

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

Result<VariantValue> makeInputEvent(const json& descriptor) {
    if (!descriptor.is_object() || !descriptor.contains("type") || !descriptor["type"].is_string()) {
        return Error::invalidArgument("Each input event must be an object with a string type");
    }
    const std::string type = descriptor["type"].get<std::string>();
    const char* class_name = nullptr;
    if (type == "key") {
        if (!hasOnlyKeys(descriptor, {"type", "keycode", "physical_keycode", "unicode", "shift", "alt", "ctrl", "meta", "device"})) {
            return Error::invalidArgument("Key event contains an unknown property");
        }
        class_name = "InputEventKey";
    } else if (type == "mouse_button") {
        if (!hasOnlyKeys(descriptor, {"type", "button_index", "device"})) return Error::invalidArgument("Mouse-button event contains an unknown property");
        class_name = "InputEventMouseButton";
    } else if (type == "joypad_button") {
        if (!hasOnlyKeys(descriptor, {"type", "button_index", "device"})) return Error::invalidArgument("Joypad-button event contains an unknown property");
        class_name = "InputEventJoypadButton";
    } else if (type == "joypad_motion") {
        if (!hasOnlyKeys(descriptor, {"type", "axis", "axis_value", "device"})) return Error::invalidArgument("Joypad-motion event contains an unknown property");
        class_name = "InputEventJoypadMotion";
    } else {
        return Error::invalidArgument("Unsupported input event type: " + type);
    }

    NativeName native_class(class_name);
    auto object = GodotApi::instance().classdb_construct_object(native_class.ptr());
    if (!object) return Error::internal("Godot could not construct " + std::string(class_name));
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

    if (descriptor.contains("device")) {
        if (!descriptor["device"].is_number_integer() || descriptor["device"].get<int64_t>() < -1) {
            return fail(Error::invalidArgument("Input event device must be an integer >= -1"));
        }
        auto set = set_int("InputEvent", "set_device", 1286410249LL, descriptor["device"].get<int64_t>());
        if (set.isErr()) return fail(set.error());
    }
    if (type == "key") {
        bool has_identity = false;
        for (const auto* key : {"keycode", "physical_keycode", "unicode"}) {
            if (!descriptor.contains(key)) continue;
            if (!descriptor[key].is_number_integer() || descriptor[key].get<int64_t>() <= 0) {
                return fail(Error::invalidArgument(std::string(key) + " must be a positive integer"));
            }
            const int64_t hash = std::string(key) == "unicode" ? 1286410249LL : 888074362LL;
            auto set = set_int("InputEventKey", (std::string("set_") + key).c_str(), hash, descriptor[key].get<int64_t>());
            if (set.isErr()) return fail(set.error());
            has_identity = true;
        }
        if (!has_identity) return fail(Error::invalidArgument("Key event requires keycode, physical_keycode, or unicode"));
        for (const auto* modifier : {"shift", "alt", "ctrl", "meta"}) {
            if (!descriptor.contains(modifier)) continue;
            if (!descriptor[modifier].is_boolean()) return fail(Error::invalidArgument(std::string(modifier) + " must be boolean"));
            auto set = set_bool((std::string("set_") + modifier + "_pressed").c_str(), descriptor[modifier].get<bool>());
            if (set.isErr()) return fail(set.error());
        }
    } else if (type == "mouse_button" || type == "joypad_button") {
        if (!descriptor.contains("button_index") || !descriptor["button_index"].is_number_integer()) {
            return fail(Error::invalidArgument("button_index is required and must be an integer"));
        }
        const int64_t button = descriptor["button_index"].get<int64_t>();
        const int64_t maximum = type == "mouse_button" ? 9 : 127;
        if (button < (type == "mouse_button" ? 1 : 0) || button > maximum) {
            return fail(Error::invalidArgument("button_index is outside the supported range"));
        }
        auto set = set_int(class_name, "set_button_index", type == "mouse_button" ? 3624991109LL : 1466368136LL, button);
        if (set.isErr()) return fail(set.error());
    } else {
        if (!descriptor.contains("axis") || !descriptor["axis"].is_number_integer() ||
            descriptor["axis"].get<int64_t>() < 0 || descriptor["axis"].get<int64_t>() > 9) {
            return fail(Error::invalidArgument("axis is required and must be in 0..9"));
        }
        if (!descriptor.contains("axis_value") || !descriptor["axis_value"].is_number() ||
            !std::isfinite(descriptor["axis_value"].get<double>()) ||
            descriptor["axis_value"].get<double>() < -1.0 || descriptor["axis_value"].get<double>() > 1.0) {
            return fail(Error::invalidArgument("axis_value is required and must be finite within -1.0..1.0"));
        }
        auto axis = set_int(class_name, "set_axis", 1332685170LL, descriptor["axis"].get<int64_t>());
        if (axis.isErr()) return fail(axis.error());
        auto axis_value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, descriptor["axis_value"].get<double>());
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
            output[modifier] = enabled.value() != 0;
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

Result<void> restoreProjectSetting(GDExtensionObjectPtr project_settings, VariantValue& name,
                                   VariantValue& previous) {
    auto restored = callObject(project_settings, "ProjectSettings", "set_setting", 402577236LL,
                               {&name, &previous});
    if (restored.isErr()) return restored.error();
    auto saved = callObject(project_settings, "ProjectSettings", "save", 166280745LL);
    if (saved.isErr()) return saved.error();
    auto code = scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (code.isErr()) return code.error();
    if (code.value() != 0) return Error::internal("Rollback ProjectSettings.save failed with Error " + std::to_string(code.value()));
    return Result<void>::ok();
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
    size_t segment_start = 0;
    while (segment_start <= path.size()) {
        const size_t segment_end = path.find('/', segment_start);
        const std::string segment = path.substr(
            segment_start,
            segment_end == std::string::npos ? std::string::npos : segment_end - segment_start);
        if (segment == "..") {
            return Error::invalidArgument("Parent-relative '..' paths are not allowed in the edited scene");
        }
        if (segment_end == std::string::npos) break;
        segment_start = segment_end + 1;
    }
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
    if (!object.value()) return Error::notFound("Scene node not found: " + path);
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

Result<bool> objectHasProperty(GDExtensionObjectPtr object, const std::string& property) {
    auto properties = callObject(object, "Object", "get_property_list", 3995934104LL);
    if (properties.isErr()) return properties.error();
    auto size_value = callVariant(properties.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    auto name_key = makeString("name");
    if (name_key.isErr()) return name_key.error();

    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) return index.error();
        auto descriptor = callVariant(properties.value(), "get", {&index.value()});
        if (descriptor.isErr()) return descriptor.error();
        auto name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
        if (name_value.isErr()) return name_value.error();
        const auto type = GodotApi::instance().variant_get_type(name_value.value().ptr());
        if (type != GDEXTENSION_VARIANT_TYPE_STRING && type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
            continue;
        }
        auto name = stringFromVariant(name_value.value(), type);
        if (name.isErr()) return name.error();
        if (name.value() == property) return true;
    }
    return false;
}

// A safety cap, not a usability budget, and the difference matters. The walk had
// no bound at all, so a large enough edited scene could exceed the IPC frame
// before the tool layer saw a byte of it. These numbers exist only to stop that,
// and are set far above any ordinary scene: the smoke fixture's edited scene is
// already about 1700 nodes and 225 KiB, so a tight cap here would silently cut
// scenes people actually work on.
//
// Callers who want a small answer ask for one, with max_nodes, class_filter or
// summary on scene_get_hierarchy. That is the lever for context cost. This is
// the lever for not losing the response entirely.
constexpr size_t kMaxHierarchyNodes = 100000;
constexpr size_t kMaxHierarchyResponseBytes = 8 * 1024 * 1024;
constexpr size_t kHierarchyEnvelopeReserveBytes = 256 * 1024;

struct HierarchyBudget {
    size_t node_count{0};
    size_t estimated_bytes{0};
    bool truncated{false};
};

Result<json> buildHierarchy(GDExtensionObjectPtr node, int depth, int max_depth,
                            const std::string& logical_path,
                            HierarchyBudget& budget) {
    auto name = nodeString(node, "get_name", 2002593661LL);
    auto type = nodeString(node, "get_class", 201670096LL);
    if (name.isErr()) return name.error();
    if (type.isErr()) return type.error();
    const std::string path = logical_path.empty() ? "/root/" + name.value() : logical_path;
    json result = {
        {"name", name.value()}, {"type", type.value()}, {"path", path},
        {"properties", json::object()}, {"children", json::array()}
    };

    // Charged before descending, so the node that crosses the limit is the one
    // that stops rather than the one after it.
    const size_t node_bytes = result.dump().size() + 2;
    const size_t tree_budget = kMaxHierarchyResponseBytes - kHierarchyEnvelopeReserveBytes;
    if (budget.node_count >= kMaxHierarchyNodes ||
        budget.estimated_bytes + node_bytes > tree_budget) {
        budget.truncated = true;
        return json(nullptr);
    }
    budget.estimated_bytes += node_bytes;
    ++budget.node_count;

    if (depth >= max_depth) return result;
    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
    if (children.isErr()) return children.error();
    auto size_result = callVariant(children.value(), "size");
    if (size_result.isErr()) return size_result.error();
    auto size = scalarFromVariant<int64_t>(size_result.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    for (int64_t i = 0; i < size.value(); ++i) {
        auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
        if (index.isErr()) return index.error();
        auto child_variant = callVariant(children.value(), "get", {&index.value()});
        if (child_variant.isErr()) return child_variant.error();
        auto child = objectFromVariant(child_variant.value());
        if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
        auto child_name = nodeString(child.value(), "get_name", 2002593661LL);
        if (child_name.isErr()) return child_name.error();
        auto child_json = buildHierarchy(child.value(), depth + 1, max_depth,
                                         path + "/" + child_name.value(), budget);
        if (child_json.isErr()) return child_json.error();
        if (child_json.value().is_null()) {
            result["children_truncated"] = true;
            break;
        }
        result["children"].push_back(child_json.value());
    }
    return result;
}

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

// Undoes the action just committed for the edited scene. Used when a
// postcondition fails after the commit, so the tool does not report failure
// while the scene and the undo stack already carry the change.
Result<void> undoLastAction(GDExtensionObjectPtr manager, GDExtensionObjectPtr root) {
    auto root_value = makeObject(root);
    if (root_value.isErr()) return root_value.error();
    auto history_id = callObject(manager, "EditorUndoRedoManager", "get_object_history_id",
                                 1107568780LL, {&root_value.value()});
    if (history_id.isErr()) return history_id.error();
    auto history = callObject(manager, "EditorUndoRedoManager", "get_history_undo_redo",
                              2417974513LL, {&history_id.value()});
    if (history.isErr()) return history.error();
    auto undo_redo = objectFromVariant(history.value());
    if (undo_redo.isErr() || !undo_redo.value()) {
        return Error::notFound("No UndoRedo history exists for the edited scene");
    }
    auto available = callObject(undo_redo.value(), "UndoRedo", "has_undo", 36873697LL);
    if (available.isErr()) return available.error();
    auto has_action = scalarFromVariant<GDExtensionBool>(available.value(),
                                                         GDEXTENSION_VARIANT_TYPE_BOOL);
    if (has_action.isErr()) return has_action.error();
    if (!has_action.value()) return Error(409, "Nothing to undo");
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

json liveResult(const json& fields) {
    json result = fields;
    result["execution_mode"] = "live";
    result["is_live_engine"] = true;
    return result;
}

} // namespace

GodotBridge& GodotBridge::instance() {
    static GodotBridge bridge;
    return bridge;
}

Result<std::vector<std::string>> GodotBridge::beginAssetReimport(
    const std::vector<std::string>& paths) {
    auto resolved = resolveReimportPaths(paths);
    if (resolved.isErr()) return resolved.error();
    auto started = startAssetReimport(resolved.value());
    if (started.isErr()) return started.error();
    return resolved;
}

Result<std::vector<std::string>> GodotBridge::resolveReimportPaths(
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
    std::vector<std::string> normalized;
    normalized.reserve(paths.size());
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
        normalized.push_back(resource_path);
    }

    return normalized;
}

Result<void> GodotBridge::startAssetReimport(const std::vector<std::string>& resolved_paths) {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto filesystem = callObject(editor.value(), "EditorInterface", "get_resource_filesystem", 780151678LL);
    if (filesystem.isErr()) return filesystem.error();
    auto object = objectFromVariant(filesystem.value());
    if (object.isErr() || !object.value()) return Error::notConnected("EditorFileSystem is unavailable");
    json path_array = resolved_paths;
    auto godot_paths = makeJsonVariant(path_array);
    if (godot_paths.isErr()) return godot_paths.error();
    auto started = callObject(object.value(), "EditorFileSystem", "reimport_files", 4015028928LL,
                              {&godot_paths.value()});
    if (started.isErr()) return started.error();
    return Result<void>::ok();
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

Result<ViewportIsolationState> GodotBridge::beginViewportIsolation(
    const std::string& node_path, const std::string& camera_identifier,
    const std::string& isolation_background) {
    if (node_path.empty()) return Error::invalidArgument("node_isolation_path must not be empty");
    if (isolation_background != "original" && isolation_background != "transparent") {
        return Error::invalidArgument("isolation_background must be original or transparent");
    }
    auto& api = GodotApi::instance();
    if (!api.object_get_instance_id || !api.object_get_instance_from_id) {
        return Error::internal("Godot object identity API is unavailable");
    }
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    auto target = resolveNode(root.value(), node_path);
    if (target.isErr()) return target.error();
    auto canonical = logicalPathFromEditedRoot(root.value(), target.value());
    if (canonical.isErr()) return canonical.error();

    auto is_class = [&](GDExtensionObjectPtr object, const char* class_name) -> Result<bool> {
        auto name = makeString(class_name);
        if (name.isErr()) return name.error();
        auto called = callObject(object, "Object", "is_class", 3927539163LL, {&name.value()});
        if (called.isErr()) return called.error();
        auto value = scalarFromVariant<GDExtensionBool>(called.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (value.isErr()) return value.error();
        return value.value() != 0;
    };
    auto is_ancestor = [&](GDExtensionObjectPtr ancestor, GDExtensionObjectPtr node) -> Result<bool> {
        auto node_value = makeObject(node);
        if (node_value.isErr()) return node_value.error();
        auto called = callObject(ancestor, "Node", "is_ancestor_of", 3093956946LL,
                                 {&node_value.value()});
        if (called.isErr()) return called.error();
        auto value = scalarFromVariant<GDExtensionBool>(called.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (value.isErr()) return value.error();
        return value.value() != 0;
    };

    ViewportIsolationState state;
    state.canonical_node_path = canonical.value();
    state.isolation_background = isolation_background;
    std::vector<GDExtensionObjectPtr> stack{root.value()};
    constexpr size_t kMaxIsolationNodes = 100000;
    size_t visited = 0;
    while (!stack.empty()) {
        auto current = stack.back();
        stack.pop_back();
        if (++visited > kMaxIsolationNodes) {
            return Error::invalidArgument("Edited scene exceeds the 100000-node isolation limit");
        }

        bool keep = current == target.value();
        if (!keep) {
            auto current_is_ancestor = is_ancestor(current, target.value());
            if (current_is_ancestor.isErr()) return current_is_ancestor.error();
            auto target_is_ancestor = is_ancestor(target.value(), current);
            if (target_is_ancestor.isErr()) return target_is_ancestor.error();
            keep = current_is_ancestor.value() || target_is_ancestor.value();
        }
        if (!keep) {
            std::string owner;
            auto canvas = is_class(current, "CanvasItem");
            if (canvas.isErr()) return canvas.error();
            if (canvas.value()) owner = "CanvasItem";
            else {
                auto node3d = is_class(current, "Node3D");
                if (node3d.isErr()) return node3d.error();
                if (node3d.value()) owner = "Node3D";
            }
            if (!owner.empty()) {
                auto visible_value = callObject(current, owner.c_str(), "is_visible", 36873697LL);
                if (visible_value.isErr()) return visible_value.error();
                auto visible = scalarFromVariant<GDExtensionBool>(visible_value.value(),
                                                                  GDEXTENSION_VARIANT_TYPE_BOOL);
                if (visible.isErr()) return visible.error();
                if (visible.value()) {
                    state.visibility.push_back({static_cast<uint64_t>(api.object_get_instance_id(current)),
                                                owner, true});
                }
            }
        }

        auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        if (include_internal.isErr()) return include_internal.error();
        auto children = callObject(current, "Node", "get_children", 873284517LL,
                                   {&include_internal.value()});
        if (children.isErr()) return children.error();
        auto size_value = callVariant(children.value(), "size");
        if (size_value.isErr()) return size_value.error();
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return size.error();
        for (int64_t i = size.value(); i-- > 0;) {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
            if (index.isErr()) return index.error();
            auto child_value = callVariant(children.value(), "get", {&index.value()});
            if (child_value.isErr()) return child_value.error();
            auto child = objectFromVariant(child_value.value());
            if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child during isolation");
            stack.push_back(child.value());
        }
    }

    RestorationGuard mutation_guard([&]() {
        return restoreViewportIsolation(state);
    });
    auto rollback = [&]() -> Result<ViewportIsolationState> {
        auto restored = restoreViewportIsolation(state);
        mutation_guard.dismiss();
        if (restored.isErr()) {
            return Error(500, "Viewport isolation failed and rollback was incomplete: " +
                              restored.error().message);
        }
        return Error::internal("Viewport isolation mutation failed");
    };
    auto hidden = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (hidden.isErr()) return hidden.error();
    for (const auto& point : state.visibility) {
        auto object = api.object_get_instance_from_id(static_cast<GDObjectInstanceID>(point.instance_id));
        if (!object) return rollback();
        auto changed = callObject(object, point.class_name.c_str(), "set_visible", 2586408642LL,
                                  {&hidden.value()});
        if (changed.isErr()) return rollback();
    }

    if (isolation_background == "transparent") {
        bool capture_2d = camera_identifier == "editor_2d" ||
                          camera_identifier == "active_editor_view_2d";
        Result<VariantValue> viewport = capture_2d
            ? callObject(editor.value(), "EditorInterface", "get_editor_viewport_2d", 3750751911LL)
            : [&]() -> Result<VariantValue> {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
                if (index.isErr()) return index.error();
                return callObject(editor.value(), "EditorInterface", "get_editor_viewport_3d",
                                  1970834490LL, {&index.value()});
            }();
        if (viewport.isErr()) return rollback();
        auto viewport_object = objectFromVariant(viewport.value());
        if (viewport_object.isErr() || !viewport_object.value()) return rollback();
        auto original_value = callObject(viewport_object.value(), "Viewport",
                                         "has_transparent_background", 36873697LL);
        if (original_value.isErr()) return rollback();
        auto original = scalarFromVariant<GDExtensionBool>(original_value.value(),
                                                           GDEXTENSION_VARIANT_TYPE_BOOL);
        if (original.isErr()) return rollback();
        state.viewport_instance_id = static_cast<uint64_t>(
            api.object_get_instance_id(viewport_object.value()));
        state.original_transparent_background = original.value() != 0;
        state.restore_transparent_background = !state.original_transparent_background;
        if (state.restore_transparent_background) {
            auto enabled = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
            if (enabled.isErr()) return rollback();
            auto changed = callObject(viewport_object.value(), "Viewport", "set_transparent_background",
                                      2586408642LL, {&enabled.value()});
            if (changed.isErr()) return rollback();
        }
    }
    mutation_guard.dismiss();
    return state;
}

Result<void> GodotBridge::restoreViewportIsolation(const ViewportIsolationState& state) {
    auto& api = GodotApi::instance();
    if (!api.object_get_instance_from_id) return Error::internal("Godot object identity API is unavailable");
    std::vector<std::string> failures;
    if (state.restore_transparent_background) {
        auto viewport = api.object_get_instance_from_id(
            static_cast<GDObjectInstanceID>(state.viewport_instance_id));
        if (!viewport) failures.push_back("viewport was freed");
        else {
            auto original = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                       static_cast<GDExtensionBool>(state.original_transparent_background));
            auto restored = original.isOk()
                ? callObject(viewport, "Viewport", "set_transparent_background", 2586408642LL,
                             {&original.value()})
                : Result<VariantValue>(original.error());
            if (restored.isErr()) failures.push_back("viewport background restore failed");
        }
    }
    for (auto it = state.visibility.rbegin(); it != state.visibility.rend(); ++it) {
        auto object = api.object_get_instance_from_id(static_cast<GDObjectInstanceID>(it->instance_id));
        if (!object) {
            failures.push_back("scene object was freed");
            continue;
        }
        auto visible = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                  static_cast<GDExtensionBool>(it->visible));
        if (visible.isErr()) {
            failures.push_back("visibility value construction failed");
            continue;
        }
        auto restored = callObject(object, it->class_name.c_str(), "set_visible", 2586408642LL,
                                   {&visible.value()});
        if (restored.isErr()) failures.push_back("scene visibility restore failed");
    }
    if (!failures.empty()) {
        return Error(409, "Temporary viewport isolation could not fully restore editor state (" +
                          std::to_string(failures.size()) + " failure(s))");
    }
    return Result<void>::ok();
}

Result<void> GodotBridge::forceDraw() {
    auto& api = GodotApi::instance();
    if (!api.isLiveReady()) return Error::notConnected("Godot main-loop bridge is not ready");
    NativeName name("RenderingServer");
    auto server = api.global_get_singleton(name.ptr());
    if (!server) return Error::notConnected("RenderingServer singleton is unavailable");
    auto swap_buffers = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    auto frame_step = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, 0.0);
    if (swap_buffers.isErr()) return swap_buffers.error();
    if (frame_step.isErr()) return frame_step.error();
    auto drawn = callObject(server, "RenderingServer", "force_draw", 1076185472LL,
                            {&swap_buffers.value(), &frame_step.value()});
    if (drawn.isErr()) return drawn.error();
    return Result<void>::ok();
}

json GodotBridge::execute(const std::string& method, const json& params,
                          const std::string& session_kind) {
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
    if (method == "phase7SignalTest.configure") {
        static const std::set<std::string> admitted = {
            "malformed_metadata", "missing_required_api", "conversion_failure",
            "connect_postcondition_mismatch", "disconnect_postcondition_mismatch",
            "missing_destination_float_constructor",
            "connect_postcondition_mismatch_rollback_failure"};
        if (!hasOnlyKeys(params, {"seam"}) || !params.contains("seam") ||
            !params["seam"].is_string() || !admitted.count(params["seam"].get<std::string>())) {
            return errorJson(400, "invalid_phase7_signal_test_seam");
        }
        g_phase7SignalTestSeam = params["seam"].get<std::string>();
        return liveResult({{"status", "configured"}});
    }
#endif
    if (method == "runtime.evalGdscript") {
        return executeExpression(params, session_kind);
    }
    if (method == "runtime.getTree" || method == "runtime.setPaused" ||
        method == "runtime.stop") {
        return executeRuntimeBridge(method, params, session_kind);
    }
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return errorJson(editor_result.error().code, editor_result.error().message);
    auto editor = editor_result.value();

    if (method == "signal.listConnections" || method == "signal.connect" ||
        method == "signal.disconnect" || method == "signal.emit") {
        if (session_kind != "editor") return errorJson(409, "session_kind_rejected");

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
                return Error(413, "signal_metadata_work_limit");
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
                    return Error(413, "signal_argument_metadata_work_limit");
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
                return errorJson(400, "invalid_signal_list_connections_request");
            }
            if (!preflight_object_binds({
                    {"get_signal_list", 3995934104LL},
                    {"get_signal_connection_list", 3147814860LL},
                    {"is_class", 3927539163LL}})) {
                return errorJson(501, "required_bind_unavailable");
            }
            auto root = editedSceneRoot(editor);
            if (root.isErr()) return errorJson(root.error().code, root.error().message);
            auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
            if (target.isErr()) return errorJson(target.error().code, target.error().message);
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
            if (takePhase7SignalTestSeam("malformed_metadata")) {
                return errorJson(500, "extension_protocol_error");
            }
#endif
            auto signals = callObject(target.value(), "Object", "get_signal_list", 3995934104LL);
            if (signals.isErr()) return errorJson(500, signals.error().message);
            auto native_signal_count = array_size(signals.value());
            if (native_signal_count.isErr() || native_signal_count.value() < 0) {
                return errorJson(500, "extension_protocol_error");
            }
            if (native_signal_count.value() > kSignalNativeWorkCeiling) {
                return errorJson(413, "signal_metadata_work_limit");
            }
            struct SignalRecord { std::string name; json arguments; };
            std::vector<SignalRecord> descriptors;
            descriptors.reserve(static_cast<size_t>(native_signal_count.value()));
            for (int64_t index = 0; index < native_signal_count.value(); ++index) {
                auto native_descriptor = array_at(signals.value(), index);
                if (native_descriptor.isErr()) return errorJson(500, "extension_protocol_error");
                auto serialized = variantToJson(native_descriptor.value());
                if (serialized.isErr() || !serialized.value().is_object() ||
                    !serialized.value().contains("name") ||
                    !serialized.value()["name"].is_string()) {
                    return errorJson(500, "extension_protocol_error");
                }
                auto arguments = serialized.value().value("args", json::array());
                if (!arguments.is_array()) return errorJson(500, "extension_protocol_error");
                if (arguments.size() > static_cast<size_t>(kSignalArgumentWorkCeiling)) {
                    return errorJson(413, "signal_argument_metadata_work_limit");
                }
                const auto name = serialized.value()["name"].get<std::string>();
                auto validated = utf8_prefix(name, name.size());
                if (validated.isErr() || validated.value().size() != name.size()) {
                    return errorJson(500, "extension_protocol_error");
                }
                descriptors.push_back({name, std::move(arguments)});
            }
            std::sort(descriptors.begin(), descriptors.end(),
                      [&](const SignalRecord& left, const SignalRecord& right) {
                          return utf8_less(left.name, right.name);
                      });

            bool truncated = false;
            json truncated_at = nullptr;
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
                if (name.isErr()) return errorJson(500, "extension_protocol_error");
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
                    if (parsed.isErr()) return errorJson(500, "extension_protocol_error");
                    const auto& raw_argument_name = parsed.value().name;
                    auto validated = utf8_prefix(raw_argument_name, raw_argument_name.size());
                    if (validated.isErr() || validated.value().size() != raw_argument_name.size()) {
                        return errorJson(500, "extension_protocol_error");
                    }
                    auto argument_name = utf8_prefix(raw_argument_name, 256);
                    if (argument_name.isErr()) return errorJson(500, "extension_protocol_error");
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
                    return errorJson(500, "extension_protocol_error");
                }
                if (connection_count_result.value() > kSignalNativeWorkCeiling) {
                    return errorJson(413, "signal_connection_metadata_work_limit");
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
                    if (connection.isErr()) return errorJson(500, "extension_protocol_error");
                    auto callable = dictionary_field(connection.value(), "callable");
                    auto flags_value = dictionary_field(connection.value(), "flags");
                    if (callable.isErr() || flags_value.isErr() ||
                        GodotApi::instance().variant_get_type(callable.value().ptr()) !=
                            GDEXTENSION_VARIANT_TYPE_CALLABLE) {
                        return errorJson(500, "extension_protocol_error");
                    }
                    auto flags = scalarFromVariant<int64_t>(
                        flags_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                    auto callable_object_value = callVariant(callable.value(), "get_object");
                    auto callable_method_value = callVariant(callable.value(), "get_method");
                    if (flags.isErr() || callable_object_value.isErr() ||
                        callable_method_value.isErr()) {
                        return errorJson(500, "extension_protocol_error");
                    }
                    if (flags.value() < 0 || flags.value() > 15) {
                        return errorJson(500, "extension_protocol_error");
                    }
                    auto callable_object = objectFromVariant(callable_object_value.value());
                    const auto callable_method_type = GodotApi::instance().variant_get_type(
                        callable_method_value.value().ptr());
                    auto callable_method = stringFromVariant(
                        callable_method_value.value(), callable_method_type);
                    if (callable_object.isErr() || callable_method.isErr()) {
                        return errorJson(500, "extension_protocol_error");
                    }
                    auto validated_method = utf8_prefix(
                        callable_method.value(), callable_method.value().size());
                    if (validated_method.isErr() ||
                        validated_method.value().size() != callable_method.value().size()) {
                        return errorJson(500, "extension_protocol_error");
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
                                    return errorJson(500, "extension_protocol_error");
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
                    if (bounded_method.isErr()) return errorJson(500, "extension_protocol_error");
                    if (bounded_method.value().size() != record.target_method.size()) {
                        mark_truncated("bytes");
                    }
                    json target_path = nullptr;
                    if (record.target_node.has_value()) {
                        auto bounded_path = utf8_prefix(*record.target_node, 1024);
                        if (bounded_path.isErr()) return errorJson(500, "extension_protocol_error");
                        if (bounded_path.value().size() != record.target_node->size()) {
                            mark_truncated("bytes");
                        }
                        target_path = bounded_path.value();
                    }
                    connections.push_back({{"target_node", std::move(target_path)},
                                           {"target_method", bounded_method.value()},
                                           {"flags", record.flags}});
                }
                json signal = {{"name", name.value()}, {"arguments", std::move(arguments)},
                               {"connections", std::move(connections)}};
                output_signals.push_back(std::move(signal));
                json candidate = {{"target_node", params["target_node"]},
                                  {"signals", output_signals},
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
                             {"truncated", truncated},
                             {"truncated_at", truncated_at}};
            auto live = liveResult(response);
            if (live.dump().size() > 64u * 1024u) {
                return errorJson(413, "response_limit");
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
                    params["flags"].is_number_unsigned()) || params["flags"] != 2))) {
                return errorJson(400, is_connect ? "invalid_signal_connect_request"
                                                 : "invalid_signal_disconnect_request");
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
                return errorJson(501, "required_bind_unavailable");
            }
            if (requireMethodBind("EditorUndoRedoManager", "get_object_history_id",
                                  1107568780LL).isErr() ||
                requireMethodBind("EditorUndoRedoManager", "get_history_undo_redo",
                                  2417974513LL).isErr() ||
                requireMethodBind("UndoRedo", "has_undo", 36873697LL).isErr() ||
                requireMethodBind("UndoRedo", "undo", 2240911060LL).isErr()) {
                return errorJson(501, "required_bind_unavailable");
            }

            auto root = editedSceneRoot(editor);
            if (root.isErr()) return errorJson(root.error().code, root.error().message);
            auto emitter = resolveNode(root.value(), params["emitter_node"].get<std::string>());
            auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
            if (emitter.isErr()) return errorJson(emitter.error().code, emitter.error().message);
            if (target.isErr()) return errorJson(target.error().code, target.error().message);
            const auto signal_name = params["signal_name"].get<std::string>();
            const auto target_method = params["target_method"].get<std::string>();
            auto signal_name_value = makeStringName(signal_name);
            auto method_name_value = makeStringName(target_method);
            if (signal_name_value.isErr() || method_name_value.isErr()) {
                return errorJson(500, "extension_protocol_error");
            }
            auto has_signal_value = callObject(
                emitter.value(), "Object", "has_signal", 2619796661LL,
                {&signal_name_value.value()});
            auto has_method_value = callObject(
                target.value(), "Object", "has_method", 2619796661LL,
                {&method_name_value.value()});
            if (has_signal_value.isErr() || has_method_value.isErr()) {
                return errorJson(500, "extension_protocol_error");
            }
            auto has_signal = scalarFromVariant<GDExtensionBool>(
                has_signal_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            auto has_method = scalarFromVariant<GDExtensionBool>(
                has_method_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (has_signal.isErr() || has_method.isErr()) {
                return errorJson(500, "extension_protocol_error");
            }
            if (!has_signal.value() || !has_method.value()) {
                return errorJson(404, !has_signal.value() ? "declared_signal_not_found"
                                                          : "target_method_not_found");
            }
            auto signal = signal_metadata(emitter.value(), signal_name);
            auto target_metadata = method_metadata(target.value(), target_method);
            if (signal.isErr()) return errorJson(signal.error().code, signal.error().message);
            if (target_metadata.isErr()) {
                return errorJson(target_metadata.error().code, target_metadata.error().message);
            }
            const int64_t signal_arity = static_cast<int64_t>(
                signal.value().arguments.size());
            if (signal_arity < target_metadata.value().required_arguments ||
                (!target_metadata.value().vararg &&
                 signal_arity > target_metadata.value().total_arguments)) {
                return errorJson(409, "signal_target_arity_incompatible");
            }
            auto callable = make_callable(target.value(), target_method);
            if (callable.isErr()) return errorJson(501, "required_bind_unavailable");

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
                return errorJson(500, "extension_protocol_error");
            }
            if ((connected.value() != 0) != !matches.value().empty()) {
                return errorJson(500, "signal_connection_state_inconsistent");
            }
            if (is_connect && !matches.value().empty()) {
                return errorJson(409, "signal_connection_already_exists");
            }
            if (is_disconnect && matches.value().size() != 1) {
                return errorJson(409, "missing_or_ambiguous_signal_connection");
            }
            if (is_disconnect && matches.value().front().flags != 2) {
                return errorJson(409, "unsupported_existing_connection_flags");
            }

            auto manager = undoManager(editor);
            if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
            auto flags_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, int64_t{2});
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
                return errorJson(500, "signal_undo_redo_registration_failed");
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
                              observed_matches.value().front().flags == 2
                        : !observed_connected.value() && observed_matches.value().empty();
                }
            }
            if (force_postcondition_mismatch) postcondition_ok = false;
            if (!postcondition_ok) {
                auto root_value = makeObject(root.value());
                Result<void> rolled_back = Result<void>::ok();
                if (root_value.isErr()) {
                    rolled_back = root_value.error();
                } else {
                    auto history_id = callObject(
                        manager.value(), "EditorUndoRedoManager", "get_object_history_id",
                        1107568780LL, {&root_value.value()});
                    if (history_id.isErr()) {
                        rolled_back = history_id.error();
                    } else {
                        auto history = callObject(
                            manager.value(), "EditorUndoRedoManager", "get_history_undo_redo",
                            2417974513LL, {&history_id.value()});
                        if (history.isErr()) {
                            rolled_back = history.error();
                        } else {
                            auto undo_redo = objectFromVariant(history.value());
                            if (undo_redo.isErr() || !undo_redo.value()) {
                                rolled_back = Error::internal(
                                    "Committed signal history is unavailable");
                            } else {
                                auto has_undo_value = callObject(
                                    undo_redo.value(), "UndoRedo", "has_undo", 36873697LL);
                                if (has_undo_value.isErr()) {
                                    rolled_back = has_undo_value.error();
                                } else {
                                    auto has_undo = scalarFromVariant<GDExtensionBool>(
                                        has_undo_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                                    if (has_undo.isErr() || !has_undo.value()) {
                                        rolled_back = Error::internal(
                                            "Committed signal action is not active");
                                    } else {
                                        auto undone = callObject(
                                            undo_redo.value(), "UndoRedo", "undo", 2240911060LL);
                                        if (undone.isErr()) rolled_back = undone.error();
                                    }
                                }
                            }
                        }
                    }
                }
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
                                      restored_matches.value().front().flags == 2;
                        }
                    }
                }
                if (force_rollback_failure) restored = false;
                return errorJson(
                    500, "signal_postcondition_mismatch",
                    {{"retryable", false},
                     {"rollback", restored ? "completed" : "failed"},
                     {"outcome", restored ? "rolled_back" : "unknown"},
                     {"restoration_observed", restored}});
            }
            if (is_connect) {
                return liveResult({{"connected", true}, {"flags", 2},
                                   {"undo_redo_registered", true},
                                   {"outcome", "completed"}, {"rollback", "undo_redo"}});
            }
            return liveResult({{"disconnected", true}, {"flags", 2},
                               {"undo_redo_registered", true},
                               {"outcome", "completed"}, {"rollback", "undo_redo"}});
        }

        if (!hasOnlyKeys(params, {"target_node", "signal_name", "arguments"}) ||
            !params.contains("target_node") ||
            !bounded_string(params["target_node"], 1, 1024) ||
            !params.contains("signal_name") ||
            !bounded_string(params["signal_name"], 1, 128) ||
            (params.contains("arguments") && !params["arguments"].is_array())) {
            return errorJson(400, "invalid_signal_emit_request");
        }
        const auto emit_arguments = params.value("arguments", json::array());
        if (emit_arguments.size() > 16) {
            return errorJson(400, "signal_emit_argument_count_exceeded");
        }
        for (const auto& argument : emit_arguments) {
            if (!valid_emit_value(argument, 0)) {
                return errorJson(400, "unsupported_signal_emit_argument");
            }
        }
        try {
            if (emit_arguments.dump().size() > 32u * 1024u) {
                return errorJson(413, "signal_emit_arguments_too_large");
            }
        } catch (const json::exception&) {
            return errorJson(400, "invalid_signal_emit_argument_encoding");
        }
        if (!preflight_object_binds({{"get_signal_list", 3995934104LL},
                                     {"has_signal", 2619796661LL},
                                     {"emit_signal", 4047867050LL}})) {
            return errorJson(501, "required_bind_unavailable");
        }
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto target = resolveNode(root.value(), params["target_node"].get<std::string>());
        if (target.isErr()) return errorJson(target.error().code, target.error().message);
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
        if (!has_signal.value()) return errorJson(404, "declared_signal_not_found");
        auto metadata = signal_metadata(target.value(), signal_name);
        if (metadata.isErr()) return errorJson(metadata.error().code, metadata.error().message);
        if (metadata.value().arguments.size() != emit_arguments.size()) {
            return errorJson(409, "signal_emit_arity_mismatch");
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
                return errorJson(400, "signal_emit_argument_type_mismatch");
            }
        }
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
        if (takePhase7SignalTestSeam("missing_required_api")) {
            return errorJson(501, "required_bind_unavailable");
        }
#endif
        for (size_t index = 0; index < emit_arguments.size(); ++index) {
            if (!preflight_json_variant(
                    emit_arguments[index],
                    static_cast<GDExtensionVariantType>(metadata.value().arguments[index].type))) {
                return errorJson(501, "required_bind_unavailable");
            }
        }
#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)
        if (takePhase7SignalTestSeam("conversion_failure")) {
            return errorJson(500, "extension_protocol_error");
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
            if (converted.isErr()) return errorJson(500, "extension_protocol_error");
            native_arguments.push_back(std::move(converted.value()));
        }
        std::vector<const VariantValue*> call_arguments{&signal_name_value.value()};
        call_arguments.reserve(native_arguments.size() + 1);
        for (auto& argument : native_arguments) call_arguments.push_back(&argument);
        auto emitted = callObject(target.value(), "Object", "emit_signal", 4047867050LL,
                                  call_arguments);
        if (emitted.isErr()) return errorJson(500, emitted.error().message);
        auto emit_code = scalarFromVariant<int64_t>(
            emitted.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (emit_code.isErr()) return errorJson(500, emit_code.error().message);
        if (emit_code.value() != 0) return errorJson(500, "signal_emit_failed");
        return liveResult({{"emitted", true},
                           {"argument_count", emit_arguments.size()},
                           {"outcome", "completed"},
                           {"rollback", "not_available"}});
    }

    if (method == "ui.hitTest") {
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

        auto edited_root = editedSceneRoot(editor);
        if (edited_root.isErr()) return errorJson(edited_root.error().code, edited_root.error().message);
        const std::string requested_root = params.value("root_path", "/root");
        auto traversal_root = resolveNode(edited_root.value(), requested_root);
        if (traversal_root.isErr()) return errorJson(traversal_root.error().code, traversal_root.error().message);
        auto point = makeVector2(x, y);
        if (point.isErr()) return errorJson(point.error().code, point.error().message);

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
                        auto path = logicalPathFromEditedRoot(edited_root.value(), node);
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
                        hits.push_back({
                            {{"node_path", path.value()}, {"class", class_name.value()},
                             {"mouse_filter", filter_name}, {"mouse_filter_value", mouse_filter.value()},
                             {"canvas_layer", canvas_layer}, {"effective_z_index", effective_z},
                             {"draw_order", current_order}, {"local_point", local_json.value()},
                             {"global_rect", rect_json.value()}},
                            canvas_layer, effective_z, current_order
                        });
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
        if (visited.isErr()) return errorJson(visited.error().code, visited.error().message);
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
        return liveResult({
            {"point", {{"x", x}, {"y", y}}}, {"root_path", requested_root},
            {"hits", output_hits}, {"topmost", topmost}, {"hit_count_total", total_hits},
            {"returned_count", output_hits.size()}, {"traversed_nodes", traversed},
            {"truncated", total_hits > output_hits.size()},
            {"ordering", "canvas_layer_desc,effective_z_index_desc,scene_draw_order_desc"},
            {"input_injected", false}
        });
    }

    if (method == "project.getSetting" || method == "project.setSetting") {
        const std::string setting = params.value("setting", "");
        auto valid_name = method == "project.setSetting"
            ? validateGenericSettingName(setting)
            : validateSettingName(setting);
        if (valid_name.isErr()) return errorJson(valid_name.error().code, valid_name.error().message);

        auto project_settings = singleton("ProjectSettings");
        if (project_settings.isErr()) {
            return errorJson(project_settings.error().code, project_settings.error().message);
        }
        auto name = makeStringName(setting);
        if (name.isErr()) return errorJson(name.error().code, name.error().message);
        auto exists_value = callObject(project_settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                                       {&name.value()});
        if (exists_value.isErr()) return errorJson(exists_value.error().code, exists_value.error().message);
        auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (exists.isErr()) return errorJson(exists.error().code, exists.error().message);

        if (method == "project.getSetting") {
            if (!exists.value()) return errorJson(404, "Project setting not found: " + setting);
            VariantValue default_value;
            auto current = callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                      {&name.value(), &default_value});
            if (current.isErr()) return errorJson(current.error().code, current.error().message);
            auto value = variantToJson(current.value());
            if (value.isErr()) return errorJson(value.error().code, value.error().message);
            return liveResult({{"status", "success"}, {"setting", setting}, {"value", value.value()}});
        }

        const bool remove = params.value("remove", false);
        if (remove && params.contains("value")) {
            return errorJson(400, "Specify either value or remove: true, not both");
        }
        if (!remove && !params.contains("value")) {
            return errorJson(400, "value is required unless remove is true");
        }
        if (remove && !exists.value()) return errorJson(404, "Project setting not found: " + setting);

        VariantValue default_value;
        auto previous = exists.value()
            ? callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                         {&name.value(), &default_value})
            : Result<VariantValue>(VariantValue{});
        if (previous.isErr()) return errorJson(previous.error().code, previous.error().message);
        auto replacement = remove ? Result<VariantValue>(VariantValue{}) : makeJsonVariant(params["value"]);
        if (replacement.isErr()) return errorJson(replacement.error().code, replacement.error().message);

        auto applied = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                                  {&name.value(), &replacement.value()});
        if (applied.isErr()) return errorJson(applied.error().code, applied.error().message);
        auto saved = callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL);
        auto save_code = saved.isOk()
            ? scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT)
            : Result<int64_t>(saved.error());
        if (save_code.isErr() || save_code.value() != 0) {
            auto rollback_save = restoreProjectSetting(project_settings.value(), name.value(), previous.value());
            const std::string detail = save_code.isErr()
                ? save_code.error().message
                : "Godot Error " + std::to_string(save_code.value());
            if (rollback_save.isErr()) {
                return errorJson(500, "ProjectSettings.save failed (" + detail + ") and rollback failed: " +
                                      rollback_save.error().message);
            }
            return errorJson(500, "ProjectSettings.save failed; mutation was rolled back (" + detail + ")");
        }
        return liveResult({{"status", "success"}, {"setting", setting}, {"persisted", true},
                           {"removed", remove}});
    }

    if (method == "project.listAutoloads" || method == "project.setAutoload" ||
        method == "project.removeAutoload") {
        auto project_settings = singleton("ProjectSettings");
        if (project_settings.isErr()) return errorJson(project_settings.error().code, project_settings.error().message);

        if (method == "project.listAutoloads") {
            auto properties = callObject(project_settings.value(), "Object", "get_property_list", 3995934104LL);
            if (properties.isErr()) return errorJson(properties.error().code, properties.error().message);
            auto size_value = callVariant(properties.value(), "size");
            if (size_value.isErr()) return errorJson(size_value.error().code, size_value.error().message);
            auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return errorJson(size.error().code, size.error().message);
            auto name_key = makeString("name");
            if (name_key.isErr()) return errorJson(name_key.error().code, name_key.error().message);
            std::vector<json> entries;
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return errorJson(index.error().code, index.error().message);
                auto descriptor = callVariant(properties.value(), "get", {&index.value()});
                if (descriptor.isErr()) return errorJson(descriptor.error().code, descriptor.error().message);
                auto property_name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
                if (property_name_value.isErr()) return errorJson(property_name_value.error().code, property_name_value.error().message);
                auto property_type = GodotApi::instance().variant_get_type(property_name_value.value().ptr());
                if (property_type != GDEXTENSION_VARIANT_TYPE_STRING && property_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) continue;
                auto property_name = stringFromVariant(property_name_value.value(), property_type);
                if (property_name.isErr()) return errorJson(property_name.error().code, property_name.error().message);
                if (!strings::startsWith(property_name.value(), "autoload/") || property_name.value().size() <= 9) continue;
                auto setting_name = makeStringName(property_name.value());
                VariantValue default_value;
                if (setting_name.isErr()) return errorJson(setting_name.error().code, setting_name.error().message);
                auto setting = callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                          {&setting_name.value(), &default_value});
                if (setting.isErr()) return errorJson(setting.error().code, setting.error().message);
                auto setting_type = GodotApi::instance().variant_get_type(setting.value().ptr());
                auto encoded = stringFromVariant(setting.value(), setting_type);
                if (encoded.isErr()) return errorJson(encoded.error().code, encoded.error().message);
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
        if (valid_name.isErr()) return errorJson(valid_name.error().code, valid_name.error().message);
        const std::string setting_path = "autoload/" + autoload_name;
        auto setting_name = makeStringName(setting_path);
        if (setting_name.isErr()) return errorJson(setting_name.error().code, setting_name.error().message);
        auto exists_value = callObject(project_settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                                       {&setting_name.value()});
        if (exists_value.isErr()) return errorJson(exists_value.error().code, exists_value.error().message);
        auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (exists.isErr()) return errorJson(exists.error().code, exists.error().message);
        const bool removing = method == "project.removeAutoload";
        if (removing && !exists.value()) return errorJson(404, "Autoload not found: " + autoload_name);
        if (!removing && exists.value() && !params.value("replace", false)) {
            return errorJson(409, "Autoload already exists; pass replace: true to update it");
        }

        VariantValue default_value;
        auto previous = exists.value()
            ? callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                         {&setting_name.value(), &default_value})
            : Result<VariantValue>(VariantValue{});
        if (previous.isErr()) return errorJson(previous.error().code, previous.error().message);
        Result<VariantValue> replacement(VariantValue{});
        std::string resource_path;
        bool autoload_singleton = true;
        if (!removing) {
            resource_path = params.value("path", "");
            auto valid_path = strings::endsWith(resource_path, ".gd") || strings::endsWith(resource_path, ".cs")
                ? validateScriptPath(resource_path)
                : validateResPath(resource_path, ".tscn");
            if (valid_path.isErr()) return errorJson(valid_path.error().code, valid_path.error().message);
            auto loader = singleton("ResourceLoader");
            if (loader.isErr()) return errorJson(loader.error().code, loader.error().message);
            auto path = makeString(resource_path);
            auto hint = makeString("");
            if (path.isErr() || hint.isErr()) return errorJson(500, "Failed to construct resource existence arguments");
            auto resource_exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                                    {&path.value(), &hint.value()});
            if (resource_exists_value.isErr()) return errorJson(resource_exists_value.error().code, resource_exists_value.error().message);
            auto resource_exists = scalarFromVariant<GDExtensionBool>(resource_exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (resource_exists.isErr()) return errorJson(resource_exists.error().code, resource_exists.error().message);
            if (!resource_exists.value()) return errorJson(404, "Autoload resource not found: " + resource_path);
            autoload_singleton = params.value("singleton", true);
            replacement = makeString((autoload_singleton ? "*" : "") + resource_path);
            if (replacement.isErr()) return errorJson(replacement.error().code, replacement.error().message);
        }

        auto applied = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                                  {&setting_name.value(), &replacement.value()});
        if (applied.isErr()) return errorJson(applied.error().code, applied.error().message);
        auto saved = callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL);
        auto save_code = saved.isOk()
            ? scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT)
            : Result<int64_t>(saved.error());
        if (save_code.isErr() || save_code.value() != 0) {
            auto rollback = restoreProjectSetting(project_settings.value(), setting_name.value(), previous.value());
            if (rollback.isErr()) return errorJson(500, "Autoload save failed and rollback failed: " + rollback.error().message);
            return errorJson(500, "ProjectSettings.save failed; autoload mutation was rolled back");
        }
        return liveResult({{"status", "success"}, {"name", autoload_name}, {"path", resource_path},
                           {"singleton", autoload_singleton}, {"removed", removing}, {"persisted", true}});
    }

    if (method == "project.listInputActions" || method == "project.setInputAction" ||
        method == "project.removeInputAction") {
        auto project_settings = singleton("ProjectSettings");
        if (project_settings.isErr()) return errorJson(project_settings.error().code, project_settings.error().message);

        if (method == "project.listInputActions") {
            auto properties = callObject(project_settings.value(), "Object", "get_property_list", 3995934104LL);
            if (properties.isErr()) return errorJson(properties.error().code, properties.error().message);
            auto size_value = callVariant(properties.value(), "size");
            if (size_value.isErr()) return errorJson(size_value.error().code, size_value.error().message);
            auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return errorJson(size.error().code, size.error().message);
            auto name_key = makeString("name");
            auto deadzone_key = makeString("deadzone");
            auto events_key = makeString("events");
            if (name_key.isErr() || deadzone_key.isErr() || events_key.isErr()) return errorJson(500, "Failed to construct InputMap keys");
            std::vector<json> actions;
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return errorJson(index.error().code, index.error().message);
                auto descriptor = callVariant(properties.value(), "get", {&index.value()});
                if (descriptor.isErr()) return errorJson(descriptor.error().code, descriptor.error().message);
                auto property_name_value = callVariant(descriptor.value(), "get", {&name_key.value()});
                if (property_name_value.isErr()) return errorJson(property_name_value.error().code, property_name_value.error().message);
                auto property_type = GodotApi::instance().variant_get_type(property_name_value.value().ptr());
                if (property_type != GDEXTENSION_VARIANT_TYPE_STRING && property_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) continue;
                auto property_name = stringFromVariant(property_name_value.value(), property_type);
                if (property_name.isErr()) return errorJson(property_name.error().code, property_name.error().message);
                if (!strings::startsWith(property_name.value(), "input/") || property_name.value().size() <= 6) continue;
                auto setting_name = makeStringName(property_name.value());
                VariantValue default_value;
                if (setting_name.isErr()) return errorJson(setting_name.error().code, setting_name.error().message);
                auto setting = callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                          {&setting_name.value(), &default_value});
                if (setting.isErr()) return errorJson(setting.error().code, setting.error().message);
                if (GodotApi::instance().variant_get_type(setting.value().ptr()) != GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
                    return errorJson(422, "InputMap setting is not a Dictionary: " + property_name.value());
                }
                auto deadzone_value = callVariant(setting.value(), "get", {&deadzone_key.value()});
                auto events_value = callVariant(setting.value(), "get", {&events_key.value()});
                if (deadzone_value.isErr() || events_value.isErr()) return errorJson(422, "InputMap setting is missing deadzone or events");
                auto deadzone = scalarFromVariant<double>(deadzone_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
                if (deadzone.isErr()) return errorJson(deadzone.error().code, deadzone.error().message);
                auto event_count_value = callVariant(events_value.value(), "size");
                if (event_count_value.isErr()) return errorJson(event_count_value.error().code, event_count_value.error().message);
                auto event_count = scalarFromVariant<int64_t>(event_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                if (event_count.isErr()) return errorJson(event_count.error().code, event_count.error().message);
                json events = json::array();
                for (int64_t event_index = 0; event_index < event_count.value(); ++event_index) {
                    auto native_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, event_index);
                    if (native_index.isErr()) return errorJson(native_index.error().code, native_index.error().message);
                    auto event = callVariant(events_value.value(), "get", {&native_index.value()});
                    if (event.isErr()) return errorJson(event.error().code, event.error().message);
                    auto normalized = inputEventToJson(event.value());
                    if (normalized.isErr()) return errorJson(normalized.error().code, normalized.error().message);
                    events.push_back(normalized.value());
                }
                actions.push_back({{"action", property_name.value().substr(6)}, {"deadzone", deadzone.value()}, {"events", events}});
            }
            std::sort(actions.begin(), actions.end(), [](const json& left, const json& right) {
                return left["action"].get<std::string>() < right["action"].get<std::string>();
            });
            return liveResult({{"status", "success"}, {"actions", actions}});
        }

        const std::string action = params.value("action", "");
        auto valid_action = validateActionName(action);
        if (valid_action.isErr()) return errorJson(valid_action.error().code, valid_action.error().message);
        const std::string setting_path = "input/" + action;
        auto setting_name = makeStringName(setting_path);
        if (setting_name.isErr()) return errorJson(setting_name.error().code, setting_name.error().message);
        auto exists_value = callObject(project_settings.value(), "ProjectSettings", "has_setting", 3927539163LL,
                                       {&setting_name.value()});
        if (exists_value.isErr()) return errorJson(exists_value.error().code, exists_value.error().message);
        auto exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (exists.isErr()) return errorJson(exists.error().code, exists.error().message);
        const bool removing = method == "project.removeInputAction";
        if (removing && !exists.value()) return errorJson(404, "Input action not found: " + action);
        if (!removing && exists.value() && !params.value("replace", false)) {
            return errorJson(409, "Input action already exists; pass replace: true to update it");
        }

        VariantValue default_value;
        auto previous = exists.value()
            ? callObject(project_settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                         {&setting_name.value(), &default_value})
            : Result<VariantValue>(VariantValue{});
        if (previous.isErr()) return errorJson(previous.error().code, previous.error().message);
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
            for (const auto& descriptor : event_descriptors) {
                auto event = makeInputEvent(descriptor);
                if (event.isErr()) return errorJson(event.error().code, event.error().message);
                auto appended = callVariant(events.value(), "append", {&event.value()});
                if (appended.isErr()) return errorJson(appended.error().code, appended.error().message);
            }
            auto set_deadzone = callVariant(dictionary.value(), "set", {&deadzone_key.value(), &deadzone_value.value()});
            auto set_events = callVariant(dictionary.value(), "set", {&events_key.value(), &events.value()});
            if (set_deadzone.isErr() || set_events.isErr()) return errorJson(500, "Failed to construct InputMap setting");
            event_count = event_descriptors.size();
            replacement = std::move(dictionary.value());
        }

        auto input_map = singleton("InputMap");
        if (input_map.isErr()) return errorJson(input_map.error().code, input_map.error().message);
        auto reload_bind = requireMethodBind("InputMap", "load_from_project_settings", 3218959716LL);
        if (reload_bind.isErr()) return errorJson(reload_bind.error().code, reload_bind.error().message);
        auto applied = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                                  {&setting_name.value(), &replacement.value()});
        if (applied.isErr()) return errorJson(applied.error().code, applied.error().message);
        auto saved = callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL);
        auto save_code = saved.isOk()
            ? scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT)
            : Result<int64_t>(saved.error());
        if (save_code.isErr() || save_code.value() != 0) {
            auto rollback = restoreProjectSetting(project_settings.value(), setting_name.value(), previous.value());
            callObject(input_map.value(), "InputMap", "load_from_project_settings", 3218959716LL);
            if (rollback.isErr()) return errorJson(500, "InputMap save failed and rollback failed: " + rollback.error().message);
            return errorJson(500, "ProjectSettings.save failed; InputMap mutation was rolled back");
        }
        auto reloaded = callObject(input_map.value(), "InputMap", "load_from_project_settings", 3218959716LL);
        // The setting is already on disk by this point, and rolling back after a
        // successful save means a second write that can fail the same way.
        // Returning a bare error here told the caller nothing had happened, so a
        // retry with replace:false came back with "already exists" and the agent
        // concluded the write had failed. The durable fact is that it persisted.
        // Report that, and report separately that the live InputMap did not
        // pick it up.
        json result = {{"status", "success"}, {"action", action}, {"deadzone", deadzone},
                       {"event_count", event_count}, {"removed", removing}, {"persisted", true},
                       {"runtime_reloaded", reloaded.isOk()}};
        if (reloaded.isErr()) {
            result["warning"] = "The input action was saved to project.godot but the live "
                                "InputMap did not reload: " + reloaded.error().message;
        }
        return liveResult(result);
    }

    if (method == "script.attachToNode" || method == "script.detachFromNode") {
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto node = resolveNode(root.value(), params.value("target_node", ""));
        if (node.isErr()) return errorJson(node.error().code, node.error().message);
        auto old_script = callObject(node.value(), "Object", "get_script", 1214101251LL);
        if (old_script.isErr()) return errorJson(old_script.error().code, old_script.error().message);
        auto old_type = GodotApi::instance().variant_get_type(old_script.value().ptr());
        GDExtensionObjectPtr old_object = nullptr;
        if (old_type == GDEXTENSION_VARIANT_TYPE_OBJECT) {
            auto converted = objectFromVariant(old_script.value());
            if (converted.isErr()) return errorJson(converted.error().code, converted.error().message);
            old_object = converted.value();
        }

        VariantValue new_script;
        GDExtensionObjectPtr requested_script_object = nullptr;
        std::string script_path;
        const bool attaching = method == "script.attachToNode";
        if (attaching) {
            script_path = params.value("script_path", "");
            auto valid_path = validateScriptPath(script_path);
            if (valid_path.isErr()) return errorJson(valid_path.error().code, valid_path.error().message);
            if (old_object) return errorJson(409, "Target node already has a script; detach it before attaching another");
            auto loader = singleton("ResourceLoader");
            if (loader.isErr()) return errorJson(loader.error().code, loader.error().message);
            auto path = makeString(script_path);
            auto hint = makeString("Script");
            auto cache_mode = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
            if (path.isErr() || hint.isErr() || cache_mode.isErr()) return errorJson(500, "Failed to construct script load arguments");
            auto loaded = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                     {&path.value(), &hint.value(), &cache_mode.value()});
            if (loaded.isErr()) return errorJson(loaded.error().code, loaded.error().message);
            auto resource = objectFromVariant(loaded.value());
            if (resource.isErr() || !resource.value()) return errorJson(404, "Script resource not found: " + script_path);
            auto class_name = makeString("Script");
            auto is_script_value = class_name.isOk()
                ? callObject(resource.value(), "Object", "is_class", 3927539163LL, {&class_name.value()})
                : Result<VariantValue>(class_name.error());
            auto is_script = is_script_value.isOk()
                ? scalarFromVariant<GDExtensionBool>(is_script_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                : Result<GDExtensionBool>(is_script_value.error());
            if (is_script.isErr() || !is_script.value()) return errorJson(400, "Resource is not a Script: " + script_path);
            auto base_type_value = callObject(resource.value(), "Script", "get_instance_base_type", 2002593661LL);
            if (base_type_value.isErr()) return errorJson(base_type_value.error().code, base_type_value.error().message);
            auto base_type = stringFromVariant(base_type_value.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (base_type.isErr()) return errorJson(base_type.error().code, base_type.error().message);
            auto base_class = makeString(base_type.value());
            auto compatible_value = base_class.isOk()
                ? callObject(node.value(), "Object", "is_class", 3927539163LL, {&base_class.value()})
                : Result<VariantValue>(base_class.error());
            auto compatible = compatible_value.isOk()
                ? scalarFromVariant<GDExtensionBool>(compatible_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                : Result<GDExtensionBool>(compatible_value.error());
            if (compatible.isErr()) return errorJson(compatible.error().code, compatible.error().message);
            if (!compatible.value()) {
                return errorJson(422, "Script base type " + base_type.value() +
                                      " is incompatible with the target node");
            }
            requested_script_object = resource.value();
            new_script = std::move(loaded.value());
        } else if (!old_object) {
            return errorJson(409, "Target node has no script to detach");
        }

        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto preflight = preflightUndoManagerBindings();
        auto method_bind = requireMethodBind("Object", "set_script", 1114965689LL);
        if (preflight.isErr()) return errorJson(preflight.error().code, preflight.error().message);
        if (method_bind.isErr()) return errorJson(method_bind.error().code, method_bind.error().message);
        auto action = createAction(manager.value(), attaching ? "Didi: attach script" : "Didi: detach script", root.value());
        if (action.isErr()) return errorJson(action.error().code, action.error().message);
        auto apply = managerMethod(manager.value(), "add_do_method", node.value(), "set_script", {&new_script});
        auto revert = managerMethod(manager.value(), "add_undo_method", node.value(), "set_script", {&old_script.value()});
        if (apply.isErr() || revert.isErr()) return errorJson(500, "Failed to register script UndoRedo transaction");
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
        auto observed_script = callObject(node.value(), "Object", "get_script", 1214101251LL);
        if (observed_script.isErr()) return errorJson(observed_script.error().code, observed_script.error().message);
        auto observed_type = GodotApi::instance().variant_get_type(observed_script.value().ptr());
        GDExtensionObjectPtr observed_object = nullptr;
        if (observed_type == GDEXTENSION_VARIANT_TYPE_OBJECT) {
            auto converted = objectFromVariant(observed_script.value());
            if (converted.isErr()) return errorJson(converted.error().code, converted.error().message);
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
            json failure = errorJson(422, reason);
            failure["error"]["data"] = {{"outcome", "reverted"}, {"rolled_back", true}};
            return failure;
        }
        return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                           {"script_path", script_path}, {"attached", attaching}, {"detached", !attaching},
                           {"undo_redo_registered", true}});
    }

    if (method == "audio.configureBus") {
        auto server = singleton("AudioServer");
        if (server.isErr()) return errorJson(server.error().code, server.error().message);

        auto count_value = callObject(server.value(), "AudioServer", "get_bus_count", 3905245786LL);
        if (count_value.isErr()) return errorJson(count_value.error().code, count_value.error().message);
        auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (count.isErr()) return errorJson(count.error().code, count.error().message);

        // A bus can be named or numbered. Names are what a person uses and what
        // the layout file records; indices are what AudioServer takes. Resolving
        // a name through the engine rather than through the layout file means a
        // bus added at runtime is still addressable.
        int64_t index = -1;
        const json bus_field = params.contains("bus") ? params["bus"] : json();
        if (bus_field.is_string()) {
            const auto wanted = bus_field.get<std::string>();
            auto name_variant = makeString(wanted);
            if (name_variant.isErr()) return errorJson(name_variant.error().code, name_variant.error().message);
            auto found = callObject(server.value(), "AudioServer", "get_bus_index", 2458036349LL,
                                    {&name_variant.value()});
            if (found.isErr()) return errorJson(found.error().code, found.error().message);
            auto resolved = scalarFromVariant<int64_t>(found.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (resolved.isErr()) return errorJson(resolved.error().code, resolved.error().message);
            index = resolved.value();
            if (index < 0) return errorJson(404, "No audio bus is named " + wanted);
        } else if (bus_field.is_number_integer()) {
            index = bus_field.get<int64_t>();
        } else {
            return errorJson(400, "bus must be a bus name or a bus index");
        }
        if (index < 0 || index >= count.value()) {
            return errorJson(404, "Audio bus index " + std::to_string(index) +
                                      " is out of range; this project has " +
                                      std::to_string(count.value()) + " buses");
        }

        auto bus_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (bus_index.isErr()) return errorJson(bus_index.error().code, bus_index.error().message);

        const auto readState = [&]() -> Result<json> {
            auto name_value = callObject(server.value(), "AudioServer", "get_bus_name", 844755477LL,
                                         {&bus_index.value()});
            if (name_value.isErr()) return name_value.error();
            auto name = stringFromVariant(name_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            if (name.isErr()) return name.error();
            auto volume_value = callObject(server.value(), "AudioServer", "get_bus_volume_db",
                                           2339986948LL, {&bus_index.value()});
            if (volume_value.isErr()) return volume_value.error();
            auto volume = scalarFromVariant<double>(volume_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
            if (volume.isErr()) return volume.error();
            auto mute_value = callObject(server.value(), "AudioServer", "is_bus_mute", 1116898809LL,
                                         {&bus_index.value()});
            if (mute_value.isErr()) return mute_value.error();
            auto mute = scalarFromVariant<bool>(mute_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (mute.isErr()) return mute.error();
            auto solo_value = callObject(server.value(), "AudioServer", "is_bus_solo", 1116898809LL,
                                         {&bus_index.value()});
            if (solo_value.isErr()) return solo_value.error();
            auto solo = scalarFromVariant<bool>(solo_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (solo.isErr()) return solo.error();
            return json{{"index", index},
                        {"name", name.value()},
                        {"volume_db", volume.value()},
                        {"mute", mute.value()},
                        {"solo", solo.value()}};
        };

        // Read before anything is written. These values are the only way back,
        // because bus state is not part of the edited scene and the editor undo
        // stack does not carry it.
        auto before = readState();
        if (before.isErr()) return errorJson(before.error().code, before.error().message);

        json applied = json::array();
        if (params.contains("volume_db")) {
            const auto& value = params["volume_db"];
            if (!value.is_number()) return errorJson(400, "volume_db must be a number");
            const double db = value.get<double>();
            // The engine bus editor spans -80 to 24 decibels. Outside that a
            // caller is either confusing decibels with a linear gain or has
            // slipped a digit, and clamping silently would hide both.
            if (!(db >= -80.0 && db <= 24.0)) {
                return errorJson(400, "volume_db must be between -80 and 24 decibels");
            }
            auto db_variant = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, db);
            if (db_variant.isErr()) return errorJson(db_variant.error().code, db_variant.error().message);
            auto set = callObject(server.value(), "AudioServer", "set_bus_volume_db", 1602489585LL,
                                  {&bus_index.value(), &db_variant.value()});
            if (set.isErr()) return errorJson(set.error().code, set.error().message);
            applied.push_back("volume_db");
        }

        const auto applyFlag = [&](const char* field, const char* method_name,
                                   int64_t hash) -> Result<bool> {
            if (!params.contains(field)) return false;
            const auto& value = params[field];
            if (!value.is_boolean()) {
                return Error::invalidArgument(std::string(field) + " must be a boolean");
            }
            auto flag = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, value.get<bool>());
            if (flag.isErr()) return flag.error();
            auto set = callObject(server.value(), "AudioServer", method_name, hash,
                                  {&bus_index.value(), &flag.value()});
            if (set.isErr()) return set.error();
            return true;
        };
        auto muted = applyFlag("mute", "set_bus_mute", 300928843LL);
        if (muted.isErr()) return errorJson(muted.error().code, muted.error().message);
        if (muted.value()) applied.push_back("mute");
        auto soloed = applyFlag("solo", "set_bus_solo", 300928843LL);
        if (soloed.isErr()) return errorJson(soloed.error().code, soloed.error().message);
        if (soloed.value()) applied.push_back("solo");

        if (applied.empty()) {
            return errorJson(400, "Give at least one of volume_db, mute or solo to change");
        }

        auto after = readState();
        if (after.isErr()) return errorJson(after.error().code, after.error().message);

        return liveResult({{"status", "success"},
                           {"bus", index},
                           {"applied", std::move(applied)},
                           {"before", before.value()},
                           {"after", after.value()},
                           {"undo_redo_registered", false},
                           {"revert_with", before.value()}});
    }

    if (method == "audio.listBuses") {
        // Every method hash below is identical on Godot 4.5.1, 4.6.2 and 4.7.2,
        // checked by dumping extension_api.json from each, so this needs no
        // per-version branch. AudioServer is a core singleton and is present
        // whether or not an editor scene is open.
        auto server = singleton("AudioServer");
        if (server.isErr()) return errorJson(server.error().code, server.error().message);

        auto count_value = callObject(server.value(), "AudioServer", "get_bus_count", 3905245786LL);
        if (count_value.isErr()) return errorJson(count_value.error().code, count_value.error().message);
        auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (count.isErr()) return errorJson(count.error().code, count.error().message);

        json buses = json::array();
        for (int64_t index = 0; index < count.value(); ++index) {
            auto bus_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (bus_index.isErr()) return errorJson(bus_index.error().code, bus_index.error().message);

            auto name_value = callObject(server.value(), "AudioServer", "get_bus_name", 844755477LL,
                                         {&bus_index.value()});
            if (name_value.isErr()) return errorJson(name_value.error().code, name_value.error().message);
            auto name = stringFromVariant(name_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            if (name.isErr()) return errorJson(name.error().code, name.error().message);

            auto volume_value = callObject(server.value(), "AudioServer", "get_bus_volume_db",
                                           2339986948LL, {&bus_index.value()});
            if (volume_value.isErr()) return errorJson(volume_value.error().code, volume_value.error().message);
            auto volume = scalarFromVariant<double>(volume_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
            if (volume.isErr()) return errorJson(volume.error().code, volume.error().message);

            const auto boolOf = [&](const char* method_name, int64_t hash) -> Result<bool> {
                auto value = callObject(server.value(), "AudioServer", method_name, hash,
                                        {&bus_index.value()});
                if (value.isErr()) return value.error();
                return scalarFromVariant<bool>(value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            };
            auto mute = boolOf("is_bus_mute", 1116898809LL);
            if (mute.isErr()) return errorJson(mute.error().code, mute.error().message);
            auto solo = boolOf("is_bus_solo", 1116898809LL);
            if (solo.isErr()) return errorJson(solo.error().code, solo.error().message);
            auto bypass = boolOf("is_bus_bypassing_effects", 1116898809LL);
            if (bypass.isErr()) return errorJson(bypass.error().code, bypass.error().message);

            auto send_value = callObject(server.value(), "AudioServer", "get_bus_send", 659327637LL,
                                         {&bus_index.value()});
            if (send_value.isErr()) return errorJson(send_value.error().code, send_value.error().message);
            auto send = stringFromVariant(send_value.value(), GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (send.isErr()) {
                send = stringFromVariant(send_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (send.isErr()) return errorJson(send.error().code, send.error().message);
            }

            auto effect_count_value = callObject(server.value(), "AudioServer",
                                                 "get_bus_effect_count", 3744713108LL,
                                                 {&bus_index.value()});
            if (effect_count_value.isErr()) {
                return errorJson(effect_count_value.error().code, effect_count_value.error().message);
            }
            auto effect_count =
                scalarFromVariant<int64_t>(effect_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (effect_count.isErr()) return errorJson(effect_count.error().code, effect_count.error().message);

            // The effect chain is the part the offline layout file cannot
            // report, so it is the reason to attach an editor at all.
            json effects = json::array();
            for (int64_t slot = 0; slot < effect_count.value(); ++slot) {
                auto slot_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, slot);
                if (slot_index.isErr()) return errorJson(slot_index.error().code, slot_index.error().message);
                auto effect = callObject(server.value(), "AudioServer", "get_bus_effect", 726064442LL,
                                         {&bus_index.value(), &slot_index.value()});
                if (effect.isErr()) return errorJson(effect.error().code, effect.error().message);
                auto class_value = callVariant(effect.value(), "get_class");
                std::string class_name = "AudioEffect";
                if (class_value.isOk()) {
                    auto text = stringFromVariant(class_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                    if (text.isOk()) class_name = text.value();
                }
                effects.push_back({{"slot", slot}, {"class", class_name}});
            }

            buses.push_back({{"index", index},
                             {"name", name.value()},
                             {"volume_db", volume.value()},
                             {"mute", mute.value()},
                             {"solo", solo.value()},
                             {"bypass_effects", bypass.value()},
                             {"send", send.value()},
                             {"effects", std::move(effects)}});
        }

        return liveResult({{"status", "success"},
                           {"bus_count", count.value()},
                           {"buses", std::move(buses)}});
    }

    if (method == "scene.listGroups" || method == "scene.addToGroup" ||
        method == "scene.removeFromGroup" || method == "scene.getGroupMembers") {
        const std::string group = params.value("group", "");
        if (method != "scene.listGroups") {
            auto valid_group = validateGroupName(group);
            if (valid_group.isErr()) return errorJson(valid_group.error().code, valid_group.error().message);
        }
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);

        if (method == "scene.getGroupMembers") {
            auto group_name = makeStringName(group);
            if (group_name.isErr()) return errorJson(group_name.error().code, group_name.error().message);
            json members = json::array();
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
            if (visited.isErr()) return errorJson(visited.error().code, visited.error().message);
            std::sort(members.begin(), members.end());
            return liveResult({{"status", "success"}, {"group", group}, {"members", members}});
        }

        auto node = resolveNode(root.value(), params.value("target_node", ""));
        if (node.isErr()) return errorJson(node.error().code, node.error().message);
        if (method == "scene.listGroups") {
            auto groups_value = callObject(node.value(), "Node", "get_groups", 3995934104LL);
            if (groups_value.isErr()) return errorJson(groups_value.error().code, groups_value.error().message);
            auto size_value = callVariant(groups_value.value(), "size");
            if (size_value.isErr()) return errorJson(size_value.error().code, size_value.error().message);
            auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (size.isErr()) return errorJson(size.error().code, size.error().message);
            std::vector<std::string> groups;
            for (int64_t i = 0; i < size.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                if (index.isErr()) return errorJson(index.error().code, index.error().message);
                auto item = callVariant(groups_value.value(), "get", {&index.value()});
                if (item.isErr()) return errorJson(item.error().code, item.error().message);
                auto type = GodotApi::instance().variant_get_type(item.value().ptr());
                auto text = stringFromVariant(item.value(), type);
                if (text.isErr()) return errorJson(text.error().code, text.error().message);
                groups.push_back(text.value());
            }
            std::sort(groups.begin(), groups.end());
            return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                               {"groups", groups}});
        }

        auto group_name = makeStringName(group);
        if (group_name.isErr()) return errorJson(group_name.error().code, group_name.error().message);
        auto membership_value = callObject(node.value(), "Node", "is_in_group", 2619796661LL, {&group_name.value()});
        if (membership_value.isErr()) return errorJson(membership_value.error().code, membership_value.error().message);
        auto membership = scalarFromVariant<GDExtensionBool>(membership_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (membership.isErr()) return errorJson(membership.error().code, membership.error().message);
        const bool adding = method == "scene.addToGroup";
        if (adding && membership.value()) return errorJson(409, "Target node is already in group: " + group);
        if (!adding && !membership.value()) return errorJson(404, "Target node is not in group: " + group);
        bool original_persistent = params.value("persistent", true);
        if (!adding) {
            NativeName packed_scene_name("PackedScene");
            auto packed_scene = GodotApi::instance().classdb_construct_object(packed_scene_name.ptr());
            if (!packed_scene) return errorJson(500, "Godot could not inspect group persistence");
            auto packed_scene_value = makeObject(packed_scene);
            auto root_value = makeObject(root.value());
            if (packed_scene_value.isErr() || root_value.isErr()) return errorJson(500, "Failed to inspect group persistence");
            auto packed = callObject(packed_scene, "PackedScene", "pack", 2584678054LL, {&root_value.value()});
            if (packed.isErr()) return errorJson(packed.error().code, packed.error().message);
            auto pack_code = scalarFromVariant<int64_t>(packed.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (pack_code.isErr() || pack_code.value() != 0) return errorJson(500, "Failed to snapshot scene groups");
            auto state_value = callObject(packed_scene, "PackedScene", "get_state", 3479783971LL);
            if (state_value.isErr()) return errorJson(state_value.error().code, state_value.error().message);
            auto state = objectFromVariant(state_value.value());
            if (state.isErr() || !state.value()) return errorJson(500, "PackedScene returned no SceneState");
            auto count_value = callObject(state.value(), "SceneState", "get_node_count", 3905245786LL);
            if (count_value.isErr()) return errorJson(count_value.error().code, count_value.error().message);
            auto count = scalarFromVariant<int64_t>(count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (count.isErr()) return errorJson(count.error().code, count.error().message);
            auto relative_path = relativePathWithinEditedRoot(root.value(), node.value());
            if (relative_path.isErr()) return errorJson(relative_path.error().code, relative_path.error().message);
            original_persistent = false;
            for (int64_t i = 0; i < count.value(); ++i) {
                auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, i);
                auto for_parent = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
                if (index.isErr() || for_parent.isErr()) return errorJson(500, "Failed to inspect SceneState node path");
                auto state_path_value = callObject(state.value(), "SceneState", "get_node_path", 2272487792LL,
                                                   {&index.value(), &for_parent.value()});
                if (state_path_value.isErr()) return errorJson(state_path_value.error().code, state_path_value.error().message);
                auto state_path = stringFromVariant(state_path_value.value(), GDEXTENSION_VARIANT_TYPE_NODE_PATH);
                if (state_path.isErr()) return errorJson(state_path.error().code, state_path.error().message);
                if (state_path.value() != relative_path.value()) continue;
                auto groups = callObject(state.value(), "SceneState", "get_node_groups", 647634434LL, {&index.value()});
                if (groups.isErr()) return errorJson(groups.error().code, groups.error().message);
                auto group_count_value = callVariant(groups.value(), "size");
                if (group_count_value.isErr()) return errorJson(group_count_value.error().code, group_count_value.error().message);
                auto group_count = scalarFromVariant<int64_t>(group_count_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                if (group_count.isErr()) return errorJson(group_count.error().code, group_count.error().message);
                for (int64_t group_index = 0; group_index < group_count.value(); ++group_index) {
                    auto native_group_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, group_index);
                    if (native_group_index.isErr()) return errorJson(native_group_index.error().code, native_group_index.error().message);
                    auto stored_group_value = callVariant(groups.value(), "get", {&native_group_index.value()});
                    if (stored_group_value.isErr()) return errorJson(stored_group_value.error().code, stored_group_value.error().message);
                    auto stored_group = stringFromVariant(stored_group_value.value(),
                        GodotApi::instance().variant_get_type(stored_group_value.value().ptr()));
                    if (stored_group.isErr()) return errorJson(stored_group.error().code, stored_group.error().message);
                    if (stored_group.value() == group) original_persistent = true;
                }
                break;
            }
        }
        auto persistent = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                     static_cast<GDExtensionBool>(original_persistent));
        if (persistent.isErr()) return errorJson(persistent.error().code, persistent.error().message);
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto preflight = preflightUndoManagerBindings();
        if (preflight.isErr()) return errorJson(preflight.error().code, preflight.error().message);
        auto add_bind = requireMethodBind("Node", "add_to_group", 3683006648LL);
        auto remove_bind = requireMethodBind("Node", "remove_from_group", 3304788590LL);
        if (add_bind.isErr()) return errorJson(add_bind.error().code, add_bind.error().message);
        if (remove_bind.isErr()) return errorJson(remove_bind.error().code, remove_bind.error().message);
        auto action = createAction(manager.value(), adding ? "Didi: add node to group" : "Didi: remove node from group", root.value());
        if (action.isErr()) return errorJson(action.error().code, action.error().message);
        auto apply = adding
            ? managerMethod(manager.value(), "add_do_method", node.value(), "add_to_group", {&group_name.value(), &persistent.value()})
            : managerMethod(manager.value(), "add_do_method", node.value(), "remove_from_group", {&group_name.value()});
        auto revert = adding
            ? managerMethod(manager.value(), "add_undo_method", node.value(), "remove_from_group", {&group_name.value()})
            : managerMethod(manager.value(), "add_undo_method", node.value(), "add_to_group", {&group_name.value(), &persistent.value()});
        if (apply.isErr() || revert.isErr()) return errorJson(500, "Failed to register group UndoRedo transaction");
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
        return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                           {"group", group}, {"added", adding}, {"removed", !adding},
                           {"undo_redo_registered", true}});
    }

    if (method == "scene.create" || method == "scene.open" || method == "scene.close" ||
        method == "scene.packBranch") {
        if (method == "scene.close") {
            auto root = editedSceneRoot(editor);
            if (root.isErr()) return errorJson(root.error().code, root.error().message);
            auto path_value = callObject(root.value(), "Node", "get_scene_file_path", 201670096LL);
            if (path_value.isErr()) return errorJson(path_value.error().code, path_value.error().message);
            auto path = stringFromVariant(path_value.value(), GDEXTENSION_VARIANT_TYPE_STRING);
            if (path.isErr()) return errorJson(path.error().code, path.error().message);
            if (!params.value("discard_unsaved", false)) {
                return errorJson(409, "Godot 4.5 cannot expose active-scene dirty state; pass discard_unsaved: true to close explicitly");
            }
            auto closed = callObject(editor, "EditorInterface", "close_scene", 166280745LL);
            if (closed.isErr()) return errorJson(closed.error().code, closed.error().message);
            auto code = scalarFromVariant<int64_t>(closed.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (code.isErr()) return errorJson(code.error().code, code.error().message);
            if (code.value() != 0) return errorJson(500, "Godot close_scene failed with Error " + std::to_string(code.value()));
            return liveResult({{"status", "success"}, {"closed", true}, {"scene_path", path.value()},
                               {"discarded_unsaved", true}});
        }

        const std::string scene_path = params.value("scene_path", "");
        auto valid_path = validateResPath(scene_path, ".tscn");
        if (valid_path.isErr()) return errorJson(valid_path.error().code, valid_path.error().message);
        auto loader = singleton("ResourceLoader");
        if (loader.isErr()) return errorJson(loader.error().code, loader.error().message);
        auto path = makeString(scene_path);
        auto packed_hint = makeString("PackedScene");
        if (path.isErr() || packed_hint.isErr()) return errorJson(500, "Failed to construct scene resource arguments");
        auto exists_value = callObject(loader.value(), "ResourceLoader", "exists", 4185558881LL,
                                       {&path.value(), &packed_hint.value()});
        if (exists_value.isErr()) return errorJson(exists_value.error().code, exists_value.error().message);
        auto target_exists = scalarFromVariant<GDExtensionBool>(exists_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (target_exists.isErr()) return errorJson(target_exists.error().code, target_exists.error().message);

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
            if (cache_mode.isErr()) return errorJson(cache_mode.error().code, cache_mode.error().message);
            auto resource = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                       {&path.value(), &packed_hint.value(), &cache_mode.value()});
            if (resource.isErr()) return errorJson(resource.error().code, resource.error().message);
            auto packed = objectFromVariant(resource.value());
            if (packed.isErr() || !packed.value()) return errorJson(422, "Resource is not a loadable PackedScene: " + scene_path);
            auto class_name = makeString("PackedScene");
            auto class_value = class_name.isOk()
                ? callObject(packed.value(), "Object", "is_class", 3927539163LL, {&class_name.value()})
                : Result<VariantValue>(class_name.error());
            auto is_packed = class_value.isOk()
                ? scalarFromVariant<GDExtensionBool>(class_value.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                : Result<GDExtensionBool>(class_value.error());
            if (is_packed.isErr() || !is_packed.value()) return errorJson(422, "Resource is not a PackedScene: " + scene_path);
            auto opened = open_and_verify();
            if (opened.isErr()) return errorJson(opened.error().code, opened.error().message);
            return liveResult({{"status", "success"}, {"opened", true}, {"scene_path", scene_path}});
        }

        if (target_exists.value() && !params.value("overwrite", false)) {
            return errorJson(409, "Scene target already exists; pass overwrite: true to replace it");
        }

        GDExtensionObjectPtr packed_root = nullptr;
        if (method == "scene.create") {
            const std::string root_type = params.value("root_type", "Node2D");
            if (root_type != "Node2D" && root_type != "Node3D" && root_type != "Control") {
                return errorJson(400, "root_type must be Node2D, Node3D, or Control");
            }
            const std::string root_name = params.value("root_name", "Root");
            if (root_name.empty() || root_name.find('/') != std::string::npos || root_name.find('\\') != std::string::npos) {
                return errorJson(400, "root_name must be a non-empty node name without path separators");
            }
            NativeName native_type(root_type);
            packed_root = GodotApi::instance().classdb_construct_object(native_type.ptr());
            if (!packed_root) return errorJson(500, "Godot could not construct scene root type: " + root_type);
            auto name = makeStringName(root_name);
            auto named = name.isOk()
                ? callObject(packed_root, "Node", "set_name", 3304788590LL, {&name.value()})
                : Result<VariantValue>(name.error());
            if (named.isErr()) {
                GodotApi::instance().object_destroy(packed_root);
                return errorJson(named.error().code, named.error().message);
            }
        } else {
            auto root = editedSceneRoot(editor);
            if (root.isErr()) return errorJson(root.error().code, root.error().message);
            auto target = resolveNode(root.value(), params.value("target_node", ""));
            if (target.isErr()) return errorJson(target.error().code, target.error().message);
            auto flags = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(15));
            if (flags.isErr()) return errorJson(flags.error().code, flags.error().message);
            auto duplicated = callObject(target.value(), "Node", "duplicate", 3511555459LL, {&flags.value()});
            if (duplicated.isErr()) return errorJson(duplicated.error().code, duplicated.error().message);
            auto duplicate_object = objectFromVariant(duplicated.value());
            if (duplicate_object.isErr() || !duplicate_object.value()) return errorJson(500, "Godot failed to duplicate the branch");
            packed_root = duplicate_object.value();
            auto owner = makeObject(packed_root);
            if (owner.isErr()) {
                GodotApi::instance().object_destroy(packed_root);
                return errorJson(owner.error().code, owner.error().message);
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
                return errorJson(normalized.error().code, normalized.error().message);
            }
        }

        NativeName packed_scene_name("PackedScene");
        auto packed_scene = GodotApi::instance().classdb_construct_object(packed_scene_name.ptr());
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
            return errorJson(packed.error().code, packed.error().message);
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
        if (saved.isErr()) return errorJson(saved.error().code, saved.error().message);
        auto save_code = scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (save_code.isErr()) return errorJson(save_code.error().code, save_code.error().message);
        if (save_code.value() != 0) return errorJson(500, "ResourceSaver.save failed with Error " + std::to_string(save_code.value()));

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
            if (target_exists.value()) {
                auto replace_cache = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(4));
                if (replace_cache.isErr()) return openFailure(replace_cache.error());
                auto refreshed = callObject(loader.value(), "ResourceLoader", "load", 3358495409LL,
                                            {&path.value(), &packed_hint.value(), &replace_cache.value()});
                if (refreshed.isErr()) return openFailure(refreshed.error());
                auto reloaded = callObject(editor, "EditorInterface", "reload_scene_from_path", 83702148LL,
                                           {&path.value()});
                if (reloaded.isErr()) return openFailure(reloaded.error());
            }
            auto opened = open_and_verify();
            if (opened.isErr()) return openFailure(opened.error());
            return liveResult({{"status", "success"}, {"saved", true}, {"opened", true}, {"scene_path", scene_path}});
        }
        return liveResult({{"status", "success"}, {"saved", true}, {"scene_path", scene_path},
                           {"source_node", params.value("target_node", "")}});
    }

    if (method == "editor.getState" || method == "scene.getHierarchy") {
        auto root_result = editedSceneRoot(editor);
        if (root_result.isErr()) return errorJson(root_result.error().code, root_result.error().message);
        auto root = root_result.value();
        if (method == "editor.getState") {
            auto root_path = nodeString(root, "get_path", 4075236667LL);
            if (root_path.isErr()) return errorJson(root_path.error().code, root_path.error().message);
            return liveResult({{"status", "online"}, {"editor_connected", true},
                               {"active_scene_root", root_path.value()}});
        }
        const std::string requested_root = params.value("root_path", "/root");
        auto target = resolveNode(root, requested_root);
        if (target.isErr()) return errorJson(target.error().code, target.error().message);
        int max_depth = std::clamp(params.value("max_depth", 10), 0, 64);
        auto logical_root = logicalPathFromEditedRoot(root, target.value());
        if (logical_root.isErr()) return errorJson(logical_root.error().code, logical_root.error().message);
        HierarchyBudget budget;
        auto hierarchy = buildHierarchy(target.value(), 0, max_depth, logical_root.value(), budget);
        if (hierarchy.isErr()) return errorJson(hierarchy.error().code, hierarchy.error().message);
        if (hierarchy.value().is_null()) {
            return errorJson(413, "The edited scene root alone exceeds the hierarchy response budget");
        }
        json omitted = json::array();
        if (params.value("include_properties", true)) omitted.push_back("bulk_properties");
        if (params.value("include_signals", true)) omitted.push_back("signals");
        if (params.value("include_scripts", true)) omitted.push_back("scripts");
        json hierarchy_result = {{"root_path", params.value("root_path", "/root")},
                                 {"source", "live_scene_tree"}, {"scene_tree", hierarchy.value()},
                                 {"omitted_fields", omitted},
                                 {"node_count", budget.node_count},
                                 {"max_nodes", kMaxHierarchyNodes},
                                 {"max_response_bytes", kMaxHierarchyResponseBytes},
                                 {"message", "Use focused property/signal tools for fields omitted from hierarchy traversal."}};
        if (budget.truncated) hierarchy_result["truncated"] = true;
        return liveResult(hierarchy_result);
    }

    if (method == "scene.getProperty" || method == "scene.setProperty") {
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto node = resolveNode(root.value(), params.value("target_node", ""));
        if (node.isErr()) return errorJson(node.error().code, node.error().message);
        std::string property = params.value("property_name", "");
        if (property.empty()) return errorJson(400, "property_name is required");
        auto has_property = objectHasProperty(node.value(), property);
        if (has_property.isErr()) return errorJson(has_property.error().code, has_property.error().message);
        if (!has_property.value()) return errorJson(404, "Property not found on target node: " + property);
        auto property_name = makeStringName(property);
        if (property_name.isErr()) return errorJson(property_name.error().code, property_name.error().message);
        auto old_value = callObject(node.value(), "Object", "get", 2760726917LL, {&property_name.value()});
        if (old_value.isErr()) return errorJson(old_value.error().code, old_value.error().message);
        if (method == "scene.getProperty") {
            auto value = variantToJson(old_value.value());
            if (value.isErr()) return errorJson(value.error().code, value.error().message);
            return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                               {"property_name", property}, {"value", value.value()}});
        }
        if (!params.contains("value")) return errorJson(400, "value is required");
        auto compatible = validateJsonForPropertyType(
            params["value"], GodotApi::instance().variant_get_type(old_value.value().ptr()));
        if (compatible.isErr()) return errorJson(compatible.error().code, compatible.error().message);
        auto new_value = makeJsonVariant(params["value"]);
        if (new_value.isErr()) return errorJson(new_value.error().code, new_value.error().message);
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto object_value = makeObject(node.value());
        if (object_value.isErr()) return errorJson(object_value.error().code, object_value.error().message);
        auto preflight = preflightUndoManagerBindings();
        if (preflight.isErr()) return errorJson(preflight.error().code, preflight.error().message);
        auto action = createAction(manager.value(), "Didi: set " + property, node.value());
        if (action.isErr()) return errorJson(action.error().code, action.error().message);
        auto do_property = callObject(manager.value(), "EditorUndoRedoManager", "add_do_property", 1017172818LL,
                                      {&object_value.value(), &property_name.value(), &new_value.value()});
        auto undo_property = callObject(manager.value(), "EditorUndoRedoManager", "add_undo_property", 1017172818LL,
                                        {&object_value.value(), &property_name.value(), &old_value.value()});
        if (do_property.isErr()) return errorJson(do_property.error().code, do_property.error().message);
        if (undo_property.isErr()) return errorJson(undo_property.error().code, undo_property.error().message);
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
        return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                           {"property_name", property}, {"value", params["value"]},
                           {"undo_redo_registered", true}});
    }

    if (method == "scene.instantiateNode") {
        if (!params.value("scene_path", "").empty()) {
            return errorJson(501, "PackedScene instantiation is outside the Phase 1 built-in-node bridge");
        }
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto parent = resolveNode(root.value(), params.value("parent_path", "/root"));
        if (parent.isErr()) return errorJson(parent.error().code, parent.error().message);
        std::string node_type = params.value("node_type", "Node");
        NativeName type_name(node_type);
        auto node = GodotApi::instance().classdb_construct_object(type_name.ptr());
        if (!node) return errorJson(400, "Godot ClassDB could not instantiate node type: " + node_type);
        auto node_class = makeString("Node");
        auto is_node_variant = node_class.isOk()
            ? callObject(node, "Object", "is_class", 3927539163LL, {&node_class.value()})
            : Result<VariantValue>(node_class.error());
        auto is_node = is_node_variant.isOk()
            ? scalarFromVariant<GDExtensionBool>(is_node_variant.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
            : Result<GDExtensionBool>(is_node_variant.error());
        if (is_node.isErr() || !is_node.value()) {
            GodotApi::instance().object_destroy(node);
            return is_node.isErr()
                ? errorJson(is_node.error().code, is_node.error().message)
                : errorJson(400, "Godot ClassDB type does not inherit Node: " + node_type);
        }
        if (!params.value("name", "").empty()) {
            auto name = makeStringName(params.value("name", ""));
            auto named = name.isOk() ? callObject(node, "Node", "set_name", 3304788590LL, {&name.value()})
                                     : Result<VariantValue>(name.error());
            if (named.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(named.error().code, named.error().message);
            }
        }
        json initial_properties = params.value("properties", json::object());
        if (!initial_properties.is_object()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(400, "properties must be a JSON object");
        }
        for (auto it = initial_properties.begin(); it != initial_properties.end(); ++it) {
            auto has_property = objectHasProperty(node, it.key());
            if (has_property.isErr() || !has_property.value()) {
                GodotApi::instance().object_destroy(node);
                return has_property.isErr()
                    ? errorJson(has_property.error().code, has_property.error().message)
                    : errorJson(404, "Property not found on new " + node_type + " node: " + it.key());
            }
            auto property_name = makeStringName(it.key());
            if (property_name.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(property_name.error().code, property_name.error().message);
            }
            auto current_value = callObject(node, "Object", "get", 2760726917LL, {&property_name.value()});
            if (current_value.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(current_value.error().code, current_value.error().message);
            }
            auto compatible = validateJsonForPropertyType(
                it.value(), GodotApi::instance().variant_get_type(current_value.value().ptr()));
            if (compatible.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(compatible.error().code, compatible.error().message);
            }
            auto property_value = makeJsonVariant(it.value());
            if (property_value.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(property_value.error().code, property_value.error().message);
            }
            auto set = callObject(node, "Object", "set", 3776071444LL,
                                  {&property_name.value(), &property_value.value()});
            if (set.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(set.error().code, set.error().message);
            }
        }
        auto manager = undoManager(editor);
        if (manager.isErr()) { GodotApi::instance().object_destroy(node); return errorJson(manager.error().code, manager.error().message); }
        auto child = makeObject(node);
        auto readable = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
        auto internal = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
        auto owner = makeObject(root.value());
        if (child.isErr() || readable.isErr() || internal.isErr() || owner.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(500, "Failed to construct node transaction arguments");
        }
        auto logical_parent = logicalPathFromEditedRoot(root.value(), parent.value());
        if (logical_parent.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(logical_parent.error().code, logical_parent.error().message);
        }
        const std::string logical_name = params.value("name", "").empty() ? node_type : params.value("name", "");
        auto preflight = preflightNodeUndoTransaction();
        if (preflight.isErr()) {
            GodotApi::instance().object_destroy(node);
            return errorJson(preflight.error().code, preflight.error().message);
        }
        auto action = createAction(manager.value(), "Didi: instantiate " + node_type, root.value());
        if (action.isErr()) { GodotApi::instance().object_destroy(node); return errorJson(action.error().code, action.error().message); }
        auto keep = managerReference(manager.value(), "add_do_reference", node);
        auto add = managerMethod(manager.value(), "add_do_method", parent.value(), "add_child",
                                 {&child.value(), &readable.value(), &internal.value()});
        auto own = managerMethod(manager.value(), "add_do_method", node, "set_owner", {&owner.value()});
        auto remove = managerMethod(manager.value(), "add_undo_method", parent.value(), "remove_child", {&child.value()});
        if (keep.isErr() || add.isErr() || own.isErr() || remove.isErr()) {
            abandonAction(manager.value());
            // add_do_reference hands the node to the undo history, which then
            // owns it. Destroying it after that would double free when the
            // history is cleared, so only clean up when the history never
            // took it. Node is not RefCounted, so nothing else will.
            if (keep.isErr()) GodotApi::instance().object_destroy(node);
            return errorJson(500, "Failed to register instantiate UndoRedo transaction");
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
        // Godot uniquifies names on insert: a second Enemy becomes Enemy2. The
        // name was set before add_child, so the path built from it is a guess.
        // Read the real one back now that the node is actually in the tree,
        // otherwise the caller's next scene_set_property hits a sibling.
        auto actual_path = logicalPathFromEditedRoot(root.value(), node);
        json result = {{"status", "success"}, {"action", "instantiate_node"},
                       {"node_type", node_type},
                       {"node_path", actual_path.isOk()
                                         ? actual_path.value()
                                         : logical_parent.value() + "/" + logical_name},
                       {"undo_redo_registered", true}};
        if (actual_path.isErr()) result["node_path_verified"] = false;
        return liveResult(result);
    }

    if (method == "scene.removeNode" || method == "scene.duplicateNode" || method == "scene.reparentNode") {
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto node = resolveNode(root.value(), params.value("target_node", ""));
        if (node.isErr()) return errorJson(node.error().code, node.error().message);
        if (node.value() == root.value()) return errorJson(400, "Cannot mutate the edited scene root");
        auto parent_variant = callObject(node.value(), "Node", "get_parent", 3160264692LL);
        if (parent_variant.isErr()) return errorJson(parent_variant.error().code, parent_variant.error().message);
        auto parent = objectFromVariant(parent_variant.value());
        if (parent.isErr() || !parent.value()) return errorJson(400, "Cannot mutate the edited scene root");
        auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        if (include_internal.isErr()) return errorJson(include_internal.error().code, include_internal.error().message);
        auto old_index = callObject(node.value(), "Node", "get_index", 894402480LL, {&include_internal.value()});
        if (old_index.isErr()) return errorJson(old_index.error().code, old_index.error().message);
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto child = makeObject(node.value());
        auto readable = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(1));
        auto internal = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
        if (child.isErr() || readable.isErr() || internal.isErr()) return errorJson(500, "Failed to construct scene transaction arguments");

        if (method == "scene.removeNode") {
            auto preflight = preflightNodeUndoTransaction();
            if (preflight.isErr()) return errorJson(preflight.error().code, preflight.error().message);
            auto action = createAction(manager.value(), "Didi: remove node", root.value());
            auto keep = managerReference(manager.value(), "add_undo_reference", node.value());
            auto remove = managerMethod(manager.value(), "add_do_method", parent.value(), "remove_child", {&child.value()});
            auto restore = managerMethod(manager.value(), "add_undo_method", parent.value(), "add_child",
                                         {&child.value(), &readable.value(), &internal.value()});
            auto restore_index = managerMethod(manager.value(), "add_undo_method", parent.value(), "move_child",
                                               {&child.value(), &old_index.value()});
            if (action.isErr() || keep.isErr() || remove.isErr() || restore_index.isErr() || restore.isErr()) {
                if (action.isOk()) abandonAction(manager.value());
                return errorJson(500, "Failed to register remove UndoRedo transaction");
            }
            auto committed = commitAction(manager.value());
            if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
            return liveResult({{"status", "success"}, {"action", "remove_node"}, {"undo_redo_registered", true}});
        }

        if (method == "scene.reparentNode") {
            auto new_parent = resolveNode(root.value(), params.value("new_parent_path", ""));
            if (new_parent.isErr()) return errorJson(new_parent.error().code, new_parent.error().message);
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
                return errorJson(descendant_check.error().code, descendant_check.error().message);
            }
            auto new_parent_is_descendant = scalarFromVariant<GDExtensionBool>(
                descendant_check.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            if (new_parent_is_descendant.isErr()) {
                return errorJson(new_parent_is_descendant.error().code,
                                 new_parent_is_descendant.error().message);
            }
            if (new_parent_is_descendant.value()) {
                return errorJson(400, "Cannot reparent a node beneath one of its descendants");
            }
            auto preflight = preflightNodeUndoTransaction();
            if (preflight.isErr()) return errorJson(preflight.error().code, preflight.error().message);
            auto action = createAction(manager.value(), "Didi: reparent node", root.value());
            if (action.isErr()) return errorJson(action.error().code, action.error().message);
            auto move = managerMethod(manager.value(), "add_do_method", node.value(), "reparent",
                                      {&new_parent_value.value(), &keep_global.value()});
            auto restore = managerMethod(manager.value(), "add_undo_method", node.value(), "reparent",
                                         {&old_parent_value.value(), &keep_global.value()});
            auto restore_index = managerMethod(manager.value(), "add_undo_method", parent.value(), "move_child",
                                               {&child.value(), &old_index.value()});
            if (move.isErr() || restore.isErr() || restore_index.isErr()) {
                abandonAction(manager.value());
                return errorJson(500, "Failed to register reparent UndoRedo transaction");
            }
            auto committed = commitAction(manager.value());
            if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
            return liveResult({{"status", "success"}, {"action", "reparent_node"}, {"undo_redo_registered", true}});
        }

        auto flags = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(15));
        if (flags.isErr()) return errorJson(flags.error().code, flags.error().message);
        auto duplicated = callObject(node.value(), "Node", "duplicate", 3511555459LL, {&flags.value()});
        if (duplicated.isErr()) return errorJson(duplicated.error().code, duplicated.error().message);
        auto duplicate_node = objectFromVariant(duplicated.value());
        if (duplicate_node.isErr() || !duplicate_node.value()) return errorJson(500, "Godot failed to duplicate node");
        auto source_name = nodeString(node.value(), "get_name", 2002593661LL);
        if (source_name.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(source_name.error().code, source_name.error().message);
        }
        auto copy_name = makeStringName(source_name.value() + "Copy");
        if (copy_name.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(copy_name.error().code, copy_name.error().message);
        }
        auto named = callObject(duplicate_node.value(), "Node", "set_name", 3304788590LL, {&copy_name.value()});
        if (named.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(named.error().code, named.error().message);
        }
        auto duplicate_name = nodeString(duplicate_node.value(), "get_name", 2002593661LL);
        if (duplicate_name.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(duplicate_name.error().code, duplicate_name.error().message);
        }
        auto logical_parent = logicalPathFromEditedRoot(root.value(), parent.value());
        if (logical_parent.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(logical_parent.error().code, logical_parent.error().message);
        }
        auto duplicate_value = makeObject(duplicate_node.value());
        auto owner = makeObject(root.value());
        if (duplicate_value.isErr() || owner.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(500, "Failed to construct duplicate transaction arguments");
        }
        auto preflight = preflightNodeUndoTransaction();
        if (preflight.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(preflight.error().code, preflight.error().message);
        }
        auto action = createAction(manager.value(), "Didi: duplicate node", root.value());
        if (action.isErr()) {
            GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(action.error().code, action.error().message);
        }
        auto keep = managerReference(manager.value(), "add_do_reference", duplicate_node.value());
        auto add = managerMethod(manager.value(), "add_do_method", parent.value(), "add_child",
                                 {&duplicate_value.value(), &readable.value(), &internal.value()});
        auto own = managerMethod(manager.value(), "add_do_method", duplicate_node.value(), "set_owner", {&owner.value()});
        auto remove = managerMethod(manager.value(), "add_undo_method", parent.value(), "remove_child", {&duplicate_value.value()});
        if (keep.isErr() || add.isErr() || own.isErr() || remove.isErr()) {
            abandonAction(manager.value());
            if (keep.isErr()) GodotApi::instance().object_destroy(duplicate_node.value());
            return errorJson(500, "Failed to register duplicate UndoRedo transaction");
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
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
        return liveResult(result);
    }

    if (method == "editor.undo" || method == "editor.redo") {
        auto root = editedSceneRoot(editor);
        auto manager = undoManager(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto root_value = makeObject(root.value());
        auto history_id = callObject(manager.value(), "EditorUndoRedoManager", "get_object_history_id", 1107568780LL, {&root_value.value()});
        if (history_id.isErr()) return errorJson(history_id.error().code, history_id.error().message);
        auto history = callObject(manager.value(), "EditorUndoRedoManager", "get_history_undo_redo", 2417974513LL, {&history_id.value()});
        if (history.isErr()) return errorJson(history.error().code, history.error().message);
        auto undo_redo = objectFromVariant(history.value());
        if (undo_redo.isErr() || !undo_redo.value()) return errorJson(404, "No UndoRedo history exists for the edited scene");
        const bool is_undo = method == "editor.undo";
        auto available = callObject(undo_redo.value(), "UndoRedo", is_undo ? "has_undo" : "has_redo", 36873697LL);
        if (available.isErr()) return errorJson(available.error().code, available.error().message);
        auto has_action = scalarFromVariant<GDExtensionBool>(available.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (has_action.isErr() || !has_action.value()) return errorJson(409, is_undo ? "Nothing to undo" : "Nothing to redo");
        auto executed = callObject(undo_redo.value(), "UndoRedo", is_undo ? "undo" : "redo", 2240911060LL);
        if (executed.isErr()) return errorJson(executed.error().code, executed.error().message);
        return liveResult({{"status", "success"}, {"action", is_undo ? "undo" : "redo"}});
    }

    if (method == "editor.saveScene") {
        auto saved = callObject(editor, "EditorInterface", "save_scene", 166280745LL);
        if (saved.isErr()) return errorJson(saved.error().code, saved.error().message);
        auto code = scalarFromVariant<int64_t>(saved.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (code.isErr()) return errorJson(code.error().code, code.error().message);
        if (code.value() != 0) return errorJson(500, "Godot save_scene failed with Error " + std::to_string(code.value()));
        return liveResult({{"status", "saved"}});
    }

    if (method == "editor.reloadProject") {
        auto filesystem = callObject(editor, "EditorInterface", "get_resource_filesystem", 780151678LL);
        if (filesystem.isErr()) return errorJson(filesystem.error().code, filesystem.error().message);
        auto object = objectFromVariant(filesystem.value());
        if (object.isErr() || !object.value()) return errorJson(503, "EditorFileSystem is unavailable");
        auto scan = callObject(object.value(), "EditorFileSystem", "scan_sources", 3218959716LL);
        if (scan.isErr()) return errorJson(scan.error().code, scan.error().message);
        return liveResult({{"status", "reloaded"}, {"message", "EditorFileSystem source scan requested"}});
    }

    return errorJson(501, "No trustworthy live implementation for method: " + method);
}

Result<ViewportPixels> GodotBridge::captureEditorViewport(const std::string& camera_identifier) {
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return editor_result.error();
    bool capture_2d = camera_identifier == "editor_2d" || camera_identifier == "active_editor_view_2d";
    Result<VariantValue> viewport = capture_2d
        ? callObject(editor_result.value(), "EditorInterface", "get_editor_viewport_2d", 3750751911LL)
        : [&]() -> Result<VariantValue> {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
            if (index.isErr()) return index.error();
            return callObject(editor_result.value(), "EditorInterface", "get_editor_viewport_3d", 1970834490LL, {&index.value()});
        }();
    if (viewport.isErr()) return viewport.error();
    auto viewport_object = objectFromVariant(viewport.value());
    if (viewport_object.isErr() || !viewport_object.value()) return Error::notFound("Editor SubViewport is unavailable");
    auto texture = callObject(viewport_object.value(), "Viewport", "get_texture", 1746695840LL);
    if (texture.isErr()) return texture.error();
    auto texture_object = objectFromVariant(texture.value());
    if (texture_object.isErr() || !texture_object.value()) return Error::notFound("Editor viewport texture is unavailable");
    auto texture_width_value = callObject(texture_object.value(), "Texture2D", "get_width", 3905245786LL);
    auto texture_height_value = callObject(texture_object.value(), "Texture2D", "get_height", 3905245786LL);
    if (texture_width_value.isErr()) return texture_width_value.error();
    if (texture_height_value.isErr()) return texture_height_value.error();
    auto texture_width = scalarFromVariant<int64_t>(texture_width_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto texture_height = scalarFromVariant<int64_t>(texture_height_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (texture_width.isErr()) return texture_width.error();
    if (texture_height.isErr()) return texture_height.error();
    if (texture_width.value() <= 0 || texture_height.value() <= 0) {
        return Error::notFound("Editor viewport has no rendered texture in the current display mode");
    }
    const auto texture_size = image::checkedRgbaSize(texture_width.value(), texture_height.value());
    if (texture_size.isErr()) return texture_size.error();
    auto image = callObject(texture_object.value(), "Texture2D", "get_image", 4190603485LL);
    if (image.isErr()) return image.error();
    auto image_object = objectFromVariant(image.value());
    if (image_object.isErr() || !image_object.value()) return Error::notFound("Editor viewport image is unavailable");
    auto rgba8 = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(5));
    auto converted = callObject(image_object.value(), "Image", "convert", 2120693146LL, {&rgba8.value()});
    if (converted.isErr()) return converted.error();
    auto width_value = callObject(image_object.value(), "Image", "get_width", 3905245786LL);
    auto height_value = callObject(image_object.value(), "Image", "get_height", 3905245786LL);
    if (width_value.isErr()) return width_value.error();
    if (height_value.isErr()) return height_value.error();
    auto width = scalarFromVariant<int64_t>(width_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto height = scalarFromVariant<int64_t>(height_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (width.isErr()) return width.error();
    if (height.isErr()) return height.error();
    const auto expected_size = image::checkedRgbaSize(width.value(), height.value());
    if (expected_size.isErr()) return expected_size.error();
    auto data = callObject(image_object.value(), "Image", "get_data", 2362200018LL);
    if (data.isErr()) return data.error();
    auto size_value = callVariant(data.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    const int64_t expected = static_cast<int64_t>(expected_size.value());
    if (size.value() != expected) return Error::internal("RGBA8 viewport byte count does not match dimensions");
    auto to_packed = GodotApi::instance().get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY);
    if (!to_packed) return Error::internal("PackedByteArray conversion API is unavailable");
    NativeValue packed(GDEXTENSION_VARIANT_TYPE_PACKED_BYTE_ARRAY);
    to_packed(packed.ptr(), data.value().ptr());
    packed.markInitialized();
    ViewportPixels output;
    output.width = static_cast<int>(width.value());
    output.height = static_cast<int>(height.value());
    const auto* first = GodotApi::instance().packed_byte_array_operator_index_const(packed.ptr(), 0);
    if (!first) return Error::internal("PackedByteArray viewport read failed");
    output.rgba.resize(static_cast<size_t>(expected));
    std::memcpy(output.rgba.data(), first, static_cast<size_t>(expected));
    return output;
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

// runtime.readProfiler reads these through Performance.get_monitor. The bind
// hash and the monitor enum values are the ones the Phase 7 feasibility gate
// pinned on Godot 4.5.1 and 4.7.2; availability is exactly that bind existing.
// A zero sample is a legitimate reading, so nothing here infers absence from
// a value.
namespace {
constexpr int64_t kPerformanceGetMonitorHash = 1943275655LL;
} // namespace

Result<void> GodotBridge::preflightPerformanceMonitors() {
    auto performance = singleton("Performance");
    if (performance.isErr()) return performance.error();
    return requireMethodBind("Performance", "get_monitor", kPerformanceGetMonitorHash);
}

Result<std::vector<double>> GodotBridge::samplePerformanceMonitors(
    const std::vector<int64_t>& monitors) {
    auto performance = singleton("Performance");
    if (performance.isErr()) return performance.error();
    std::vector<double> values;
    values.reserve(monitors.size());
    for (const auto monitor : monitors) {
        auto argument = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, monitor);
        if (argument.isErr()) return argument.error();
        auto reading = callObject(performance.value(), "Performance", "get_monitor",
                                  kPerformanceGetMonitorHash, {&argument.value()});
        if (reading.isErr()) return reading.error();
        auto value = scalarFromVariant<double>(reading.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (value.isErr()) return value.error();
        values.push_back(value.value());
    }
    return values;
}

} // namespace godot
} // namespace didi
