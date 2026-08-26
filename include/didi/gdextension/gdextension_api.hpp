#pragma once

#include "gdextension_interface.h"
#include "didi/common/types.hpp"
#include "didi/common/logger.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace didi {
namespace godot {

class GodotApi {
public:
    static GodotApi& instance() {
        static GodotApi s_instance;
        return s_instance;
    }

    void init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
              GDExtensionClassLibraryPtr p_library,
              GDExtensionInitialization* r_initialization) {
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
            mem_alloc = (GDExtensionInterfaceMemAlloc)p_get_proc_address("mem_alloc");
            mem_free = (GDExtensionInterfaceMemFree)p_get_proc_address("mem_free");
            print_warning = (GDExtensionInterfacePrintWarning)p_get_proc_address("print_warning");
            print_error = (GDExtensionInterfacePrintError)p_get_proc_address("print_error");
            variant_destroy = (GDExtensionInterfaceVariantDestroy)p_get_proc_address("variant_destroy");
            variant_new_copy = (GDExtensionInterfaceVariantNewCopy)p_get_proc_address("variant_new_copy");
            variant_get_ptr_destructor = (GDExtensionInterfaceVariantGetPtrDestructor)p_get_proc_address("variant_get_ptr_destructor");
            variant_get_type = (GDExtensionInterfaceVariantGetType)p_get_proc_address("variant_get_type");
        }

        m_initialized = true;
    }

    bool isInitialized() const { return m_initialized; }
    GDExtensionClassLibraryPtr getLibrary() const { return m_library; }
    GDExtensionInterfaceGetProcAddress getProcAddress() const { return m_getProcAddress; }

    // Core function pointers
    GDExtensionInterfaceStringNewWithUtf8Chars string_new_with_utf8_chars{nullptr};
    GDExtensionInterfaceStringToUtf8Chars string_to_utf8_chars{nullptr};
    GDExtensionInterfaceStringNameNewWithUtf8Chars string_name_new_with_utf8_chars{nullptr};
    GDExtensionInterfaceClassdbGetMethodBind classdb_get_method_bind{nullptr};
    GDExtensionInterfaceObjectMethodBindCall object_method_bind_call{nullptr};
    GDExtensionInterfaceObjectMethodBindPtrcall object_method_bind_ptrcall{nullptr};
    GDExtensionInterfaceGlobalGetSingleton global_get_singleton{nullptr};
    GDExtensionInterfaceMemAlloc mem_alloc{nullptr};
    GDExtensionInterfaceMemFree mem_free{nullptr};
    GDExtensionInterfacePrintWarning print_warning{nullptr};
    GDExtensionInterfacePrintError print_error{nullptr};
    GDExtensionInterfaceVariantDestroy variant_destroy{nullptr};
    GDExtensionInterfaceVariantNewCopy variant_new_copy{nullptr};
    GDExtensionInterfaceVariantGetPtrDestructor variant_get_ptr_destructor{nullptr};
    GDExtensionInterfaceVariantGetType variant_get_type{nullptr};

private:
    GodotApi() = default;
    bool m_initialized{false};
    GDExtensionInterfaceGetProcAddress m_getProcAddress{nullptr};
    GDExtensionClassLibraryPtr m_library{nullptr};
};

} // namespace godot
} // namespace didi
