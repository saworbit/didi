#include "didi/gdextension/engine_output_logger.hpp"

#include "didi/gdextension/gdextension_interface.h"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/common/logger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace didi {
namespace godot {

namespace {

// Method hashes for the Logger virtuals. A hash identifies a signature, so
// matching on it is what keeps this from binding to a differently-shaped
// method on a future engine. These were read from the engine's own
// extension_api.json on 4.5.1, 4.6.2 and 4.7.2, where they are identical.
// If a later engine changes them the virtual is simply never claimed and
// capture goes quiet -- the extension still loads.
constexpr uint32_t kLogMessageHash = 2678287736u;
constexpr uint32_t kLogErrorHash = 27079556u;
// OS.add_logger and OS.remove_logger share a signature, so they share a hash.
constexpr int64_t kOsLoggerMethodHash = 4261188958LL;

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
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (destructor) destructor(m_storage.data());
        }
    }
    NativeName(const NativeName&) = delete;
    NativeName& operator=(const NativeName&) = delete;
    const void* ptr() const { return m_storage.data(); }
    bool valid() const { return m_initialized; }

private:
    alignas(16) std::array<std::byte, 64> m_storage{};
    bool m_initialized{false};
};

// See the twin in godot_bridge.cpp. Only for constructions this code owns, never
// from inside create_instance_func.
GDExtensionObjectPtr constructObject(GDExtensionConstStringNamePtr class_name) {
    auto& api = GodotApi::instance();
    if (!api.classdb_construct_object) return nullptr;
    auto object = api.classdb_construct_object(class_name);
    if (!object) return nullptr;
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

// Reads a Godot String the engine handed us into a std::string.
//
// This is the only engine call made from inside a log callback. It is a pure
// conversion that cannot itself emit output, which is what makes it safe here:
// anything that could log would re-enter this logger and deadlock the editor.
std::string toUtf8(const void* string_ptr) {
    auto& api = GodotApi::instance();
    if (!string_ptr || !api.string_to_utf8_chars) return std::string();
    const auto length = api.string_to_utf8_chars(string_ptr, nullptr, 0);
    if (length <= 0) return std::string();
    std::string out(static_cast<size_t>(length), 0);
    api.string_to_utf8_chars(string_ptr, out.data(), length);
    return out;
}

GDExtensionObjectPtr g_logger_object = nullptr;
bool g_class_registered = false;

// --- Virtual implementations ----------------------------------------------
//
// These run on whatever thread produced the output, so they may only touch the
// ring (which is mutex-guarded) and must never call DIDI_LOG: Didi's own logger
// can write to Godot's output, which would call straight back in here.

void didiLogMessage(GDExtensionClassInstancePtr, const GDExtensionConstTypePtr* args,
                    GDExtensionTypePtr) {
    if (!args) return;
    std::string message = toUtf8(args[0]);
    const bool is_error = args[1] && *reinterpret_cast<const GDExtensionBool*>(args[1]) != 0;
    // print() hands the line terminator to the logger. Records are structured,
    // so the terminator is noise a reader would have to strip themselves.
    while (!message.empty() && (message.back() == 0x0A || message.back() == 0x0D)) {
        message.pop_back();
    }
    if (message.empty()) return;
    EditorHook::instance().engineOutput().append(is_error ? "error" : "info", "godot", message);
}

void didiLogError(GDExtensionClassInstancePtr, const GDExtensionConstTypePtr* args,
                  GDExtensionTypePtr) {
    if (!args) return;
    const std::string function = toUtf8(args[0]);
    const std::string file = toUtf8(args[1]);
    const int64_t line = args[2] ? *reinterpret_cast<const int64_t*>(args[2]) : 0;
    const std::string code = toUtf8(args[3]);
    const std::string rationale = toUtf8(args[4]);
    const int64_t error_type = args[6] ? *reinterpret_cast<const int64_t*>(args[6]) : 0;

    // Logger::ErrorType: 0 error, 1 warning, 2 script, 3 shader.
    const char* level = (error_type == 1) ? "warning" : "error";
    // The engine puts human-readable text in `rationale` when there is one and
    // leaves `code` as the raw failing expression, so prefer the former.
    const std::string message = rationale.empty() ? code : rationale;

    EditorHook::instance().engineOutput().append(
        level, "godot", message,
        {{"function", function}, {"file", file}, {"line", line}, {"error_type", error_type}});
}

GDExtensionClassCallVirtual getVirtual(void*, GDExtensionConstStringNamePtr, uint32_t hash) {
    if (hash == kLogMessageHash) return didiLogMessage;
    if (hash == kLogErrorHash) return didiLogError;
    return nullptr;
}

// A token handed to the engine as this class's per-instance data. The capture
// ring is process-wide, so there is no per-instance state to carry; the engine
// only requires the pointer be non-null for the object to count as extended.
int g_instance_token = 0;

// Constructing our class means constructing the engine-side `Logger` and then
// telling the engine that object is an instance of ours. `classdb_construct_object`
// routes here, so returning null from this is what makes construction fail.
GDExtensionObjectPtr createInstance(void*, GDExtensionBool) {
    auto& api = GodotApi::instance();
    NativeName parent_name("Logger");
    NativeName class_name("DidiEngineOutputLogger");
    if (!parent_name.valid() || !class_name.valid() || !api.classdb_construct_object ||
        !api.object_set_instance) {
        return nullptr;
    }
    auto object = api.classdb_construct_object(parent_name.ptr());
    if (!object) return nullptr;
    api.object_set_instance(object, class_name.ptr(), &g_instance_token);
    return object;
}

void freeInstance(void*, GDExtensionClassInstancePtr) {
    // No per-instance allocation to release -- see g_instance_token.
}

// Required for the extension to stay reloadable. The .gdextension declares
// `reloadable = true`, and Godot disables reloading for the *whole* extension
// if any registered class cannot be recreated -- so omitting this silently
// costs hot reload for every other part of Didi. There is no per-instance
// state to carry across a reload, so recreating is just handing back the
// token again.
GDExtensionClassInstancePtr recreateInstance(void*, GDExtensionObjectPtr) {
    return &g_instance_token;
}

// Logger derives from RefCounted, so the instance lives by reference count
// rather than by our holding a pointer. These keep that count balanced.
bool callRefCounted(GDExtensionObjectPtr object, const char* method_name) {
    auto& api = GodotApi::instance();
    NativeName class_name("RefCounted");
    NativeName method(method_name);
    if (!object || !class_name.valid() || !method.valid() || !api.classdb_get_method_bind ||
        !api.object_method_bind_ptrcall) {
        return false;
    }
    // init_ref, reference and unreference share a signature, so they share a hash.
    const auto bind = api.classdb_get_method_bind(class_name.ptr(), method.ptr(), 2240911060LL);
    if (!bind) return false;
    GDExtensionBool taken = 0;
    api.object_method_bind_ptrcall(bind, object, nullptr, &taken);
    return taken != 0;
}

bool callOsLogger(const char* method_name, GDExtensionObjectPtr logger) {
    auto& api = GodotApi::instance();
    NativeName os_name("OS");
    NativeName method(method_name);
    if (!os_name.valid() || !method.valid() || !api.global_get_singleton ||
        !api.classdb_get_method_bind || !api.object_method_bind_ptrcall) {
        return false;
    }
    const auto os = api.global_get_singleton(os_name.ptr());
    const auto bind = api.classdb_get_method_bind(os_name.ptr(), method.ptr(), kOsLoggerMethodHash);
    if (!os || !bind) return false;
    const void* arguments[] = {&logger};
    api.object_method_bind_ptrcall(bind, os, arguments, nullptr);
    return true;
}

} // namespace

void installEngineOutputLogger() {
    auto& api = GodotApi::instance();
    if (!api.classdb_register_extension_class4 || !api.classdb_construct_object || !api.getLibrary()) {
        DIDI_LOG_WARN("GDEXTENSION",
                      "Engine output capture is unavailable: this engine does not expose the "
                      "class-registration interface. runtime_read_output will return no records.");
        return;
    }

    NativeName class_name("DidiEngineOutputLogger");
    NativeName parent_name("Logger");
    if (!class_name.valid() || !parent_name.valid()) return;

    GDExtensionClassCreationInfo4 info{};
    info.is_runtime = 1;
    info.create_instance_func = createInstance;
    info.free_instance_func = freeInstance;
    info.recreate_instance_func = recreateInstance;
    info.get_virtual_func = getVirtual;

    api.classdb_register_extension_class4(api.getLibrary(), class_name.ptr(), parent_name.ptr(), &info);
    g_class_registered = true;

    // Post-initialization is this caller's job: classdb_construct_object2 is
    // ClassDB::instantiate_without_postinitialization. Note the deliberate
    // asymmetry with createInstance above, which must NOT notify: that runs as
    // the engine's create_instance_func, before the instance is fully bound,
    // and the engine sends the notification once it returns.
    g_logger_object = constructObject(class_name.ptr());
    if (g_logger_object) {
        // Take the first reference ourselves. The engine adds its own when
        // OS.add_logger stores the instance, and drops it on remove_logger;
        // ours is what keeps the object alive across that whole window.
        callRefCounted(g_logger_object, "init_ref");
    }
    if (!g_logger_object) {
        DIDI_LOG_WARN("GDEXTENSION",
                      "Engine output capture is unavailable: the logger instance could not be "
                      "constructed. runtime_read_output will return no records.");
        return;
    }

    if (!callOsLogger("add_logger", g_logger_object)) {
        DIDI_LOG_WARN("GDEXTENSION",
                      "Engine output capture is unavailable: OS.add_logger could not be called. "
                      "runtime_read_output will return no records.");
        return;
    }
    DIDI_LOG_INFO("GDEXTENSION", "Engine output capture is active");
}

void removeEngineOutputLogger() {
    auto& api = GodotApi::instance();
    // Order matters: the engine must stop holding the instance before the class
    // describing it goes away, or a later log line calls into freed code.
    if (g_logger_object) {
        callOsLogger("remove_logger", g_logger_object);
        // Release the reference taken at install. When this drops the count to
        // zero the engine destroys the object and calls freeInstance.
        callRefCounted(g_logger_object, "unreference");
        g_logger_object = nullptr;
    }
    if (g_class_registered && api.classdb_unregister_extension_class && api.getLibrary()) {
        NativeName class_name("DidiEngineOutputLogger");
        if (class_name.valid()) {
            api.classdb_unregister_extension_class(api.getLibrary(), class_name.ptr());
        }
        g_class_registered = false;
    }
}

} // namespace godot
} // namespace didi
