#include "didi/gdextension/godot_values.hpp"

#include <optional>
#include <string>
#include <vector>

namespace didi {
namespace godot {

namespace {

// The text as UTF-32 when it starts with a byte-order mark, and nothing
// otherwise, including for bytes that are not UTF-8.
//
// Godot's UTF-8 reader drops a leading U+FEFF, so "\xEF\xBB\xBFnote" reached
// the engine as "note", and the tool that set it compared its read-back with
// the same shortened value and called it applied (#948). The engine itself
// keeps the mark in a String, a StringName, a NodePath, a node name and a
// Label's text on 4.5.1, 4.6.2 and 4.7.2; only the reader loses it, so that
// text is handed over as UTF-32 instead. Everything else keeps the UTF-8
// reader, and what it does with bytes that are not UTF-8.
std::optional<std::u32string> utf32WhenMarkLeads(const std::string& text) {
    if (text.rfind("\xEF\xBB\xBF", 0) != 0) return std::nullopt;
    std::u32string wide;
    wide.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                                                 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) return std::nullopt;
        char32_t code = length == 1 ? lead : lead & (0x7F >> length);
        for (size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return std::nullopt;
            code = (code << 6) | (next & 0x3F);
        }
        wide.push_back(code);
        i += length;
    }
    return wide;
}

}  // namespace

Result<VariantValue> variantFromNative(GDExtensionVariantType type, void* native) {
    auto ctor = GodotApi::instance().get_variant_from_type_constructor(type);
    if (!ctor) return Error::internal("Missing Variant constructor for type " + std::to_string(type));
    VariantValue value(VariantValue::Uninitialized{});
    ctor(value.ptr(), native);
    value.markInitialized();
    return std::move(value);
}

void constructNativeString(NativeValue& native, const std::string& text) {
    auto& api = GodotApi::instance();
    const auto wide = api.string_new_with_utf32_chars_and_len ? utf32WhenMarkLeads(text)
                                                              : std::optional<std::u32string>{};
    if (wide.has_value()) {
        api.string_new_with_utf32_chars_and_len(native.ptr(), wide->data(),
                                                static_cast<GDExtensionInt>(wide->size()));
    } else {
        api.string_new_with_utf8_chars(native.ptr(), text.c_str());
    }
    native.markInitialized();
}

Result<VariantValue> makeString(const std::string& text) {
    auto& api = GodotApi::instance();
    if (!api.string_new_with_utf8_chars) return Error::internal("Godot String constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_STRING);
    constructNativeString(native, text);
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_STRING, native.ptr());
}

Result<VariantValue> makeStringName(const std::string& text) {
    auto& api = GodotApi::instance();
    if (!api.string_name_new_with_utf8_chars) return Error::internal("Godot StringName constructor is unavailable");
    NativeValue native(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0 && api.string_new_with_utf8_chars) {
        // StringName has no UTF-32 constructor, so the mark goes through a
        // String first; StringName(String) is constructor 2.
        NativeValue native_string(GDEXTENSION_VARIANT_TYPE_STRING);
        constructNativeString(native_string, text);
        auto ctor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME, 2);
        if (!ctor) return Error::internal("Godot StringName(String) constructor is unavailable");
        const void* args[] = {native_string.ptr()};
        ctor(native.ptr(), args);
    } else {
        api.string_name_new_with_utf8_chars(native.ptr(), text.c_str());
    }
    native.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_STRING_NAME, native.ptr());
}

Result<VariantValue> makeNodePath(const std::string& text) {
    auto& api = GodotApi::instance();
    NativeValue native_string(GDEXTENSION_VARIANT_TYPE_STRING);
    constructNativeString(native_string, text);

    auto ctor = api.variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_NODE_PATH, 2);
    if (!ctor) return Error::internal("Godot NodePath(String) constructor is unavailable");
    NativeValue native_path(GDEXTENSION_VARIANT_TYPE_NODE_PATH);
    const void* args[] = {native_string.ptr()};
    ctor(native_path.ptr(), args);
    native_path.markInitialized();
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_NODE_PATH, native_path.ptr());
}

Result<VariantValue> makeObject(GDExtensionObjectPtr object) {
    return variantFromNative(GDEXTENSION_VARIANT_TYPE_OBJECT, &object);
}

// Godot hands an extension the pointer a Variant holds without asking whether
// the object still exists (Variant::operator Object *, read at 4.5.1-stable),
// so a value read from project state can name a freed node: an enemy's target
// after the target was freed. Calling into one crashes once its memory is
// reused, and reads garbage until then (#1266). A Variant's truth asks ObjectDB,
// so a freed object comes back as null here and nothing calls into it.
Result<GDExtensionObjectPtr> objectFromVariant(VariantValue& value) {
    auto object = scalarFromVariant<GDExtensionObjectPtr>(value, GDEXTENSION_VARIANT_TYPE_OBJECT);
    if (object.isErr() || !object.value()) return object;
    const auto& api = GodotApi::instance();
    if (!api.variant_booleanize) return Error::internal("Godot's Variant truth test is unavailable");
    if (!api.variant_booleanize(value.ptr())) return GDExtensionObjectPtr{nullptr};
    return object;
}

// The engine's function pointers are checked before they are called, as the
// sandbox's copy did, and a failure names the call and its error code, as the
// bridge's did.
Result<GDExtensionObjectPtr> singleton(const std::string& name) {
    auto& api = GodotApi::instance();
    NativeName native_name(name);
    if (!native_name.valid()) return Error::internal("Failed to construct singleton name: " + name);
    if (!api.global_get_singleton) return Error::internal("Godot singleton API is unavailable");
    auto object = api.global_get_singleton(native_name.ptr());
    if (!object) return Error::notConnected("Godot singleton is unavailable: " + name);
    return object;
}

Result<VariantValue> callObject(GDExtensionObjectPtr object, const char* class_name,
                                const char* method_name, int64_t hash,
                                const std::vector<const VariantValue*>& arguments) {
    if (!object) return Error::notFound(std::string("Cannot call ") + method_name + " on a null Godot object");
    auto& api = GodotApi::instance();
    NativeName klass(class_name);
    NativeName method(method_name);
    if (!klass.valid() || !method.valid()) return Error::internal("Failed to construct Godot method identifiers");
    if (!api.classdb_get_method_bind || !api.object_method_bind_call) {
        return Error::internal("Godot method binding API is unavailable");
    }
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

Result<VariantValue> callVariant(VariantValue& target, const std::string& method_name,
                                 const std::vector<const VariantValue*>& arguments) {
    auto& api = GodotApi::instance();
    NativeName method(method_name);
    if (!method.valid() || !api.variant_call) return Error::internal("Godot Variant call API is unavailable");
    std::vector<const void*> raw_args;
    raw_args.reserve(arguments.size());
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

} // namespace godot
} // namespace didi
