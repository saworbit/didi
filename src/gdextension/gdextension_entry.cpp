#include "didi/gdextension/gdextension_interface.h"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/gdextension_ipc.hpp"
#include "didi/gdextension/godot_bridge.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/common/logger.hpp"
#include "didi/offline/deep_domain_support.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#define GDE_EXPORT __declspec(dllexport)
#else
#define GDE_EXPORT __attribute__((visibility("default")))
#endif

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
        if (m_initialized) {
            auto destructor = GodotApi::instance().variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
            if (destructor) destructor(m_storage.data());
        }
    }
    const void* ptr() const { return m_storage.data(); }
    bool valid() const { return m_initialized; }

private:
    alignas(16) std::array<std::byte, 64> m_storage{};
    bool m_initialized{false};
};

bool engineIsEditorHint() {
    auto& api = GodotApi::instance();
    NativeName engine_name("Engine");
    NativeName method_name("is_editor_hint");
    if (!engine_name.valid() || !method_name.valid() || !api.global_get_singleton ||
        !api.classdb_get_method_bind || !api.object_method_bind_ptrcall) {
        DIDI_LOG_ERROR("GDEXTENSION", "Engine.is_editor_hint API is unavailable; classifying session as game");
        return false;
    }
    const auto engine = api.global_get_singleton(engine_name.ptr());
    const auto bind = api.classdb_get_method_bind(engine_name.ptr(), method_name.ptr(), 36873697LL);
    if (!engine || !bind) {
        DIDI_LOG_ERROR("GDEXTENSION", "Engine.is_editor_hint binding is unavailable; classifying session as game");
        return false;
    }
    GDExtensionBool editor_hint = 0;
    api.object_method_bind_ptrcall(bind, engine, nullptr, &editor_hint);
    return editor_hint != 0;
}

std::string fallbackCanonicalProjectPath() {
    std::error_code ec;
    const auto current = std::filesystem::current_path(ec);
    if (ec) return {};
    const auto canonical = std::filesystem::weakly_canonical(current, ec);
    return (ec ? current.lexically_normal() : canonical).string();
}

} // namespace

static void didi_main_loop_frame() {
    EditorHook::instance().processQueue();
}

static void didi_main_loop_shutdown() {
    GodotApi::instance().markMainLoopStopped();
    EditorHook::instance().cancelPendingCommands("Godot main loop is shutting down");
}

static void initialize_didi_module(void *userdata, GDExtensionInitializationLevel p_level) {
    (void)userdata;
    if (p_level == GDEXTENSION_INITIALIZATION_SCENE) {
        const auto kind = engineIsEditorHint() ? "editor" : "game";
        const auto resolved_project = resolveGodotProjectPath();
        const auto project_path = resolved_project.isOk() ? resolved_project.value() : fallbackCanonicalProjectPath();
        if (resolved_project.isErr()) {
            DIDI_LOG_WARN("GDEXTENSION", "Unable to resolve res:// for runtime session; using canonical process path: ",
                          resolved_project.error().message);
        }
        DIDI_LOG_INFO("GDEXTENSION", "Initializing Didi GDExtension runtime session for Godot ", kind);
        if (GodotApi::instance().isLiveReady() && !GDExtensionIpc::instance().isRunning()) {
            if (!GDExtensionIpc::instance().start(kind, project_path)) {
                DIDI_LOG_ERROR("GDEXTENSION", "Unable to start authenticated runtime IPC session");
            }
        } else if (!GodotApi::instance().isLiveReady()) {
            DIDI_LOG_ERROR("GDEXTENSION", "Main-loop callback unavailable; refusing to start live IPC");
        }
    }
}

static void deinitialize_didi_module(void *userdata, GDExtensionInitializationLevel p_level) {
    (void)userdata;
    if (p_level == GDEXTENSION_INITIALIZATION_SCENE) {
        DIDI_LOG_INFO("GDEXTENSION", "Deinitializing Didi GDExtension module");
        GodotApi::instance().markMainLoopStopped();
        EditorHook::instance().cancelPendingCommands("Godot editor extension is shutting down");
        if (GDExtensionIpc::instance().isRunning()) {
            GDExtensionIpc::instance().stop();
        }
    }
}

static void initialize_offline_helper(void*, GDExtensionInitializationLevel) {}
static void deinitialize_offline_helper(void*, GDExtensionInitializationLevel) {}

} // namespace godot
} // namespace didi

extern "C" {

GDE_EXPORT GDExtensionBool didi_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
                                            GDExtensionClassLibraryPtr p_library,
                                            GDExtensionInitialization *r_initialization) {
    const auto* offline_helper = std::getenv(didi::offline::kOfflineHelperEnvironment);
    if (offline_helper && std::string(offline_helper) == "1") {
        didi::godot::GodotApi::instance().init(p_get_proc_address, p_library, r_initialization);
        r_initialization->initialize = didi::godot::initialize_offline_helper;
        r_initialization->deinitialize = didi::godot::deinitialize_offline_helper;
        r_initialization->userdata = nullptr;
        r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_CORE;
        DIDI_LOG_INFO("GDEXTENSION", "Didi runtime disabled for isolated offline helper");
        return 1;
    }
    didi::godot::GodotApi::instance().init(p_get_proc_address, p_library, r_initialization);

    GDExtensionMainLoopCallbacks main_loop_callbacks{};
    main_loop_callbacks.frame_func = didi::godot::didi_main_loop_frame;
    main_loop_callbacks.shutdown_func = didi::godot::didi_main_loop_shutdown;
    if (!didi::godot::GodotApi::instance().registerMainLoop(main_loop_callbacks)) {
        DIDI_LOG_ERROR("GDEXTENSION", "Godot 4.7+ register_main_loop_callbacks API is required for live execution");
    }

    r_initialization->initialize = didi::godot::initialize_didi_module;
    r_initialization->deinitialize = didi::godot::deinitialize_didi_module;
    r_initialization->userdata = nullptr;
    r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_CORE;

    DIDI_LOG_INFO("GDEXTENSION", "Didi GDExtension library entry point successfully bound");
    return 1;
}

}
