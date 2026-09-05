#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/expression_sandbox.hpp"
#include "didi/gdextension/runtime_bridge.hpp"
#include "didi/gdextension/viewport_renderer.hpp"
#include "didi/common/logger.hpp"
#include "didi/common/json_bounds.hpp"
#include "didi/common/project_path.hpp"
#include "didi/runtime/input_injection.hpp"
#include "didi/runtime/ghost_preview.hpp"
#include "didi/runtime/spatial_queries.hpp"
#include "didi/runtime/segmentation.hpp"
#include "didi/runtime/animation_requests.hpp"
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
#include <tuple>
#include <utility>
#include <vector>

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
        default:
            return false;
    }
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

// Did a property end up holding what the caller asked for?
//
// Compares by value rather than by JSON type, because Godot legitimately
// changes the type on the way in: an integer written to a float property reads
// back as a real, and reporting that as "not applied" would be a false alarm
// on a write that worked perfectly. Only a genuine difference should be
// reported as one.
bool jsonScalarsEquivalent(const json& observed, const json& requested) {
    if (observed.is_number() && requested.is_number()) {
        const double a = observed.get<double>();
        const double b = requested.get<double>();
        if (!std::isfinite(a) || !std::isfinite(b)) return observed == requested;
        const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
        return std::fabs(a - b) <= 1e-9 * scale;
    }
    return observed == requested;
}

// The property name is not decoration. A scene_instantiate_node call carries
// several properties, and a rejection that does not say which one leaves the
// caller guessing at the very moment they have nothing else to look at.
Result<void> validateJsonForPropertyType(const std::string& property_name, const json& value,
                                         GDExtensionVariantType type) {
    switch (matchJsonToPropertyType(value, static_cast<int>(type))) {
        case PropertyTypeMatch::Compatible:
            return Result<void>::ok();
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

struct PropertyDescriptor {
    int declared_type{0};
    std::string class_name;
};

Result<GDExtensionObjectPtr> objectFromVariant(VariantValue& value);
Result<GDExtensionObjectPtr> singleton(const std::string& name);
Result<bool> objectIsClass(GDExtensionObjectPtr object, const char* class_name);
Result<std::string> stringFromVariant(VariantValue& value, GDExtensionVariantType type);
Result<VariantValue> callObject(GDExtensionObjectPtr object, const char* class_name,
                                const char* method_name, int64_t hash,
                                const std::vector<const VariantValue*>& arguments = {});
Result<VariantValue> makeVector3(double x, double y, double z);
Result<VariantValue> makeVector2i(int64_t x, int64_t y);
Result<VariantValue> makeVector3i(int64_t x, int64_t y, int64_t z);

// Loads the resource the caller named and refuses it if it is not what the
// property holds. Writing a Texture2D into a tile_set slot would either be
// dropped by Godot or leave a scene nobody can open, and neither is something
// to report as a successful write. Same check script_attach_to_node makes for a
// Script.
Result<VariantValue> makeResourceForProperty(const std::string& property_name,
                                             const json& value,
                                             const std::string& expected_class) {
    if (value.is_null()) return VariantValue{};
    auto loader = singleton("ResourceLoader");
    if (loader.isErr()) return loader.error();
    const auto path = value.get<std::string>();
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
    if (!expected_class.empty()) {
        auto matches = objectIsClass(loaded.value(), expected_class.c_str());
        if (matches.isErr()) return matches.error();
        if (!matches.value()) {
            auto actual = callObject(loaded.value(), "Object", "get_class", 201670096LL);
            std::string actual_name = "an unrecognised type";
            if (actual.isOk()) {
                auto text = stringFromVariant(actual.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (text.isOk()) actual_name = text.value();
            }
            return Error::invalidArgument("Property \"" + property_name + "\" holds a " +
                                          expected_class + "; " + path + " is " + actual_name);
        }
    }
    return makeObject(loaded.value());
}

// Godot narrows a real to an int on assignment, but converting the whole
// number here keeps the value that reaches the property the one the caller
// named rather than one the engine derived.
Result<VariantValue> makeJsonVariantForProperty(const json& value, GDExtensionVariantType type) {
    switch (type) {
        case GDEXTENSION_VARIANT_TYPE_INT:
            if (isWholeNumberJsonReal(value)) {
                return makeScalar(GDEXTENSION_VARIANT_TYPE_INT,
                                  static_cast<int64_t>(value.get<double>()));
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

Result<VariantValue> callObject(GDExtensionObjectPtr object, const char* class_name,
                                const char* method_name, int64_t hash,
                                const std::vector<const VariantValue*>& arguments) {
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

// Construct a Godot object and finish constructing it.
//
// `GodotApi::classdb_construct_object` is bound to the interface entry point
// `classdb_construct_object2`, which the engine implements as
// `ClassDB::instantiate_without_postinitialization`. The name is the whole
// story: the object comes back before NOTIFICATION_POSTINITIALIZE has been
// sent. Godot's own `memnew` path sends it for built-in classes, so a class
// that does real work there, a themed Control resolving theme items being the
// case that found this, is otherwise handed back half-built. Constructing
// a Label that way segfaults the editor.
//
// Every construction goes through here so a new call site cannot reintroduce
// the omission. `classdb_construct_object3` carries the same requirement, so
// this stays correct across that migration.
GDExtensionObjectPtr constructObject(GDExtensionConstStringNamePtr class_name) {
    auto& api = GodotApi::instance();
    if (!api.classdb_construct_object) return nullptr;
    auto object = api.classdb_construct_object(class_name);
    if (!object) return nullptr;

    // A missing bind leaves the object exactly as it was before this function
    // existed, which is survivable, rather than failing a construction that
    // would otherwise have worked.
    NativeName object_class("Object");
    NativeName notification("notification");
    if (!object_class.valid() || !notification.valid()) return object;
    auto bind = api.classdb_get_method_bind(object_class.ptr(), notification.ptr(),
                                            kObjectNotificationHash);
    if (!bind) return object;

    int64_t what = kNotificationPostInitialize;
    GDExtensionBool reversed = 0;
    const void* arguments[] = {&what, &reversed};
    api.object_method_bind_ptrcall(bind, object, arguments, nullptr);
    return object;
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
Result<json> realVectorToJson(VariantValue& value, int dimensions);
Result<json> wholeVectorToJson(VariantValue& value, int dimensions);
Result<json> colorToJson(VariantValue& value);
Result<json> resourcePathToJson(VariantValue& value);

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
            output[member] = component;
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
    auto object = constructObject(native_class.ptr());
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

// What the class says about a property, as opposed to what it currently holds.
// An empty resource slot reads back as nil, so the value alone cannot say the
// slot is a TileSet, and a resource write has nothing to validate against
// without the declared class.
Result<std::optional<PropertyDescriptor>> findPropertyDescriptor(GDExtensionObjectPtr object,
                                                                 const std::string& property) {
    auto properties = callObject(object, "Object", "get_property_list", 3995934104LL);
    if (properties.isErr()) return properties.error();
    auto size_value = callVariant(properties.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    auto name_key = makeString("name");
    if (name_key.isErr()) return name_key.error();
    auto type_key = makeString("type");
    if (type_key.isErr()) return type_key.error();
    auto class_key = makeString("class_name");
    if (class_key.isErr()) return class_key.error();

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
        if (name.value() != property) continue;

        PropertyDescriptor found;
        auto declared_value = callVariant(descriptor.value(), "get", {&type_key.value()});
        if (declared_value.isOk()) {
            auto declared = scalarFromVariant<int64_t>(declared_value.value(),
                                                       GDEXTENSION_VARIANT_TYPE_INT);
            if (declared.isOk()) found.declared_type = static_cast<int>(declared.value());
        }
        auto class_value = callVariant(descriptor.value(), "get", {&class_key.value()});
        if (class_value.isOk()) {
            const auto class_type = GodotApi::instance().variant_get_type(class_value.value().ptr());
            if (class_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                class_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                auto class_text = stringFromVariant(class_value.value(), class_type);
                if (class_text.isOk()) found.class_name = class_text.value();
            }
        }
        return std::optional<PropertyDescriptor>(found);
    }
    return std::optional<PropertyDescriptor>{};
}

Result<bool> objectHasProperty(GDExtensionObjectPtr object, const std::string& property) {
    auto descriptor = findPropertyDescriptor(object, property);
    if (descriptor.isErr()) return descriptor.error();
    return descriptor.value().has_value();
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


// runtime.injectInput. Every event is constructed and fully configured before
// the first one is dispatched, so a bad event in position five fails the
// batch with nothing sent. Dispatch is Input.parse_input_event, which returns
// void: the count is calls made, not events accepted, and only a fixture that
// observes _input can say more.
namespace {

constexpr int64_t kInputParseInputEventHash = 3754044979LL;

Result<VariantValue> buildInjectedEvent(const runtime::InjectedInputEvent& spec) {
    using Kind = runtime::InjectedInputEvent::Kind;
    const char* class_name = nullptr;
    switch (spec.kind) {
        case Kind::action: class_name = "InputEventAction"; break;
        case Kind::key: class_name = "InputEventKey"; break;
        case Kind::mouse_button: class_name = "InputEventMouseButton"; break;
        case Kind::joypad_button: class_name = "InputEventJoypadButton"; break;
        case Kind::joypad_motion: class_name = "InputEventJoypadMotion"; break;
    }
    NativeName native_class(class_name);
    if (!native_class.valid()) return Error::internal("Failed to construct input event class name");
    auto object = constructObject(native_class.ptr());
    if (!object) return Error::internal(std::string("Godot could not construct ") + class_name);
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
    auto set_bool = [&](const char* owner, const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    auto set_float = [&](const char* owner, const char* method, double number) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, number);
        if (value.isErr()) return value.error();
        auto result = callObject(object, owner, method, 373806689LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    auto check = [&](const Result<void>& step) { return step.isErr(); };

    Result<void> step = Result<void>::ok();
    switch (spec.kind) {
        case Kind::action: {
            auto action = makeStringName(spec.action_name);
            if (action.isErr()) return fail(action.error());
            auto set = callObject(object, "InputEventAction", "set_action", 3304788590LL, {&action.value()});
            if (set.isErr()) return fail(set.error());
            if (check(step = set_bool("InputEventAction", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_float("InputEventAction", "set_strength", spec.strength))) return fail(step.error());
            break;
        }
        case Kind::key: {
            if (spec.keycode > 0 &&
                check(step = set_int("InputEventKey", "set_keycode", 888074362LL, spec.keycode))) return fail(step.error());
            if (spec.physical_keycode > 0 &&
                check(step = set_int("InputEventKey", "set_physical_keycode", 888074362LL, spec.physical_keycode))) return fail(step.error());
            if (spec.unicode > 0 &&
                check(step = set_int("InputEventKey", "set_unicode", 1286410249LL, spec.unicode))) return fail(step.error());
            if (check(step = set_bool("InputEventKey", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventKey", "set_echo", spec.echo))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_shift_pressed", spec.shift_pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_alt_pressed", spec.alt_pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_ctrl_pressed", spec.ctrl_pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventWithModifiers", "set_meta_pressed", spec.meta_pressed))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::mouse_button: {
            if (check(step = set_int("InputEventMouseButton", "set_button_index", 3624991109LL, spec.button_index))) return fail(step.error());
            if (check(step = set_bool("InputEventMouseButton", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_bool("InputEventMouseButton", "set_double_click", spec.double_click))) return fail(step.error());
            if (check(step = set_float("InputEventMouseButton", "set_factor", spec.factor))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::joypad_button: {
            if (check(step = set_int("InputEventJoypadButton", "set_button_index", 1466368136LL, spec.button_index))) return fail(step.error());
            if (check(step = set_bool("InputEventJoypadButton", "set_pressed", spec.pressed))) return fail(step.error());
            if (check(step = set_float("InputEventJoypadButton", "set_pressure", spec.pressure))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
        case Kind::joypad_motion: {
            if (check(step = set_int("InputEventJoypadMotion", "set_axis", 1332685170LL, spec.axis))) return fail(step.error());
            if (check(step = set_float("InputEventJoypadMotion", "set_axis_value", spec.axis_value))) return fail(step.error());
            if (check(step = set_int("InputEvent", "set_device", 1286410249LL, spec.device))) return fail(step.error());
            break;
        }
    }
    auto wrapped = makeObject(object);
    if (wrapped.isErr()) return fail(wrapped.error());
    return wrapped;
}

json injectInput(const json& params, const std::string& session_kind) {
    if (session_kind != "game") return errorJson(409, "session_kind_rejected");
    auto parsed = runtime::parseInputInjectionRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    auto input = singleton("Input");
    if (input.isErr()) return errorJson(501, "Input singleton is unavailable: " + input.error().message);
    auto bind = requireMethodBind("Input", "parse_input_event", kInputParseInputEventHash);
    if (bind.isErr()) return errorJson(501, "Input.parse_input_event is unavailable: " + bind.error().message);

    std::vector<VariantValue> events;
    events.reserve(parsed.value().size());
    json event_types = json::array();
    for (const auto& spec : parsed.value()) {
        auto built = buildInjectedEvent(spec);
        if (built.isErr()) return errorJson(built.error().code, built.error().message);
        events.push_back(std::move(built.value()));
        event_types.push_back(spec.kindName());
    }
    size_t dispatched = 0;
    for (auto& event : events) {
        auto sent = callObject(input.value(), "Input", "parse_input_event", kInputParseInputEventHash, {&event});
        if (sent.isErr()) {
            // Some events went out and this one did not. That is exactly the
            // ambiguous case the contract calls unknown, and it is not retried.
            return errorJson(dispatched == 0 ? 500 : 504, sent.error().message,
                             {{"outcome", dispatched == 0 ? "not_started" : "unknown_outcome"},
                              {"dispatched_event_count", dispatched}, {"retryable", false}});
        }
        ++dispatched;
    }
    return liveResult({{"dispatched_event_count", dispatched}, {"event_types", std::move(event_types)},
                       {"outcome", "completed"}, {"rollback", "not_available"},
                       {"session_kind", session_kind}});
}

} // namespace


// Phase 7B spatial reads. Both use only the attached session root viewport's
// existing World2D/World3D, so nothing here creates a world, a map or a body.
namespace {

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

// What a shader declares a uniform to be when nothing overrides it.
//
// The default lives in the rendering server rather than on the Shader resource,
// so this needs the shader's RID and a renderer behind it. A session running
// without one answers nil, and the caller is told nothing rather than told
// something invented.
Result<VariantValue> shaderParameterDefault(GDExtensionObjectPtr shader, VariantValue& uniform_name) {
    for (const auto& bind : {std::make_tuple("Resource", "get_rid", 2944877500LL),
                             std::make_tuple("RenderingServer", "shader_get_parameter_default",
                                             2621281810LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }
    auto server = singleton("RenderingServer");
    if (server.isErr()) return server.error();
    auto rid = callObject(shader, "Resource", "get_rid", 2944877500LL);
    if (rid.isErr()) return rid.error();
    return callObject(server.value(), "RenderingServer", "shader_get_parameter_default",
                      2621281810LL, {&rid.value(), &uniform_name});
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

Result<VariantValue> makePoint(const runtime::SpatialPoint& point) {
    return point.dimension == 2 ? makeVector2(point.x, point.y)
                                : makeVector3(point.x, point.y, point.z);
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

// The root viewport's World2D or World3D as a Variant, or the object behind it.
Result<VariantValue> rootWorld(int dimension) {
    auto tree = liveSceneTree();
    if (tree.isErr()) return tree.error();
    auto root = liveSceneTreeRoot(tree.value());
    if (root.isErr()) return root.error();
    auto world = dimension == 2
                     ? callObject(root.value(), "Viewport", "get_world_2d", 2339128592LL)
                     : callObject(root.value(), "Viewport", "get_world_3d", 317588385LL);
    if (world.isErr()) return world.error();
    auto object = objectFromVariant(world.value());
    if (object.isErr() || !object.value()) {
        return Error(409, dimension == 2 ? "Root viewport has no World2D" : "Root viewport has no World3D");
    }
    return std::move(world.value());
}

// The binds and the space state a ray needs, resolved once. A batch pays for
// this lookup once rather than once per ray, and every ray in the batch is then
// answered against the same physics state instead of against N successive ones.
// The direct space state of the root viewport's world, which every physics
// query in this file asks its question of. Shared so a ray and a shape sweep
// cannot end up reading two different worlds.
Result<GDExtensionObjectPtr> openDirectSpaceState(int dimension) {
    const int64_t state_hash = dimension == 2 ? 2506717822LL : 2069328350LL;
    auto required = requireMethodBind(dimension == 2 ? "World2D" : "World3D",
                                      "get_direct_space_state", state_hash);
    if (required.isErr()) return Error(501, required.error().message);
    auto world = rootWorld(dimension);
    if (world.isErr()) return world.error();
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr()) return Error::internal(world_object.error().message);
    auto state = callObject(world_object.value(), dimension == 2 ? "World2D" : "World3D",
                            "get_direct_space_state", state_hash);
    if (state.isErr()) return Error::internal(state.error().message);
    auto state_object = objectFromVariant(state.value());
    if (state_object.isErr() || !state_object.value()) {
        return Error(409, "Root viewport world has no direct space state");
    }
    return state_object.value();
}

struct RaycastSpace {
    int dimension{3};
    const char* params_class{nullptr};
    const char* state_class{nullptr};
    int64_t create_hash{0};
    int64_t ray_hash{0};
    GDExtensionObjectPtr state{nullptr};
};

Result<RaycastSpace> openRaycastSpace(int dimension) {
    RaycastSpace space;
    space.dimension = dimension;
    space.params_class = dimension == 2 ? "PhysicsRayQueryParameters2D" : "PhysicsRayQueryParameters3D";
    space.state_class = dimension == 2 ? "PhysicsDirectSpaceState2D" : "PhysicsDirectSpaceState3D";
    space.create_hash = dimension == 2 ? 3196569324LL : 3110599579LL;
    space.ray_hash = dimension == 2 ? 1590275562LL : 3957970750LL;
    const int64_t state_hash = dimension == 2 ? 2506717822LL : 2069328350LL;

    for (const auto& bind : {std::make_tuple(space.params_class, "create", space.create_hash),
                             std::make_tuple(space.state_class, "intersect_ray", space.ray_hash),
                             std::make_tuple(dimension == 2 ? "World2D" : "World3D",
                                             "get_direct_space_state", state_hash)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }

    auto state = openDirectSpaceState(dimension);
    if (state.isErr()) return state.error();
    space.state = state.value();
    return space;
}

// One ray against an already-open space. The record is the same one
// physics_raycast_query returns, so a batch entry and a single call cannot
// describe the same hit differently.
Result<json> castOneRay(const RaycastSpace& space, const runtime::RaycastRequest& request) {
    const int dimension = space.dimension;
    const char* params_class = space.params_class;
    auto from = makePoint(request.from);
    auto to = makePoint(request.to);
    auto mask = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.collision_mask);
    if (from.isErr() || to.isErr() || mask.isErr()) {
        return Error::internal("Failed to construct ray arguments");
    }
    Result<VariantValue> query = [&]() -> Result<VariantValue> {
        auto& api = GodotApi::instance();
        NativeName klass(params_class);
        NativeName method("create");
        if (!klass.valid() || !method.valid()) return Error::internal("Failed to construct ray query identifiers");
        auto bind = api.classdb_get_method_bind(klass.ptr(), method.ptr(), space.create_hash);
        if (!bind) return Error(501, "Godot method binding unavailable for ray query creation");
        const void* raw_args[] = {from.value().ptr(), to.value().ptr(), mask.value().ptr()};
        VariantValue created(VariantValue::Uninitialized{});
        GDExtensionCallError error{};
        api.object_method_bind_call(bind, nullptr, raw_args, 3, created.ptr(), &error);
        created.markInitialized();
        if (error.error != GDEXTENSION_CALL_OK) {
            return Error::internal("PhysicsRayQueryParameters.create failed (call error " +
                                   std::to_string(error.error) + ")");
        }
        return std::move(created);
    }();
    if (query.isErr()) return query.error();
    auto query_object = objectFromVariant(query.value());
    if (query_object.isErr() || !query_object.value()) {
        return Error::internal("Ray query parameters were not created");
    }
    auto set_flag = [&](const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(query_object.value(), params_class, method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    for (const auto& flag : {std::make_pair("set_collide_with_bodies", true),
                             std::make_pair("set_collide_with_areas", true),
                             std::make_pair("set_hit_from_inside", false)}) {
        auto set = set_flag(flag.first, flag.second);
        if (set.isErr()) return Error::internal(set.error().message);
    }
    if (dimension == 3) {
        auto set = set_flag("set_hit_back_faces", true);
        if (set.isErr()) return Error::internal(set.error().message);
    }

    auto hit = callObject(space.state, space.state_class, "intersect_ray", space.ray_hash,
                          {&query.value()});
    if (hit.isErr()) return Error::internal(hit.error().message);
    json result = {{"dimension", dimension}, {"hit", false}, {"collider_path", nullptr},
                   {"collider_class", nullptr}, {"position", nullptr}, {"normal", nullptr},
                   {"collision_layer", nullptr}};
    auto size = callVariant(hit.value(), "size");
    if (size.isErr()) return Error::internal(size.error().message);
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr() || count.value() == 0) return result;

    auto lookup = [&](const char* key) -> Result<VariantValue> {
        auto key_value = makeString(key);
        if (key_value.isErr()) return key_value.error();
        return callVariant(hit.value(), "get", {&key_value.value()});
    };
    auto position = lookup("position");
    auto normal = lookup("normal");
    auto collider = lookup("collider");
    if (position.isErr() || normal.isErr() || collider.isErr()) {
        return Error::internal("Ray hit dictionary is incomplete");
    }
    auto position_json = pointVariantToJson(position.value(), dimension);
    auto normal_json = pointVariantToJson(normal.value(), dimension);
    if (position_json.isErr() || normal_json.isErr()) {
        return Error::internal("Ray hit vectors could not be read");
    }
    result["hit"] = true;
    result["position"] = position_json.value();
    result["normal"] = normal_json.value();
    auto collider_object = objectFromVariant(collider.value());
    if (collider_object.isOk() && collider_object.value()) {
        auto class_name = nodeString(collider_object.value(), "get_class", 201670096LL);
        if (class_name.isOk()) {
            auto bounded = boundUtf8(class_name.value(), 256);
            result["collider_class"] = bounded.value;
        }
        auto is_node_name = makeString("Node");
        if (is_node_name.isOk()) {
            auto is_node = callObject(collider_object.value(), "Object", "is_class", 3927539163LL, {&is_node_name.value()});
            if (is_node.isOk()) {
                auto node_flag = scalarFromVariant<GDExtensionBool>(is_node.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (node_flag.isOk() && node_flag.value()) {
                    auto path = nodeString(collider_object.value(), "get_path", 4075236667LL);
                    if (path.isOk() && !path.value().empty() && path.value().size() <= 1024) {
                        result["collider_path"] = path.value();
                    }
                }
            }
        }
        auto layer = callObject(collider_object.value(),
                                dimension == 2 ? "CollisionObject2D" : "CollisionObject3D",
                                "get_collision_layer", 3905245786LL);
        if (layer.isOk()) {
            auto layer_value = scalarFromVariant<int64_t>(layer.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (layer_value.isOk()) result["collision_layer"] = layer_value.value();
        }
    }
    return result;
}

// A transform with no rotation at a point. A sweep asks where a shape fits, not
// which way it faces, so the basis is the identity and only the origin varies.
Result<VariantValue> makeUprightTransform(const runtime::SpatialPoint& origin) {
    auto& api = GodotApi::instance();
    if (origin.dimension == 2) {
        auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_TRANSFORM2D, 2);
        if (!constructor) return Error::internal("Godot Transform2D constructor is unavailable");
        double rotation = 0.0;
        auto position = makeVector2(origin.x, origin.y);
        if (position.isErr()) return position.error();
        NativeValue native_position(GDEXTENSION_VARIANT_TYPE_VECTOR2);
        auto to_native = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR2);
        if (!to_native) return Error::internal("Godot Vector2 conversion is unavailable");
        to_native(native_position.ptr(), position.value().ptr());
        native_position.markInitialized();
        NativeValue native(GDEXTENSION_VARIANT_TYPE_TRANSFORM2D);
        const void* arguments[] = {&rotation, native_position.ptr()};
        constructor(native.ptr(), arguments);
        native.markInitialized();
        return variantFromNative(GDEXTENSION_VARIANT_TYPE_TRANSFORM2D, native.ptr());
    }
    auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, 3);
    if (!constructor) return Error::internal("Godot Transform3D constructor is unavailable");
    auto to_native = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    if (!to_native) return Error::internal("Godot Vector3 conversion is unavailable");
    NativeValue axes[4] = {NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3)};
    const double components[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                     {origin.x, origin.y, origin.z}};
    for (int index = 0; index < 4; ++index) {
        auto vector = makeVector3(components[index][0], components[index][1], components[index][2]);
        if (vector.isErr()) return vector.error();
        to_native(axes[index].ptr(), vector.value().ptr());
        axes[index].markInitialized();
    }
    NativeValue native(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D);
    const void* arguments[] = {axes[0].ptr(), axes[1].ptr(), axes[2].ptr(), axes[3].ptr()};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, native.ptr());
}

// The shape the caller described, built through ClassDB the same way every
// other object in this file is.
constexpr int64_t kSetFloatHash = 373806689LL;

Result<GDExtensionObjectPtr> makeClearanceShape(const runtime::ClearanceRequest& request) {
    const int dimension = request.dimension();
    const char* class_name = nullptr;
    switch (request.shape) {
        case runtime::ClearanceShapeKind::box:
            class_name = dimension == 2 ? "RectangleShape2D" : "BoxShape3D";
            break;
        case runtime::ClearanceShapeKind::sphere:
            class_name = dimension == 2 ? "CircleShape2D" : "SphereShape3D";
            break;
        case runtime::ClearanceShapeKind::capsule:
            class_name = dimension == 2 ? "CapsuleShape2D" : "CapsuleShape3D";
            break;
    }
    NativeName type_name(class_name);
    auto shape = constructObject(type_name.ptr());
    if (!shape) return Error(501, std::string("Godot ClassDB could not construct ") + class_name);

    // void(float), which is what set_radius and set_height both are.
    const auto set_number = [&](const char* method, double value) -> Result<void> {
        auto argument = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, value);
        if (argument.isErr()) return argument.error();
        auto called = callObject(shape, class_name, method, kSetFloatHash, {&argument.value()});
        return called.isOk() ? Result<void>::ok() : Result<void>(called.error());
    };

    if (request.shape == runtime::ClearanceShapeKind::box) {
        auto size = dimension == 2 ? makeVector2(request.size.x, request.size.y)
                                   : makeVector3(request.size.x, request.size.y, request.size.z);
        if (size.isErr()) return size.error();
        // The same numbers as set_motion above, and not a copied constant. A
        // Godot method bind hash is computed from the signature and not the
        // name, so every void(Vector3) setter shares one, which is why
        // BoxShape3D.set_size and PhysicsShapeQueryParameters3D.set_motion
        // agree. Verified against the shipped extension_api dump.
        const int64_t hash = dimension == 2 ? 743155724LL : 3460891852LL;
        auto called = callObject(shape, class_name, "set_size", hash, {&size.value()});
        if (called.isErr()) return called.error();
    } else {
        auto radius = set_number("set_radius", request.radius);
        if (radius.isErr()) return radius.error();
        if (request.shape == runtime::ClearanceShapeKind::capsule) {
            auto height = set_number("set_height", request.height);
            if (height.isErr()) return height.error();
        }
    }
    return shape;
}

// Named so a hash appears once and carries the same type wherever it is used.
constexpr int64_t kSetShapeHash = 968641751LL;
constexpr int64_t kSetCollisionMaskHash = 1286410249LL;
constexpr int64_t kSetBoolHash = 2586408642LL;

json physicsClearance(const json& params) {
    auto parsed = runtime::parseClearanceRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    const auto& request = parsed.value();
    const int dimension = request.dimension();
    const char* params_class = dimension == 2 ? "PhysicsShapeQueryParameters2D"
                                              : "PhysicsShapeQueryParameters3D";
    const char* state_class = dimension == 2 ? "PhysicsDirectSpaceState2D"
                                             : "PhysicsDirectSpaceState3D";
    const int64_t cast_hash = dimension == 2 ? 711275086LL : 1778757334LL;
    const int64_t transform_hash = dimension == 2 ? 2761652528LL : 2952846383LL;
    const int64_t motion_hash = dimension == 2 ? 743155724LL : 3460891852LL;

    // Spelled out rather than deduced. int64_t is long on Linux and long long on
    // Windows, so a list mixing a hash variable with an LL literal has no common
    // type on gcc and only fails there.
    using ShapeBind = std::tuple<const char*, const char*, int64_t>;
    for (const ShapeBind& bind : {
             ShapeBind{state_class, "cast_motion", cast_hash},
             ShapeBind{params_class, "set_shape", kSetShapeHash},
             ShapeBind{params_class, "set_transform", transform_hash},
             ShapeBind{params_class, "set_motion", motion_hash},
             ShapeBind{params_class, "set_collision_mask", kSetCollisionMaskHash},
             ShapeBind{params_class, "set_collide_with_bodies", kSetBoolHash},
             ShapeBind{params_class, "set_collide_with_areas", kSetBoolHash}}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    auto state = openDirectSpaceState(dimension);
    if (state.isErr()) return errorJson(state.error().code, state.error().message);

    auto shape = makeClearanceShape(request);
    if (shape.isErr()) return errorJson(shape.error().code, shape.error().message);
    auto shape_value = makeObject(shape.value());
    if (shape_value.isErr()) return errorJson(500, shape_value.error().message);

    NativeName query_name(params_class);
    auto query = constructObject(query_name.ptr());
    if (!query) return errorJson(501, std::string("Godot ClassDB could not construct ") + params_class);
    auto query_value = makeObject(query);
    if (query_value.isErr()) return errorJson(500, query_value.error().message);

    auto transform = makeUprightTransform(request.from);
    if (transform.isErr()) return errorJson(transform.error().code, transform.error().message);
    runtime::SpatialPoint motion_point = request.to;
    motion_point.x -= request.from.x;
    motion_point.y -= request.from.y;
    motion_point.z -= request.from.z;
    auto motion = makePoint(motion_point);
    if (motion.isErr()) return errorJson(500, motion.error().message);
    auto mask = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.collision_mask);
    if (mask.isErr()) return errorJson(500, mask.error().message);

    using ShapeSetter = std::tuple<const char*, int64_t, const VariantValue*>;
    for (const ShapeSetter& call : {
             ShapeSetter{"set_shape", kSetShapeHash, &shape_value.value()},
             ShapeSetter{"set_transform", transform_hash, &transform.value()},
             ShapeSetter{"set_motion", motion_hash, &motion.value()},
             ShapeSetter{"set_collision_mask", kSetCollisionMaskHash, &mask.value()}}) {
        auto called = callObject(query, params_class, std::get<0>(call), std::get<1>(call),
                                 {std::get<2>(call)});
        if (called.isErr()) return errorJson(500, called.error().message);
    }
    // Bodies block a body; an area does not. A trigger volume is not geometry,
    // and a corridor that reports blocked because a checkpoint sits in it is
    // answering a different question from the one asked. This differs from the
    // raycast on purpose, and the tool reference says so.
    for (const auto& flag : {std::make_pair("set_collide_with_bodies", true),
                             std::make_pair("set_collide_with_areas", false)}) {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                static_cast<GDExtensionBool>(flag.second));
        if (value.isErr()) return errorJson(500, value.error().message);
        auto called = callObject(query, params_class, flag.first, kSetBoolHash, {&value.value()});
        if (called.isErr()) return errorJson(500, called.error().message);
    }

    auto swept = callObject(state.value(), state_class, "cast_motion", cast_hash, {&query_value.value()});
    if (swept.isErr()) return errorJson(500, swept.error().message);
    auto size_value = callVariant(swept.value(), "size");
    if (size_value.isErr()) return errorJson(500, size_value.error().message);
    auto count = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr() || count.value() < 2) {
        return errorJson(500, "cast_motion did not return a safe and an unsafe fraction");
    }
    const auto fraction = [&](int64_t index) -> Result<double> {
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return index_value.error();
        auto item = callVariant(swept.value(), "get", {&index_value.value()});
        if (item.isErr()) return item.error();
        return scalarFromVariant<double>(item.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
    };
    auto safe = fraction(0);
    auto unsafe = fraction(1);
    if (safe.isErr() || unsafe.isErr()) return errorJson(500, "cast_motion fractions could not be read");

    // Reported as the engine gave them, plus the arithmetic that turns a
    // fraction into the point a caller can act on. Nothing here interprets what
    // a particular pair of fractions means.
    runtime::SpatialPoint reached = request.from;
    reached.x += motion_point.x * safe.value();
    reached.y += motion_point.y * safe.value();
    if (dimension == 3) reached.z += motion_point.z * safe.value();

    return liveResult({{"dimension", dimension},
                       {"clear", safe.value() >= 1.0},
                       {"safe_fraction", safe.value()},
                       {"unsafe_fraction", unsafe.value()},
                       {"safe_position", reached.toJson()},
                       {"collide_with_areas", false}});
}

// A frustum reduced to the six half-spaces every point in this file is tested
// against. Both request forms end here, so a node one form calls visible is
// never a node the other calls hidden.
//
// The frustum is stored as an origin and three unit axes rather than as a
// matrix, because that is what a half-space test actually reads and because a
// basis assembled once cannot drift from the one the response reports.
struct FrustumBasis {
    double origin[3]{0.0, 0.0, 0.0};
    double forward[3]{0.0, 0.0, -1.0};
    double up[3]{0.0, 1.0, 0.0};
    double right[3]{1.0, 0.0, 0.0};
    double near_plane{0.05};
    double far_plane{100.0};
    // Perspective: half extents at unit depth. Orthogonal: half extents flat.
    double half_x{1.0};
    double half_y{1.0};
    bool orthogonal{false};
    double aspect{1.0};
    // Reported back so the caller can see the frustum that answered, rather
    // than the one it believes it asked for.
    double fov_degrees{0.0};
    double ortho_size{0.0};
    std::string aspect_source{"parameters"};
};

void frustumAxis(double* out, double x, double y, double z) {
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length < 1e-12) {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 0.0;
        return;
    }
    out[0] = x / length;
    out[1] = y / length;
    out[2] = z / length;
}

void frustumCross(double* out, const double* a, const double* b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

double frustumDot(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

json frustumAxisJson(const double* axis) {
    return json{{"x", axis[0]}, {"y", axis[1]}, {"z", axis[2]}};
}

// Right and up are rebuilt from forward so a caller-supplied up that is merely
// near the view direction still yields an orthogonal basis.
Result<void> orientFrustum(FrustumBasis& basis, double fx, double fy, double fz,
                           double ux, double uy, double uz) {
    frustumAxis(basis.forward, fx, fy, fz);
    double raw_up[3];
    frustumAxis(raw_up, ux, uy, uz);
    if (frustumDot(basis.forward, basis.forward) < 0.5 || frustumDot(raw_up, raw_up) < 0.5) {
        return Error(409, "The camera basis has no direction to look along");
    }
    frustumCross(basis.right, basis.forward, raw_up);
    frustumAxis(basis.right, basis.right[0], basis.right[1], basis.right[2]);
    if (frustumDot(basis.right, basis.right) < 0.5) {
        return Error(409, "The camera up direction is parallel to the view direction, which leaves "
                          "the roll of the frustum undefined");
    }
    frustumCross(basis.up, basis.right, basis.forward);
    frustumAxis(basis.up, basis.up[0], basis.up[1], basis.up[2]);
    return Result<void>::ok();
}

// Inside is every value at or above zero. The planes are not unit length, which
// does not matter: only the sign is ever read.
void frustumHalfSpaces(const FrustumBasis& basis, const double* point, double* out) {
    const double delta[3] = {point[0] - basis.origin[0], point[1] - basis.origin[1],
                             point[2] - basis.origin[2]};
    const double depth = frustumDot(delta, basis.forward);
    const double lateral = frustumDot(delta, basis.right);
    const double vertical = frustumDot(delta, basis.up);
    out[0] = depth - basis.near_plane;
    out[1] = basis.far_plane - depth;
    if (basis.orthogonal) {
        out[2] = basis.half_x + lateral;
        out[3] = basis.half_x - lateral;
        out[4] = basis.half_y + vertical;
        out[5] = basis.half_y - vertical;
        return;
    }
    out[2] = basis.half_x * depth + lateral;
    out[3] = basis.half_x * depth - lateral;
    out[4] = basis.half_y * depth + vertical;
    out[5] = basis.half_y * depth - vertical;
}

enum class FrustumContainment { outside, intersecting, inside };

// Whole corners inside means inside. Every corner outside one single plane
// means outside. Anything else is called intersecting, which is the ordinary
// conservative frustum test: a box that straddles two planes without entering
// the volume is reported as intersecting rather than missed, because saying a
// node might be visible when it is not is the safe direction to be wrong in.
FrustumContainment frustumContains(const FrustumBasis& basis,
                                   const std::vector<std::array<double, 3>>& corners) {
    if (corners.empty()) return FrustumContainment::outside;
    bool all_inside = true;
    bool outside_plane[6] = {true, true, true, true, true, true};
    for (const auto& corner : corners) {
        double values[6];
        frustumHalfSpaces(basis, corner.data(), values);
        for (int plane = 0; plane < 6; ++plane) {
            if (values[plane] >= 0.0) {
                outside_plane[plane] = false;
            } else {
                all_inside = false;
            }
        }
    }
    for (int plane = 0; plane < 6; ++plane) {
        if (outside_plane[plane]) return FrustumContainment::outside;
    }
    return all_inside ? FrustumContainment::inside : FrustumContainment::intersecting;
}

Result<double> projectViewportAspect(std::string& source) {
    auto settings = singleton("ProjectSettings");
    if (settings.isErr()) return settings.error();
    const auto read = [&](const char* key) -> Result<double> {
        auto name = makeString(key);
        if (name.isErr()) return name.error();
        VariantValue fallback;
        auto value = callObject(settings.value(), "ProjectSettings", "get_setting", 223050753LL,
                                {&name.value(), &fallback});
        if (value.isErr()) return value.error();
        auto number = scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (number.isErr()) return number.error();
        return static_cast<double>(number.value());
    };
    auto width = read("display/window/size/viewport_width");
    auto height = read("display/window/size/viewport_height");
    if (width.isErr() || height.isErr() || width.value() <= 0.0 || height.value() <= 0.0) {
        return Error(409, "The project has no viewport size to take an aspect ratio from");
    }
    source = "project_settings";
    return width.value() / height.value();
}

// The frustum of a Camera3D already in the scene.
//
// Aspect ratio does not live on the camera, so where it comes from is a real
// choice and the answer changes with it. A running game renders through the
// camera's own viewport, so that is the frustum being drawn. An edited scene
// renders through an editor pane whose shape is a fact about the window, not
// about the game, so the project's configured viewport size is the honest
// source there. Which one was used is reported either way.
Result<FrustumBasis> frustumFromCamera(GDExtensionObjectPtr camera, const std::string& session_kind) {
    for (const auto& bind : {std::make_tuple("Node3D", "get_global_position", 3360562783LL),
                             std::make_tuple("Node3D", "to_global", 192990374LL),
                             std::make_tuple("Camera3D", "get_fov", 1740695150LL),
                             std::make_tuple("Camera3D", "get_near", 1740695150LL),
                             std::make_tuple("Camera3D", "get_far", 1740695150LL),
                             std::make_tuple("Camera3D", "get_size", 1740695150LL),
                             std::make_tuple("Camera3D", "get_projection", 2624185235LL),
                             std::make_tuple("Camera3D", "get_keep_aspect_mode", 2790278316LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }

    const auto read_point = [&](const char* method, int64_t hash,
                                const std::vector<const VariantValue*>& args)
        -> Result<std::array<double, 3>> {
        auto value = callObject(camera, "Node3D", method, hash, args);
        if (value.isErr()) return value.error();
        auto point = pointVariantToJson(value.value(), 3);
        if (point.isErr()) return point.error();
        return std::array<double, 3>{point.value()["x"].get<double>(), point.value()["y"].get<double>(),
                                     point.value()["z"].get<double>()};
    };
    auto origin = read_point("get_global_position", 3360562783LL, {});
    if (origin.isErr()) return origin.error();
    auto local_forward = makeVector3(0.0, 0.0, -1.0);
    auto local_up = makeVector3(0.0, 1.0, 0.0);
    if (local_forward.isErr() || local_up.isErr()) {
        return Error::internal("Failed to construct camera basis probes");
    }
    auto ahead = read_point("to_global", 192990374LL, {&local_forward.value()});
    if (ahead.isErr()) return ahead.error();
    auto above = read_point("to_global", 192990374LL, {&local_up.value()});
    if (above.isErr()) return above.error();

    FrustumBasis basis;
    basis.origin[0] = origin.value()[0];
    basis.origin[1] = origin.value()[1];
    basis.origin[2] = origin.value()[2];
    auto oriented = orientFrustum(basis,
                                  ahead.value()[0] - origin.value()[0],
                                  ahead.value()[1] - origin.value()[1],
                                  ahead.value()[2] - origin.value()[2],
                                  above.value()[0] - origin.value()[0],
                                  above.value()[1] - origin.value()[1],
                                  above.value()[2] - origin.value()[2]);
    if (oriented.isErr()) return oriented.error();

    const auto read_number = [&](const char* method, int64_t hash) -> Result<double> {
        auto value = callObject(camera, "Camera3D", method, hash);
        if (value.isErr()) return value.error();
        auto number = scalarFromVariant<double>(value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (number.isErr()) return number.error();
        return number.value();
    };
    const auto read_enum = [&](const char* method, int64_t hash) -> Result<int64_t> {
        auto value = callObject(camera, "Camera3D", method, hash);
        if (value.isErr()) return value.error();
        return scalarFromVariant<int64_t>(value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    };
    auto projection = read_enum("get_projection", 2624185235LL);
    if (projection.isErr()) return projection.error();
    if (projection.value() == 2) {
        // A frustum-mode camera carries an off-axis offset that this basis has
        // no place to put. Answering with the centred frustum would describe a
        // volume the camera is not looking through.
        return Error(409, "The camera uses the frustum projection mode, whose off-axis offset this "
                          "query does not model");
    }
    if (projection.value() != 0 && projection.value() != 1) {
        return Error(409, "The camera uses a projection mode this query does not model");
    }
    auto near_plane = read_number("get_near", 1740695150LL);
    auto far_plane = read_number("get_far", 1740695150LL);
    if (near_plane.isErr()) return near_plane.error();
    if (far_plane.isErr()) return far_plane.error();
    basis.near_plane = near_plane.value();
    basis.far_plane = far_plane.value();
    if (!(basis.near_plane > 0.0) || !(basis.far_plane > basis.near_plane)) {
        return Error(409, "The camera near and far planes do not describe a volume");
    }

    if (session_kind == "game") {
        auto required = requireMethodBind("Viewport", "get_visible_rect", 1639390495LL);
        if (required.isErr()) return Error(501, required.error().message);
        auto viewport_value = callObject(camera, "Node", "get_viewport", 3596683776LL);
        if (viewport_value.isErr()) return viewport_value.error();
        auto viewport = objectFromVariant(viewport_value.value());
        if (viewport.isErr() || !viewport.value()) {
            return Error(409, "The camera is not inside a viewport to take an aspect ratio from");
        }
        auto rect_value = callObject(viewport.value(), "Viewport", "get_visible_rect", 1639390495LL);
        if (rect_value.isErr()) return rect_value.error();
        auto rect = rect2ToJson(rect_value.value());
        if (rect.isErr()) return rect.error();
        const double width = rect.value()["size"]["x"].get<double>();
        const double height = rect.value()["size"]["y"].get<double>();
        if (!(width > 0.0) || !(height > 0.0)) {
            return Error(409, "The camera viewport has no visible size to take an aspect ratio from");
        }
        basis.aspect = width / height;
        basis.aspect_source = "viewport";
    } else {
        auto aspect = projectViewportAspect(basis.aspect_source);
        if (aspect.isErr()) return aspect.error();
        basis.aspect = aspect.value();
    }

    auto keep_aspect = read_enum("get_keep_aspect_mode", 2790278316LL);
    if (keep_aspect.isErr()) return keep_aspect.error();
    // Godot names the enum after the axis the value is held on: KEEP_WIDTH is
    // 0 and makes fov horizontal, KEEP_HEIGHT is 1 and makes it vertical.
    const bool keep_width = keep_aspect.value() == 0;
    basis.orthogonal = projection.value() == 1;
    if (basis.orthogonal) {
        auto size = read_number("get_size", 1740695150LL);
        if (size.isErr()) return size.error();
        if (!(size.value() > 0.0)) return Error(409, "The orthogonal camera has no size");
        basis.ortho_size = size.value();
        if (keep_width) {
            basis.half_x = size.value() / 2.0;
            basis.half_y = basis.half_x / basis.aspect;
        } else {
            basis.half_y = size.value() / 2.0;
            basis.half_x = basis.half_y * basis.aspect;
        }
    } else {
        auto fov = read_number("get_fov", 1740695150LL);
        if (fov.isErr()) return fov.error();
        if (!(fov.value() > 0.0) || fov.value() >= 180.0) {
            return Error(409, "The camera field of view does not describe a frustum");
        }
        basis.fov_degrees = fov.value();
        const double half_angle = fov.value() * 3.14159265358979323846 / 360.0;
        if (keep_width) {
            basis.half_x = std::tan(half_angle);
            basis.half_y = basis.half_x / basis.aspect;
        } else {
            basis.half_y = std::tan(half_angle);
            basis.half_x = basis.half_y * basis.aspect;
        }
    }
    return basis;
}

// One node that survived the frustum test, held until every node has been seen
// so the nearest can be reported first.
struct FrustumCandidate {
    std::string path;
    std::string class_name;
    FrustumContainment containment{FrustumContainment::inside};
    bool bounded{false};
    bool visible_in_tree{true};
    double distance{0.0};
    std::vector<std::array<double, 3>> samples;
};

const char* frustumContainmentName(FrustumContainment containment) {
    return containment == FrustumContainment::inside ? "inside" : "intersecting";
}

// The eight corners of a node's own bounding box in world space.
//
// Node3D.to_global is asked for each corner rather than the transform being
// read and multiplied here, so a scaled, sheared or deeply nested node is
// placed by the same code the engine places it with.
Result<std::vector<std::array<double, 3>>> worldBoundsCorners(GDExtensionObjectPtr node) {
    auto aabb_value = callObject(node, "VisualInstance3D", "get_aabb", 1068685055LL);
    if (aabb_value.isErr()) return aabb_value.error();
    auto& api = GodotApi::instance();
    if (api.variant_get_type(aabb_value.value().ptr()) != GDEXTENSION_VARIANT_TYPE_AABB ||
        !api.variant_get_ptr_getter) {
        return Error::internal("Godot did not return an AABB for the node bounds");
    }
    auto converter = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_AABB);
    if (!converter) return Error::internal("Godot AABB conversion is unavailable");
    NativeValue native_aabb(GDEXTENSION_VARIANT_TYPE_AABB);
    converter(native_aabb.ptr(), aabb_value.value().ptr());
    native_aabb.markInitialized();
    double corner[3] = {0.0, 0.0, 0.0};
    double extent[3] = {0.0, 0.0, 0.0};
    for (const auto& member : {std::make_pair("position", corner), std::make_pair("size", extent)}) {
        NativeName name(member.first);
        auto getter = name.valid()
            ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_AABB, name.ptr()) : nullptr;
        if (!getter) return Error::internal("Godot AABB member getter is unavailable");
        NativeValue native_vector(GDEXTENSION_VARIANT_TYPE_VECTOR3);
        getter(native_aabb.ptr(), native_vector.ptr());
        native_vector.markInitialized();
        int axis_index = 0;
        for (const auto* axis : {"x", "y", "z"}) {
            NativeName axis_name(axis);
            auto axis_getter = axis_name.valid()
                ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_VECTOR3, axis_name.ptr()) : nullptr;
            if (!axis_getter) return Error::internal("Godot Vector3 member getter is unavailable");
            axis_getter(native_vector.ptr(), &member.second[axis_index]);
            ++axis_index;
        }
    }

    std::vector<std::array<double, 3>> corners;
    corners.reserve(8);
    for (int mask = 0; mask < 8; ++mask) {
        const double local_x = corner[0] + ((mask & 1) ? extent[0] : 0.0);
        const double local_y = corner[1] + ((mask & 2) ? extent[1] : 0.0);
        const double local_z = corner[2] + ((mask & 4) ? extent[2] : 0.0);
        auto local = makeVector3(local_x, local_y, local_z);
        if (local.isErr()) return local.error();
        auto world = callObject(node, "Node3D", "to_global", 192990374LL, {&local.value()});
        if (world.isErr()) return world.error();
        auto point = pointVariantToJson(world.value(), 3);
        if (point.isErr()) return point.error();
        corners.push_back({point.value()["x"].get<double>(), point.value()["y"].get<double>(),
                           point.value()["z"].get<double>()});
    }
    return corners;
}

// Nearest first, and no more of them held than will be reported.
//
// A scene can put far more nodes inside a frustum than a caller asked to see,
// and keeping all of them only to throw most away would make the cost of the
// query depend on the scene rather than on the question. The kept set is a
// max-heap on distance, so the furthest held node is the one displaced.
bool frustumFurther(const FrustumCandidate& a, const FrustumCandidate& b) {
    return a.distance < b.distance;
}

struct FrustumScan {
    const FrustumBasis* basis{nullptr};
    GDExtensionObjectPtr root{nullptr};
    bool editor{true};
    int64_t keep{64};
    int64_t examined{0};
    int64_t inside_count{0};
    bool examine_limit_hit{false};
    std::vector<FrustumCandidate> candidates;

    void offer(FrustumCandidate&& candidate) {
        if (static_cast<int64_t>(candidates.size()) < keep) {
            candidates.push_back(std::move(candidate));
            std::push_heap(candidates.begin(), candidates.end(), frustumFurther);
            return;
        }
        if (!(candidate.distance < candidates.front().distance)) return;
        std::pop_heap(candidates.begin(), candidates.end(), frustumFurther);
        candidates.back() = std::move(candidate);
        std::push_heap(candidates.begin(), candidates.end(), frustumFurther);
    }
};

Result<void> scanFrustumNode(GDExtensionObjectPtr node, FrustumScan& scan) {
    if (scan.examined >= runtime::kMaxFrustumNodesExamined) {
        scan.examine_limit_hit = true;
        return Result<void>::ok();
    }
    ++scan.examined;

    auto spatial = objectIsClass(node, "Node3D");
    if (spatial.isErr()) return spatial.error();
    if (spatial.value()) {
        auto bounded = objectIsClass(node, "VisualInstance3D");
        if (bounded.isErr()) return bounded.error();
        std::vector<std::array<double, 3>> corners;
        if (bounded.value()) {
            auto world_corners = worldBoundsCorners(node);
            if (world_corners.isErr()) return world_corners.error();
            corners = std::move(world_corners.value());
        } else {
            auto origin = callObject(node, "Node3D", "get_global_position", 3360562783LL);
            if (origin.isErr()) return origin.error();
            auto point = pointVariantToJson(origin.value(), 3);
            if (point.isErr()) return point.error();
            corners.push_back({point.value()["x"].get<double>(), point.value()["y"].get<double>(),
                               point.value()["z"].get<double>()});
        }
        const auto containment = frustumContains(*scan.basis, corners);
        if (containment != FrustumContainment::outside) {
            ++scan.inside_count;
            FrustumCandidate candidate;
            candidate.containment = containment;
            candidate.bounded = bounded.value();
            auto class_name = nodeString(node, "get_class", 201670096LL);
            if (class_name.isErr()) return class_name.error();
            candidate.class_name = boundUtf8(class_name.value(), 256).value;
            auto path = scan.editor ? logicalPathFromEditedRoot(scan.root, node)
                                    : nodeString(node, "get_path", 4075236667LL);
            if (path.isErr()) return path.error();
            candidate.path = boundUtf8(path.value(), 1024).value;
            auto visible = callObject(node, "Node3D", "is_visible_in_tree", 36873697LL);
            if (visible.isErr()) return visible.error();
            auto visible_flag = scalarFromVariant<GDExtensionBool>(visible.value(),
                                                                   GDEXTENSION_VARIANT_TYPE_BOOL);
            if (visible_flag.isErr()) return visible_flag.error();
            candidate.visible_in_tree = visible_flag.value() != 0;

            double centre[3] = {0.0, 0.0, 0.0};
            for (const auto& point : corners) {
                centre[0] += point[0] / static_cast<double>(corners.size());
                centre[1] += point[1] / static_cast<double>(corners.size());
                centre[2] += point[2] / static_cast<double>(corners.size());
            }
            const double delta[3] = {centre[0] - scan.basis->origin[0],
                                     centre[1] - scan.basis->origin[1],
                                     centre[2] - scan.basis->origin[2]};
            candidate.distance = std::sqrt(frustumDot(delta, delta));
            candidate.samples = std::move(corners);
            if (candidate.bounded) {
                candidate.samples.push_back({centre[0], centre[1], centre[2]});
            }
            scan.offer(std::move(candidate));
        }
    }

    // A node with no place in the world can still hold children that have one,
    // so the walk descends whatever the node itself turned out to be.
    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
    if (children.isErr()) return children.error();
    auto size_value = callVariant(children.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    for (int64_t index = 0; index < size.value(); ++index) {
        auto slot = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (slot.isErr()) return slot.error();
        auto child_value = callVariant(children.value(), "get", {&slot.value()});
        if (child_value.isErr()) return child_value.error();
        auto child = objectFromVariant(child_value.value());
        if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
        auto walked = scanFrustumNode(child.value(), scan);
        if (walked.isErr()) return walked.error();
        if (scan.examine_limit_hit) break;
    }
    return Result<void>::ok();
}

// Whether a hit stands between the camera and the node, or is the node itself.
//
// A ray aimed at a node's own bounding box normally hits that node's collider
// just short of the sample point, which is not an obstruction. Godot puts a
// collider on a body that may be the node, its child, or its parent, so a hit
// is treated as the node when either path contains the other.
bool frustumHitIsSelf(const std::string& node_path, const std::string& collider_path) {
    if (collider_path.empty()) return false;
    if (collider_path == node_path) return true;
    // Equal lengths that are not equal strings are two different nodes, and the
    // separator check below would be reading one past the end of the shorter.
    if (collider_path.size() == node_path.size()) return false;
    const std::string& longer = collider_path.size() > node_path.size() ? collider_path : node_path;
    const std::string& shorter = collider_path.size() > node_path.size() ? node_path : collider_path;
    return longer.compare(0, shorter.size(), shorter) == 0 && longer[shorter.size()] == '/';
}

json visionFrustumQuery(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseFrustumRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    const auto& request = parsed.value();

    for (const auto& bind : {std::make_tuple("Node", "get_children", 873284517LL),
                             std::make_tuple("Node3D", "is_visible_in_tree", 36873697LL),
                             std::make_tuple("Node3D", "get_global_position", 3360562783LL),
                             std::make_tuple("Node3D", "to_global", 192990374LL),
                             std::make_tuple("VisualInstance3D", "get_aabb", 1068685055LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    Result<GDExtensionObjectPtr> root = Error::internal("unresolved");
    const bool editor = session_kind == "editor";
    if (editor) {
        auto interface = editorInterface();
        if (interface.isErr()) return errorJson(interface.error().code, interface.error().message);
        root = editedSceneRoot(interface.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return errorJson(tree.error().code, tree.error().message);
        root = liveSceneTreeRoot(tree.value());
    }
    if (root.isErr()) return errorJson(root.error().code, root.error().message);

    FrustumBasis basis;
    json camera_json = json::object();
    if (request.source == runtime::FrustumSource::camera_node) {
        auto node = resolveNode(root.value(), request.camera_node);
        if (node.isErr()) return errorJson(node.error().code, node.error().message);
        auto is_camera = objectIsClass(node.value(), "Camera3D");
        if (is_camera.isErr()) return errorJson(500, is_camera.error().message);
        if (!is_camera.value()) {
            return errorJson(409, "The node at " + request.camera_node + " is not a Camera3D");
        }
        auto built = frustumFromCamera(node.value(), session_kind);
        if (built.isErr()) return errorJson(built.error().code, built.error().message);
        basis = std::move(built.value());
        camera_json["source"] = "camera_node";
        camera_json["node_path"] = request.camera_node;
    } else {
        basis.origin[0] = request.position.x;
        basis.origin[1] = request.position.y;
        basis.origin[2] = request.position.z;
        auto oriented = orientFrustum(basis, request.look_at.x - request.position.x,
                                      request.look_at.y - request.position.y,
                                      request.look_at.z - request.position.z,
                                      request.up.x, request.up.y, request.up.z);
        if (oriented.isErr()) return errorJson(oriented.error().code, oriented.error().message);
        basis.near_plane = request.near_plane;
        basis.far_plane = request.far_plane;
        basis.aspect = request.aspect;
        basis.fov_degrees = request.fov_degrees;
        // The parameter form takes a vertical field of view, which is what a
        // Godot camera holds by default.
        basis.half_y = std::tan(request.fov_degrees * 3.14159265358979323846 / 360.0);
        basis.half_x = basis.half_y * basis.aspect;
        camera_json["source"] = "parameters";
        camera_json["node_path"] = nullptr;
        camera_json["up_defaulted"] = !request.up_given;
    }
    camera_json["position"] = frustumAxisJson(basis.origin);
    camera_json["forward"] = frustumAxisJson(basis.forward);
    camera_json["up"] = frustumAxisJson(basis.up);
    camera_json["right"] = frustumAxisJson(basis.right);
    camera_json["near"] = basis.near_plane;
    camera_json["far"] = basis.far_plane;
    camera_json["aspect"] = basis.aspect;
    camera_json["aspect_source"] = basis.aspect_source;
    camera_json["projection"] = basis.orthogonal ? "orthogonal" : "perspective";
    camera_json["fov_degrees"] = basis.orthogonal ? json(nullptr) : json(basis.fov_degrees);
    camera_json["orthogonal_size"] = basis.orthogonal ? json(basis.ortho_size) : json(nullptr);

    FrustumScan scan;
    scan.basis = &basis;
    scan.root = root.value();
    scan.editor = editor;
    scan.keep = request.max_results;
    auto walked = scanFrustumNode(root.value(), scan);
    if (walked.isErr()) return errorJson(walked.error().code, walked.error().message);

    std::sort_heap(scan.candidates.begin(), scan.candidates.end(), frustumFurther);
    const size_t reported = scan.candidates.size();

    // Sightline sampling opens the space state once for the whole answer, for
    // the same reason a ray batch does.
    std::optional<RaycastSpace> space;
    int64_t rays_cast = 0;
    bool sightline_truncated = false;
    if (request.sightline && reported > 0) {
        auto opened = openRaycastSpace(3);
        if (opened.isErr()) return errorJson(opened.error().code, opened.error().message);
        space = opened.value();
    }

    json nodes = json::array();
    for (size_t index = 0; index < reported; ++index) {
        auto& candidate = scan.candidates[index];
        json entry = {{"path", candidate.path},
                      {"class", candidate.class_name},
                      {"containment", frustumContainmentName(candidate.containment)},
                      {"tested", candidate.bounded ? "bounds" : "origin"},
                      {"visible_in_tree", candidate.visible_in_tree},
                      {"distance", candidate.distance}};
        if (space.has_value()) {
            if (rays_cast + static_cast<int64_t>(candidate.samples.size()) >
                runtime::kMaxSightlineRays) {
                // The field is left off rather than filled in, because an
                // unmeasured sightline reported as clear is a claim nobody
                // made.
                sightline_truncated = true;
                nodes.push_back(std::move(entry));
                continue;
            }
            int64_t clear = 0;
            for (const auto& sample : candidate.samples) {
                runtime::RaycastRequest ray;
                ray.from = runtime::SpatialPoint{3, basis.origin[0], basis.origin[1], basis.origin[2]};
                ray.to = runtime::SpatialPoint{3, sample[0], sample[1], sample[2]};
                ray.collision_mask = request.collision_mask;
                const double span[3] = {sample[0] - basis.origin[0], sample[1] - basis.origin[1],
                                        sample[2] - basis.origin[2]};
                const double reach = std::sqrt(frustumDot(span, span));
                if (reach < 1e-6) {
                    // The camera stands on the sample, so nothing can be
                    // between them.
                    ++clear;
                    continue;
                }
                auto cast = castOneRay(space.value(), ray);
                if (cast.isErr()) return errorJson(cast.error().code, cast.error().message);
                ++rays_cast;
                const json& hit = cast.value();
                if (!hit["hit"].get<bool>()) {
                    ++clear;
                    continue;
                }
                const std::string collider = hit["collider_path"].is_string()
                    ? hit["collider_path"].get<std::string>() : std::string();
                if (frustumHitIsSelf(candidate.path, collider)) {
                    ++clear;
                    continue;
                }
                const double hit_point[3] = {hit["position"]["x"].get<double>(),
                                             hit["position"]["y"].get<double>(),
                                             hit["position"]["z"].get<double>()};
                const double gap[3] = {hit_point[0] - basis.origin[0], hit_point[1] - basis.origin[1],
                                       hit_point[2] - basis.origin[2]};
                const double hit_distance = std::sqrt(frustumDot(gap, gap));
                // An obstruction at or past the sample point is not between the
                // camera and the node.
                if (hit_distance >= reach - (0.001 + reach * 0.001)) ++clear;
            }
            const int64_t samples = static_cast<int64_t>(candidate.samples.size());
            entry["sightline"] = {
                {"samples", samples},
                {"samples_clear", clear},
                {"status", clear == samples ? "clear" : (clear == 0 ? "blocked" : "partial")}};
        }
        nodes.push_back(std::move(entry));
    }

    json result = {{"camera", std::move(camera_json)},
                   {"nodes", std::move(nodes)},
                   {"node_count", scan.inside_count},
                   {"examined", scan.examined},
                   {"truncated", static_cast<int64_t>(reported) < scan.inside_count ||
                                 scan.examine_limit_hit},
                   {"scan_limit_reached", scan.examine_limit_hit}};
    if (request.sightline) {
        result["sightline_rays"] = rays_cast;
        result["sightline_truncated"] = sightline_truncated;
    }
    return liveResult(result);
}

json physicsRaycastBatch(const json& params) {
    auto parsed = runtime::parseRaycastBatchRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    const auto& request = parsed.value();
    auto space = openRaycastSpace(request.dimension());
    if (space.isErr()) return errorJson(space.error().code, space.error().message);

    json results = json::array();
    size_t hits = 0;
    for (size_t index = 0; index < request.rays.size(); ++index) {
        auto cast = castOneRay(space.value(), request.rays[index]);
        // One ray that cannot be answered fails the batch. A partial batch that
        // looked complete would be read as fifty clear sightlines when it was
        // forty-nine and a silence.
        if (cast.isErr()) {
            return errorJson(cast.error().code,
                             "rays[" + std::to_string(index) + "]: " + cast.error().message);
        }
        auto record = cast.value();
        if (record.value("hit", false)) ++hits;
        record["index"] = index;
        results.push_back(std::move(record));
    }
    return liveResult({{"dimension", request.dimension()},
                       {"requested_rays", request.rays.size()},
                       {"hit_count", hits},
                       {"results", std::move(results)}});
}

json physicsRaycast(const json& params) {
    auto parsed = runtime::parseRaycastRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    const auto& request = parsed.value();
    const int dimension = request.dimension();
    const char* params_class = dimension == 2 ? "PhysicsRayQueryParameters2D" : "PhysicsRayQueryParameters3D";
    const char* state_class = dimension == 2 ? "PhysicsDirectSpaceState2D" : "PhysicsDirectSpaceState3D";
    const int64_t create_hash = dimension == 2 ? 3196569324LL : 3110599579LL;
    const int64_t state_hash = dimension == 2 ? 2506717822LL : 2069328350LL;
    const int64_t ray_hash = dimension == 2 ? 1590275562LL : 3957970750LL;

    for (const auto& bind : {std::make_tuple(params_class, "create", create_hash),
                             std::make_tuple(state_class, "intersect_ray", ray_hash),
                             std::make_tuple(dimension == 2 ? "World2D" : "World3D", "get_direct_space_state", state_hash)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    auto world = rootWorld(dimension);
    if (world.isErr()) return errorJson(world.error().code, world.error().message);
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr()) return errorJson(500, world_object.error().message);
    auto state = callObject(world_object.value(), dimension == 2 ? "World2D" : "World3D",
                            "get_direct_space_state", state_hash);
    if (state.isErr()) return errorJson(500, state.error().message);
    auto state_object = objectFromVariant(state.value());
    if (state_object.isErr() || !state_object.value()) {
        return errorJson(409, "Root viewport world has no direct space state");
    }

    auto from = makePoint(request.from);
    auto to = makePoint(request.to);
    auto mask = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.collision_mask);
    if (from.isErr() || to.isErr() || mask.isErr()) return errorJson(500, "Failed to construct ray arguments");
    // Static: the bind is called with a null instance and the trailing exclude
    // list takes its default.
    Result<VariantValue> query = [&]() -> Result<VariantValue> {
        auto& api = GodotApi::instance();
        NativeName klass(params_class);
        NativeName method("create");
        if (!klass.valid() || !method.valid()) return Error::internal("Failed to construct ray query identifiers");
        auto bind = api.classdb_get_method_bind(klass.ptr(), method.ptr(), create_hash);
        if (!bind) return Error(501, "Godot method binding unavailable for ray query creation");
        const void* raw_args[] = {from.value().ptr(), to.value().ptr(), mask.value().ptr()};
        VariantValue created(VariantValue::Uninitialized{});
        GDExtensionCallError error{};
        api.object_method_bind_call(bind, nullptr, raw_args, 3, created.ptr(), &error);
        created.markInitialized();
        if (error.error != GDEXTENSION_CALL_OK) {
            return Error::internal("PhysicsRayQueryParameters.create failed (call error " +
                                   std::to_string(error.error) + ")");
        }
        return std::move(created);
    }();
    if (query.isErr()) return errorJson(query.error().code, query.error().message);
    auto query_object = objectFromVariant(query.value());
    if (query_object.isErr() || !query_object.value()) return errorJson(500, "Ray query parameters were not created");
    // Fixed flags from the contract: bodies and areas on, hit-from-inside off,
    // back faces on in 3D.
    auto set_flag = [&](const char* method, bool enabled) -> Result<void> {
        auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(enabled));
        if (value.isErr()) return value.error();
        auto result = callObject(query_object.value(), params_class, method, 2586408642LL, {&value.value()});
        return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
    };
    for (const auto& flag : {std::make_pair("set_collide_with_bodies", true),
                             std::make_pair("set_collide_with_areas", true),
                             std::make_pair("set_hit_from_inside", false)}) {
        auto set = set_flag(flag.first, flag.second);
        if (set.isErr()) return errorJson(500, set.error().message);
    }
    if (dimension == 3) {
        auto set = set_flag("set_hit_back_faces", true);
        if (set.isErr()) return errorJson(500, set.error().message);
    }

    auto hit = callObject(state_object.value(), state_class, "intersect_ray", ray_hash, {&query.value()});
    if (hit.isErr()) return errorJson(500, hit.error().message);
    json result = {{"dimension", dimension}, {"hit", false}, {"collider_path", nullptr},
                   {"collider_class", nullptr}, {"position", nullptr}, {"normal", nullptr},
                   {"collision_layer", nullptr}};
    auto size = callVariant(hit.value(), "size");
    if (size.isErr()) return errorJson(500, size.error().message);
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr() || count.value() == 0) return liveResult(result);

    auto lookup = [&](const char* key) -> Result<VariantValue> {
        auto key_value = makeString(key);
        if (key_value.isErr()) return key_value.error();
        return callVariant(hit.value(), "get", {&key_value.value()});
    };
    auto position = lookup("position");
    auto normal = lookup("normal");
    auto collider = lookup("collider");
    if (position.isErr() || normal.isErr() || collider.isErr()) return errorJson(500, "Ray hit dictionary is incomplete");
    auto position_json = pointVariantToJson(position.value(), dimension);
    auto normal_json = pointVariantToJson(normal.value(), dimension);
    if (position_json.isErr() || normal_json.isErr()) return errorJson(500, "Ray hit vectors could not be read");
    result["hit"] = true;
    result["position"] = position_json.value();
    result["normal"] = normal_json.value();
    auto collider_object = objectFromVariant(collider.value());
    if (collider_object.isOk() && collider_object.value()) {
        auto class_name = nodeString(collider_object.value(), "get_class", 201670096LL);
        if (class_name.isOk()) {
            auto bounded = boundUtf8(class_name.value(), 256);
            result["collider_class"] = bounded.value;
        }
        auto is_node_name = makeString("Node");
        if (is_node_name.isOk()) {
            auto is_node = callObject(collider_object.value(), "Object", "is_class", 3927539163LL, {&is_node_name.value()});
            if (is_node.isOk()) {
                auto node_flag = scalarFromVariant<GDExtensionBool>(is_node.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
                if (node_flag.isOk() && node_flag.value()) {
                    auto path = nodeString(collider_object.value(), "get_path", 4075236667LL);
                    if (path.isOk() && !path.value().empty() && path.value().size() <= 1024) {
                        result["collider_path"] = path.value();
                    }
                }
            }
        }
        auto layer = callObject(collider_object.value(),
                                dimension == 2 ? "CollisionObject2D" : "CollisionObject3D",
                                "get_collision_layer", 3905245786LL);
        if (layer.isOk()) {
            auto layer_value = scalarFromVariant<int64_t>(layer.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (layer_value.isOk()) result["collision_layer"] = layer_value.value();
        }
    }
    return liveResult(result);
}

json navQueryPath(const json& params) {
    auto parsed = runtime::parseNavPathRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    const auto& request = parsed.value();
    const int dimension = request.dimension();
    const char* server_class = dimension == 2 ? "NavigationServer2D" : "NavigationServer3D";
    const int64_t path_hash = dimension == 2 ? 1279824844LL : 276783190LL;
    auto required = requireMethodBind(server_class, "map_get_path", path_hash);
    if (required.isErr()) return errorJson(501, required.error().message);
    auto map_bind = requireMethodBind(dimension == 2 ? "World2D" : "World3D", "get_navigation_map", 2944877500LL);
    if (map_bind.isErr()) return errorJson(501, map_bind.error().message);

    auto world = rootWorld(dimension);
    if (world.isErr()) return errorJson(world.error().code, world.error().message);
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr()) return errorJson(500, world_object.error().message);
    auto map = callObject(world_object.value(), dimension == 2 ? "World2D" : "World3D",
                          "get_navigation_map", 2944877500LL);
    if (map.isErr()) return errorJson(500, map.error().message);
    if (GodotApi::instance().variant_get_type(map.value().ptr()) != GDEXTENSION_VARIANT_TYPE_RID) {
        return errorJson(409, "Root viewport world has no navigation map");
    }
    auto server = singleton(server_class);
    if (server.isErr()) return errorJson(501, server.error().message);

    auto start = makePoint(request.start_point);
    auto end = makePoint(request.end_point);
    auto optimize = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(request.optimize));
    auto layers = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, request.navigation_layers);
    if (start.isErr() || end.isErr() || optimize.isErr() || layers.isErr()) {
        return errorJson(500, "Failed to construct navigation arguments");
    }
    auto path = callObject(server.value(), server_class, "map_get_path", path_hash,
                           {&map.value(), &start.value(), &end.value(), &optimize.value(), &layers.value()});
    if (path.isErr()) return errorJson(500, path.error().message);
    auto size = callVariant(path.value(), "size");
    if (size.isErr()) return errorJson(500, size.error().message);
    auto count = scalarFromVariant<int64_t>(size.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (count.isErr()) return errorJson(500, count.error().message);

    constexpr int64_t kMaxPoints = 256;
    constexpr size_t kMaxBytes = 256u * 1024u;
    json points = json::array();
    bool truncated = false;
    for (int64_t index = 0; index < count.value(); ++index) {
        if (index >= kMaxPoints) { truncated = true; break; }
        auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (index_value.isErr()) return errorJson(500, index_value.error().message);
        auto point = callVariant(path.value(), "get", {&index_value.value()});
        if (point.isErr()) return errorJson(500, point.error().message);
        auto converted = pointVariantToJson(point.value(), dimension);
        if (converted.isErr()) return errorJson(500, converted.error().message);
        points.push_back(converted.value());
        if (points.dump().size() > kMaxBytes) {
            points.erase(points.size() - 1);
            truncated = true;
            break;
        }
    }
    return liveResult({{"dimension", dimension}, {"reachable", count.value() > 0},
                       {"points", std::move(points)}, {"truncated", truncated},
                       {"navigation_layers", request.navigation_layers},
                       {"optimize", request.optimize}});
}

} // namespace


// Phase 7B animation. The list reads an AnimationPlayer's library through the
// pinned AnimationMixer and Animation binds and never touches a key. The play
// is game-only, calls AnimationPlayer.play once, and rereads state through
// Object.get because get_current_animation changes hash between 4.5 and 4.7.
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

json animListTracks(const json& params, const std::string& session_kind) {
    auto parsed = runtime::parseAnimListRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
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
    if (player.isErr()) return errorJson(player.error().code, player.error().message);

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
    if (session_kind != "game") return errorJson(409, "session_kind_rejected");
    auto parsed = runtime::parseAnimPlayRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    for (const auto& bind : {std::make_tuple("AnimationMixer", "has_animation", 2619796661LL),
                             std::make_tuple("AnimationPlayer", "play", 3118260607LL),
                             std::make_tuple("AnimationPlayer", "is_playing", 36873697LL),
                             std::make_tuple("Object", "get", 2760726917LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }
    const auto& request = parsed.value();
    auto player = resolveAnimationPlayer(request.animation_player_path, session_kind);
    if (player.isErr()) return errorJson(player.error().code, player.error().message);

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

} // namespace

namespace {

// Wireframe previews of a mutation nobody has made yet.
//
// These are handed to the rendering server directly rather than added to the
// scene, which is the whole point: the scene tree, the scene dock and the file
// on disk are untouched, so looking at a proposal cannot dirty the project. The
// cost is that they are not owned by anything the editor cleans up, so every
// RID made here is remembered and freed by hand.
struct GhostBatch {
    int dimension{3};
    size_t shape_count{0};
    // Instances first, then the meshes and materials they referred to.
    std::vector<VariantValue> rids;
};

std::map<std::string, GhostBatch>& ghostBatches() {
    static std::map<std::string, GhostBatch> batches;
    return batches;
}

size_t liveGhostShapes() {
    size_t total = 0;
    for (const auto& entry : ghostBatches()) total += entry.second.shape_count;
    return total;
}

std::string makeGhostId() {
    static uint64_t counter = 0;
    return "ghost_" + std::to_string(++counter);
}

Result<GDExtensionObjectPtr> renderingServer() {
    return singleton("RenderingServer");
}

Result<void> freeGhostRids(GhostBatch& batch) {
    auto server = renderingServer();
    if (server.isErr()) return server.error();
    Result<void> outcome = Result<void>::ok();
    for (auto& rid : batch.rids) {
        auto freed = callObject(server.value(), "RenderingServer", "free_rid", 2722037293LL, {&rid});
        if (freed.isErr() && outcome.isOk()) outcome = freed.error();
    }
    batch.rids.clear();
    batch.shape_count = 0;
    return outcome;
}

// A tinted, unshaded line material. depth_draw_never keeps a preview visible
// through the geometry it is being compared against, which is what makes it
// readable as an overlay rather than something buried in the scene.
constexpr const char* kGhostShaderSource = R"(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, depth_test_disabled;
uniform vec4 didi_tint : source_color = vec4(0.0, 1.0, 1.0, 1.0);
void fragment() {
	ALBEDO = didi_tint.rgb;
	ALPHA = didi_tint.a;
}
)";

Result<VariantValue> makeGhostMaterial(GDExtensionObjectPtr server,
                                       const runtime::GhostColor& color,
                                       std::vector<VariantValue>& owned) {
    auto shader = callObject(server, "RenderingServer", "shader_create", 529393457LL);
    if (shader.isErr()) return shader.error();
    auto code = makeString(kGhostShaderSource);
    if (code.isErr()) return code.error();
    auto coded = callObject(server, "RenderingServer", "shader_set_code", 2726140452LL,
                            {&shader.value(), &code.value()});
    if (coded.isErr()) return coded.error();
    auto material = callObject(server, "RenderingServer", "material_create", 529393457LL);
    if (material.isErr()) return material.error();
    auto attached = callObject(server, "RenderingServer", "material_set_shader", 395945892LL,
                               {&material.value(), &shader.value()});
    if (attached.isErr()) return attached.error();
    auto tint_name = makeStringName("didi_tint");
    if (tint_name.isErr()) return tint_name.error();
    auto tint = makeColor(color.red, color.green, color.blue, 1.0);
    if (tint.isErr()) return tint.error();
    auto tinted = callObject(server, "RenderingServer", "material_set_param", 3477296213LL,
                             {&material.value(), &tint_name.value(), &tint.value()});
    if (tinted.isErr()) return tinted.error();

    // The shader outlives this call as part of the material, so its RID is
    // handed back to be freed with the batch rather than dropped here.
    owned.push_back(std::move(shader.value()));
    return std::move(material.value());
}

// The twelve edges of a unit box centred on the origin, as line pairs. The
// instance transform scales and places it, so one vertex layout serves every
// preview.
Result<VariantValue> makeGhostBoxMesh(GDExtensionObjectPtr server) {
    auto& api = GodotApi::instance();
    auto vertex_constructor =
        api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY, 0);
    if (!vertex_constructor) {
        return Error::internal("Godot PackedVector3Array constructor is unavailable");
    }
    NativeValue native_vertices(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY);
    vertex_constructor(native_vertices.ptr(), nullptr);
    native_vertices.markInitialized();
    auto vertices = variantFromNative(GDEXTENSION_VARIANT_TYPE_PACKED_VECTOR3_ARRAY,
                                      native_vertices.ptr());
    if (vertices.isErr()) return vertices.error();

    const auto corner = [](int mask, int axis) {
        return (mask & (1 << axis)) ? 0.5 : -0.5;
    };
    for (int mask = 0; mask < 8; ++mask) {
        for (int axis = 0; axis < 3; ++axis) {
            const int other = mask ^ (1 << axis);
            if (other <= mask) continue;
            for (const int end : {mask, other}) {
                auto point = makeVector3(corner(end, 0), corner(end, 1), corner(end, 2));
                if (point.isErr()) return point.error();
                auto appended = callVariant(vertices.value(), "append", {&point.value()});
                if (appended.isErr()) return appended.error();
            }
        }
    }

    auto array_constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_ARRAY, 0);
    if (!array_constructor) return Error::internal("Godot Array constructor is unavailable");
    NativeValue native_arrays(GDEXTENSION_VARIANT_TYPE_ARRAY);
    array_constructor(native_arrays.ptr(), nullptr);
    native_arrays.markInitialized();
    auto arrays = variantFromNative(GDEXTENSION_VARIANT_TYPE_ARRAY, native_arrays.ptr());
    if (arrays.isErr()) return arrays.error();
    // RenderingServer.ARRAY_MAX is 13 and ARRAY_VERTEX is 0.
    auto array_max = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(13));
    if (array_max.isErr()) return array_max.error();
    auto resized = callVariant(arrays.value(), "resize", {&array_max.value()});
    if (resized.isErr()) return resized.error();
    auto vertex_slot = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (vertex_slot.isErr()) return vertex_slot.error();
    auto stored = callVariant(arrays.value(), "set", {&vertex_slot.value(), &vertices.value()});
    if (stored.isErr()) return stored.error();

    auto mesh = callObject(server, "RenderingServer", "mesh_create", 529393457LL);
    if (mesh.isErr()) return mesh.error();
    auto lines = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(1));
    if (lines.isErr()) return lines.error();
    NativeValue empty_blend(GDEXTENSION_VARIANT_TYPE_ARRAY);
    array_constructor(empty_blend.ptr(), nullptr);
    empty_blend.markInitialized();
    auto blend = variantFromNative(GDEXTENSION_VARIANT_TYPE_ARRAY, empty_blend.ptr());
    if (blend.isErr()) return blend.error();
    auto dictionary_constructor =
        api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY, 0);
    if (!dictionary_constructor) return Error::internal("Godot Dictionary constructor is unavailable");
    NativeValue native_lods(GDEXTENSION_VARIANT_TYPE_DICTIONARY);
    dictionary_constructor(native_lods.ptr(), nullptr);
    native_lods.markInitialized();
    auto lods = variantFromNative(GDEXTENSION_VARIANT_TYPE_DICTIONARY, native_lods.ptr());
    if (lods.isErr()) return lods.error();
    auto format = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
    if (format.isErr()) return format.error();
    auto surfaced = callObject(server, "RenderingServer", "mesh_add_surface_from_arrays",
                               2342446560LL,
                               {&mesh.value(), &lines.value(), &arrays.value(), &blend.value(),
                                &lods.value(), &format.value()});
    if (surfaced.isErr()) return surfaced.error();
    return std::move(mesh.value());
}

// A basis with the requested rotation applied, in Godot's YXZ order, scaled to
// the requested size.
Result<VariantValue> makeGhostTransform(const runtime::GhostShape& shape) {
    auto& api = GodotApi::instance();
    auto constructor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, 3);
    if (!constructor) return Error::internal("Godot Transform3D constructor is unavailable");
    auto to_native = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_VECTOR3);
    if (!to_native) return Error::internal("Godot Vector3 conversion is unavailable");

    constexpr double kPi = 3.14159265358979323846;
    const double x = shape.rotation_degrees[0] * kPi / 180.0;
    const double y = shape.rotation_degrees[1] * kPi / 180.0;
    const double z = shape.rotation_degrees[2] * kPi / 180.0;
    const double cx = std::cos(x), sx = std::sin(x);
    const double cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    // Godot composes Euler angles as Y then X then Z.
    const double basis[3][3] = {
        {cy * cz + sy * sx * sz, cz * sy * sx - cy * sz, cx * sy},
        {cx * sz, cx * cz, -sx},
        {cy * sx * sz - sy * cz, sy * sz + cy * sx * cz, cy * cx}};

    const double scale[3] = {shape.size.x, shape.size.y, shape.size.z};
    NativeValue axes[4] = {NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3),
                           NativeValue(GDEXTENSION_VARIANT_TYPE_VECTOR3)};
    for (int column = 0; column < 3; ++column) {
        auto vector = makeVector3(basis[0][column] * scale[column],
                                  basis[1][column] * scale[column],
                                  basis[2][column] * scale[column]);
        if (vector.isErr()) return vector.error();
        to_native(axes[column].ptr(), vector.value().ptr());
        axes[column].markInitialized();
    }
    auto origin = makeVector3(shape.position.x, shape.position.y, shape.position.z);
    if (origin.isErr()) return origin.error();
    to_native(axes[3].ptr(), origin.value().ptr());
    axes[3].markInitialized();

    NativeValue native(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D);
    const void* arguments[] = {axes[0].ptr(), axes[1].ptr(), axes[2].ptr(), axes[3].ptr()};
    constructor(native.ptr(), arguments);
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_TRANSFORM3D, native.ptr());
}

Result<VariantValue> editedSceneScenario() {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    auto is_spatial = objectIsClass(root.value(), "Node3D");
    if (is_spatial.isErr()) return is_spatial.error();
    if (!is_spatial.value()) {
        return Error(409, "The edited scene has no 3D world to draw a preview in; its root is not a Node3D");
    }
    auto world = callObject(root.value(), "Node3D", "get_world_3d", 317588385LL);
    if (world.isErr()) return world.error();
    auto world_object = objectFromVariant(world.value());
    if (world_object.isErr() || !world_object.value()) {
        return Error(409, "The edited scene is not in a viewport with a 3D world");
    }
    return callObject(world_object.value(), "World3D", "get_scenario", 2944877500LL);
}

Result<VariantValue> editedSceneCanvas() {
    auto editor = editorInterface();
    if (editor.isErr()) return editor.error();
    auto root = editedSceneRoot(editor.value());
    if (root.isErr()) return root.error();
    auto is_canvas = objectIsClass(root.value(), "CanvasItem");
    if (is_canvas.isErr()) return is_canvas.error();
    if (!is_canvas.value()) {
        return Error(409, "The edited scene has no 2D canvas to draw a preview in; its root is not a CanvasItem");
    }
    return callObject(root.value(), "CanvasItem", "get_canvas", 2944877500LL);
}

json ghostPreviewRender(const json& params) {
    auto parsed = runtime::parseGhostPreviewRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    const auto& request = parsed.value();

    for (const auto& bind : {std::make_tuple("RenderingServer", "free_rid", 2722037293LL),
                             std::make_tuple("RenderingServer", "shader_create", 529393457LL),
                             std::make_tuple("RenderingServer", "shader_set_code", 2726140452LL),
                             std::make_tuple("RenderingServer", "material_create", 529393457LL),
                             std::make_tuple("RenderingServer", "material_set_shader", 395945892LL),
                             std::make_tuple("RenderingServer", "material_set_param", 3477296213LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }
    const bool flat = request.dimension() == 2;
    for (const auto& bind : flat
             ? std::vector<std::tuple<const char*, const char*, int64_t>>{
                   {"RenderingServer", "canvas_item_create", 529393457LL},
                   {"RenderingServer", "canvas_item_set_parent", 395945892LL},
                   {"RenderingServer", "canvas_item_add_line", 1819681853LL},
                   {"RenderingServer", "canvas_item_set_z_index", 3411492887LL}}
             : std::vector<std::tuple<const char*, const char*, int64_t>>{
                   {"RenderingServer", "mesh_create", 529393457LL},
                   {"RenderingServer", "mesh_add_surface_from_arrays", 2342446560LL},
                   {"RenderingServer", "instance_create2", 746547085LL},
                   {"RenderingServer", "instance_set_transform", 3935195649LL},
                   {"RenderingServer", "instance_geometry_set_material_override", 395945892LL}}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return errorJson(501, required.error().message);
    }

    auto server = renderingServer();
    if (server.isErr()) return errorJson(server.error().code, server.error().message);

    size_t cleared_shapes = 0;
    if (request.replace) {
        for (auto& entry : ghostBatches()) {
            cleared_shapes += entry.second.shape_count;
            (void)freeGhostRids(entry.second);
        }
        ghostBatches().clear();
    }
    if (liveGhostShapes() + request.shapes.size() > runtime::kMaxLiveGhostShapes) {
        return errorJson(409, "This would leave more than " +
                                  std::to_string(runtime::kMaxLiveGhostShapes) +
                                  " preview shapes on screen; clear some first");
    }

    GhostBatch batch;
    batch.dimension = request.dimension();
    // Anything already made is freed if a later shape fails, so a refused call
    // does not leave half a proposal drawn over the scene.
    const auto abandon = [&](const Error& error) {
        (void)freeGhostRids(batch);
        return errorJson(error.code, error.message);
    };

    if (flat) {
        auto canvas = editedSceneCanvas();
        if (canvas.isErr()) return abandon(canvas.error());
        for (const auto& shape : request.shapes) {
            auto item = callObject(server.value(), "RenderingServer", "canvas_item_create",
                                   529393457LL);
            if (item.isErr()) return abandon(item.error());
            auto parented = callObject(server.value(), "RenderingServer", "canvas_item_set_parent",
                                       395945892LL, {&item.value(), &canvas.value()});
            if (parented.isErr()) return abandon(parented.error());
            auto z_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(4096));
            if (z_index.isErr()) return abandon(z_index.error());
            auto layered = callObject(server.value(), "RenderingServer", "canvas_item_set_z_index",
                                      3411492887LL, {&item.value(), &z_index.value()});
            if (layered.isErr()) return abandon(layered.error());

            const double half_x = shape.size.x / 2.0;
            const double half_y = shape.size.y / 2.0;
            const double corners[4][2] = {{shape.position.x - half_x, shape.position.y - half_y},
                                          {shape.position.x + half_x, shape.position.y - half_y},
                                          {shape.position.x + half_x, shape.position.y + half_y},
                                          {shape.position.x - half_x, shape.position.y + half_y}};
            auto tint = makeColor(shape.color.red, shape.color.green, shape.color.blue, 1.0);
            if (tint.isErr()) return abandon(tint.error());
            for (int edge = 0; edge < 4; ++edge) {
                const int next = (edge + 1) % 4;
                auto from = makeVector2(corners[edge][0], corners[edge][1]);
                auto to = makeVector2(corners[next][0], corners[next][1]);
                if (from.isErr()) return abandon(from.error());
                if (to.isErr()) return abandon(to.error());
                auto width = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, 1.0);
                auto antialiased = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                              static_cast<GDExtensionBool>(0));
                if (width.isErr()) return abandon(width.error());
                if (antialiased.isErr()) return abandon(antialiased.error());
                auto drawn = callObject(server.value(), "RenderingServer", "canvas_item_add_line",
                                        1819681853LL,
                                        {&item.value(), &from.value(), &to.value(), &tint.value(),
                                         &width.value(), &antialiased.value()});
                if (drawn.isErr()) return abandon(drawn.error());
            }
            batch.rids.push_back(std::move(item.value()));
            ++batch.shape_count;
        }
    } else {
        auto scenario = editedSceneScenario();
        if (scenario.isErr()) return abandon(scenario.error());
        for (const auto& shape : request.shapes) {
            auto mesh = makeGhostBoxMesh(server.value());
            if (mesh.isErr()) return abandon(mesh.error());
            std::vector<VariantValue> owned;
            auto material = makeGhostMaterial(server.value(), shape.color, owned);
            if (material.isErr()) return abandon(material.error());
            auto instance = callObject(server.value(), "RenderingServer", "instance_create2",
                                       746547085LL, {&mesh.value(), &scenario.value()});
            if (instance.isErr()) return abandon(instance.error());
            auto overridden = callObject(server.value(), "RenderingServer",
                                         "instance_geometry_set_material_override", 395945892LL,
                                         {&instance.value(), &material.value()});
            if (overridden.isErr()) return abandon(overridden.error());
            auto transform = makeGhostTransform(shape);
            if (transform.isErr()) return abandon(transform.error());
            auto placed = callObject(server.value(), "RenderingServer", "instance_set_transform",
                                     3935195649LL, {&instance.value(), &transform.value()});
            if (placed.isErr()) return abandon(placed.error());

            // Instance first: freeing a mesh an instance still points at is the
            // order that leaves a dangling reference.
            batch.rids.push_back(std::move(instance.value()));
            batch.rids.push_back(std::move(mesh.value()));
            batch.rids.push_back(std::move(material.value()));
            for (auto& extra : owned) batch.rids.push_back(std::move(extra));
            ++batch.shape_count;
        }
    }

    const std::string id = makeGhostId();
    json drawn = json::array();
    for (size_t index = 0; index < request.shapes.size(); ++index) {
        const auto& shape = request.shapes[index];
        json entry = {{"index", static_cast<int64_t>(index)},
                      {"kind", runtime::nameForGhostKind(shape.kind)},
                      {"color", shape.color.toJson()},
                      {"color_defaulted", !shape.color_given}};
        if (!shape.label.empty()) entry["label"] = boundUtf8(shape.label, 256).value;
        drawn.push_back(std::move(entry));
    }
    const size_t shapes = batch.shape_count;
    ghostBatches().emplace(id, std::move(batch));
    // The viewport redraws when something it owns changes, and these shapes are
    // deliberately not among them. Without this the proposal only appears on
    // whatever frame the editor drew next for its own reasons.
    (void)GodotBridge::instance().forceDraw();

    return liveResult({{"preview_id", id},
                       {"dimension", request.dimension()},
                       {"drawn", static_cast<int64_t>(shapes)},
                       {"previews", std::move(drawn)},
                       {"replaced_shapes", static_cast<int64_t>(cleared_shapes)},
                       {"live_shapes", static_cast<int64_t>(liveGhostShapes())},
                       // Nothing here touched the scene, so there is nothing to
                       // undo and nothing to save.
                       {"scene_modified", false}});
}

json ghostPreviewClear(const json& params) {
    auto parsed = runtime::parseGhostClearRequest(params);
    if (parsed.isErr()) return errorJson(parsed.error().code, parsed.error().message);
    auto required = requireMethodBind("RenderingServer", "free_rid", 2722037293LL);
    if (required.isErr()) return errorJson(501, required.error().message);

    size_t batches = 0;
    size_t shapes = 0;
    Result<void> outcome = Result<void>::ok();
    if (parsed.value().preview_id.empty()) {
        for (auto& entry : ghostBatches()) {
            ++batches;
            shapes += entry.second.shape_count;
            auto freed = freeGhostRids(entry.second);
            if (freed.isErr() && outcome.isOk()) outcome = freed.error();
        }
        ghostBatches().clear();
    } else {
        auto found = ghostBatches().find(parsed.value().preview_id);
        if (found == ghostBatches().end()) {
            return errorJson(404, "No preview with id " + parsed.value().preview_id);
        }
        batches = 1;
        shapes = found->second.shape_count;
        outcome = freeGhostRids(found->second);
        ghostBatches().erase(found);
    }
    if (outcome.isErr()) return errorJson(outcome.error().code, outcome.error().message);
    // Same reason as drawing them: freeing the shapes does not by itself put a
    // frame on screen without them.
    (void)GodotBridge::instance().forceDraw();
    return liveResult({{"cleared_previews", static_cast<int64_t>(batches)},
                       {"cleared_shapes", static_cast<int64_t>(shapes)},
                       {"live_shapes", static_cast<int64_t>(liveGhostShapes())},
                       {"scene_modified", false}});
}

} // namespace

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
    // A game has no EditorInterface, so this runs before the editor lookup
    // the editor-only methods below depend on.
    if (method == "runtime.injectInput") {
        return injectInput(params, session_kind);
    }
    if (method == "physics.raycast") return physicsRaycast(params);
    if (method == "physics.raycastBatch") return physicsRaycastBatch(params);
    if (method == "physics.clearance") return physicsClearance(params);
    if (method == "vision.frustumQuery") return visionFrustumQuery(params, session_kind);
    if (method == "preview.renderGhost") return ghostPreviewRender(params);
    if (method == "preview.clearGhosts") return ghostPreviewClear(params);
    if (method == "nav.queryPath") return navQueryPath(params);
    if (method == "anim.listTracks") return animListTracks(params, session_kind);
    if (method == "anim.playTrack") return animPlayTrack(params, session_kind);
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return errorJson(editor_result.error().code, editor_result.error().message);
    auto editor = editor_result.value();

    if (method == "tilemap.getUsedRect") {
        if (session_kind != "editor") return errorJson(409, "session_kind_rejected");
        if (!hasOnlyKeys(params, {"tilemap_path"}) || !params.contains("tilemap_path") ||
            !params["tilemap_path"].is_string() || params["tilemap_path"].get<std::string>().empty() ||
            params["tilemap_path"].get<std::string>().size() > 1024) {
            return errorJson(400, "invalid_tilemap_get_used_rect_request");
        }
        if (requireMethodBind("TileMapLayer", "get_used_rect", 410525958LL).isErr() ||
            requireMethodBind("Object", "is_class", 3927539163LL).isErr()) {
            return errorJson(501, "required_bind_unavailable");
        }
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto layer = resolveNode(root.value(), params["tilemap_path"].get<std::string>());
        if (layer.isErr()) return errorJson(404, "tilemap_target_not_found");
        auto correct_class = objectIsClass(layer.value(), "TileMapLayer");
        if (correct_class.isErr()) return errorJson(500, correct_class.error().message);
        if (!correct_class.value()) return errorJson(404, "tilemap_target_not_found");
        auto rect = callObject(layer.value(), "TileMapLayer", "get_used_rect", 410525958LL);
        if (rect.isErr()) return errorJson(500, rect.error().message);
        auto& api = GodotApi::instance();
        if (api.variant_get_type(rect.value().ptr()) != GDEXTENSION_VARIANT_TYPE_RECT2I ||
            !api.variant_get_ptr_getter) return errorJson(500, "extension_protocol_error");
        auto converter = api.get_variant_to_type_constructor(GDEXTENSION_VARIANT_TYPE_RECT2I);
        NativeName position_name("position"), size_name("size");
        auto get_position = position_name.valid()
                                ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_RECT2I, position_name.ptr())
                                : nullptr;
        auto get_size = size_name.valid()
                            ? api.variant_get_ptr_getter(GDEXTENSION_VARIANT_TYPE_RECT2I, size_name.ptr())
                            : nullptr;
        if (!converter || !get_position || !get_size) return errorJson(501, "required_bind_unavailable");
        NativeValue native_rect(GDEXTENSION_VARIANT_TYPE_RECT2I);
        converter(native_rect.ptr(), rect.value().ptr());
        native_rect.markInitialized();
        NativeValue native_position(GDEXTENSION_VARIANT_TYPE_VECTOR2I);
        NativeValue native_size(GDEXTENSION_VARIANT_TYPE_VECTOR2I);
        get_position(native_rect.ptr(), native_position.ptr());
        get_size(native_rect.ptr(), native_size.ptr());
        native_position.markInitialized();
        native_size.markInitialized();
        auto position_variant = variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR2I, native_position.ptr());
        auto size_variant = variantFromNative(GDEXTENSION_VARIANT_TYPE_VECTOR2I, native_size.ptr());
        if (position_variant.isErr() || size_variant.isErr()) return errorJson(500, "extension_protocol_error");
        auto position = integerVectorToJson(position_variant.value(), 2);
        auto size = integerVectorToJson(size_variant.value(), 2);
        if (position.isErr() || size.isErr()) return errorJson(500, "extension_protocol_error");
        return liveResult({{"tilemap_path", params["tilemap_path"]},
                           {"position", position.value()}, {"size", size.value()},
                           {"end", {{"x", position.value()["x"].get<int64_t>() + size.value()["x"].get<int64_t>()},
                                    {"y", position.value()["y"].get<int64_t>() + size.value()["y"].get<int64_t>()}}}});
    }

    if (method == "tilemap.setCells") {
        if (session_kind != "editor") return errorJson(409, "session_kind_rejected");
        if (!hasOnlyKeys(params, {"tilemap_path", "cells"}) ||
            !params.contains("tilemap_path") || !params["tilemap_path"].is_string() ||
            params["tilemap_path"].get<std::string>().empty() ||
            params["tilemap_path"].get<std::string>().size() > 1024 ||
            !params.contains("cells") || !params["cells"].is_array() ||
            params["cells"].empty() || params["cells"].size() > 256) {
            return errorJson(400, "invalid_tilemap_set_cells_request");
        }
        for (const auto& bind : {
                 std::make_tuple("Object", "is_class", 3927539163LL),
                 std::make_tuple("TileMapLayer", "get_tile_set", 2678226422LL),
                 std::make_tuple("TileMapLayer", "set_cell", 2428518503LL),
                 std::make_tuple("TileMapLayer", "erase_cell", 1130785943LL),
                 std::make_tuple("TileMapLayer", "get_cell_source_id", 2485466453LL),
                 std::make_tuple("TileMapLayer", "get_cell_atlas_coords", 3050897911LL),
                 std::make_tuple("TileMapLayer", "get_cell_alternative_tile", 2485466453LL),
                 std::make_tuple("TileSet", "has_source", 1116898809LL),
                 std::make_tuple("TileSet", "get_source", 1763540252LL),
                 std::make_tuple("TileSetAtlasSource", "get_tile_data", 3534028207LL)}) {
            if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr())
                return errorJson(501, "required_bind_unavailable");
        }
        if (preflightUndoManagerBindings().isErr() || preflightUndoRollbackBindings().isErr())
            return errorJson(501, "required_bind_unavailable");
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto layer = resolveNode(root.value(), params["tilemap_path"].get<std::string>());
        if (layer.isErr()) return errorJson(404, "tilemap_target_not_found");
        auto correct_class = objectIsClass(layer.value(), "TileMapLayer");
        if (correct_class.isErr() || !correct_class.value()) return errorJson(404, "tilemap_target_not_found");
        auto tile_set_variant = callObject(layer.value(), "TileMapLayer", "get_tile_set", 2678226422LL);
        if (tile_set_variant.isErr()) return errorJson(500, tile_set_variant.error().message);
        auto tile_set = objectFromVariant(tile_set_variant.value());
        if (tile_set.isErr()) return errorJson(500, tile_set.error().message);
        // A layer with no TileSet used to be reported as tilemap_tile_not_found
        // once the first cell was examined, which reads as a bad source id or
        // bad atlas coordinates and sends the caller looking at their payload.
        // The cause is one unset property on the node, and it is worth saying so
        // before any cell is looked at.
        if (!tile_set.value()) {
            return errorJson(409, "tilemap_layer_has_no_tileset",
                             {{"tilemap_path", params["tilemap_path"].get<std::string>()},
                              {"retryable", false}});
        }

        struct Cell {
            VariantValue coords;
            bool erase{false};
            int64_t source{-1};
            VariantValue atlas;
            int64_t alternative{-1};
            int64_t old_source{-1};
            VariantValue old_atlas;
            int64_t old_alternative{-1};
            bool changed{false};
        };
        std::vector<Cell> cells;
        std::set<std::pair<int64_t, int64_t>> seen;
        for (const auto& record : params["cells"]) {
            if (!record.is_object() || !record.contains("coords") || !record["coords"].is_array() ||
                record["coords"].size() != 2) return errorJson(400, "invalid_tilemap_set_cells_request");
            const auto x_value = boundedJsonInteger(record["coords"][0], -1048576, 1048576);
            const auto y_value = boundedJsonInteger(record["coords"][1], -1048576, 1048576);
            if (!x_value.has_value() || !y_value.has_value())
                return errorJson(400, "invalid_tilemap_set_cells_request");
            const int64_t x = *x_value;
            const int64_t y = *y_value;
            if (!seen.emplace(x, y).second) return errorJson(409, "duplicate_tilemap_coordinate");
            const bool erase = record.contains("erase");
            if ((erase && (!hasOnlyKeys(record, {"coords", "erase"}) || !record["erase"].is_boolean() || !record["erase"].get<bool>())) ||
                (!erase && (!hasOnlyKeys(record, {"coords", "source_id", "atlas_coords", "alternative_tile"}) ||
                            !record.contains("source_id") || !record["source_id"].is_number_integer() ||
                            !record.contains("atlas_coords") || !record["atlas_coords"].is_array() || record["atlas_coords"].size() != 2)))
                return errorJson(400, "invalid_tilemap_set_cells_request");
            auto coords = makeVector2i(x, y);
            auto empty_atlas = makeVector2i(-1, -1);
            if (coords.isErr() || empty_atlas.isErr()) return errorJson(501, "required_bind_unavailable");
            int64_t source = -1, alternative = -1;
            VariantValue atlas = std::move(empty_atlas.value());
            if (!erase) {
                const auto source_value_int = boundedJsonInteger(record["source_id"], 0, 2147483647LL);
                const auto alternative_value_int = record.contains("alternative_tile")
                    ? boundedJsonInteger(record["alternative_tile"], 0, 65535)
                    : std::optional<int64_t>(0);
                if (!source_value_int.has_value() || !alternative_value_int.has_value())
                    return errorJson(400, "invalid_tilemap_set_cells_request");
                source = *source_value_int;
                alternative = *alternative_value_int;
                const auto ax_value = boundedJsonInteger(record["atlas_coords"][0], 0, 1048576);
                const auto ay_value = boundedJsonInteger(record["atlas_coords"][1], 0, 1048576);
                if (!ax_value.has_value() || !ay_value.has_value())
                    return errorJson(400, "invalid_tilemap_set_cells_request");
                const int64_t ax = *ax_value;
                const int64_t ay = *ay_value;
                auto source_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, source);
                auto has_source = callObject(tile_set.value(), "TileSet", "has_source", 1116898809LL, {&source_value.value()});
                auto has_flag = has_source.isOk() ? scalarFromVariant<GDExtensionBool>(has_source.value(), GDEXTENSION_VARIANT_TYPE_BOOL)
                                                  : Result<GDExtensionBool>(has_source.error());
                if (has_flag.isErr() || !has_flag.value()) return errorJson(404, "tilemap_tile_not_found");
                auto source_object_value = callObject(tile_set.value(), "TileSet", "get_source", 1763540252LL, {&source_value.value()});
                auto source_object = source_object_value.isOk() ? objectFromVariant(source_object_value.value())
                                                                : Result<GDExtensionObjectPtr>(source_object_value.error());
                if (source_object.isErr() || !source_object.value()) return errorJson(404, "tilemap_tile_not_found");
                auto atlas_class = objectIsClass(source_object.value(), "TileSetAtlasSource");
                if (atlas_class.isErr() || !atlas_class.value()) return errorJson(404, "tilemap_tile_not_found");
                auto requested_atlas = makeVector2i(ax, ay);
                auto alternative_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, alternative);
                auto tile_data_value = callObject(source_object.value(), "TileSetAtlasSource", "get_tile_data", 3534028207LL,
                                                  {&requested_atlas.value(), &alternative_value.value()});
                auto tile_data = tile_data_value.isOk() ? objectFromVariant(tile_data_value.value())
                                                        : Result<GDExtensionObjectPtr>(tile_data_value.error());
                if (tile_data.isErr() || !tile_data.value()) return errorJson(404, "tilemap_tile_not_found");
                atlas = std::move(requested_atlas.value());
            }
            auto old_source_value = callObject(layer.value(), "TileMapLayer", "get_cell_source_id", 2485466453LL, {&coords.value()});
            auto old_atlas = callObject(layer.value(), "TileMapLayer", "get_cell_atlas_coords", 3050897911LL, {&coords.value()});
            auto old_alt_value = callObject(layer.value(), "TileMapLayer", "get_cell_alternative_tile", 2485466453LL, {&coords.value()});
            if (old_source_value.isErr() || old_atlas.isErr() || old_alt_value.isErr()) return errorJson(500, "tilemap_snapshot_failed");
            auto old_source = scalarFromVariant<int64_t>(old_source_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            auto old_alt = scalarFromVariant<int64_t>(old_alt_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            auto old_atlas_json = integerVectorToJson(old_atlas.value(), 2);
            auto atlas_json = integerVectorToJson(atlas, 2);
            if (old_source.isErr() || old_alt.isErr() || old_atlas_json.isErr() || atlas_json.isErr()) return errorJson(500, "tilemap_snapshot_failed");
            const bool changed = old_source.value() != source || old_alt.value() != alternative || old_atlas_json.value() != atlas_json.value();
            cells.push_back(Cell{std::move(coords.value()), erase, source, std::move(atlas), alternative,
                                 old_source.value(), std::move(old_atlas.value()), old_alt.value(), changed});
        }
        const size_t changed_count = std::count_if(cells.begin(), cells.end(), [](const Cell& cell) { return cell.changed; });
        if (changed_count == 0) return liveResult({{"requested_cells", cells.size()}, {"changed_cells", 0},
            {"unchanged_cells", cells.size()}, {"undo_redo_registered", false}, {"outcome", "completed"}, {"rollback", "not_required"}});
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto action = createAction(manager.value(), "Didi: set TileMapLayer cells", layer.value());
        if (action.isErr()) return errorJson(500, action.error().message);
        for (auto& cell : cells) if (cell.changed) {
            auto source_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.source);
            auto alt_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.alternative);
            auto old_source_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_source);
            auto old_alt_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_alternative);
            auto registered = cell.erase
                ? managerMethod(manager.value(), "add_do_method", layer.value(), "erase_cell", {&cell.coords})
                : managerMethod(manager.value(), "add_do_method", layer.value(), "set_cell", {&cell.coords, &source_value.value(), &cell.atlas, &alt_value.value()});
            if (registered.isOk()) registered = cell.old_source < 0
                ? managerMethod(manager.value(), "add_undo_method", layer.value(), "erase_cell", {&cell.coords})
                : managerMethod(manager.value(), "add_undo_method", layer.value(), "set_cell", {&cell.coords, &old_source_value.value(), &cell.old_atlas, &old_alt_value.value()});
            if (registered.isErr()) { abandonAction(manager.value()); return errorJson(500, "tilemap_undo_registration_failed"); }
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(500, committed.error().message);
        for (auto& cell : cells) if (cell.changed) {
            auto observed_source_value = callObject(layer.value(), "TileMapLayer", "get_cell_source_id", 2485466453LL, {&cell.coords});
            auto observed_atlas_value = callObject(layer.value(), "TileMapLayer", "get_cell_atlas_coords", 3050897911LL, {&cell.coords});
            auto observed_alt_value = callObject(layer.value(), "TileMapLayer", "get_cell_alternative_tile", 2485466453LL, {&cell.coords});
            auto observed_source = observed_source_value.isOk() ? scalarFromVariant<int64_t>(observed_source_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                                : Result<int64_t>(observed_source_value.error());
            auto observed_alt = observed_alt_value.isOk() ? scalarFromVariant<int64_t>(observed_alt_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                          : Result<int64_t>(observed_alt_value.error());
            auto observed_atlas = observed_atlas_value.isOk() ? integerVectorToJson(observed_atlas_value.value(), 2)
                                                              : Result<json>(observed_atlas_value.error());
            auto expected_atlas = integerVectorToJson(cell.atlas, 2);
            if (observed_source.isErr() || observed_alt.isErr() || observed_atlas.isErr() ||
                expected_atlas.isErr() || observed_source.value() != cell.source ||
                observed_alt.value() != cell.alternative || observed_atlas.value() != expected_atlas.value()) {
                const auto restored = undoLastAction(manager.value(), root.value());
                return errorJson(500, "tilemap_postcondition_mismatch", {{"outcome", restored.isOk() ? "rolled_back" : "unknown"}});
            }
        }
        return liveResult({{"requested_cells", cells.size()}, {"changed_cells", changed_count},
            {"unchanged_cells", cells.size() - changed_count}, {"undo_redo_registered", true}, {"outcome", "completed"}, {"rollback", "undo_redo"}});
    }

    if (method == "gridmap.setCells") {
        if (session_kind != "editor") return errorJson(409, "session_kind_rejected");
        if (!hasOnlyKeys(params, {"gridmap_path", "cells"}) || !params.contains("gridmap_path") ||
            !params["gridmap_path"].is_string() || params["gridmap_path"].get<std::string>().empty() ||
            params["gridmap_path"].get<std::string>().size() > 1024 ||
            !params.contains("cells") || !params["cells"].is_array() || params["cells"].empty() ||
            params["cells"].size() > 256) return errorJson(400, "invalid_gridmap_set_cells_request");
        for (const auto& bind : {std::make_tuple("Object", "is_class", 3927539163LL),
                 std::make_tuple("GridMap", "get_mesh_library", 3350993772LL),
                 std::make_tuple("GridMap", "set_cell_item", 3449088946LL),
                 std::make_tuple("GridMap", "get_cell_item", 3724960147LL),
                 std::make_tuple("GridMap", "get_cell_item_orientation", 3724960147LL),
                 std::make_tuple("MeshLibrary", "get_item_list", 1930428628LL)}) {
            if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) return errorJson(501, "required_bind_unavailable");
        }
        if (preflightUndoManagerBindings().isErr() || preflightUndoRollbackBindings().isErr())
            return errorJson(501, "required_bind_unavailable");
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto grid = resolveNode(root.value(), params["gridmap_path"].get<std::string>());
        if (grid.isErr()) return errorJson(404, "gridmap_target_not_found");
        auto correct_class = objectIsClass(grid.value(), "GridMap");
        if (correct_class.isErr() || !correct_class.value()) return errorJson(404, "gridmap_target_not_found");
        auto library_value = callObject(grid.value(), "GridMap", "get_mesh_library", 3350993772LL);
        auto library = library_value.isOk() ? objectFromVariant(library_value.value())
                                            : Result<GDExtensionObjectPtr>(library_value.error());
        std::set<int64_t> item_ids;
        if (library.isOk() && library.value()) {
            auto list = callObject(library.value(), "MeshLibrary", "get_item_list", 1930428628LL);
            if (list.isErr()) return errorJson(500, list.error().message);
            auto ids = packedIntegerSet(list.value());
            if (ids.isErr()) return errorJson(500, ids.error().message);
            item_ids = std::move(ids.value());
        }
        struct Cell { VariantValue position; int64_t item; int64_t orientation; int64_t old_item; int64_t old_orientation; bool changed; };
        std::vector<Cell> cells;
        std::set<std::tuple<int64_t, int64_t, int64_t>> seen;
        for (const auto& record : params["cells"]) {
            if (!hasOnlyKeys(record, {"position", "item", "orientation"}) || !record.contains("position") ||
                !record["position"].is_array() || record["position"].size() != 3 || !record.contains("item") ||
                !record["item"].is_number_integer()) return errorJson(400, "invalid_gridmap_set_cells_request");
            int64_t xyz[3];
            for (int axis = 0; axis < 3; ++axis) {
                const auto coordinate = boundedJsonInteger(record["position"][axis], -1048576, 1048576);
                if (!coordinate.has_value()) return errorJson(400, "invalid_gridmap_set_cells_request");
                xyz[axis] = *coordinate;
            }
            if (!seen.emplace(xyz[0], xyz[1], xyz[2]).second) return errorJson(409, "duplicate_gridmap_position");
            const auto item_value = boundedJsonInteger(record["item"], -1, 2147483647LL);
            const auto orientation_value = record.contains("orientation")
                ? boundedJsonInteger(record["orientation"], 0, 23)
                : std::optional<int64_t>(0);
            if (!item_value.has_value() || !orientation_value.has_value())
                return errorJson(400, "invalid_gridmap_set_cells_request");
            const int64_t item = *item_value;
            const int64_t orientation = *orientation_value;
            if (item == -1 && orientation != 0) return errorJson(400, "invalid_gridmap_set_cells_request");
            if (item >= 0 && (!library.isOk() || !library.value() || !item_ids.count(item))) return errorJson(404, "gridmap_item_not_found");
            auto position = makeVector3i(xyz[0], xyz[1], xyz[2]);
            if (position.isErr()) return errorJson(501, "required_bind_unavailable");
            auto old_item_value = callObject(grid.value(), "GridMap", "get_cell_item", 3724960147LL, {&position.value()});
            auto old_orientation_value = callObject(grid.value(), "GridMap", "get_cell_item_orientation", 3724960147LL, {&position.value()});
            if (old_item_value.isErr() || old_orientation_value.isErr()) return errorJson(500, "gridmap_snapshot_failed");
            auto old_item = scalarFromVariant<int64_t>(old_item_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            auto old_orientation = scalarFromVariant<int64_t>(old_orientation_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (old_item.isErr() || old_orientation.isErr()) return errorJson(500, "gridmap_snapshot_failed");
            const bool changed = item != old_item.value() || (item >= 0 && orientation != old_orientation.value());
            cells.push_back(Cell{std::move(position.value()), item, orientation, old_item.value(), old_orientation.value(), changed});
        }
        const size_t changed_count = std::count_if(cells.begin(), cells.end(), [](const Cell& cell) { return cell.changed; });
        if (changed_count == 0) return liveResult({{"requested_cells", cells.size()}, {"changed_cells", 0},
            {"unchanged_cells", cells.size()}, {"undo_redo_registered", false}, {"outcome", "completed"}, {"rollback", "not_required"}});
        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto action = createAction(manager.value(), "Didi: set GridMap cells", grid.value());
        if (action.isErr()) return errorJson(500, action.error().message);
        for (auto& cell : cells) if (cell.changed) {
            auto item = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.item);
            auto orientation = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.orientation);
            auto old_item = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_item);
            auto old_orientation = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, cell.old_orientation);
            auto registered = managerMethod(manager.value(), "add_do_method", grid.value(), "set_cell_item", {&cell.position, &item.value(), &orientation.value()});
            if (registered.isOk()) registered = managerMethod(manager.value(), "add_undo_method", grid.value(), "set_cell_item", {&cell.position, &old_item.value(), &old_orientation.value()});
            if (registered.isErr()) { abandonAction(manager.value()); return errorJson(500, "gridmap_undo_registration_failed"); }
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(500, committed.error().message);
        for (auto& cell : cells) if (cell.changed) {
            auto observed_item_value = callObject(grid.value(), "GridMap", "get_cell_item", 3724960147LL, {&cell.position});
            auto observed_orientation_value = callObject(grid.value(), "GridMap", "get_cell_item_orientation", 3724960147LL, {&cell.position});
            auto observed_item = observed_item_value.isOk() ? scalarFromVariant<int64_t>(observed_item_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                            : Result<int64_t>(observed_item_value.error());
            auto observed_orientation = observed_orientation_value.isOk() ? scalarFromVariant<int64_t>(observed_orientation_value.value(), GDEXTENSION_VARIANT_TYPE_INT)
                                                                          : Result<int64_t>(observed_orientation_value.error());
            if (observed_item.isErr() || observed_orientation.isErr() ||
                observed_item.value() != cell.item ||
                (cell.item >= 0 && observed_orientation.value() != cell.orientation)) {
                const auto restored = undoLastAction(manager.value(), root.value());
                return errorJson(500, "gridmap_postcondition_mismatch", {{"outcome", restored.isOk() ? "rolled_back" : "unknown"}});
            }
        }
        return liveResult({{"requested_cells", cells.size()}, {"changed_cells", changed_count},
            {"unchanged_cells", cells.size() - changed_count}, {"undo_redo_registered", true}, {"outcome", "completed"}, {"rollback", "undo_redo"}});
    }

    if (method == "vision.setCameraTransform") {
        if (session_kind != "editor") return errorJson(409, "session_kind_rejected");
        auto vector_is_valid = [](const json& value, double limit) {
            if (!value.is_object() || value.size() != 3) return false;
            for (const auto* axis : {"x", "y", "z"}) {
                if (!value.contains(axis) || !value[axis].is_number()) return false;
                const double component = value[axis].get<double>();
                if (!std::isfinite(component) || component < -limit || component > limit) {
                    return false;
                }
            }
            return true;
        };
        if (!hasOnlyKeys(params, {"camera_path", "position", "rotation_degrees", "fov"}) ||
            !params.contains("camera_path") || !params["camera_path"].is_string() ||
            params["camera_path"].get_ref<const std::string&>().empty() ||
            params["camera_path"].get_ref<const std::string&>().size() > 1024 ||
            !params.contains("position") || !vector_is_valid(params["position"], 1000000.0) ||
            (params.contains("rotation_degrees") &&
             !vector_is_valid(params["rotation_degrees"], 360000.0)) ||
            (params.contains("fov") &&
             (!params["fov"].is_number() || !std::isfinite(params["fov"].get<double>()) ||
              params["fov"].get<double>() < 1.0 || params["fov"].get<double>() > 179.0))) {
            return errorJson(400, "invalid_viewport_set_camera_transform_request");
        }

        for (const auto& bind : {
                 std::make_tuple("Object", "is_class", 3927539163LL),
                 std::make_tuple("Node3D", "get_position", 3360562783LL),
                 std::make_tuple("Node3D", "set_position", 3460891852LL),
                 std::make_tuple("Node3D", "get_rotation_degrees", 3360562783LL),
                 std::make_tuple("Node3D", "set_rotation_degrees", 3460891852LL),
                 std::make_tuple("Camera3D", "get_fov", 1740695150LL),
                 std::make_tuple("Camera3D", "set_fov", 373806689LL),
                 std::make_tuple("EditorUndoRedoManager", "get_object_history_id", 1107568780LL),
                 std::make_tuple("EditorUndoRedoManager", "get_history_undo_redo", 2417974513LL),
                 std::make_tuple("UndoRedo", "has_undo", 36873697LL),
                 std::make_tuple("UndoRedo", "undo", 2240911060LL)}) {
            if (requireMethodBind(std::get<0>(bind), std::get<1>(bind),
                                  std::get<2>(bind)).isErr()) {
                return errorJson(501, "required_bind_unavailable");
            }
        }
        if (preflightUndoManagerBindings().isErr()) {
            return errorJson(501, "required_bind_unavailable");
        }

        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        const auto camera_path = params["camera_path"].get<std::string>();
        auto camera = resolveNode(root.value(), camera_path);
        if (camera.isErr()) return errorJson(camera.error().code, camera.error().message);
        auto class_name = makeString("Camera3D");
        if (class_name.isErr()) return errorJson(500, class_name.error().message);
        auto class_result = callObject(camera.value(), "Object", "is_class", 3927539163LL,
                                       {&class_name.value()});
        if (class_result.isErr()) return errorJson(500, class_result.error().message);
        auto is_camera = scalarFromVariant<GDExtensionBool>(
            class_result.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
        if (is_camera.isErr()) return errorJson(500, is_camera.error().message);
        if (!is_camera.value()) return errorJson(404, "camera_path_does_not_resolve_to_camera3d");

        auto old_position = callObject(camera.value(), "Node3D", "get_position", 3360562783LL);
        auto old_rotation = callObject(camera.value(), "Node3D", "get_rotation_degrees", 3360562783LL);
        auto old_fov_value = callObject(camera.value(), "Camera3D", "get_fov", 1740695150LL);
        if (old_position.isErr() || old_rotation.isErr() || old_fov_value.isErr()) {
            return errorJson(500, "camera_state_read_failed");
        }
        auto old_position_json = pointVariantToJson(old_position.value(), 3);
        auto old_rotation_json = pointVariantToJson(old_rotation.value(), 3);
        auto old_fov = scalarFromVariant<double>(old_fov_value.value(),
                                                 GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (old_position_json.isErr() || old_rotation_json.isErr() || old_fov.isErr()) {
            return errorJson(500, "extension_protocol_error");
        }

        const auto& requested_position = params["position"];
        auto new_position = makeVector3(requested_position["x"].get<double>(),
                                        requested_position["y"].get<double>(),
                                        requested_position["z"].get<double>());
        if (new_position.isErr()) return errorJson(501, "required_bind_unavailable");
        std::optional<VariantValue> new_rotation;
        if (params.contains("rotation_degrees")) {
            const auto& requested = params["rotation_degrees"];
            auto value = makeVector3(requested["x"].get<double>(), requested["y"].get<double>(),
                                     requested["z"].get<double>());
            if (value.isErr()) return errorJson(501, "required_bind_unavailable");
            new_rotation.emplace(std::move(value.value()));
        }
        std::optional<VariantValue> new_fov;
        if (params.contains("fov")) {
            auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT,
                                    params["fov"].get<double>());
            if (value.isErr()) return errorJson(501, "required_bind_unavailable");
            new_fov.emplace(std::move(value.value()));
        }

        auto manager = undoManager(editor);
        if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
        auto action = createAction(manager.value(), "Set Camera3D Transform", camera.value());
        if (action.isErr()) return errorJson(500, action.error().message);
        auto add_step = [&](const char* operation, const char* property_method,
                            VariantValue& value) {
            return managerMethod(manager.value(), operation, camera.value(), property_method,
                                 {&value});
        };
        auto registered = add_step("add_do_method", "set_position", new_position.value());
        if (registered.isOk()) {
            registered = add_step("add_undo_method", "set_position", old_position.value());
        }
        if (registered.isOk() && new_rotation.has_value()) {
            registered = add_step("add_do_method", "set_rotation_degrees", *new_rotation);
        }
        if (registered.isOk() && new_rotation.has_value()) {
            registered = add_step("add_undo_method", "set_rotation_degrees", old_rotation.value());
        }
        if (registered.isOk() && new_fov.has_value()) {
            registered = add_step("add_do_method", "set_fov", *new_fov);
        }
        if (registered.isOk() && new_fov.has_value()) {
            registered = add_step("add_undo_method", "set_fov", old_fov_value.value());
        }
        if (registered.isErr()) {
            abandonAction(manager.value());
            return errorJson(500, "camera_undo_registration_failed");
        }
        auto committed = commitAction(manager.value());
        if (committed.isErr()) return errorJson(500, committed.error().message);

        auto observed_position = callObject(camera.value(), "Node3D", "get_position", 3360562783LL);
        auto observed_rotation = callObject(camera.value(), "Node3D", "get_rotation_degrees", 3360562783LL);
        auto observed_fov_value = callObject(camera.value(), "Camera3D", "get_fov", 1740695150LL);
        if (observed_position.isErr() || observed_rotation.isErr() || observed_fov_value.isErr()) {
            (void)undoLastAction(manager.value(), root.value());
            return errorJson(500, "camera_postcondition_read_failed");
        }
        auto observed_position_json = pointVariantToJson(observed_position.value(), 3);
        auto observed_rotation_json = pointVariantToJson(observed_rotation.value(), 3);
        auto observed_fov = scalarFromVariant<double>(observed_fov_value.value(),
                                                      GDEXTENSION_VARIANT_TYPE_FLOAT);
        if (observed_position_json.isErr() || observed_rotation_json.isErr() ||
            observed_fov.isErr()) {
            (void)undoLastAction(manager.value(), root.value());
            return errorJson(500, "extension_protocol_error");
        }
        auto close_enough = [](double left, double right) {
            return std::abs(left - right) <=
                   1e-5 * std::max({1.0, std::abs(left), std::abs(right)});
        };
        auto vector_matches = [&](const json& observed, const json& requested) {
            for (const auto* axis : {"x", "y", "z"}) {
                if (!close_enough(observed[axis].get<double>(), requested[axis].get<double>())) {
                    return false;
                }
            }
            return true;
        };
        bool matched = vector_matches(observed_position_json.value(), requested_position);
        if (params.contains("rotation_degrees")) {
            matched = matched && vector_matches(observed_rotation_json.value(),
                                                params["rotation_degrees"]);
        }
        if (params.contains("fov")) {
            matched = matched && close_enough(observed_fov.value(), params["fov"].get<double>());
        }
        if (!matched) {
            const auto restored = undoLastAction(manager.value(), root.value());
            return errorJson(500, "camera_postcondition_mismatch",
                             {{"outcome", restored.isOk() ? "rolled_back" : "unknown"},
                              {"rollback", restored.isOk() ? "completed" : "failed"},
                              {"retryable", false}});
        }
        return liveResult({
            {"camera_path", camera_path},
            {"old", {{"position", old_position_json.value()},
                     {"rotation_degrees", old_rotation_json.value()},
                     {"fov", old_fov.value()}}},
            {"new", {{"position", observed_position_json.value()},
                     {"rotation_degrees", observed_rotation_json.value()},
                     {"fov", observed_fov.value()}}},
            {"undo_redo_registered", true}, {"outcome", "completed"},
            {"rollback", "undo_redo"}});
    }

    if (method == "vision.toggleDebugDraw") {
        if (session_kind != "editor") return errorJson(409, "session_kind_rejected");
        if (!hasOnlyKeys(params, {"collision_shapes", "navigation_mesh", "wireframe"}) ||
            (!params.contains("collision_shapes") && !params.contains("navigation_mesh")) ||
            (params.contains("collision_shapes") && !params["collision_shapes"].is_boolean()) ||
            (params.contains("navigation_mesh") && !params["navigation_mesh"].is_boolean()) ||
            (params.contains("wireframe") &&
             (!params["wireframe"].is_boolean() || params["wireframe"].get<bool>()))) {
            return errorJson(400, "invalid_viewport_toggle_debug_draw_request");
        }
        for (const auto& bind : {
                 std::make_pair("is_debugging_collisions_hint", 36873697LL),
                 std::make_pair("set_debug_collisions_hint", 2586408642LL),
                 std::make_pair("is_debugging_navigation_hint", 36873697LL),
                 std::make_pair("set_debug_navigation_hint", 2586408642LL)}) {
            if (requireMethodBind("SceneTree", bind.first, bind.second).isErr()) {
                return errorJson(501, "required_bind_unavailable");
            }
        }
        auto tree = liveSceneTree();
        if (tree.isErr()) return errorJson(tree.error().code, tree.error().message);
        auto read_hint = [&](const char* getter) -> Result<bool> {
            auto value = callObject(tree.value(), "SceneTree", getter, 36873697LL);
            if (value.isErr()) return value.error();
            auto enabled = scalarFromVariant<GDExtensionBool>(
                value.value(), GDEXTENSION_VARIANT_TYPE_BOOL);
            return enabled.isOk() ? Result<bool>(enabled.value() != 0)
                                  : Result<bool>(enabled.error());
        };
        auto old_collision = read_hint("is_debugging_collisions_hint");
        auto old_navigation = read_hint("is_debugging_navigation_hint");
        if (old_collision.isErr() || old_navigation.isErr()) {
            return errorJson(500, "debug_hint_read_failed");
        }
        auto set_hint = [&](const char* setter, bool enabled) -> Result<void> {
            auto value = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL,
                                    static_cast<GDExtensionBool>(enabled));
            if (value.isErr()) return value.error();
            auto result = callObject(tree.value(), "SceneTree", setter, 2586408642LL,
                                     {&value.value()});
            return result.isOk() ? Result<void>::ok() : Result<void>(result.error());
        };
        auto restore_hints = [&]() {
            const auto collision = set_hint("set_debug_collisions_hint", old_collision.value());
            const auto navigation = set_hint("set_debug_navigation_hint", old_navigation.value());
            return collision.isOk() && navigation.isOk();
        };
        if (params.contains("collision_shapes")) {
            auto changed = set_hint("set_debug_collisions_hint",
                                    params["collision_shapes"].get<bool>());
            if (changed.isErr()) {
                const bool restored = restore_hints();
                return errorJson(500, changed.error().message,
                                 {{"outcome", restored ? "rolled_back" : "unknown"},
                                  {"rollback", restored ? "completed" : "failed"},
                                  {"retryable", false}});
            }
        }
        if (params.contains("navigation_mesh")) {
            auto changed = set_hint("set_debug_navigation_hint",
                                    params["navigation_mesh"].get<bool>());
            if (changed.isErr()) {
                const bool restored = restore_hints();
                return errorJson(500, changed.error().message,
                                 {{"outcome", restored ? "rolled_back" : "unknown"},
                                  {"rollback", restored ? "completed" : "failed"},
                                  {"retryable", false}});
            }
        }
        auto observed_collision = read_hint("is_debugging_collisions_hint");
        auto observed_navigation = read_hint("is_debugging_navigation_hint");
        const bool matched = observed_collision.isOk() && observed_navigation.isOk() &&
            (!params.contains("collision_shapes") ||
             observed_collision.value() == params["collision_shapes"].get<bool>()) &&
            (!params.contains("navigation_mesh") ||
             observed_navigation.value() == params["navigation_mesh"].get<bool>()) &&
            (params.contains("collision_shapes") ||
             observed_collision.value() == old_collision.value()) &&
            (params.contains("navigation_mesh") ||
             observed_navigation.value() == old_navigation.value());
        if (!matched) {
            const bool restored = restore_hints();
            return errorJson(500, "debug_draw_postcondition_mismatch",
                             {{"outcome", restored ? "rolled_back" : "unknown"},
                              {"rollback", restored ? "completed" : "failed"},
                              {"retryable", false}});
        }
        return liveResult({
            {"previous", {{"collision_shapes", old_collision.value()},
                          {"navigation_mesh", old_navigation.value()}}},
            {"observed", {{"collision_shapes", observed_collision.value()},
                          {"navigation_mesh", observed_navigation.value()}}},
            {"effective_scope", "future_games_run_from_editor"},
            {"outcome", "completed"}, {"rollback", "explicit_restore"}});
    }

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
        // Say what this did and did not do. Writing the setting is not the same
        // as the attached editor knowing about it: Godot registers an autoload's
        // global name through editor-internal paths a GDExtension cannot reach,
        // so scripts referring to the singleton keep failing to compile in this
        // editor session until it restarts. `editor_reload_project` does not
        // clear it either. Reporting only `persisted: true` left callers
        // debugging their own scripts for a state this call had created.
        return liveResult({{"status", "success"}, {"name", autoload_name}, {"path", resource_path},
                           {"singleton", autoload_singleton}, {"removed", removing}, {"persisted", true},
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
            auto packed_scene = constructObject(packed_scene_name.ptr());
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
            packed_root = constructObject(native_type.ptr());
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

    if (method == "editor.getSelection") {
        // What the person has selected is the referent of "this node". Nothing
        // else in the surface can answer it, and a second MCP client cannot ask
        // at all, because only one may hold the editor route.
        auto selection_value = callObject(editor, "EditorInterface", "get_selection", 2690272531LL);
        if (selection_value.isErr()) {
            return errorJson(selection_value.error().code, selection_value.error().message);
        }
        auto selection = objectFromVariant(selection_value.value());
        if (selection.isErr()) return errorJson(selection.error().code, selection.error().message);
        if (!selection.value()) {
            return errorJson(500, "Godot returned no EditorSelection");
        }

        auto root_result = editedSceneRoot(editor);
        if (root_result.isErr()) {
            return errorJson(root_result.error().code, root_result.error().message);
        }
        auto root = root_result.value();

        auto nodes_value = callObject(selection.value(), "EditorSelection", "get_selected_nodes",
                                      2915620761LL);
        if (nodes_value.isErr()) {
            return errorJson(nodes_value.error().code, nodes_value.error().message);
        }
        auto size_value = callVariant(nodes_value.value(), "size");
        if (size_value.isErr()) return errorJson(size_value.error().code, size_value.error().message);
        auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (size.isErr()) return errorJson(size.error().code, size.error().message);

        // A selection is a handful of nodes, but it is engine-supplied and this
        // runs on the main loop, so it is bounded like every other list.
        constexpr int64_t kMaxSelected = 256;
        const int64_t total = size.value();
        const int64_t reported = std::min(total, kMaxSelected);

        json selected = json::array();
        for (int64_t index = 0; index < reported; ++index) {
            auto position = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (position.isErr()) return errorJson(position.error().code, position.error().message);
            auto node_value = callVariant(nodes_value.value(), "get", {&position.value()});
            if (node_value.isErr()) return errorJson(node_value.error().code, node_value.error().message);
            auto node = objectFromVariant(node_value.value());
            if (node.isErr()) return errorJson(node.error().code, node.error().message);
            // A selected node can be freed between the engine building the list
            // and this reading it. Skipping is right: reporting a null path
            // would be a node path that resolves to nothing.
            if (!node.value()) continue;

            auto path = logicalPathFromEditedRoot(root, node.value());
            auto node_class = nodeString(node.value(), "get_class", 201670096LL);
            auto node_name = nodeString(node.value(), "get_name", 2002593661LL);
            // A node selected in another open scene is not addressable from the
            // edited root, so it is counted and not named rather than reported
            // with a path that does not resolve.
            if (path.isErr()) continue;
            json entry = {{"node_path", path.value()}};
            if (node_class.isOk()) entry["class"] = node_class.value();
            if (node_name.isOk()) entry["name"] = node_name.value();
            selected.push_back(std::move(entry));
        }

        return liveResult({{"status", "success"},
                           {"selected", selected},
                           {"count", selected.size()},
                           {"selected_total", total},
                           {"truncated", total > reported}});
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

    if (method == "shader.listUniforms" || method == "shader.setUniform" ||
        method == "shader.getVisualGraph") {
        // One path to the material for all three, so a slot one of them accepts
        // is never a slot another refuses.
        const bool setting = method == "shader.setUniform";
        const bool graphing = method == "shader.getVisualGraph";
        const bool shape_ok = setting
            ? (hasOnlyKeys(params, {"target_node", "property_name", "uniform_name", "value"}) &&
               params.contains("uniform_name") && params["uniform_name"].is_string() &&
               params.contains("value"))
            : hasOnlyKeys(params, {"target_node", "property_name"});
        if (!shape_ok || !params.contains("target_node") || !params["target_node"].is_string() ||
            !params.contains("property_name") || !params["property_name"].is_string()) {
            return errorJson(400, setting ? "invalid_shader_set_uniform_request"
                                          : (graphing ? "invalid_shader_get_visual_graph_request"
                                                      : "invalid_shader_list_uniforms_request"));
        }
        if (graphing) {
            for (const auto& bind : {
                     std::make_tuple("VisualShader", "get_node_list", 2370592410LL),
                     std::make_tuple("VisualShader", "get_node", 3784670312LL),
                     std::make_tuple("VisualShader", "get_node_position", 2175036082LL),
                     std::make_tuple("VisualShader", "get_node_connections", 1441964831LL)}) {
                if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
                    return errorJson(501, "required_bind_unavailable");
                }
            }
        }
        for (const auto& bind : {
                 std::make_tuple("ShaderMaterial", "get_shader", 2078273437LL),
                 std::make_tuple("Shader", "get_shader_uniform_list", 1230511656LL),
                 std::make_tuple("Shader", "get_mode", 3392948163LL),
                 std::make_tuple("ShaderMaterial", "get_shader_parameter", 2760726917LL)}) {
            if (requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind)).isErr()) {
                return errorJson(501, "required_bind_unavailable");
            }
        }
        if (setting && preflightUndoManagerBindings().isErr()) {
            return errorJson(501, "required_bind_unavailable");
        }
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        const auto target_path = params["target_node"].get<std::string>();
        const auto property = params["property_name"].get<std::string>();
        auto node = resolveNode(root.value(), target_path);
        if (node.isErr()) return errorJson(404, "shader_target_not_found");

        auto has_property = objectHasProperty(node.value(), property);
        if (has_property.isErr()) return errorJson(has_property.error().code, has_property.error().message);
        if (!has_property.value()) {
            return errorJson(404, "Property not found on target node: " + property);
        }
        auto property_name = makeStringName(property);
        if (property_name.isErr()) return errorJson(500, property_name.error().message);
        auto slot = callObject(node.value(), "Object", "get", 2760726917LL, {&property_name.value()});
        if (slot.isErr()) return errorJson(500, slot.error().message);
        auto material = objectFromVariant(slot.value());
        if (material.isErr()) return errorJson(500, material.error().message);
        if (!material.value()) {
            return errorJson(404, "Property \"" + property + "\" on " + target_path +
                                      " holds nothing; there is no material to read");
        }
        auto is_shader_material = objectIsClass(material.value(), "ShaderMaterial");
        if (is_shader_material.isErr()) return errorJson(500, is_shader_material.error().message);
        if (!is_shader_material.value()) {
            // A StandardMaterial3D has properties, not shader uniforms. Reading
            // it here and reporting an empty uniform list would look like a
            // shader with nothing to set.
            auto actual = callObject(material.value(), "Object", "get_class", 201670096LL);
            std::string actual_name = "an unrecognised type";
            if (actual.isOk()) {
                auto text = stringFromVariant(actual.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                if (text.isOk()) actual_name = text.value();
            }
            return errorJson(409, "Property \"" + property + "\" on " + target_path + " holds " +
                                      actual_name + " and not a ShaderMaterial");
        }

        auto shader_value = callObject(material.value(), "ShaderMaterial", "get_shader", 2078273437LL);
        if (shader_value.isErr()) return errorJson(500, shader_value.error().message);
        auto shader = objectFromVariant(shader_value.value());
        if (shader.isErr()) return errorJson(500, shader.error().message);
        if (!shader.value()) {
            return errorJson(409, "The ShaderMaterial on " + target_path + " has no shader assigned");
        }

        json result = {{"target_node", target_path}, {"property_name", property}};
        auto shader_path_name = makeStringName("resource_path");
        if (shader_path_name.isOk()) {
            auto shader_path = callObject(shader.value(), "Object", "get", 2760726917LL,
                                          {&shader_path_name.value()});
            if (shader_path.isOk()) {
                auto text = stringFromVariant(shader_path.value(), GDEXTENSION_VARIANT_TYPE_STRING);
                result["shader_path"] = (text.isOk() && !text.value().empty())
                                            ? json(text.value()) : json(nullptr);
            }
        }
        auto mode = callObject(shader.value(), "Shader", "get_mode", 3392948163LL);
        if (mode.isOk()) {
            auto mode_value = scalarFromVariant<int64_t>(mode.value(), GDEXTENSION_VARIANT_TYPE_INT);
            if (mode_value.isOk()) {
                // 0 spatial, 1 canvas_item, 2 particles, 3 sky, 4 fog. Named so
                // a caller is not handed a bare number to look up.
                static const char* kModes[] = {"spatial", "canvas_item", "particles", "sky", "fog"};
                const auto index = mode_value.value();
                result["shader_mode"] = (index >= 0 && index < 5) ? json(kModes[index])
                                                                  : json(nullptr);
            }
        }

        if (graphing) {
            auto is_visual = objectIsClass(shader.value(), "VisualShader");
            if (is_visual.isErr()) return errorJson(500, is_visual.error().message);
            if (!is_visual.value()) {
                // A hand written .gdshader has code and no graph. Returning an
                // empty node list would read as a graph with nothing in it.
                return errorJson(409, "The shader on this material is written in code, not built as "
                                      "a VisualShader graph, so it has no nodes to report");
            }
            // The shader types a VisualShader can hold, in enum order. Named so
            // a caller is not handed a bare number, and skipped entirely when a
            // type holds nothing.
            static const char* kGraphTypes[] = {"vertex", "fragment", "light", "start", "process",
                                                "collide", "start_custom", "process_custom", "sky",
                                                "fog"};
            constexpr int64_t kMaxGraphNodes = 256;
            constexpr int64_t kMaxGraphConnections = 512;
            json types = json::array();
            bool any_truncated = false;
            for (int64_t type_index = 0; type_index < 10; ++type_index) {
                auto type_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, type_index);
                if (type_value.isErr()) return errorJson(500, type_value.error().message);
                auto ids = callObject(shader.value(), "VisualShader", "get_node_list",
                                      2370592410LL, {&type_value.value()});
                if (ids.isErr()) continue;
                auto id_count_value = callVariant(ids.value(), "size");
                if (id_count_value.isErr()) continue;
                auto id_count = scalarFromVariant<int64_t>(id_count_value.value(),
                                                           GDEXTENSION_VARIANT_TYPE_INT);
                if (id_count.isErr() || id_count.value() == 0) continue;

                json nodes = json::array();
                const int64_t reported_nodes = std::min<int64_t>(id_count.value(), kMaxGraphNodes);
                for (int64_t slot = 0; slot < reported_nodes; ++slot) {
                    auto slot_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, slot);
                    if (slot_value.isErr()) return errorJson(500, slot_value.error().message);
                    auto id_value = callVariant(ids.value(), "get", {&slot_value.value()});
                    if (id_value.isErr()) continue;
                    auto node_id = scalarFromVariant<int64_t>(id_value.value(),
                                                              GDEXTENSION_VARIANT_TYPE_INT);
                    if (node_id.isErr()) continue;
                    auto graph_id = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, node_id.value());
                    if (graph_id.isErr()) return errorJson(500, graph_id.error().message);

                    json node_entry = {{"id", node_id.value()}};
                    auto node_object_value = callObject(shader.value(), "VisualShader", "get_node",
                                                        3784670312LL,
                                                        {&type_value.value(), &graph_id.value()});
                    if (node_object_value.isOk()) {
                        auto node_object = objectFromVariant(node_object_value.value());
                        if (node_object.isOk() && node_object.value()) {
                            auto class_name = callObject(node_object.value(), "Object", "get_class",
                                                         201670096LL);
                            if (class_name.isOk()) {
                                auto text = stringFromVariant(class_name.value(),
                                                              GDEXTENSION_VARIANT_TYPE_STRING);
                                if (text.isOk()) {
                                    node_entry["class"] = boundUtf8(text.value(), 256).value;
                                }
                            }
                        }
                    }
                    auto position = callObject(shader.value(), "VisualShader", "get_node_position",
                                               2175036082LL, {&type_value.value(), &graph_id.value()});
                    if (position.isOk()) {
                        auto point = pointVariantToJson(position.value(), 2);
                        if (point.isOk()) node_entry["position"] = point.value();
                    }
                    nodes.push_back(std::move(node_entry));
                }

                json links = json::array();
                int64_t connection_count = 0;
                auto connections = callObject(shader.value(), "VisualShader", "get_node_connections",
                                              1441964831LL, {&type_value.value()});
                if (connections.isOk()) {
                    auto connection_size = callVariant(connections.value(), "size");
                    if (connection_size.isOk()) {
                        auto total = scalarFromVariant<int64_t>(connection_size.value(),
                                                                 GDEXTENSION_VARIANT_TYPE_INT);
                        if (total.isOk()) {
                            connection_count = total.value();
                            const int64_t reported =
                                std::min<int64_t>(connection_count, kMaxGraphConnections);
                            for (int64_t link = 0; link < reported; ++link) {
                                auto link_index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, link);
                                if (link_index.isErr()) break;
                                auto entry = callVariant(connections.value(), "get",
                                                          {&link_index.value()});
                                if (entry.isErr()) continue;
                                json link_entry = json::object();
                                for (const auto* field : {"from_node", "from_port", "to_node",
                                                          "to_port"}) {
                                    auto field_key = makeString(field);
                                    if (field_key.isErr()) continue;
                                    auto field_value = callVariant(entry.value(), "get",
                                                                    {&field_key.value()});
                                    if (field_value.isErr()) continue;
                                    auto number = scalarFromVariant<int64_t>(
                                        field_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
                                    if (number.isOk()) link_entry[field] = number.value();
                                }
                                if (!link_entry.empty()) links.push_back(std::move(link_entry));
                            }
                        }
                    }
                }

                const bool truncated = id_count.value() > kMaxGraphNodes ||
                                       connection_count > kMaxGraphConnections;
                any_truncated = any_truncated || truncated;
                types.push_back({{"type", kGraphTypes[type_index]},
                                 {"nodes", std::move(nodes)},
                                 {"node_count", id_count.value()},
                                 {"connections", std::move(links)},
                                 {"connection_count", connection_count},
                                 {"truncated", truncated}});
            }
            result["shader_types"] = std::move(types);
            result["truncated"] = any_truncated;
            return liveResult(result);
        }

        auto groups = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
        if (groups.isErr()) return errorJson(500, groups.error().message);
        auto uniforms = callObject(shader.value(), "Shader", "get_shader_uniform_list", 1230511656LL,
                                   {&groups.value()});
        if (uniforms.isErr()) return errorJson(500, uniforms.error().message);
        auto size_value = callVariant(uniforms.value(), "size");
        if (size_value.isErr()) return errorJson(500, size_value.error().message);
        auto count = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
        if (count.isErr()) return errorJson(500, count.error().message);

        if (setting) {
            const auto requested_name = params["uniform_name"].get<std::string>();
            // The shader list decides whether this uniform exists.
            // set_shader_parameter accepts any name and does nothing with one
            // the shader never declared, so a typo would otherwise come back as
            // a write that worked.
            auto name_key = makeString("name");
            auto type_key = makeString("type");
            auto class_key = makeString("class_name");
            if (name_key.isErr() || type_key.isErr() || class_key.isErr()) {
                return errorJson(500, "Failed to build uniform keys");
            }
            bool declared = false;
            int64_t declared_type = 0;
            std::string declared_class;
            for (int64_t index = 0; index < count.value() && !declared; ++index) {
                auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
                if (index_value.isErr()) return errorJson(500, index_value.error().message);
                auto entry = callVariant(uniforms.value(), "get", {&index_value.value()});
                if (entry.isErr()) return errorJson(500, entry.error().message);
                auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
                if (name_value.isErr()) return errorJson(500, name_value.error().message);
                const auto name_type = GodotApi::instance().variant_get_type(name_value.value().ptr());
                if (name_type != GDEXTENSION_VARIANT_TYPE_STRING &&
                    name_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                    continue;
                }
                auto name = stringFromVariant(name_value.value(), name_type);
                if (name.isErr()) return errorJson(500, name.error().message);
                if (name.value() != requested_name) continue;
                declared = true;
                auto type_value = callVariant(entry.value(), "get", {&type_key.value()});
                if (type_value.isOk()) {
                    auto value = scalarFromVariant<int64_t>(type_value.value(),
                                                            GDEXTENSION_VARIANT_TYPE_INT);
                    if (value.isOk()) declared_type = value.value();
                }
                auto class_value = callVariant(entry.value(), "get", {&class_key.value()});
                if (class_value.isOk()) {
                    const auto class_type =
                        GodotApi::instance().variant_get_type(class_value.value().ptr());
                    if (class_type == GDEXTENSION_VARIANT_TYPE_STRING ||
                        class_type == GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                        auto text = stringFromVariant(class_value.value(), class_type);
                        if (text.isOk()) declared_class = text.value();
                    }
                }
            }
            if (!declared) {
                return errorJson(404, "The shader declares no uniform named " + requested_name);
            }

            // The same contract scene_set_property applies, so a caller learns
            // one set of JSON spellings rather than two.
            const auto uniform_type = static_cast<GDExtensionVariantType>(declared_type);
            auto compatible = validateJsonForPropertyType(requested_name, params["value"], uniform_type);
            if (compatible.isErr()) return errorJson(compatible.error().code, compatible.error().message);
            auto new_value = uniform_type == GDEXTENSION_VARIANT_TYPE_OBJECT
                ? makeResourceForProperty(requested_name, params["value"], declared_class)
                : makeJsonVariantForProperty(params["value"], uniform_type);
            if (new_value.isErr()) return errorJson(new_value.error().code, new_value.error().message);

            auto uniform_name = makeStringName(requested_name);
            if (uniform_name.isErr()) return errorJson(500, uniform_name.error().message);
            auto old_value = callObject(material.value(), "ShaderMaterial", "get_shader_parameter",
                                        2760726917LL, {&uniform_name.value()});
            if (old_value.isErr()) return errorJson(500, old_value.error().message);

            // Undo goes through the shader_parameter/<name> property on the
            // material, which is the one the scene file writes and the one the
            // inspector edits, so undoing this is the undo a person expects.
            auto stored_name = makeStringName("shader_parameter/" + requested_name);
            if (stored_name.isErr()) return errorJson(500, stored_name.error().message);
            auto manager = undoManager(editor);
            if (manager.isErr()) return errorJson(manager.error().code, manager.error().message);
            auto material_value = makeObject(material.value());
            if (material_value.isErr()) return errorJson(500, material_value.error().message);
            auto action = createAction(manager.value(), "Didi: set shader uniform " + requested_name,
                                       material.value());
            if (action.isErr()) return errorJson(500, action.error().message);
            auto do_property = callObject(manager.value(), "EditorUndoRedoManager", "add_do_property",
                                          1017172818LL,
                                          {&material_value.value(), &stored_name.value(),
                                           &new_value.value()});
            auto undo_property = callObject(manager.value(), "EditorUndoRedoManager", "add_undo_property",
                                            1017172818LL,
                                            {&material_value.value(), &stored_name.value(),
                                             &old_value.value()});
            if (do_property.isErr()) return errorJson(500, do_property.error().message);
            if (undo_property.isErr()) return errorJson(500, undo_property.error().message);
            auto committed = commitAction(manager.value());
            if (committed.isErr()) return errorJson(committed.error().code, committed.error().message);

            // Report what it now holds and not what was asked for, the same way
            // scene_set_property does and for the same reason.
            auto observed = callObject(material.value(), "ShaderMaterial", "get_shader_parameter",
                                       2760726917LL, {&uniform_name.value()});
            if (observed.isErr()) return errorJson(500, observed.error().message);
            auto observed_json = variantToJson(observed.value(), 0, true);
            auto old_json = variantToJson(old_value.value(), 0, true);
            json old_payload = old_json.isOk() ? old_json.value() : json(nullptr);
            if (isNilVariant(old_value.value())) {
                // The undo entry above restores nil on purpose, which takes the
                // override off again rather than pinning the default in its
                // place. What gets reported is the value that was in effect,
                // and for a uniform the material did not set that is the
                // shader's declared default, read the same way the list reads
                // it.
                auto fallback = shaderParameterDefault(shader.value(), uniform_name.value());
                if (fallback.isOk() && !isNilVariant(fallback.value())) {
                    auto encoded = variantToJson(fallback.value(), 0, true);
                    if (encoded.isOk()) old_payload = encoded.value();
                }
            }
            json observed_payload = observed_json.isOk() ? observed_json.value() : json(nullptr);
            return liveResult({{"status", "success"},
                               {"target_node", target_path},
                               {"property_name", property},
                               {"uniform_name", requested_name},
                               {"type", godotVariantTypeName(static_cast<int>(declared_type))},
                               {"value", observed_payload},
                               {"requested_value", params["value"]},
                               {"old_value", old_payload},
                               {"applied", jsonScalarsEquivalent(observed_payload, params["value"])},
                               {"undo_redo_registered", true}});
        }

        constexpr int64_t kMaxUniforms = 256;
        const int64_t reported = std::min<int64_t>(count.value(), kMaxUniforms);
        auto name_key = makeString("name");
        auto type_key = makeString("type");
        if (name_key.isErr() || type_key.isErr()) return errorJson(500, "Failed to build uniform keys");

        json listed = json::array();
        for (int64_t index = 0; index < reported; ++index) {
            auto index_value = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
            if (index_value.isErr()) return errorJson(500, index_value.error().message);
            auto entry = callVariant(uniforms.value(), "get", {&index_value.value()});
            if (entry.isErr()) return errorJson(500, entry.error().message);
            auto name_value = callVariant(entry.value(), "get", {&name_key.value()});
            if (name_value.isErr()) return errorJson(500, name_value.error().message);
            const auto name_type = GodotApi::instance().variant_get_type(name_value.value().ptr());
            if (name_type != GDEXTENSION_VARIANT_TYPE_STRING &&
                name_type != GDEXTENSION_VARIANT_TYPE_STRING_NAME) {
                continue;
            }
            auto name = stringFromVariant(name_value.value(), name_type);
            if (name.isErr()) return errorJson(500, name.error().message);

            int64_t declared_type = 0;
            auto type_value = callVariant(entry.value(), "get", {&type_key.value()});
            if (type_value.isOk()) {
                auto declared = scalarFromVariant<int64_t>(type_value.value(),
                                                           GDEXTENSION_VARIANT_TYPE_INT);
                if (declared.isOk()) declared_type = declared.value();
            }

            json uniform = {{"name", name.value()},
                            {"type", godotVariantTypeName(static_cast<int>(declared_type))},
                            {"settable", jsonTypeIsInsidePropertyContract(static_cast<int>(declared_type))}};

            auto uniform_name = makeStringName(name.value());
            if (uniform_name.isErr()) return errorJson(500, uniform_name.error().message);
            auto current = callObject(material.value(), "ShaderMaterial", "get_shader_parameter",
                                      2760726917LL, {&uniform_name.value()});
            if (current.isErr()) return errorJson(500, current.error().message);
            // The effective value: the material's override where it has one and
            // the shader's own declared default otherwise.
            //
            // get_shader_parameter alone cannot supply that. In a running game
            // on 4.5.1, 4.6.2 and 4.7.2 it answers nil for a uniform the
            // material does not set, so the declared default was reported as no
            // value at all. The shader keeps its defaults in the rendering
            // server, and that is where they have to be read from.
            //
            // There is still deliberately no flag separating an override from a
            // default. The same call that answers nil in a game answers with the
            // default in a 4.7.2 editor, so nil is not evidence of anything and
            // a flag built on it would be wrong in one context or the other.
            if (isNilVariant(current.value())) {
                auto fallback = shaderParameterDefault(shader.value(), uniform_name.value());
                if (fallback.isOk()) current = std::move(fallback.value());
            }
            // Lenient: a uniform of a type this cannot encode is reported by
            // name and type with a null value, rather than failing the whole
            // read and leaving the caller with nothing.
            auto encoded = variantToJson(current.value(), 0, true);
            // A null value here is not a uniform without one. It is a value
            // neither the material nor the rendering server could supply, which
            // is what a session with no renderer looks like.
            uniform["value"] = encoded.isOk() ? encoded.value() : json(nullptr);
            listed.push_back(std::move(uniform));
        }

        result["uniforms"] = std::move(listed);
        result["uniform_count"] = count.value();
        result["truncated"] = count.value() > kMaxUniforms;
        return liveResult(result);
    }

    if (method == "scene.getProperty" || method == "scene.setProperty") {
        auto root = editedSceneRoot(editor);
        if (root.isErr()) return errorJson(root.error().code, root.error().message);
        auto node = resolveNode(root.value(), params.value("target_node", ""));
        if (node.isErr()) return errorJson(node.error().code, node.error().message);
        std::string property = params.value("property_name", "");
        if (property.empty()) return errorJson(400, "property_name is required");
        auto descriptor = findPropertyDescriptor(node.value(), property);
        if (descriptor.isErr()) return errorJson(descriptor.error().code, descriptor.error().message);
        if (!descriptor.value().has_value()) {
            return errorJson(404, "Property not found on target node: " + property);
        }
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
        // An empty resource slot holds nil, so the current value cannot say what
        // the slot is for. The class's own declaration can, and it is the same
        // answer for a slot that is full.
        auto property_type = GodotApi::instance().variant_get_type(old_value.value().ptr());
        if (property_type == GDEXTENSION_VARIANT_TYPE_NIL &&
            descriptor.value()->declared_type != GDEXTENSION_VARIANT_TYPE_NIL) {
            property_type = static_cast<GDExtensionVariantType>(descriptor.value()->declared_type);
        }
        auto compatible = validateJsonForPropertyType(property, params["value"], property_type);
        if (compatible.isErr()) return errorJson(compatible.error().code, compatible.error().message);
        auto new_value = property_type == GDEXTENSION_VARIANT_TYPE_OBJECT
            ? makeResourceForProperty(property, params["value"], descriptor.value()->class_name)
            : makeJsonVariantForProperty(params["value"], property_type);
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

        // Report what the property now holds, not what was asked for. A commit
        // that succeeds is not a property that changed: Godot discards some
        // writes outright, `anchors_preset` on a Control still in layout_mode 0
        // being the case that found this, and echoing the request back reported
        // success for a scene that had not moved. The caller has no other way
        // to see the difference. Same shape as vision.setCameraTransform, which
        // returns observed state rather than the values it was handed.
        auto observed = callObject(node.value(), "Object", "get", 2760726917LL, {&property_name.value()});
        if (observed.isErr()) return errorJson(observed.error().code, observed.error().message);
        auto observed_json = variantToJson(observed.value());
        if (observed_json.isErr()) return errorJson(observed_json.error().code, observed_json.error().message);
        auto old_json = variantToJson(old_value.value());
        if (old_json.isErr()) return errorJson(old_json.error().code, old_json.error().message);
        return liveResult({{"status", "success"}, {"target_node", params.value("target_node", "")},
                           {"property_name", property}, {"value", observed_json.value()},
                           {"requested_value", params["value"]}, {"old_value", old_json.value()},
                           {"applied", jsonScalarsEquivalent(observed_json.value(), params["value"])},
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
        auto node = constructObject(type_name.ptr());
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
            auto declared = findPropertyDescriptor(node, it.key());
            auto property_type =
                GodotApi::instance().variant_get_type(current_value.value().ptr());
            std::string declared_class;
            if (declared.isOk() && declared.value().has_value()) {
                declared_class = declared.value()->class_name;
                if (property_type == GDEXTENSION_VARIANT_TYPE_NIL &&
                    declared.value()->declared_type != GDEXTENSION_VARIANT_TYPE_NIL) {
                    property_type =
                        static_cast<GDExtensionVariantType>(declared.value()->declared_type);
                }
            }
            auto compatible = validateJsonForPropertyType(it.key(), it.value(), property_type);
            if (compatible.isErr()) {
                GodotApi::instance().object_destroy(node);
                return errorJson(compatible.error().code, compatible.error().message);
            }
            auto property_value = property_type == GDEXTENSION_VARIANT_TYPE_OBJECT
                ? makeResourceForProperty(it.key(), it.value(), declared_class)
                : makeJsonVariantForProperty(it.value(), property_type);
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

namespace {

// A viewport control that is not the one on screen has no size, and Godot hands
// back its 2x2 minimum rather than refusing. Capturing that produced a
// four-pixel image reported as a successful live frame, which a caller cannot
// tell from a scene that happens to be empty. Nothing smaller than this is a
// picture of anything.
constexpr int64_t kMinimumCaptureEdge = 8;

Result<ViewportPixels> captureViewportObject(GDExtensionObjectPtr viewport_object,
                                             const std::string& described_target) {
    if (!viewport_object) return Error::notFound("Viewport is unavailable");
    auto texture = callObject(viewport_object, "Viewport", "get_texture", 1746695840LL);
    if (texture.isErr()) return texture.error();
    auto texture_object = objectFromVariant(texture.value());
    if (texture_object.isErr() || !texture_object.value()) return Error::notFound("Viewport texture is unavailable");
    auto texture_width_value = callObject(texture_object.value(), "Texture2D", "get_width", 3905245786LL);
    auto texture_height_value = callObject(texture_object.value(), "Texture2D", "get_height", 3905245786LL);
    if (texture_width_value.isErr()) return texture_width_value.error();
    if (texture_height_value.isErr()) return texture_height_value.error();
    auto texture_width = scalarFromVariant<int64_t>(texture_width_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    auto texture_height = scalarFromVariant<int64_t>(texture_height_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (texture_width.isErr()) return texture_width.error();
    if (texture_height.isErr()) return texture_height.error();
    if (texture_width.value() <= 0 || texture_height.value() <= 0) {
        return Error::notFound("Viewport has no rendered texture in the current display mode");
    }
    if (texture_width.value() < kMinimumCaptureEdge || texture_height.value() < kMinimumCaptureEdge) {
        return Error::notFound(
            "Viewport '" + described_target + "' has no size on screen (" +
            std::to_string(texture_width.value()) + "x" + std::to_string(texture_height.value()) +
            "), so there is no frame to capture. For an editor viewport this means that main "
            "screen is not the one selected; switch to it in the editor and call again.");
    }
    const auto texture_size = image::checkedRgbaSize(texture_width.value(), texture_height.value());
    if (texture_size.isErr()) return texture_size.error();
    auto image = callObject(texture_object.value(), "Texture2D", "get_image", 4190603485LL);
    if (image.isErr()) return image.error();
    auto image_object = objectFromVariant(image.value());
    if (image_object.isErr() || !image_object.value()) return Error::notFound("Viewport image is unavailable");
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

} // namespace

namespace {

// The scene drawn again with every geometry node's material replaced, so the
// picture answers one question instead of showing one appearance.
//
// Every pass writes the inverse of the sRGB curve the framebuffer applies on
// the way out, which is what keeps a mid grey from arriving as a much lighter
// one. It does not make the stored byte the written byte: the viewport applies
// its own post-processing after this, and how much it changes depends on the
// engine. A 4.7.2 editor returns these values unchanged and a 4.5.1 editor
// returns them scaled by about a quarter. So a pass is an ordering that can be
// read and compared, not a calibrated measurement, and nothing here claims
// otherwise.
constexpr const char* kPassStoreHelper = R"(
vec3 didi_store(vec3 c) {
	vec3 lo = c / 12.92;
	vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
	return mix(hi, lo, step(c, vec3(0.04045)));
}
)";

std::string passShaderSource(const std::string& kind) {
    std::string source = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n";
    if (kind == "segmentation") {
        // One flat colour a material sets per node, so the picture is a map
        // from pixel to node rather than a picture of the scene.
        source += "uniform vec3 didi_colour = vec3(1.0);\n";
        source += kPassStoreHelper;
        source += "void fragment() {\n"
                  "\tALBEDO = didi_store(didi_colour);\n"
                  "}\n";
        return source;
    }
    if (kind == "depth") {
        source += "uniform float didi_far = 100.0;\n";
        source += kPassStoreHelper;
        // VERTEX is view space in a fragment shader and the camera looks down
        // -Z, so -VERTEX.z is the distance in front of the camera.
        source += "void fragment() {\n"
                  "\tALBEDO = didi_store(vec3(clamp(-VERTEX.z / didi_far, 0.0, 1.0)));\n"
                  "}\n";
        return source;
    }
    // World space rather than view space, so a surface that faces up reads the
    // same whichever way the camera happens to be turned.
    source += kPassStoreHelper;
    source += "void fragment() {\n"
              "\tvec3 world_normal = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);\n"
              "\tALBEDO = didi_store(world_normal * 0.5 + 0.5);\n"
              "}\n";
    return source;
}

struct PassSubject {
    GDExtensionObjectPtr node{nullptr};
    std::string path;
    std::string class_name;
};

constexpr size_t kMaxPassNodes = 4096;

Result<void> collectPassSubjects(GDExtensionObjectPtr node, GDExtensionObjectPtr root, bool editor,
                                 std::vector<PassSubject>& subjects, size_t& examined,
                                 bool& limit_reached) {
    if (examined >= kMaxPassNodes) {
        limit_reached = true;
        return Result<void>::ok();
    }
    ++examined;
    auto paintable = objectIsClass(node, "GeometryInstance3D");
    if (paintable.isErr()) return paintable.error();
    if (paintable.value()) {
        PassSubject subject;
        subject.node = node;
        auto class_name = nodeString(node, "get_class", 201670096LL);
        if (class_name.isErr()) return class_name.error();
        subject.class_name = boundUtf8(class_name.value(), 256).value;
        auto path = editor ? logicalPathFromEditedRoot(root, node)
                           : nodeString(node, "get_path", 4075236667LL);
        if (path.isErr()) return path.error();
        subject.path = boundUtf8(path.value(), 1024).value;
        subjects.push_back(std::move(subject));
    }

    auto include_internal = makeScalar(GDEXTENSION_VARIANT_TYPE_BOOL, static_cast<GDExtensionBool>(0));
    if (include_internal.isErr()) return include_internal.error();
    auto children = callObject(node, "Node", "get_children", 873284517LL, {&include_internal.value()});
    if (children.isErr()) return children.error();
    auto size_value = callVariant(children.value(), "size");
    if (size_value.isErr()) return size_value.error();
    auto size = scalarFromVariant<int64_t>(size_value.value(), GDEXTENSION_VARIANT_TYPE_INT);
    if (size.isErr()) return size.error();
    for (int64_t index = 0; index < size.value(); ++index) {
        auto slot = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, index);
        if (slot.isErr()) return slot.error();
        auto child_value = callVariant(children.value(), "get", {&slot.value()});
        if (child_value.isErr()) return child_value.error();
        auto child = objectFromVariant(child_value.value());
        if (child.isErr() || !child.value()) return Error::internal("Godot returned an invalid child node");
        auto walked = collectPassSubjects(child.value(), root, editor, subjects, examined, limit_reached);
        if (walked.isErr()) return walked.error();
        if (limit_reached) break;
    }
    return Result<void>::ok();
}

// Holds what every painted node had before, and puts it back.
//
// The previous material is kept as a Variant rather than as a pointer on
// purpose: an inline material's only reference is often the override itself, so
// a raw pointer would be left pointing at a freed resource the moment the
// replacement went on.
class MaterialOverrides {
public:
    ~MaterialOverrides() { (void)restore(); }

    Result<void> paint(const std::vector<PassSubject>& subjects,
                       const std::vector<GDExtensionObjectPtr>& materials) {
        for (size_t index = 0; index < subjects.size(); ++index) {
            auto previous = callObject(subjects[index].node, "GeometryInstance3D",
                                       "get_material_override", 5934680LL);
            if (previous.isErr()) return previous.error();
            m_previous.emplace_back(subjects[index].node, std::move(previous.value()));
            auto replacement = makeObject(materials[index]);
            if (replacement.isErr()) return replacement.error();
            auto applied = callObject(subjects[index].node, "GeometryInstance3D",
                                      "set_material_override", 2757459619LL, {&replacement.value()});
            if (applied.isErr()) return applied.error();
        }
        return Result<void>::ok();
    }

    // Every node is attempted even when one fails, because stopping at the
    // first failure would leave the rest of the scene wearing a debug material.
    Result<void> restore() {
        Result<void> outcome = Result<void>::ok();
        for (auto& entry : m_previous) {
            auto applied = callObject(entry.first, "GeometryInstance3D", "set_material_override",
                                      2757459619LL, {&entry.second});
            if (applied.isErr() && outcome.isOk()) outcome = applied.error();
        }
        m_previous.clear();
        return outcome;
    }

private:
    std::vector<std::pair<GDExtensionObjectPtr, VariantValue>> m_previous;
};

Result<GDExtensionObjectPtr> makePassShader(const std::string& kind) {
    NativeName shader_class("Shader");
    auto shader = constructObject(shader_class.ptr());
    if (!shader) return Error(501, "Godot ClassDB could not construct a Shader");
    auto code = makeString(passShaderSource(kind));
    if (code.isErr()) return code.error();
    auto applied = callObject(shader, "Shader", "set_code", 83702148LL, {&code.value()});
    if (applied.isErr()) return applied.error();
    return shader;
}

Result<GDExtensionObjectPtr> makePassMaterial(GDExtensionObjectPtr shader) {
    NativeName material_class("ShaderMaterial");
    auto material = constructObject(material_class.ptr());
    if (!material) return Error(501, "Godot ClassDB could not construct a ShaderMaterial");
    auto shader_value = makeObject(shader);
    if (shader_value.isErr()) return shader_value.error();
    auto applied = callObject(material, "ShaderMaterial", "set_shader", 3341921675LL,
                              {&shader_value.value()});
    if (applied.isErr()) return applied.error();
    return material;
}

// The shader takes the colour in the 0..1 range the framebuffer will store, so
// the byte the legend matches on is the byte the palette names.
Result<VariantValue> makeColorVector3(const runtime::SegmentationColor& colour) {
    return makeVector3(static_cast<double>(colour.r) / 255.0,
                       static_cast<double>(colour.g) / 255.0,
                       static_cast<double>(colour.b) / 255.0);
}

Result<void> setPassParameter(GDExtensionObjectPtr material, const char* name, VariantValue& value) {
    auto parameter = makeStringName(name);
    if (parameter.isErr()) return parameter.error();
    auto applied = callObject(material, "ShaderMaterial", "set_shader_parameter", 3776071444LL,
                              {&parameter.value(), &value});
    return applied.isOk() ? Result<void>::ok() : Result<void>(applied.error());
}

// The far distance a depth pass divides by. The camera's own far plane is the
// honest default: it is the distance past which that camera draws nothing.
Result<double> cameraFarPlane(const std::string& camera_identifier, const std::string& session_kind) {
    auto required = requireMethodBind("Camera3D", "get_far", 1740695150LL);
    if (required.isErr()) return Error(501, required.error().message);
    Result<GDExtensionObjectPtr> camera = Error::notFound("No camera");
    if (session_kind == "game") {
        auto tree = liveSceneTree();
        if (tree.isErr()) return tree.error();
        auto root = liveSceneTreeRoot(tree.value());
        if (root.isErr()) return root.error();
        auto found = callObject(root.value(), "Viewport", "get_camera_3d", 2285090890LL);
        if (found.isErr()) return found.error();
        auto object = objectFromVariant(found.value());
        if (object.isErr() || !object.value()) {
            return Error(409, "The game viewport has no 3D camera to take a far plane from");
        }
        camera = object.value();
    } else {
        (void)camera_identifier;
        auto editor = editorInterface();
        if (editor.isErr()) return editor.error();
        auto root = editedSceneRoot(editor.value());
        if (root.isErr()) return root.error();
        auto viewport = callObject(root.value(), "Node", "get_viewport", 3596683776LL);
        if (viewport.isErr()) return viewport.error();
        auto viewport_object = objectFromVariant(viewport.value());
        if (viewport_object.isErr() || !viewport_object.value()) {
            return Error(409, "The edited scene is not in a viewport with a camera");
        }
        auto found = callObject(viewport_object.value(), "Viewport", "get_camera_3d", 2285090890LL);
        if (found.isErr()) return found.error();
        auto object = objectFromVariant(found.value());
        if (object.isErr() || !object.value()) {
            return Error(409, "No 3D camera is rendering this viewport to take a far plane from");
        }
        camera = object.value();
    }
    if (camera.isErr()) return camera.error();
    auto far_value = callObject(camera.value(), "Camera3D", "get_far", 1740695150LL);
    if (far_value.isErr()) return far_value.error();
    auto number = scalarFromVariant<double>(far_value.value(), GDEXTENSION_VARIANT_TYPE_FLOAT);
    if (number.isErr()) return number.error();
    if (!(number.value() > 0.0)) return Error(409, "The camera far plane is not a distance");
    return number.value();
}

} // namespace

Result<MultipassCapture> GodotBridge::captureViewportPasses(const std::vector<std::string>& passes,
                                                            const std::string& camera_identifier,
                                                            const std::string& session_kind,
                                                            double requested_depth_far) {
    for (const auto& bind : {std::make_tuple("GeometryInstance3D", "get_material_override", 5934680LL),
                             std::make_tuple("GeometryInstance3D", "set_material_override", 2757459619LL),
                             std::make_tuple("Shader", "set_code", 83702148LL),
                             std::make_tuple("ShaderMaterial", "set_shader", 3341921675LL),
                             std::make_tuple("ShaderMaterial", "set_shader_parameter", 3776071444LL)}) {
        auto required = requireMethodBind(std::get<0>(bind), std::get<1>(bind), std::get<2>(bind));
        if (required.isErr()) return Error(501, required.error().message);
    }

    const bool editor = session_kind != "game";
    Result<GDExtensionObjectPtr> root = Error::internal("unresolved");
    if (editor) {
        auto interface = editorInterface();
        if (interface.isErr()) return interface.error();
        root = editedSceneRoot(interface.value());
    } else {
        auto tree = liveSceneTree();
        if (tree.isErr()) return tree.error();
        root = liveSceneTreeRoot(tree.value());
    }
    if (root.isErr()) return root.error();

    MultipassCapture capture;
    const bool wants_depth =
        std::find(passes.begin(), passes.end(), "depth") != passes.end();
    if (wants_depth) {
        if (requested_depth_far > 0.0) {
            capture.depth_far = requested_depth_far;
        } else {
            auto far_plane = cameraFarPlane(camera_identifier, session_kind);
            if (far_plane.isErr()) return far_plane.error();
            capture.depth_far = far_plane.value();
        }
    }

    std::vector<PassSubject> subjects;
    size_t examined = 0;
    bool limit_reached = false;
    const bool needs_geometry =
        std::any_of(passes.begin(), passes.end(),
                    [](const std::string& kind) { return kind != "color"; });
    if (needs_geometry) {
        auto walked = collectPassSubjects(root.value(), root.value(), editor, subjects, examined,
                                          limit_reached);
        if (walked.isErr()) return walked.error();
    }
    capture.examined = static_cast<int>(examined);
    capture.painted = static_cast<int>(subjects.size());
    capture.scan_limit_reached = limit_reached;

    for (const auto& kind : passes) {
        if (kind == "color") {
            auto frame = editor ? captureEditorViewport(camera_identifier) : captureGameViewport();
            if (frame.isErr()) return frame.error();
            capture.frames.push_back(PassFrame{kind, std::move(frame.value())});
            continue;
        }
        // A scene with nothing to paint still answers, with a picture of an
        // empty pass rather than an error about a scene that is simply bare.
        auto shader = makePassShader(kind);
        if (shader.isErr()) return shader.error();

        auto material = makePassMaterial(shader.value());
        if (material.isErr()) return material.error();
        if (kind == "depth") {
            auto far_value = makeScalar(GDEXTENSION_VARIANT_TYPE_FLOAT, capture.depth_far);
            if (far_value.isErr()) return far_value.error();
            auto applied = setPassParameter(material.value(), "didi_far", far_value.value());
            if (applied.isErr()) return applied.error();
        }
        // Who this pass paints, which is everyone except where segmentation
        // runs out of palette. A pass after it must still paint the whole
        // scene, so the shared subject list is not what gets shortened.
        std::vector<PassSubject> painted_subjects = subjects;
        std::vector<GDExtensionObjectPtr> materials(painted_subjects.size(), material.value());
        if (kind == "segmentation") {
            // The one pass where every node wears a different material, because
            // the colour is the answer rather than the subject.
            const auto& palette = runtime::segmentationPalette();
            if (painted_subjects.size() > palette.size()) {
                // A node with no entry left is not painted at all, so it keeps
                // the colour it already had rather than borrowing another
                // node's and being reported as it.
                for (size_t index = palette.size(); index < painted_subjects.size(); ++index) {
                    capture.unsegmented.push_back(painted_subjects[index].path);
                }
                painted_subjects.resize(palette.size());
                materials.resize(palette.size());
            }
            for (size_t index = 0; index < painted_subjects.size(); ++index) {
                auto node_material = makePassMaterial(shader.value());
                if (node_material.isErr()) return node_material.error();
                auto colour = makeColorVector3(palette[index]);
                if (colour.isErr()) return colour.error();
                auto applied = setPassParameter(node_material.value(), "didi_colour", colour.value());
                if (applied.isErr()) return applied.error();
                materials[index] = node_material.value();
                capture.segmented.push_back(
                    SegmentedNode{painted_subjects[index].path, painted_subjects[index].class_name,
                                  index});
            }
        }

        MaterialOverrides overrides;
        auto painted = overrides.paint(painted_subjects, materials);
        if (painted.isErr()) return painted.error();
        auto drawn = forceDraw();
        if (drawn.isErr()) return drawn.error();
        auto frame = editor ? captureEditorViewport(camera_identifier) : captureGameViewport();
        // The restore runs whatever the capture did, and its failure is the one
        // that gets reported: a scene left wearing a debug material matters more
        // than a frame that did not arrive.
        auto restored = overrides.restore();
        auto redrawn = forceDraw();
        if (restored.isErr()) {
            DIDI_LOG_ERROR("GODOT_BRIDGE", "Pass materials could not be restored: ",
                           restored.error().message);
            return restored.error();
        }
        if (redrawn.isErr()) return redrawn.error();
        if (frame.isErr()) return frame.error();
        capture.frames.push_back(PassFrame{kind, std::move(frame.value())});
    }
    return capture;
}

Result<ViewportPixels> GodotBridge::captureEditorViewport(const std::string& camera_identifier) {
    auto editor_result = editorInterface();
    if (editor_result.isErr()) return editor_result.error();
    const bool capture_2d =
        camera_identifier == "editor_2d" || camera_identifier == "active_editor_view_2d";
    Result<VariantValue> viewport = capture_2d
        ? callObject(editor_result.value(), "EditorInterface", "get_editor_viewport_2d", 3750751911LL)
        : [&]() -> Result<VariantValue> {
            auto index = makeScalar(GDEXTENSION_VARIANT_TYPE_INT, static_cast<int64_t>(0));
            if (index.isErr()) return index.error();
            return callObject(editor_result.value(), "EditorInterface", "get_editor_viewport_3d", 1970834490LL, {&index.value()});
        }();
    if (viewport.isErr()) return viewport.error();
    auto viewport_object = objectFromVariant(viewport.value());
    if (viewport_object.isErr() || !viewport_object.value()) {
        return Error::notFound("Editor SubViewport is unavailable");
    }
    return captureViewportObject(viewport_object.value(), camera_identifier);
}

// A game has one root viewport and no camera selection to make. The frame is
// already in the process Didi is attached to, so a caller that can pause the
// game, step it and read its tree can now also see it.
Result<ViewportPixels> GodotBridge::captureGameViewport() {
    auto tree = liveSceneTree();
    if (tree.isErr()) return tree.error();
    auto root = liveSceneTreeRoot(tree.value());
    if (root.isErr()) return root.error();
    return captureViewportObject(root.value(), "root_viewport");
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
            break;
        default:
            return PropertyTypeMatch::UnsupportedPropertyType;
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
