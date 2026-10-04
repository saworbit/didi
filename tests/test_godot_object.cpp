// constructObject, held to its one rule with fakes in place of the engine
// (#1166): an object that cannot be sent NOTIFICATION_POSTINITIALIZE is
// destroyed and never handed back. Two copies of it once returned the
// half-built object instead.

#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/godot_object.hpp"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#define ASSERT_TRUE(cond) if (!(cond)) throw std::runtime_error("Assertion failed: " #cond);
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

void registerTest(const std::string& name, std::function<void()> fn);

namespace {

using didi::godot::GodotApi;

int g_destroyed = 0;
int g_notified = 0;
int64_t g_what = -1;
bool g_has_bind = false;
alignas(16) char g_object_storage[16];
alignas(16) char g_bind_storage[16];

GDExtensionObjectPtr fakeConstruct(GDExtensionConstStringNamePtr) { return g_object_storage; }
void fakeDestroy(GDExtensionObjectPtr object) {
    if (object == g_object_storage) ++g_destroyed;
}
void fakeStringName(GDExtensionUninitializedStringNamePtr, const char*) {}
GDExtensionPtrDestructor fakeDestructorFor(GDExtensionVariantType) { return nullptr; }
GDExtensionMethodBindPtr fakeBind(GDExtensionConstStringNamePtr, GDExtensionConstStringNamePtr, GDExtensionInt) {
    return g_has_bind ? g_bind_storage : nullptr;
}
void fakePtrcall(GDExtensionMethodBindPtr, GDExtensionObjectPtr object, const GDExtensionConstTypePtr* arguments,
                 GDExtensionTypePtr) {
    if (object == g_object_storage) {
        ++g_notified;
        g_what = *static_cast<const int64_t*>(arguments[0]);
    }
}

// The engine's entry points as fakes for one test, and nothing afterwards, so
// no other test meets an engine that is not there.
class FakeEngine {
public:
    FakeEngine() {
        auto& api = GodotApi::instance();
        api.classdb_construct_object = fakeConstruct;
        api.object_destroy = fakeDestroy;
        api.string_name_new_with_utf8_chars = fakeStringName;
        api.variant_get_ptr_destructor = fakeDestructorFor;
        api.classdb_get_method_bind = fakeBind;
        api.object_method_bind_ptrcall = fakePtrcall;
        g_destroyed = 0;
        g_notified = 0;
        g_what = -1;
    }
    ~FakeEngine() {
        auto& api = GodotApi::instance();
        api.classdb_construct_object = nullptr;
        api.object_destroy = nullptr;
        api.string_name_new_with_utf8_chars = nullptr;
        api.variant_get_ptr_destructor = nullptr;
        api.classdb_get_method_bind = nullptr;
        api.object_method_bind_ptrcall = nullptr;
    }
};

void test_an_object_that_cannot_be_post_initialized_is_destroyed() {
    FakeEngine engine;
    g_has_bind = false;
    ASSERT_TRUE(didi::godot::constructObject(nullptr) == nullptr);
    ASSERT_EQ(g_destroyed, 1);
    ASSERT_EQ(g_notified, 0);
}

void test_an_object_is_post_initialized_before_it_is_handed_back() {
    FakeEngine engine;
    g_has_bind = true;
    ASSERT_TRUE(didi::godot::constructObject(nullptr) == g_object_storage);
    ASSERT_EQ(g_notified, 1);
    ASSERT_EQ(g_what, didi::godot::kNotificationPostInitialize);
    ASSERT_EQ(g_destroyed, 0);
}

struct Register {
    Register() {
        registerTest("GodotObject.UnfinishableConstructionFails",
                     test_an_object_that_cannot_be_post_initialized_is_destroyed);
        registerTest("GodotObject.PostInitializedBeforeHandedBack",
                     test_an_object_is_post_initialized_before_it_is_handed_back);
    }
} g_register;

} // namespace
