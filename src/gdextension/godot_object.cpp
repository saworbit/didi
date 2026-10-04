#include "didi/gdextension/godot_object.hpp"

#include "didi/gdextension/gdextension_api.hpp"

#include <array>
#include <cstddef>

namespace didi {
namespace godot {
namespace {

class NativeName {
public:
    explicit NativeName(const char* value) {
        auto& api = GodotApi::instance();
        if (api.string_name_new_with_utf8_chars) {
            api.string_name_new_with_utf8_chars(m_storage.data(), value);
            m_initialized = true;
        }
    }
    ~NativeName() {
        if (!m_initialized) return;
        auto destructor = GodotApi::instance().variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
        if (destructor) destructor(m_storage.data());
    }
    NativeName(const NativeName&) = delete;
    NativeName& operator=(const NativeName&) = delete;
    const void* ptr() const { return m_storage.data(); }
    bool valid() const { return m_initialized; }

private:
    alignas(16) std::array<std::byte, 64> m_storage{};
    bool m_initialized{false};
};

} // namespace

GDExtensionObjectPtr constructObject(GDExtensionConstStringNamePtr class_name) {
    auto& api = GodotApi::instance();
    if (!api.classdb_construct_object) return nullptr;
    auto object = api.classdb_construct_object(class_name);
    if (!object) return nullptr;

    // A missing bind fails the construction rather than handing the caller the
    // object anyway.
    //
    // This used to return the object, on the reasoning that it left things
    // exactly as they were before this function existed. That reasoning is
    // wrong here: what it was before is the half-built object the header
    // describes, and the case that found this took the editor down with it. A
    // construction that cannot finish is a construction that failed, and every
    // caller already reports that. Handing back something documented to
    // segfault the editor is not the survivable option.
    //
    // The hash is 4023243586 on Godot 4.5.1, 4.6.2 and 4.7.2, checked by
    // dumping the extension API from each, so this is a guard against a future
    // engine rather than a live condition.
    NativeName object_class("Object");
    NativeName notification("notification");
    if (!object_class.valid() || !notification.valid()) {
        api.object_destroy(object);
        return nullptr;
    }
    auto bind = api.classdb_get_method_bind(object_class.ptr(), notification.ptr(),
                                            kObjectNotificationHash);
    if (!bind) {
        api.object_destroy(object);
        return nullptr;
    }
    int64_t what = kNotificationPostInitialize;
    GDExtensionBool reversed = 0;
    const void* arguments[] = {&what, &reversed};
    api.object_method_bind_ptrcall(bind, object, arguments, nullptr);
    return object;
}

} // namespace godot
} // namespace didi
