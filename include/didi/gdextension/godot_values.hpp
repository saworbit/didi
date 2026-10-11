#pragma once

#include "didi/common/json.hpp"
#include "didi/common/types.hpp"
#include "didi/gdextension/gdextension_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace didi {
namespace godot {

// One definition of the wrappers every GDExtension source holds Godot values
// in, and of the error answers they build. Each file kept its own copy, so a
// fix had to reach every copy by hand and nothing checked that they agreed
// (#1255, #1266, #1267).

inline constexpr size_t kOpaqueBytes = 64;
using Opaque = std::array<std::byte, kOpaqueBytes>;

// A built-in value the engine constructs in place, destroyed with the
// destructor for its type once something marks it initialized.
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

// A Variant, nil until something writes into it.
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

// A StringName built from UTF-8, for naming classes, methods and properties.
class NativeName {
public:
    explicit NativeName(const char* value) {
        auto& api = GodotApi::instance();
        if (api.string_name_new_with_utf8_chars) {
            api.string_name_new_with_utf8_chars(m_storage.data(), value);
            m_initialized = true;
        }
    }
    explicit NativeName(const std::string& value) : NativeName(value.c_str()) {}
    NativeName(const NativeName&) = delete;
    NativeName& operator=(const NativeName&) = delete;
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

inline json errorJson(int code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

inline json errorJson(int code, const std::string& message, json data) {
    return {{"error", {{"code", code}, {"message", message}, {"data", std::move(data)}}}};
}

// A Result's error as an answer, its data kept. Rebuilding one from its code
// and message drops the data, and with it the code that names the refusal's
// fix (#1222, #1267).
inline json errorJson(const Error& error) {
    if (error.data.is_null()) return errorJson(error.code, error.message);
    return errorJson(error.code, error.message, error.data);
}


// Values built from text keep a leading byte-order mark, which Godot's UTF-8
// reader drops on its own (#948).
Result<VariantValue> variantFromNative(GDExtensionVariantType type, void* native);
void constructNativeString(NativeValue& native, const std::string& text);
Result<VariantValue> makeString(const std::string& text);
Result<VariantValue> makeStringName(const std::string& text);
Result<VariantValue> makeNodePath(const std::string& text);
Result<VariantValue> makeObject(GDExtensionObjectPtr object);

template <typename T>
Result<VariantValue> makeScalar(GDExtensionVariantType type, T value) {
    return variantFromNative(type, &value);
}

template <typename T>
Result<T> scalarFromVariant(VariantValue& value, GDExtensionVariantType type) {
    auto ctor = GodotApi::instance().get_variant_to_type_constructor(type);
    if (!ctor) return Error::internal("Missing Variant-to-native constructor for type " + std::to_string(type));
    T result{};
    ctor(&result, value.ptr());
    return result;
}

Result<GDExtensionObjectPtr> objectFromVariant(VariantValue& value);
Result<GDExtensionObjectPtr> singleton(const std::string& name);
Result<VariantValue> callObject(GDExtensionObjectPtr object, const char* class_name,
                                const char* method_name, int64_t hash,
                                const std::vector<const VariantValue*>& arguments = {});
Result<VariantValue> callVariant(VariantValue& target, const std::string& method_name,
                                 const std::vector<const VariantValue*>& arguments = {});

} // namespace godot
} // namespace didi
