#pragma once

#include "gdextension_interface.h"
#include "didi/common/types.hpp"
#include "didi/common/logger.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>

namespace didi {
namespace godot {

// `Object::notification(what, reversed)`, from extension_api.json. The
// signature has been stable since 4.0; a hash the running engine disagrees
// with yields a null bind, which callers treat as "skip", not as an error.
inline constexpr int64_t kObjectNotificationHash = 4023243586LL;

// `Object::NOTIFICATION_POSTINITIALIZE`. Objects built through
// `classdb_construct_object2` or `3` have not received it: the engine
// implements both as `ClassDB::instantiate_without_postinitialization`, and
// sending it is the extension's responsibility.
inline constexpr int64_t kNotificationPostInitialize = 0;

class GodotApi {
public:
    static GodotApi& instance() {
        static GodotApi s_instance;
        return s_instance;
    }

    void init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
              GDExtensionClassLibraryPtr p_library,
              GDExtensionInitialization* r_initialization) {
        (void)r_initialization;
        m_getProcAddress = p_get_proc_address;
        m_library = p_library;

        if (p_get_proc_address) {
            string_new_with_utf8_chars = (GDExtensionInterfaceStringNewWithUtf8Chars)p_get_proc_address("string_new_with_utf8_chars");
            string_to_utf8_chars = (GDExtensionInterfaceStringToUtf8Chars)p_get_proc_address("string_to_utf8_chars");
            string_name_new_with_utf8_chars = (GDExtensionInterfaceStringNameNewWithUtf8Chars)p_get_proc_address("string_name_new_with_utf8_chars");
            classdb_get_method_bind = (GDExtensionInterfaceClassdbGetMethodBind)p_get_proc_address("classdb_get_method_bind");
            object_method_bind_call = (GDExtensionInterfaceObjectMethodBindCall)p_get_proc_address("object_method_bind_call");
            object_method_bind_ptrcall = (GDExtensionInterfaceObjectMethodBindPtrcall)p_get_proc_address("object_method_bind_ptrcall");
            global_get_singleton = (GDExtensionInterfaceGlobalGetSingleton)p_get_proc_address("global_get_singleton");
            object_destroy = (GDExtensionInterfaceObjectDestroy)p_get_proc_address("object_destroy");
            object_get_instance_from_id = (GDExtensionInterfaceObjectGetInstanceFromId)p_get_proc_address("object_get_instance_from_id");
            object_get_instance_id = (GDExtensionInterfaceObjectGetInstanceId)p_get_proc_address("object_get_instance_id");
            classdb_construct_object = (GDExtensionInterfaceClassdbConstructObject2)p_get_proc_address("classdb_construct_object2");
            // Class registration. Didi has only ever called the engine, never
            // extended it, so these are new. The interface exposes several
            // variants with different GDExtensionClassCreationInfo layouts;
            // which are present depends on the running engine, so each is
            // resolved and the caller selects the newest available.
            classdb_register_extension_class6 = (GDExtensionInterfaceClassdbRegisterExtensionClass6)p_get_proc_address("classdb_register_extension_class6");
            classdb_register_extension_class4 = (GDExtensionInterfaceClassdbRegisterExtensionClass4)p_get_proc_address("classdb_register_extension_class4");
            classdb_unregister_extension_class = (GDExtensionInterfaceClassdbUnregisterExtensionClass)p_get_proc_address("classdb_unregister_extension_class");
            object_set_instance = (GDExtensionInterfaceObjectSetInstance)p_get_proc_address("object_set_instance");
            mem_alloc = (GDExtensionInterfaceMemAlloc)p_get_proc_address("mem_alloc");
            mem_free = (GDExtensionInterfaceMemFree)p_get_proc_address("mem_free");
            print_warning = (GDExtensionInterfacePrintWarning)p_get_proc_address("print_warning");
            print_error = (GDExtensionInterfacePrintError)p_get_proc_address("print_error");
            variant_destroy = (GDExtensionInterfaceVariantDestroy)p_get_proc_address("variant_destroy");
            variant_new_nil = (GDExtensionInterfaceVariantNewNil)p_get_proc_address("variant_new_nil");
            variant_new_copy = (GDExtensionInterfaceVariantNewCopy)p_get_proc_address("variant_new_copy");
            variant_call = (GDExtensionInterfaceVariantCall)p_get_proc_address("variant_call");
            get_variant_from_type_constructor = (GDExtensionInterfaceGetVariantFromTypeConstructor)p_get_proc_address("get_variant_from_type_constructor");
            get_variant_to_type_constructor = (GDExtensionInterfaceGetVariantToTypeConstructor)p_get_proc_address("get_variant_to_type_constructor");
            variant_get_ptr_constructor = (GDExtensionInterfaceVariantGetPtrConstructor)p_get_proc_address("variant_get_ptr_constructor");
            variant_get_ptr_destructor = (GDExtensionInterfaceVariantGetPtrDestructor)p_get_proc_address("variant_get_ptr_destructor");
            variant_get_ptr_getter = (GDExtensionInterfaceVariantGetPtrGetter)p_get_proc_address("variant_get_ptr_getter");
            variant_get_type = (GDExtensionInterfaceVariantGetType)p_get_proc_address("variant_get_type");
            packed_byte_array_operator_index_const = (GDExtensionInterfacePackedByteArrayOperatorIndexConst)p_get_proc_address("packed_byte_array_operator_index_const");
            register_main_loop_callbacks = (GDExtensionInterfaceRegisterMainLoopCallbacks)p_get_proc_address("register_main_loop_callbacks");
        }

        m_initialized = p_get_proc_address != nullptr && m_library != nullptr;
    }

    bool isInitialized() const { return m_initialized; }
    bool isMainLoopRegistered() const { return m_mainLoopRegistered.load(); }
    bool isLiveReady() const { return m_initialized && m_mainLoopRegistered.load(); }
    bool registerMainLoop(const GDExtensionMainLoopCallbacks& callbacks) {
        if (!m_initialized || !register_main_loop_callbacks) {
            return false;
        }
        register_main_loop_callbacks(m_library, &callbacks);
        m_mainLoopRegistered.store(true);
        return true;
    }
    void markMainLoopStopped() { m_mainLoopRegistered.store(false); }
    GDExtensionClassLibraryPtr getLibrary() const { return m_library; }
    GDExtensionInterfaceGetProcAddress getProcAddress() const { return m_getProcAddress; }

    // Core function pointers
    GDExtensionInterfaceStringNewWithUtf8Chars string_new_with_utf8_chars{nullptr};
    GDExtensionInterfaceStringToUtf8Chars string_to_utf8_chars{nullptr};
    GDExtensionInterfaceStringNameNewWithUtf8Chars string_name_new_with_utf8_chars{nullptr};
    GDExtensionInterfaceClassdbGetMethodBind classdb_get_method_bind{nullptr};
    GDExtensionInterfaceClassdbRegisterExtensionClass6 classdb_register_extension_class6{nullptr};
    GDExtensionInterfaceClassdbRegisterExtensionClass4 classdb_register_extension_class4{nullptr};
    GDExtensionInterfaceClassdbUnregisterExtensionClass classdb_unregister_extension_class{nullptr};
    GDExtensionInterfaceObjectSetInstance object_set_instance{nullptr};
    GDExtensionInterfaceObjectMethodBindCall object_method_bind_call{nullptr};
    GDExtensionInterfaceObjectMethodBindPtrcall object_method_bind_ptrcall{nullptr};
    GDExtensionInterfaceGlobalGetSingleton global_get_singleton{nullptr};
    GDExtensionInterfaceObjectDestroy object_destroy{nullptr};
    GDExtensionInterfaceObjectGetInstanceFromId object_get_instance_from_id{nullptr};
    GDExtensionInterfaceObjectGetInstanceId object_get_instance_id{nullptr};
    GDExtensionInterfaceClassdbConstructObject2 classdb_construct_object{nullptr};
    GDExtensionInterfaceMemAlloc mem_alloc{nullptr};
    GDExtensionInterfaceMemFree mem_free{nullptr};
    GDExtensionInterfacePrintWarning print_warning{nullptr};
    GDExtensionInterfacePrintError print_error{nullptr};
    GDExtensionInterfaceVariantDestroy variant_destroy{nullptr};
    GDExtensionInterfaceVariantNewNil variant_new_nil{nullptr};
    GDExtensionInterfaceVariantNewCopy variant_new_copy{nullptr};
    GDExtensionInterfaceVariantCall variant_call{nullptr};
    GDExtensionInterfaceGetVariantFromTypeConstructor get_variant_from_type_constructor{nullptr};
    GDExtensionInterfaceGetVariantToTypeConstructor get_variant_to_type_constructor{nullptr};
    GDExtensionInterfaceVariantGetPtrConstructor variant_get_ptr_constructor{nullptr};
    GDExtensionInterfaceVariantGetPtrDestructor variant_get_ptr_destructor{nullptr};
    GDExtensionInterfaceVariantGetPtrGetter variant_get_ptr_getter{nullptr};
    GDExtensionInterfaceVariantGetType variant_get_type{nullptr};
    GDExtensionInterfacePackedByteArrayOperatorIndexConst packed_byte_array_operator_index_const{nullptr};
    GDExtensionInterfaceRegisterMainLoopCallbacks register_main_loop_callbacks{nullptr};

private:
    GodotApi() = default;
    bool m_initialized{false};
    std::atomic<bool> m_mainLoopRegistered{false};
    GDExtensionInterfaceGetProcAddress m_getProcAddress{nullptr};
    GDExtensionClassLibraryPtr m_library{nullptr};
};

} // namespace godot
} // namespace didi
