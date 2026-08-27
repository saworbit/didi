#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/common/logger.hpp"
#include <array>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <cstring>
#include <limits>
#include <utility>

namespace didi {
namespace godot {
namespace {

constexpr size_t kOpaqueBytes = 64;
using Opaque = std::array<std::byte, kOpaqueBytes>;

json errorJson(int code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

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
    if (value.is_number_float()) return makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, value.get<double>());
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

Result<json> variantToJson(VariantValue& value, int depth = 0) {
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
                auto converted = variantToJson(item.value(), depth + 1);
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
                auto converted = variantToJson(item.value(), depth + 1);
                if (converted.isErr()) return converted.error();
                output[key_text.value()] = converted.value();
            }
            return output;
        }
        default:
            return Error::invalidArgument("Godot Variant type " + std::to_string(type) + " is not JSON-coercible");
    }
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

Result<void> validateResPath(const std::string& path, const std::string& expected_suffix) {
    if (!strings::startsWith(path, "res://") || path.find("..") != std::string::npos ||
        path.find('\\') != std::string::npos ||
        (!expected_suffix.empty() && !strings::endsWith(path, expected_suffix))) {
        return Error::invalidArgument("path must be a normalized res:// path ending in " + expected_suffix);
    }
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

Result<json> buildHierarchy(GDExtensionObjectPtr node, int depth, int max_depth,
                            const std::string& logical_path = {}) {
    auto name = nodeString(node, "get_name", 2002593661LL);
    auto type = nodeString(node, "get_class", 201670096LL);
    if (name.isErr()) return name.error();
    if (type.isErr()) return type.error();
    const std::string path = logical_path.empty() ? "/root/" + name.value() : logical_path;
    json result = {
        {"name", name.value()}, {"type", type.value()}, {"path", path},
        {"properties", json::object()}, {"children", json::array()}
    };
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
                                         path + "/" + child_name.value());
        if (child_json.isErr()) return child_json.error();
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

json GodotBridge::execute(const std::string& method, const json& params) {
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return errorJson(editor_result.error().code, editor_result.error().message);
    auto editor = editor_result.value();

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
            auto rollback = callObject(project_settings.value(), "ProjectSettings", "set_setting", 402577236LL,
                                       {&name.value(), &previous.value()});
            auto rollback_save = rollback.isOk()
                ? callObject(project_settings.value(), "ProjectSettings", "save", 166280745LL)
                : Result<VariantValue>(rollback.error());
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
        std::string script_path;
        const bool attaching = method == "script.attachToNode";
        if (attaching) {
            script_path = params.value("script_path", "");
            auto valid_path = validateResPath(script_path, ".gd");
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
        return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                           {"script_path", script_path}, {"attached", attaching}, {"detached", !attaching},
                           {"undo_redo_registered", true}});
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
        auto persistent = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                     static_cast<GDExtensionBool>(adding ? params.value("persistent", true) : true));
        if (persistent.isErr()) return errorJson(persistent.error().code, persistent.error().message);
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto preflight = preflightUndoManagerBindings();
        if (preflight.isErr()) return errorJson(preflight.error().code, preflight.error().message);
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
        auto hierarchy = buildHierarchy(target.value(), 0, max_depth, logical_root.value());
        if (hierarchy.isErr()) return errorJson(hierarchy.error().code, hierarchy.error().message);
        json omitted = json::array();
        if (params.value("include_properties", true)) omitted.push_back("bulk_properties");
        if (params.value("include_signals", true)) omitted.push_back("signals");
        if (params.value("include_scripts", true)) omitted.push_back("scripts");
        return liveResult({{"root_path", params.value("root_path", "/root")},
                           {"source", "live_scene_tree"}, {"scene_tree", hierarchy.value()},
                           {"omitted_fields", omitted},
                           {"message", "Use focused property/signal tools for fields omitted from hierarchy traversal."}});
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
        if (keep.isErr() || add.isErr() || own.isErr() || remove.isErr()) return errorJson(500, "Failed to register instantiate UndoRedo transaction");
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
        return liveResult({{"status", "success"}, {"action", "instantiate_node"},
                           {"node_type", node_type}, {"node_path", logical_parent.value() + "/" + logical_name},
                           {"undo_redo_registered", true}});
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
            if (action.isErr() || move.isErr() || restore.isErr() || restore_index.isErr()) {
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
        if (keep.isErr() || add.isErr() || own.isErr() || remove.isErr()) return errorJson(500, "Failed to register duplicate UndoRedo transaction");
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);
        return liveResult({{"status", "success"}, {"action", "duplicate_node"},
                           {"duplicated_node", logical_parent.value() + "/" + duplicate_name.value()},
                           {"undo_redo_registered", true}});
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

    return errorJson(501, "No trustworthy Phase 1 live implementation for method: " + method);
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
    if (width.value() <= 0 || height.value() <= 0 || width.value() > 8192 || height.value() > 8192) {
        return Error::internal("Editor viewport returned invalid dimensions");
    }
    auto data = callObject(image_object.value(), "Image", "get_data", 2362200018LL);
    if (data.isErr()) return data.error();
    auto size_value = callVariant(data.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    const int64_t expected = width.value() * height.value() * 4;
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

} // namespace godot
} // namespace didi
