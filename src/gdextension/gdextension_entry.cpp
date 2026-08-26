#include "didi/gdextension/gdextension_interface.h"
#include "didi/gdextension/gdextension_api.hpp"
#include "didi/gdextension/gdextension_ipc.hpp"
#include "didi/gdextension/editor_hook.hpp"
#include "didi/common/logger.hpp"

#if defined(_WIN32)
#define GDE_EXPORT __declspec(dllexport)
#else
#define GDE_EXPORT __attribute__((visibility("default")))
#endif

namespace didi {
namespace godot {

static void initialize_didi_module(void *userdata, GDExtensionInitializationLevel p_level) {
    if (p_level == GDEXTENSION_INITIALIZATION_EDITOR) {
        DIDI_LOG_INFO("GDEXTENSION", "Initializing Didi GDExtension module for Godot Editor");
        if (!GDExtensionIpc::instance().isRunning()) {
            GDExtensionIpc::instance().start();
        }
    }
}

static void deinitialize_didi_module(void *userdata, GDExtensionInitializationLevel p_level) {
    if (p_level == GDEXTENSION_INITIALIZATION_EDITOR) {
        DIDI_LOG_INFO("GDEXTENSION", "Deinitializing Didi GDExtension module");
        if (GDExtensionIpc::instance().isRunning()) {
            GDExtensionIpc::instance().stop();
        }
    }
}

} // namespace godot
} // namespace didi

extern "C" {

GDE_EXPORT void didi_pump_queue() {
    didi::godot::EditorHook::instance().processQueue();
}

GDE_EXPORT GDExtensionBool didi_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
                                            GDExtensionClassLibraryPtr p_library,
                                            GDExtensionInitialization *r_initialization) {
    didi::godot::GodotApi::instance().init(p_get_proc_address, p_library, r_initialization);

    r_initialization->initialize = didi::godot::initialize_didi_module;
    r_initialization->deinitialize = didi::godot::deinitialize_didi_module;
    r_initialization->userdata = nullptr;
    r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_CORE;

    DIDI_LOG_INFO("GDEXTENSION", "Didi GDExtension library entry point successfully bound");
    return 1;
}

}
