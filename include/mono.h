#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

/*
 * mono.h — Mono Runtime API Definitions
 *
 * Type definitions and function pointer signatures for the Mono embedding API.
 * Used to resolve exports from mono-2.0-bdwgc.dll (or mono.dll) at runtime
 * after injection into a Unity/Mono game process.
 */

// ── Opaque Mono types ──────────────────────────────────────────────
typedef void MonoDomain;
typedef void MonoAssembly;
typedef void MonoImage;
typedef void MonoClass;
typedef void MonoClassField;
typedef void MonoMethod;
typedef void MonoType;
typedef void MonoTableInfo;
typedef void MonoThread;
typedef void MonoMethodSignature;
typedef void MonoProperty;
typedef void MonoVTable;

// ── Mono metadata table IDs ────────────────────────────────────────
constexpr int MONO_TABLE_TYPEDEF = 0x02;

// Token encoding: table ID in high byte, row index in low 3 bytes
inline uint32_t mono_token(int table, int row) {
    return (uint32_t)((table << 24) | (row + 1));
}

// ── Mono method flags (subset we care about) ───────────────────────
enum MonoMethodFlags : uint32_t {
    METHOD_ATTR_STATIC           = 0x0010,
    METHOD_ATTR_VIRTUAL          = 0x0040,
    METHOD_ATTR_ABSTRACT         = 0x0400,
    METHOD_ATTR_SPECIAL_NAME     = 0x0800,
    METHOD_ATTR_RT_SPECIAL_NAME  = 0x1000,
};

// ── Mono field flags ───────────────────────────────────────────────
enum MonoFieldFlags : uint32_t {
    FIELD_ATTR_STATIC            = 0x0010,
    FIELD_ATTR_LITERAL           = 0x0040,
    FIELD_ATTR_NOT_SERIALIZED    = 0x0080,
};

// ── Function pointer typedefs ──────────────────────────────────────

// Domain
typedef MonoDomain*          (__cdecl* fn_mono_get_root_domain)();
typedef MonoThread*          (__cdecl* fn_mono_thread_attach)(MonoDomain*);
typedef void                 (__cdecl* fn_mono_thread_detach)(MonoThread*);

// Assembly enumeration
typedef void                 (__cdecl* fn_mono_assembly_foreach)(void(*callback)(MonoAssembly*, void*), void* user_data);
typedef MonoImage*           (__cdecl* fn_mono_assembly_get_image)(MonoAssembly*);

// Image / table
typedef const char*          (__cdecl* fn_mono_image_get_name)(MonoImage*);
typedef const char*          (__cdecl* fn_mono_image_get_filename)(MonoImage*);
typedef const MonoTableInfo* (__cdecl* fn_mono_image_get_table_info)(MonoImage*, int table_id);
typedef int                  (__cdecl* fn_mono_table_info_get_rows)(const MonoTableInfo*);

// Class
typedef MonoClass*           (__cdecl* fn_mono_class_get)(MonoImage*, uint32_t type_token);
typedef const char*          (__cdecl* fn_mono_class_get_name)(MonoClass*);
typedef const char*          (__cdecl* fn_mono_class_get_namespace)(MonoClass*);
typedef MonoClass*           (__cdecl* fn_mono_class_get_parent)(MonoClass*);
typedef MonoClassField*      (__cdecl* fn_mono_class_get_fields)(MonoClass*, void** iter);
typedef MonoMethod*          (__cdecl* fn_mono_class_get_methods)(MonoClass*, void** iter);
typedef MonoProperty*        (__cdecl* fn_mono_class_get_properties)(MonoClass*, void** iter);
typedef int                  (__cdecl* fn_mono_class_get_flags)(MonoClass*);
typedef MonoType*            (__cdecl* fn_mono_class_get_type)(MonoClass*);
typedef int                  (__cdecl* fn_mono_class_is_enum)(MonoClass*);
typedef int                  (__cdecl* fn_mono_class_is_valuetype)(MonoClass*);
typedef int                  (__cdecl* fn_mono_class_instance_size)(MonoClass*);
typedef MonoVTable*          (__cdecl* fn_mono_class_vtable)(MonoDomain*, MonoClass*);
typedef void*                (__cdecl* fn_mono_vtable_get_static_field_data)(MonoVTable*);
typedef int                  (__cdecl* fn_mono_class_num_fields)(MonoClass*);
typedef int                  (__cdecl* fn_mono_class_num_methods)(MonoClass*);

// Field
typedef const char*          (__cdecl* fn_mono_field_get_name)(MonoClassField*);
typedef int                  (__cdecl* fn_mono_field_get_offset)(MonoClassField*);
typedef MonoType*            (__cdecl* fn_mono_field_get_type)(MonoClassField*);
typedef uint32_t             (__cdecl* fn_mono_field_get_flags)(MonoClassField*);
typedef MonoClass*           (__cdecl* fn_mono_field_get_parent)(MonoClassField*);

// Method
typedef const char*          (__cdecl* fn_mono_method_get_name)(MonoMethod*);
typedef void*                (__cdecl* fn_mono_compile_method)(MonoMethod*);
typedef MonoMethodSignature* (__cdecl* fn_mono_method_signature)(MonoMethod*);
typedef uint32_t             (__cdecl* fn_mono_method_get_flags)(MonoMethod*, uint32_t*);
typedef MonoClass*           (__cdecl* fn_mono_method_get_class)(MonoMethod*);
typedef uint32_t             (__cdecl* fn_mono_method_get_token)(MonoMethod*);

// Signature
typedef MonoType*            (__cdecl* fn_mono_signature_get_return_type)(MonoMethodSignature*);
typedef MonoType*            (__cdecl* fn_mono_signature_get_params)(MonoMethodSignature*, void** iter);
typedef uint32_t             (__cdecl* fn_mono_signature_get_param_count)(MonoMethodSignature*);

// Type
typedef const char*          (__cdecl* fn_mono_type_get_name)(MonoType*);
typedef int                  (__cdecl* fn_mono_type_get_type)(MonoType*);
typedef MonoClass*           (__cdecl* fn_mono_type_get_class)(MonoType*);
typedef const char*          (__cdecl* fn_mono_type_full_name)(MonoType*);

// Property
typedef const char*          (__cdecl* fn_mono_property_get_name)(MonoProperty*);
typedef MonoMethod*          (__cdecl* fn_mono_property_get_get_method)(MonoProperty*);
typedef MonoMethod*          (__cdecl* fn_mono_property_get_set_method)(MonoProperty*);

// String
typedef wchar_t*             (__cdecl* fn_mono_string_chars)(void*);
typedef int                  (__cdecl* fn_mono_string_length)(void*);


// ── Resolved API container ─────────────────────────────────────────
struct MonoApi {
    HMODULE hMono = nullptr;

    // Domain
    fn_mono_get_root_domain          get_root_domain          = nullptr;
    fn_mono_thread_attach            thread_attach            = nullptr;
    fn_mono_thread_detach            thread_detach            = nullptr;

    // Assembly
    fn_mono_assembly_foreach         assembly_foreach         = nullptr;
    fn_mono_assembly_get_image       assembly_get_image       = nullptr;

    // Image
    fn_mono_image_get_name           image_get_name           = nullptr;
    fn_mono_image_get_filename       image_get_filename       = nullptr;
    fn_mono_image_get_table_info     image_get_table_info     = nullptr;
    fn_mono_table_info_get_rows      table_info_get_rows      = nullptr;

    // Class
    fn_mono_class_get                class_get                = nullptr;
    fn_mono_class_get_name           class_get_name           = nullptr;
    fn_mono_class_get_namespace      class_get_namespace      = nullptr;
    fn_mono_class_get_parent         class_get_parent         = nullptr;
    fn_mono_class_get_fields         class_get_fields         = nullptr;
    fn_mono_class_get_methods        class_get_methods        = nullptr;
    fn_mono_class_get_properties     class_get_properties     = nullptr;
    fn_mono_class_get_flags          class_get_flags          = nullptr;
    fn_mono_class_get_type           class_get_type           = nullptr;
    fn_mono_class_is_enum            class_is_enum            = nullptr;
    fn_mono_class_is_valuetype       class_is_valuetype       = nullptr;
    fn_mono_class_instance_size      class_instance_size      = nullptr;
    fn_mono_class_vtable             class_vtable             = nullptr;
    fn_mono_vtable_get_static_field_data vtable_get_static_field_data = nullptr;
    fn_mono_class_num_fields         class_num_fields         = nullptr;
    fn_mono_class_num_methods        class_num_methods        = nullptr;

    // Field
    fn_mono_field_get_name           field_get_name           = nullptr;
    fn_mono_field_get_offset         field_get_offset         = nullptr;
    fn_mono_field_get_type           field_get_type           = nullptr;
    fn_mono_field_get_flags          field_get_flags          = nullptr;
    fn_mono_field_get_parent         field_get_parent         = nullptr;

    // Method
    fn_mono_method_get_name          method_get_name          = nullptr;
    fn_mono_compile_method           compile_method           = nullptr;
    fn_mono_method_signature         method_signature         = nullptr;
    fn_mono_method_get_flags         method_get_flags         = nullptr;
    fn_mono_method_get_class         method_get_class         = nullptr;
    fn_mono_method_get_token         method_get_token         = nullptr;

    // Signature
    fn_mono_signature_get_return_type signature_get_return_type = nullptr;
    fn_mono_signature_get_params     signature_get_params     = nullptr;
    fn_mono_signature_get_param_count signature_get_param_count = nullptr;

    // Type
    fn_mono_type_get_name            type_get_name            = nullptr;
    fn_mono_type_get_type            type_get_type            = nullptr;
    fn_mono_type_get_class           type_get_class           = nullptr;
    fn_mono_type_full_name           type_full_name           = nullptr;

    // Property
    fn_mono_property_get_name        property_get_name        = nullptr;
    fn_mono_property_get_get_method  property_get_get_method  = nullptr;
    fn_mono_property_get_set_method  property_get_set_method  = nullptr;

    // String
    fn_mono_string_chars             string_chars             = nullptr;
    fn_mono_string_length            string_length            = nullptr;

    // ── Resolve all exports ────────────────────────────────────────
    // Macro: member name → mono_ prefixed export
    // RESOLVE(get_root_domain) → get_root_domain = GetProcAddress(hMono, "mono_get_root_domain")
    bool Resolve() {
        const char* dllNames[] = {
            "mono-2.0-bdwgc.dll",
            "mono-2.0-sgen.dll",
            "mono.dll",
        };

        for (auto name : dllNames) {
            hMono = GetModuleHandleA(name);
            if (hMono) break;
        }

        if (!hMono) return false;

        #define RESOLVE(short_name) \
            short_name = (fn_mono_##short_name)GetProcAddress(hMono, "mono_" #short_name); \
            if (!short_name) return false;

        #define RESOLVE_OPT(short_name) \
            short_name = (fn_mono_##short_name)GetProcAddress(hMono, "mono_" #short_name);

        // Required
        RESOLVE(get_root_domain);
        RESOLVE(thread_attach);
        RESOLVE(assembly_foreach);
        RESOLVE(assembly_get_image);
        RESOLVE(image_get_name);
        RESOLVE(image_get_table_info);
        RESOLVE(table_info_get_rows);
        RESOLVE(class_get);
        RESOLVE(class_get_name);
        RESOLVE(class_get_namespace);
        RESOLVE(class_get_parent);
        RESOLVE(class_get_fields);
        RESOLVE(class_get_methods);
        RESOLVE(field_get_name);
        RESOLVE(field_get_offset);
        RESOLVE(field_get_type);
        RESOLVE(type_get_name);
        RESOLVE(method_get_name);
        RESOLVE(method_signature);
        RESOLVE(signature_get_return_type);
        RESOLVE(signature_get_params);
        RESOLVE(signature_get_param_count);

        // Optional (not all Mono builds export these)
        RESOLVE_OPT(thread_detach);
        RESOLVE_OPT(image_get_filename);
        RESOLVE_OPT(class_get_properties);
        RESOLVE_OPT(class_get_flags);
        RESOLVE_OPT(class_get_type);
        RESOLVE_OPT(class_is_enum);
        RESOLVE_OPT(class_is_valuetype);
        RESOLVE_OPT(class_instance_size);
        RESOLVE_OPT(class_vtable);
        RESOLVE_OPT(vtable_get_static_field_data);
        RESOLVE_OPT(class_num_fields);
        RESOLVE_OPT(class_num_methods);
        RESOLVE_OPT(field_get_flags);
        RESOLVE_OPT(field_get_parent);
        RESOLVE_OPT(compile_method);
        RESOLVE_OPT(method_get_flags);
        RESOLVE_OPT(method_get_class);
        RESOLVE_OPT(method_get_token);
        RESOLVE_OPT(type_get_type);
        RESOLVE_OPT(type_get_class);
        RESOLVE_OPT(type_full_name);
        RESOLVE_OPT(property_get_name);
        RESOLVE_OPT(property_get_get_method);
        RESOLVE_OPT(property_get_set_method);
        RESOLVE_OPT(string_chars);
        RESOLVE_OPT(string_length);

        #undef RESOLVE
        #undef RESOLVE_OPT

        return true;
    }
};
