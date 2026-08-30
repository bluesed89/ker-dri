/*
 * proxy.cpp — version.dll Proxy + Albion Online Mono Offset Dumper
 *
 * Bu dosya iki iş yapıyor:
 *   1. Gerçek version.dll'in tüm export'larını System32'den yönlendiriyor
 *   2. Albion Online process'inde olduğumuzu tespit edince Mono dump başlatıyor
 *
 * Kullanım:
 *   - Derlenen "version.dll" dosyasını Albion-Online.exe'nin yanına koy
 *   - Oyunu normal başlat
 *   - Masaüstünde albion_dump.txt oluşur
 *
 * Build: cmake --build build --config Release
 * Çıktı: build/bin/version.dll
 */

#include <windows.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <thread>
#include <mutex>
#include <cmath>
#include "../include/mono.h"

// ── Radar Ayarları ve Verileri ─────────────────────────────────────────
namespace RadarConfig {
    constexpr int WindowWidth     = 350;
    constexpr int WindowHeight    = 350;
    constexpr float RadarRadius   = 120.0f;
    constexpr float MaxDistance   = 100.0f;
    constexpr int PosX            = 20;
    constexpr int PosY            = 20;
}

namespace Offsets {
    constexpr uintptr_t GOM_ActiveNodes = 0x18; 
    constexpr uintptr_t Node_GameObject = 0x10;
    constexpr uintptr_t Node_Next       = 0x8;
    constexpr uintptr_t GO_Name         = 0x60;
    constexpr uintptr_t GO_Components   = 0x30;
    constexpr uintptr_t Component_Transform = 0x8;
    constexpr uintptr_t Transform_Matrix = 0x38; 
}

struct RadarEntity {
    std::string name;
    float posX;
    float posY;
    bool isEnemy;
};

struct Vector3D {
    float x, y, z;
};

static std::vector<RadarEntity> g_entities;
static std::mutex g_entitiesMutex;
static float g_localX = 0.0f;
static float g_localY = 0.0f;
static bool g_isRunning = true;
static HWND g_hwnd = NULL;

// ── Internal Pattern Scanner ───────────────────────────────────────────
uintptr_t FindPatternInternal(uintptr_t moduleBase, size_t moduleSize, const char* signature) {
    auto patternToByte = [](const char* pattern) {
        std::vector<int> bytes;
        char* start = const_cast<char*>(pattern);
        char* end = const_cast<char*>(pattern) + strlen(pattern);
        for (char* current = start; current < end; ++current) {
            if (*current == '?') {
                ++current;
                if (*current == '?') ++current;
                bytes.push_back(-1);
            } else {
                bytes.push_back(strtoul(current, &current, 16));
            }
        }
        return bytes;
    };

    std::vector<int> patternBytes = patternToByte(signature);
    size_t patternSize = patternBytes.size();
    uint8_t* scanBytes = reinterpret_cast<uint8_t*>(moduleBase);

    for (size_t i = 0; i < moduleSize - patternSize; ++i) {
        bool found = true;
        for (size_t j = 0; j < patternSize; ++j) {
            if (patternBytes[j] != -1 && scanBytes[i + j] != patternBytes[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return moduleBase + i;
        }
    }
    return 0;
}

// ── SEH Raw Memory Read Helper ─────────────────────────────────────────
static bool SafeReadBuffer(uintptr_t srcAddress, void* destBuffer, size_t bytesToRead) {
    if (!srcAddress || srcAddress < 0x10000 || !destBuffer) return false;
    __try {
        memcpy(destBuffer, reinterpret_cast<const void*>(srcAddress), bytesToRead);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ── Safe SEH Read Helper ───────────────────────────────────────────────
template<typename T>
static bool SafeRead(uintptr_t address, T& outValue) {
    return SafeReadBuffer(address, &outValue, sizeof(T));
}

// ── Safe String Read Helper ─────────────────────────────────────────────
static size_t SafeStrLen(uintptr_t address, size_t maxLen) {
    if (!address || address < 0x10000) return 0;
    __try {
        const char* strPtr = reinterpret_cast<const char*>(address);
        return strnlen(strPtr, maxLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ── Bellekten String Okuma (Internal) ──────────────────────────────────
std::string ReadStringInternal(uintptr_t address, size_t maxLength = 32) {
    size_t limit = (maxLength < 255) ? maxLength : 255;
    size_t len = SafeStrLen(address, limit);
    if (len == 0) return "";

    char buffer[256] = {0};
    if (!SafeReadBuffer(address, buffer, len)) return "";
    buffer[len] = '\0';
    return std::string(buffer, len);
}

// ═══════════════════════════════════════════════════════════════════
//  BÖLÜM 1: version.dll PROXY
// ═══════════════════════════════════════════════════════════════════

static HMODULE g_hRealVersion = nullptr;

// ── Gerçek version.dll function pointer'ları ───────────────────────
typedef BOOL(WINAPI* t_GetFileVersionInfoA)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI* t_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI* t_GetFileVersionInfoExA)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI* t_GetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeA)(LPCSTR, LPDWORD);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeExA)(DWORD, LPCSTR, LPDWORD);
typedef DWORD(WINAPI* t_GetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* t_VerQueryValueA)(LPCVOID, LPCSTR, LPVOID*, PUINT);
typedef BOOL(WINAPI* t_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
typedef DWORD(WINAPI* t_VerFindFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
typedef DWORD(WINAPI* t_VerFindFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD(WINAPI* t_VerInstallFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
typedef DWORD(WINAPI* t_VerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD(WINAPI* t_VerLanguageNameA)(DWORD, LPSTR, DWORD);
typedef DWORD(WINAPI* t_VerLanguageNameW)(DWORD, LPWSTR, DWORD);
typedef int(WINAPI* t_GetFileVersionInfoByHandle)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);

static t_GetFileVersionInfoA        real_GetFileVersionInfoA        = nullptr;
static t_GetFileVersionInfoW        real_GetFileVersionInfoW        = nullptr;
static t_GetFileVersionInfoExA      real_GetFileVersionInfoExA      = nullptr;
static t_GetFileVersionInfoExW      real_GetFileVersionInfoExW      = nullptr;
static t_GetFileVersionInfoSizeA    real_GetFileVersionInfoSizeA    = nullptr;
static t_GetFileVersionInfoSizeW    real_GetFileVersionInfoSizeW    = nullptr;
static t_GetFileVersionInfoSizeExA  real_GetFileVersionInfoSizeExA  = nullptr;
static t_GetFileVersionInfoSizeExW  real_GetFileVersionInfoSizeExW  = nullptr;
static t_VerQueryValueA             real_VerQueryValueA             = nullptr;
static t_VerQueryValueW             real_VerQueryValueW             = nullptr;
static t_VerFindFileA               real_VerFindFileA               = nullptr;
static t_VerFindFileW               real_VerFindFileW               = nullptr;
static t_VerInstallFileA            real_VerInstallFileA            = nullptr;
static t_VerInstallFileW            real_VerInstallFileW            = nullptr;
static t_VerLanguageNameA           real_VerLanguageNameA           = nullptr;
static t_VerLanguageNameW           real_VerLanguageNameW           = nullptr;
static t_GetFileVersionInfoByHandle real_GetFileVersionInfoByHandle = nullptr;

static bool LoadRealVersionDll() {
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string realPath = std::string(sysDir) + "\\version.dll";

    g_hRealVersion = LoadLibraryA(realPath.c_str());
    if (!g_hRealVersion) return false;

    #define LOAD_FUNC(name) real_##name = (t_##name)GetProcAddress(g_hRealVersion, #name)

    LOAD_FUNC(GetFileVersionInfoA);
    LOAD_FUNC(GetFileVersionInfoW);
    LOAD_FUNC(GetFileVersionInfoExA);
    LOAD_FUNC(GetFileVersionInfoExW);
    LOAD_FUNC(GetFileVersionInfoSizeA);
    LOAD_FUNC(GetFileVersionInfoSizeW);
    LOAD_FUNC(GetFileVersionInfoSizeExA);
    LOAD_FUNC(GetFileVersionInfoSizeExW);
    LOAD_FUNC(VerQueryValueA);
    LOAD_FUNC(VerQueryValueW);
    LOAD_FUNC(VerFindFileA);
    LOAD_FUNC(VerFindFileW);
    LOAD_FUNC(VerInstallFileA);
    LOAD_FUNC(VerInstallFileW);
    LOAD_FUNC(VerLanguageNameA);
    LOAD_FUNC(VerLanguageNameW);
    LOAD_FUNC(GetFileVersionInfoByHandle);

    #undef LOAD_FUNC
    return true;
}

// ── Export'lar — gerçek version.dll'e yönlendirme ──────────────────
#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=proxy_GetFileVersionInfoA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=proxy_GetFileVersionInfoW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=proxy_GetFileVersionInfoExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=proxy_GetFileVersionInfoExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=proxy_GetFileVersionInfoSizeA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=proxy_GetFileVersionInfoSizeW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=proxy_GetFileVersionInfoSizeExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=proxy_GetFileVersionInfoSizeExW")
#pragma comment(linker, "/EXPORT:VerQueryValueA=proxy_VerQueryValueA")
#pragma comment(linker, "/EXPORT:VerQueryValueW=proxy_VerQueryValueW")
#pragma comment(linker, "/EXPORT:VerFindFileA=proxy_VerFindFileA")
#pragma comment(linker, "/EXPORT:VerFindFileW=proxy_VerFindFileW")
#pragma comment(linker, "/EXPORT:VerInstallFileA=proxy_VerInstallFileA")
#pragma comment(linker, "/EXPORT:VerInstallFileW=proxy_VerInstallFileW")
#pragma comment(linker, "/EXPORT:VerLanguageNameA=proxy_VerLanguageNameA")
#pragma comment(linker, "/EXPORT:VerLanguageNameW=proxy_VerLanguageNameW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=proxy_GetFileVersionInfoByHandle")

extern "C" {

__declspec(dllexport) BOOL WINAPI proxy_GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    return real_GetFileVersionInfoA ? real_GetFileVersionInfoA(a, b, c, d) : FALSE;
}
__declspec(dllexport) BOOL WINAPI proxy_GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    return real_GetFileVersionInfoW ? real_GetFileVersionInfoW(a, b, c, d) : FALSE;
}
__declspec(dllexport) BOOL WINAPI proxy_GetFileVersionInfoExA(DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    return real_GetFileVersionInfoExA ? real_GetFileVersionInfoExA(f, a, b, c, d) : FALSE;
}
__declspec(dllexport) BOOL WINAPI proxy_GetFileVersionInfoExW(DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    return real_GetFileVersionInfoExW ? real_GetFileVersionInfoExW(f, a, b, c, d) : FALSE;
}
__declspec(dllexport) DWORD WINAPI proxy_GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    return real_GetFileVersionInfoSizeA ? real_GetFileVersionInfoSizeA(a, b) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    return real_GetFileVersionInfoSizeW ? real_GetFileVersionInfoSizeW(a, b) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_GetFileVersionInfoSizeExA(DWORD f, LPCSTR a, LPDWORD b) {
    return real_GetFileVersionInfoSizeExA ? real_GetFileVersionInfoSizeExA(f, a, b) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_GetFileVersionInfoSizeExW(DWORD f, LPCWSTR a, LPDWORD b) {
    return real_GetFileVersionInfoSizeExW ? real_GetFileVersionInfoSizeExW(f, a, b) : 0;
}
__declspec(dllexport) BOOL WINAPI proxy_VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d) {
    return real_VerQueryValueA ? real_VerQueryValueA(a, b, c, d) : FALSE;
}
__declspec(dllexport) BOOL WINAPI proxy_VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d) {
    return real_VerQueryValueW ? real_VerQueryValueW(a, b, c, d) : FALSE;
}
__declspec(dllexport) DWORD WINAPI proxy_VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h) {
    return real_VerFindFileA ? real_VerFindFileA(a, b, c, d, e, f, g, h) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h) {
    return real_VerFindFileW ? real_VerFindFileW(a, b, c, d, e, f, g, h) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h) {
    return real_VerInstallFileA ? real_VerInstallFileA(a, b, c, d, e, f, g, h) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h) {
    return real_VerInstallFileW ? real_VerInstallFileW(a, b, c, d, e, f, g, h) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_VerLanguageNameA(DWORD a, LPSTR b, DWORD c) {
    return real_VerLanguageNameA ? real_VerLanguageNameA(a, b, c) : 0;
}
__declspec(dllexport) DWORD WINAPI proxy_VerLanguageNameW(DWORD a, LPWSTR b, DWORD c) {
    return real_VerLanguageNameW ? real_VerLanguageNameW(a, b, c) : 0;
}
__declspec(dllexport) int WINAPI proxy_GetFileVersionInfoByHandle(DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e) {
    return real_GetFileVersionInfoByHandle ? real_GetFileVersionInfoByHandle(a, b, c, d, e) : 0;
}

} // extern "C"


// ═══════════════════════════════════════════════════════════════════
//  BÖLÜM 2: MONO OFFSET DUMPER
// ═══════════════════════════════════════════════════════════════════

static MonoApi g_mono;
static std::ofstream g_out;
static int g_totalClasses = 0;
static int g_totalFields  = 0;
static int g_totalMethods = 0;

static std::string SafeStr(const char* s) {
    return s ? s : "<null>";
}

static std::string GetFieldFlagsStr(uint32_t flags) {
    std::string result;
    if (flags & FIELD_ATTR_STATIC)          result += "static ";
    if (flags & FIELD_ATTR_LITERAL)         result += "const ";
    if (flags & FIELD_ATTR_NOT_SERIALIZED)  result += "[NonSerialized] ";
    return result;
}

static std::string GetMethodFlagsStr(uint32_t flags) {
    std::string result;
    if (flags & METHOD_ATTR_STATIC)         result += "static ";
    if (flags & METHOD_ATTR_VIRTUAL)        result += "virtual ";
    if (flags & METHOD_ATTR_ABSTRACT)       result += "abstract ";
    return result;
}

static std::string GetTypeName(MonoType* type) {
    if (!type) return "???";
    if (g_mono.type_full_name) {
        const char* full = g_mono.type_full_name(type);
        if (full) return full;
    }
    const char* name = g_mono.type_get_name(type);
    return name ? name : "???";
}

static std::string BuildMethodSignature(MonoMethod* method) {
    std::ostringstream sig;

    if (g_mono.method_get_flags) {
        uint32_t iflags = 0;
        uint32_t flags = g_mono.method_get_flags(method, &iflags);
        sig << GetMethodFlagsStr(flags);
    }

    MonoMethodSignature* msig = g_mono.method_signature(method);
    if (!msig) {
        sig << "??? " << SafeStr(g_mono.method_get_name(method)) << "(???)";
        return sig.str();
    }

    MonoType* retType = g_mono.signature_get_return_type(msig);
    sig << GetTypeName(retType) << " ";
    sig << SafeStr(g_mono.method_get_name(method));

    sig << "(";
    uint32_t paramCount = g_mono.signature_get_param_count(msig);
    void* piter = nullptr;
    for (uint32_t i = 0; i < paramCount; i++) {
        MonoType* ptype = g_mono.signature_get_params(msig, &piter);
        if (i > 0) sig << ", ";
        sig << GetTypeName(ptype);
    }
    sig << ")";

    return sig.str();
}

// ── Assembly collector ─────────────────────────────────────────────
struct AssemblyEntry {
    MonoAssembly* assembly;
    MonoImage*    image;
    std::string   name;
};

static std::vector<AssemblyEntry> g_assemblies;

static void __cdecl CollectAssembly(MonoAssembly* assembly, void*) {
    MonoImage* image = g_mono.assembly_get_image(assembly);
    if (!image) return;
    AssemblyEntry entry;
    entry.assembly = assembly;
    entry.image    = image;
    entry.name     = SafeStr(g_mono.image_get_name(image));
    g_assemblies.push_back(entry);
}

// ── Safe JIT compile (izole SEH) ───────────────────────────────────
static void* SafeCompileMethod(MonoMethod* method) {
    if (!g_mono.compile_method) return nullptr;
    __try {
        return g_mono.compile_method(method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ── Tek bir class'ı dump et ────────────────────────────────────────
static void DumpClass(MonoClass* klass) {
    if (!klass) return;

    const char* ns   = g_mono.class_get_namespace(klass);
    const char* name = g_mono.class_get_name(klass);
    if (!name) return;

    std::string fqn;
    if (ns && ns[0]) fqn = std::string(ns) + "." + name;
    else fqn = name;

    std::string parentName;
    MonoClass* parent = g_mono.class_get_parent(klass);
    if (parent) {
        const char* pns = g_mono.class_get_namespace(parent);
        const char* pn  = g_mono.class_get_name(parent);
        if (pns && pns[0]) parentName = std::string(pns) + "." + SafeStr(pn);
        else parentName = SafeStr(pn);
    }

    std::string classKind = "class";
    if (g_mono.class_is_enum && g_mono.class_is_enum(klass))
        classKind = "enum";
    else if (g_mono.class_is_valuetype && g_mono.class_is_valuetype(klass))
        classKind = "struct";

    int instanceSize = 0;
    if (g_mono.class_instance_size)
        instanceSize = g_mono.class_instance_size(klass);

    g_out << "\n  " << classKind << " " << fqn;
    if (!parentName.empty() && parentName != "System.Object" &&
        parentName != "System.ValueType" && parentName != "System.Enum")
        g_out << " : " << parentName;
    if (instanceSize > 0)
        g_out << "  [Size: 0x" << std::hex << instanceSize << std::dec << "]";
    g_out << "\n  {\n";

    g_totalClasses++;

    // Fields
    void* fiter = nullptr;
    MonoClassField* field;
    bool hasFields = false;

    while ((field = g_mono.class_get_fields(klass, &fiter)) != nullptr) {
        if (!hasFields) { g_out << "    // Fields\n"; hasFields = true; }

        const char* fname = g_mono.field_get_name(field);
        int offset = g_mono.field_get_offset(field);
        MonoType* ftype = g_mono.field_get_type(field);
        std::string typeName = GetTypeName(ftype);

        if (g_mono.field_get_flags) {
            uint32_t fflags = g_mono.field_get_flags(field);
            std::string flagsStr = GetFieldFlagsStr(fflags);
            const char* prefix = (fflags & FIELD_ATTR_STATIC) ? "[S] " : "    ";
            g_out << "    " << prefix << "0x" << std::hex << std::setw(4) << std::setfill('0')
                  << offset << std::dec << "  " << flagsStr << typeName << " " << SafeStr(fname) << "\n";
        } else {
            g_out << "        0x" << std::hex << std::setw(4) << std::setfill('0')
                  << offset << std::dec << "  " << typeName << " " << SafeStr(fname) << "\n";
        }
        g_totalFields++;
    }

    // Properties
    if (g_mono.class_get_properties && g_mono.property_get_name) {
        void* piter = nullptr;
        MonoProperty* prop;
        bool hasProps = false;
        while ((prop = g_mono.class_get_properties(klass, &piter)) != nullptr) {
            if (!hasProps) { if (hasFields) g_out << "\n"; g_out << "    // Properties\n"; hasProps = true; }
            const char* pname = g_mono.property_get_name(prop);
            std::string propType = "???";
            if (g_mono.property_get_get_method) {
                MonoMethod* getter = g_mono.property_get_get_method(prop);
                if (getter) {
                    MonoMethodSignature* gsig = g_mono.method_signature(getter);
                    if (gsig) propType = GetTypeName(g_mono.signature_get_return_type(gsig));
                }
            }
            bool hasGet = g_mono.property_get_get_method && g_mono.property_get_get_method(prop);
            bool hasSet = g_mono.property_get_set_method && g_mono.property_get_set_method(prop);
            g_out << "    [P] " << propType << " " << SafeStr(pname)
                  << " { " << (hasGet ? "get; " : "") << (hasSet ? "set; " : "") << "}\n";
        }
    }

    // Methods
    void* miter = nullptr;
    MonoMethod* method;
    bool hasMethods = false;

    while ((method = g_mono.class_get_methods(klass, &miter)) != nullptr) {
        if (!hasMethods) { if (hasFields) g_out << "\n"; g_out << "    // Methods\n"; hasMethods = true; }

        void* compiled = SafeCompileMethod(method);
        uint32_t token = g_mono.method_get_token ? g_mono.method_get_token(method) : 0;
        std::string signature = BuildMethodSignature(method);

        if (compiled)
            g_out << "    [M] 0x" << std::hex << reinterpret_cast<uintptr_t>(compiled) << std::dec;
        else
            g_out << "    [M] <no-jit>     ";

        if (token) g_out << "  [T:0x" << std::hex << token << std::dec << "]";
        g_out << "  " << signature << "\n";
        g_totalMethods++;
    }

    g_out << "  }\n";
}

// ── Assembly dump ──────────────────────────────────────────────────
static void DumpAssembly(const AssemblyEntry& entry) {
    g_out << "\n" << std::string(72, '=') << "\n";
    g_out << "[Assembly] " << entry.name << "\n";
    if (g_mono.image_get_filename) {
        const char* fn = g_mono.image_get_filename(entry.image);
        if (fn) g_out << "[Path]     " << fn << "\n";
    }
    g_out << std::string(72, '=') << "\n";

    const MonoTableInfo* tdef = g_mono.image_get_table_info(entry.image, MONO_TABLE_TYPEDEF);
    if (!tdef) { g_out << "  <no type table>\n"; return; }

    int rows = g_mono.table_info_get_rows(tdef);
    g_out << "[Types]    " << rows << " type definitions\n";

    for (int i = 0; i < rows; i++) {
        uint32_t token = mono_token(MONO_TABLE_TYPEDEF, i);
        MonoClass* klass = g_mono.class_get(entry.image, token);
        if (klass) DumpClass(klass);
    }
}

// ── Çıktı dosya yolu ──────────────────────────────────────────────
static std::string GetOutputPath() {
    char desktop[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", desktop, MAX_PATH))
        return std::string(desktop) + "\\Desktop\\albion_dump.txt";
    return "C:\\albion_dump.txt";
}

// ── Log dosyası (debug için) ───────────────────────────────────────
static void WriteLog(const std::string& msg) {
    char desktop[MAX_PATH];
    std::string logPath = "C:\\albion_proxy_log.txt";
    if (GetEnvironmentVariableA("USERPROFILE", desktop, MAX_PATH))
        logPath = std::string(desktop) + "\\Desktop\\albion_proxy_log.txt";

    std::ofstream log(logPath, std::ios::app);
    if (log.is_open()) {
        auto now = std::time(nullptr);
        auto tm = std::localtime(&now);
        log << std::put_time(tm, "[%H:%M:%S] ") << msg << "\n";
        log.close();
    }
}

// ── Ana dumper thread'i ────────────────────────────────────────────
static DWORD WINAPI DumperThread(LPVOID) {
    WriteLog("Dumper thread baslatildi, Mono bekleniyor...");

    // Mono runtime'ın yüklenmesini bekle
    // Oyun başlarken version.dll çok erken yüklenir, Mono henüz hazır değil
    int waitSeconds = 0;
    const int maxWait = 120; // max 2 dakika bekle

    while (waitSeconds < maxWait) {
        if (GetModuleHandleA("mono-2.0-bdwgc.dll") ||
            GetModuleHandleA("mono-2.0-sgen.dll") ||
            GetModuleHandleA("mono.dll")) {
            break;
        }
        Sleep(2000);
        waitSeconds += 2;
    }

    if (waitSeconds >= maxWait) {
        WriteLog("HATA: Mono runtime bulunamadi (timeout)");
        MessageBoxA(nullptr, "Mono runtime bulunamadi!\nOyun Mono kullaniyor mu?",
                     "Albion Dumper", MB_ICONERROR);
        return 1;
    }

    WriteLog("Mono DLL tespit edildi, initialization bekleniyor...");

    // Mono'nun kendini initialize etmesini bekle
    Sleep(10000);

    WriteLog("Mono API cozumleniyor...");

    // Mono API resolve
    if (!g_mono.Resolve()) {
        WriteLog("HATA: Mono API cozumlenemedi");
        MessageBoxA(nullptr, "Mono API cozumlenemedi!",
                     "Albion Dumper", MB_ICONERROR);
        return 1;
    }

    WriteLog("Mono API basariyla cozumlendi");

    // Mono thread'e bağlan
    MonoDomain* domain = g_mono.get_root_domain();
    if (!domain) {
        WriteLog("HATA: Mono root domain bulunamadi");
        MessageBoxA(nullptr, "Mono root domain bulunamadi!",
                     "Albion Dumper", MB_ICONERROR);
        return 1;
    }

    MonoThread* thread = g_mono.thread_attach(domain);
    WriteLog("Mono thread'e baglanildi");

    // Assembly'leri topla
    g_assemblies.clear();
    g_mono.assembly_foreach(CollectAssembly, nullptr);
    std::sort(g_assemblies.begin(), g_assemblies.end(),
        [](const AssemblyEntry& a, const AssemblyEntry& b) { return a.name < b.name; });

    WriteLog("Assembly sayisi: " + std::to_string(g_assemblies.size()));

    // Çıktı dosyasını aç
    std::string outPath = GetOutputPath();
    g_out.open(outPath, std::ios::out | std::ios::trunc);
    if (!g_out.is_open()) {
        WriteLog("HATA: Dosya acilamadi: " + outPath);
        if (thread && g_mono.thread_detach) g_mono.thread_detach(thread);
        return 1;
    }

    // Header
    auto now = std::time(nullptr);
    auto tm  = std::localtime(&now);

    g_out << "========================================================================\n";
    g_out << "  ALBION ONLINE - MONO OFFSET DUMP (version.dll proxy)\n";
    g_out << "========================================================================\n\n";
    g_out << "Timestamp:   " << std::put_time(tm, "%Y-%m-%d %H:%M:%S") << "\n";
    g_out << "Assemblies:  " << g_assemblies.size() << "\n";
    g_out << "Process ID:  " << GetCurrentProcessId() << "\n\n";
    g_out << "Legend:\n";
    g_out << "  [S] = Static field    offset = static area offset\n";
    g_out << "      = Instance field  offset = object instance offset\n";
    g_out << "  [P] = Property        { get; set; }\n";
    g_out << "  [M] = Method          address = JIT compiled native address\n";
    g_out << "  [T:0x...] = Metadata token\n\n";

    WriteLog("Dump basliyor...");

    // Her assembly'yi dump et
    for (const auto& entry : g_assemblies) {
        DumpAssembly(entry);
    }

    // Footer
    g_out << "\n\n" << std::string(72, '=') << "\n";
    g_out << "DUMP TAMAMLANDI\n";
    g_out << "  Toplam Class:   " << g_totalClasses << "\n";
    g_out << "  Toplam Field:   " << g_totalFields  << "\n";
    g_out << "  Toplam Method:  " << g_totalMethods << "\n";
    g_out << std::string(72, '=') << "\n";

    g_out.close();

    if (thread && g_mono.thread_detach)
        g_mono.thread_detach(thread);

    WriteLog("Dump tamamlandi: " + outPath);

    std::string msg = "Dump tamamlandi!\n\n"
                      "Classes: " + std::to_string(g_totalClasses) + "\n"
                      "Fields:  " + std::to_string(g_totalFields)  + "\n"
                      "Methods: " + std::to_string(g_totalMethods) + "\n\n"
                      "Dosya: " + outPath;
    MessageBoxA(nullptr, msg.c_str(), "Albion Dumper", MB_ICONINFORMATION);

    return 0;
}

// ── Albion process'inde miyiz kontrol et ───────────────────────────
static bool IsAlbionProcess() {
    char exeName[MAX_PATH];
    GetModuleFileNameA(nullptr, exeName, MAX_PATH);

    std::string path(exeName);
    // Küçük harfe çevir
    for (auto& c : path) c = (char)tolower(c);

    return path.find("albion") != std::string::npos;
}


// ═══════════════════════════════════════════════════════════════════
//  BÖLÜM 3: INTERNAL RADAR (GDI)
// ═══════════════════════════════════════════════════════════════════

void RenderRadar(HDC hdc) {
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, RadarConfig::WindowWidth, RadarConfig::WindowHeight);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hbmMem);

    HBRUSH hbgBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT rect = { 0, 0, RadarConfig::WindowWidth, RadarConfig::WindowHeight };
    FillRect(hdcMem, &rect, hbgBrush);
    DeleteObject(hbgBrush);

    int centerX = RadarConfig::RadarRadius + 20;
    int centerY = RadarConfig::RadarRadius + 20;
    int radius  = static_cast<int>(RadarConfig::RadarRadius);

    HBRUSH hRadarBg = CreateSolidBrush(RGB(18, 22, 30));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hRadarBg);
    HPEN hBorderPen = CreatePen(PS_SOLID, 2, RGB(0, 255, 180));
    HPEN hOldPen = (HPEN)SelectObject(hdcMem, hBorderPen);

    Ellipse(hdcMem, centerX - radius, centerY - radius, centerX + radius, centerY + radius);

    HPEN hGridPen = CreatePen(PS_DOT, 1, RGB(60, 80, 100));
    SelectObject(hdcMem, hGridPen);
    
    MoveToEx(hdcMem, centerX - radius, centerY, NULL);
    LineTo(hdcMem, centerX + radius, centerY);
    MoveToEx(hdcMem, centerX, centerY - radius, NULL);
    LineTo(hdcMem, centerX, centerY + radius);

    SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
    Ellipse(hdcMem, centerX - radius / 2, centerY - radius / 2, centerX + radius / 2, centerY + radius / 2);
    DeleteObject(hGridPen);

    HBRUSH hSelfBrush = CreateSolidBrush(RGB(0, 220, 255));
    SelectObject(hdcMem, hSelfBrush);
    Ellipse(hdcMem, centerX - 4, centerY - 4, centerX + 4, centerY + 4);
    DeleteObject(hSelfBrush);

    SetBkMode(hdcMem, TRANSPARENT);
    HFONT hFont = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

    std::vector<RadarEntity> drawEntities;
    {
        std::lock_guard<std::mutex> lock(g_entitiesMutex);
        drawEntities = g_entities;
    }

    for (const auto& entity : drawEntities) {
        float dx = entity.posX - g_localX;
        float dy = entity.posY - g_localY;
        float dist = std::sqrt(dx * dx + dy * dy);

        float normDist = dist / RadarConfig::MaxDistance;
        if (normDist > 1.0f) normDist = 1.0f;

        float angle = std::atan2(dy, dx);
        int dotX = centerX + static_cast<int>(normDist * radius * std::cos(angle));
        int dotY = centerY - static_cast<int>(normDist * radius * std::sin(angle));

        COLORREF color = entity.isEnemy ? RGB(255, 60, 60) : RGB(60, 255, 100);
        HBRUSH hDotBrush = CreateSolidBrush(color);
        HPEN hDotPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        SelectObject(hdcMem, hDotBrush);
        SelectObject(hdcMem, hDotPen);

        Ellipse(hdcMem, dotX - 4, dotY - 4, dotX + 4, dotY + 4);

        SetTextColor(hdcMem, RGB(240, 240, 240));
        std::string label = entity.name;
        TextOutA(hdcMem, dotX + 7, dotY - 6, label.c_str(), static_cast<int>(label.length()));

        DeleteObject(hDotBrush);
        DeleteObject(hDotPen);
    }

    BitBlt(hdc, 0, 0, RadarConfig::WindowWidth, RadarConfig::WindowHeight, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);
    SelectObject(hdcMem, hOldPen);
    DeleteObject(hBorderPen);
    SelectObject(hdcMem, hOldBrush);
    DeleteObject(hRadarBg);
    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

LRESULT CALLBACK WindowProcInternal(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RenderRadar(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_isRunning = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Internal Scanner Thread
void InternalRadarScanner() {
    HMODULE hUnity = NULL;
    while (g_isRunning && !hUnity) {
        hUnity = GetModuleHandleA("UnityPlayer.dll");
        if (!hUnity) Sleep(1000);
    }
    if (!g_isRunning) return;

    uintptr_t unityPlayerBase = reinterpret_cast<uintptr_t>(hUnity);
    
    // Basit bir boyut tahmini (Memory API gerektirmeden)
    size_t unityPlayerSize = 0x5000000; 

    static uintptr_t gomPtr = 0;
    const char* sigs[] = {
        "48 8B 15 ? ? ? ? 66 39", 
        "48 8B 05 ? ? ? ? 48 8B 08 4C 8B 01", 
        "48 8B 0D ? ? ? ? 48 8D 55 ? 48 8B 01",
        "48 8B 15 ? ? ? ? 48 8B C8 48 83 C4",
        "48 89 05 ? ? ? ? 48 8D 4C 24 ? 48 89 45",
    };

    while (g_isRunning && !gomPtr) {
        for (const char* sig : sigs) {
            uintptr_t sigAddr = FindPatternInternal(unityPlayerBase, unityPlayerSize, sig);
            if (sigAddr) {
                int32_t offset = 0;
                if (SafeRead<int32_t>(sigAddr + 3, offset)) {
                    gomPtr = sigAddr + 7 + offset;
                }
                if (gomPtr) break;
            }
        }
        if (!gomPtr) Sleep(1000);
    }

    while (g_isRunning && gomPtr) {
        uintptr_t actualGom = 0;
        if (!SafeRead<uintptr_t>(gomPtr, actualGom) || !actualGom) { Sleep(100); continue; }

        uintptr_t activeNodes = 0;
        if (!SafeRead<uintptr_t>(actualGom + Offsets::GOM_ActiveNodes, activeNodes) || !activeNodes) { Sleep(100); continue; }

        uintptr_t node = activeNodes;
        std::vector<RadarEntity> newEntities;
        int count = 0;

        while (node && count < 2000) {
            uintptr_t gameObject = 0;
            if (!SafeRead<uintptr_t>(node + Offsets::Node_GameObject, gameObject) || !gameObject) break;

            uintptr_t namePtr = 0;
            if (SafeRead<uintptr_t>(gameObject + Offsets::GO_Name, namePtr) && namePtr) {
                std::string name = ReadStringInternal(namePtr, 24);

                if (name.length() > 2) {
                    uintptr_t components = 0;
                    uintptr_t transformComp = 0;
                    uintptr_t transform = 0;
                    Vector3D pos = {};

                    if (SafeRead<uintptr_t>(gameObject + Offsets::GO_Components, components) &&
                        SafeRead<uintptr_t>(components + Offsets::Component_Transform, transformComp) &&
                        SafeRead<uintptr_t>(transformComp + 0x10, transform) &&
                        SafeRead<Vector3D>(transform + Offsets::Transform_Matrix, pos))
                    {
                        RadarEntity ent;
                        ent.name = name;
                        ent.posX = pos.x;
                        ent.posY = pos.z;
                        ent.isEnemy = (name.find("Mob") != std::string::npos || name.find("Enemy") != std::string::npos);

                        if (name.find("LocalPlayer") != std::string::npos) {
                            g_localX = pos.x;
                            g_localY = pos.z;
                        } else {
                            newEntities.push_back(ent);
                        }
                    }
                }
            }

            uintptr_t nextNode = 0;
            if (!SafeRead<uintptr_t>(node + Offsets::Node_Next, nextNode)) break;
            node = nextNode;
            count++;
        }

        {
            std::lock_guard<std::mutex> lock(g_entitiesMutex);
            g_entities = newEntities;
        }
        Sleep(50);
    }
}

// Window Thread
DWORD WINAPI InternalRadarUIThread(LPVOID hInstance) {
    std::thread scanner(InternalRadarScanner);
    scanner.detach();

    const char* className = "InternalRadarClass";
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc   = WindowProcInternal;
    wc.hInstance     = (HINSTANCE)hInstance;
    wc.lpszClassName = className;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        className, "Albion INTERNAL Radar", WS_POPUP,
        RadarConfig::PosX, RadarConfig::PosY,
        RadarConfig::WindowWidth, RadarConfig::WindowHeight,
        NULL, NULL, (HINSTANCE)hInstance, NULL
    );

    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 230, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOWNA);
    UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, 1, 16, NULL);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) && g_isRunning) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}


// ═══════════════════════════════════════════════════════════════════
//  DLL ENTRY POINT
// ═══════════════════════════════════════════════════════════════════

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // 1. Gerçek version.dll'i yükle
        if (!LoadRealVersionDll()) {
            // Gerçek DLL yüklenemezse devam edemeyiz
            return FALSE;
        }

        // 2. Albion process'inde miyiz kontrol et
        if (IsAlbionProcess()) {
            WriteLog("Albion Online process'i tespit edildi, dumper baslatiliyor...");
            HANDLE hThread = CreateThread(nullptr, 0, DumperThread, nullptr, 0, nullptr);
            if (hThread) CloseHandle(hThread);

            // 3. İç (Internal) Radarı Başlat
            HANDLE hRadarThread = CreateThread(nullptr, 0, InternalRadarUIThread, hModule, 0, nullptr);
            if (hRadarThread) CloseHandle(hRadarThread);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_isRunning = false;
        if (g_hwnd) {
            PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        }
        if (g_hRealVersion) {
            FreeLibrary(g_hRealVersion);
            g_hRealVersion = nullptr;
        }
    }

    return TRUE;
}
