/*
 * dumper.cpp — Albion Online Mono Offset Dumper (DLL)
 *
 * Injected DLL that resolves the Mono runtime API, walks all loaded assemblies,
 * and dumps every class with its field offsets and method addresses to a file.
 *
 * Output: %USERPROFILE%\Desktop\albion_dump.txt
 *
 * Build as a DLL (see CMakeLists.txt).
 */

#include <windows.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include "../include/mono.h"

// ── Globals ────────────────────────────────────────────────────────
static MonoApi g_mono;
static std::ofstream g_out;
static int g_totalClasses = 0;
static int g_totalFields  = 0;
static int g_totalMethods = 0;

// ── Utility ────────────────────────────────────────────────────────
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

    // Try full name first (includes namespace)
    if (g_mono.type_full_name) {
        const char* full = g_mono.type_full_name(type);
        if (full) return full;
    }

    const char* name = g_mono.type_get_name(type);
    return name ? name : "???";
}

static std::string BuildMethodSignature(MonoMethod* method) {
    std::ostringstream sig;

    // Flags
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

    // Return type
    MonoType* retType = g_mono.signature_get_return_type(msig);
    sig << GetTypeName(retType) << " ";

    // Name
    sig << SafeStr(g_mono.method_get_name(method));

    // Parameters
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

// ── Assembly collector callback ────────────────────────────────────
struct AssemblyEntry {
    MonoAssembly* assembly;
    MonoImage*    image;
    std::string   name;
};

static std::vector<AssemblyEntry> g_assemblies;

static void __cdecl CollectAssembly(MonoAssembly* assembly, void* /*userData*/) {
    MonoImage* image = g_mono.assembly_get_image(assembly);
    if (!image) return;

    AssemblyEntry entry;
    entry.assembly = assembly;
    entry.image    = image;
    entry.name     = SafeStr(g_mono.image_get_name(image));
    g_assemblies.push_back(entry);
}

// ── Safe JIT compile (isolated SEH — no C++ objects allowed here) ──
static void* SafeCompileMethod(MonoMethod* method) {
    if (!g_mono.compile_method) return nullptr;
    __try {
        return g_mono.compile_method(method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ── Dump a single class ────────────────────────────────────────────
static void DumpClass(MonoClass* klass) {
    if (!klass) return;

    const char* ns   = g_mono.class_get_namespace(klass);
    const char* name = g_mono.class_get_name(klass);
    if (!name) return;

    // Build fully qualified name
    std::string fqn;
    if (ns && ns[0]) {
        fqn = std::string(ns) + "." + name;
    } else {
        fqn = name;
    }

    // Parent class
    std::string parentName = "";
    MonoClass* parent = g_mono.class_get_parent(klass);
    if (parent) {
        const char* pns = g_mono.class_get_namespace(parent);
        const char* pn  = g_mono.class_get_name(parent);
        if (pns && pns[0])
            parentName = std::string(pns) + "." + SafeStr(pn);
        else
            parentName = SafeStr(pn);
    }

    // Class metadata
    std::string classKind = "class";
    if (g_mono.class_is_enum && g_mono.class_is_enum(klass))
        classKind = "enum";
    else if (g_mono.class_is_valuetype && g_mono.class_is_valuetype(klass))
        classKind = "struct";

    int instanceSize = 0;
    if (g_mono.class_instance_size) {
        instanceSize = g_mono.class_instance_size(klass);
    }

    // Header
    g_out << "\n  " << classKind << " " << fqn;
    if (!parentName.empty() && parentName != "System.Object" && parentName != "System.ValueType" && parentName != "System.Enum") {
        g_out << " : " << parentName;
    }
    if (instanceSize > 0) {
        g_out << "  [Size: 0x" << std::hex << instanceSize << std::dec << "]";
    }
    g_out << "\n";
    g_out << "  {\n";

    g_totalClasses++;

    // ── Fields ─────────────────────────────────────────────────────
    void* fiter = nullptr;
    MonoClassField* field;
    bool hasFields = false;

    while ((field = g_mono.class_get_fields(klass, &fiter)) != nullptr) {
        if (!hasFields) {
            g_out << "    // Fields\n";
            hasFields = true;
        }

        const char* fname = g_mono.field_get_name(field);
        int offset = g_mono.field_get_offset(field);
        MonoType* ftype = g_mono.field_get_type(field);
        std::string typeName = GetTypeName(ftype);

        // Flags
        std::string flagsStr;
        if (g_mono.field_get_flags) {
            uint32_t fflags = g_mono.field_get_flags(field);
            flagsStr = GetFieldFlagsStr(fflags);

            // Static fields have a different offset meaning
            if (fflags & FIELD_ATTR_STATIC) {
                g_out << "    [S] 0x" << std::hex << std::setw(4) << std::setfill('0')
                       << offset << std::dec << "  "
                       << flagsStr << typeName << " " << SafeStr(fname) << "\n";
            } else {
                g_out << "        0x" << std::hex << std::setw(4) << std::setfill('0')
                       << offset << std::dec << "  "
                       << flagsStr << typeName << " " << SafeStr(fname) << "\n";
            }
        } else {
            g_out << "        0x" << std::hex << std::setw(4) << std::setfill('0')
                   << offset << std::dec << "  "
                   << typeName << " " << SafeStr(fname) << "\n";
        }

        g_totalFields++;
    }

    // ── Properties ─────────────────────────────────────────────────
    if (g_mono.class_get_properties && g_mono.property_get_name) {
        void* piter = nullptr;
        MonoProperty* prop;
        bool hasProps = false;

        while ((prop = g_mono.class_get_properties(klass, &piter)) != nullptr) {
            if (!hasProps) {
                if (hasFields) g_out << "\n";
                g_out << "    // Properties\n";
                hasProps = true;
            }

            const char* pname = g_mono.property_get_name(prop);
            std::string propType = "???";

            // Try to get type from getter return type
            if (g_mono.property_get_get_method) {
                MonoMethod* getter = g_mono.property_get_get_method(prop);
                if (getter) {
                    MonoMethodSignature* gsig = g_mono.method_signature(getter);
                    if (gsig) {
                        MonoType* retType = g_mono.signature_get_return_type(gsig);
                        propType = GetTypeName(retType);
                    }
                }
            }

            bool hasGet = g_mono.property_get_get_method && g_mono.property_get_get_method(prop);
            bool hasSet = g_mono.property_get_set_method && g_mono.property_get_set_method(prop);

            g_out << "    [P] " << propType << " " << SafeStr(pname)
                   << " { " << (hasGet ? "get; " : "") << (hasSet ? "set; " : "") << "}\n";
        }
    }

    // ── Methods ────────────────────────────────────────────────────
    void* miter = nullptr;
    MonoMethod* method;
    bool hasMethods = false;

    while ((method = g_mono.class_get_methods(klass, &miter)) != nullptr) {
        if (!hasMethods) {
            if (hasFields) g_out << "\n";
            g_out << "    // Methods\n";
            hasMethods = true;
        }

        // Try to JIT compile and get native address
        void* compiled = SafeCompileMethod(method);

        // Method token
        uint32_t token = 0;
        if (g_mono.method_get_token)
            token = g_mono.method_get_token(method);

        std::string signature = BuildMethodSignature(method);

        if (compiled) {
            g_out << "    [M] 0x" << std::hex << reinterpret_cast<uintptr_t>(compiled)
                   << std::dec;
        } else {
            g_out << "    [M] <no-jit>     ";
        }

        if (token) {
            g_out << "  [T:0x" << std::hex << token << std::dec << "]";
        }

        g_out << "  " << signature << "\n";

        g_totalMethods++;
    }

    g_out << "  }\n";
}

// ── Dump a single assembly ─────────────────────────────────────────
static void DumpAssembly(const AssemblyEntry& entry) {
    g_out << "\n" << std::string(72, '=') << "\n";
    g_out << "[Assembly] " << entry.name << "\n";

    if (g_mono.image_get_filename) {
        const char* fname = g_mono.image_get_filename(entry.image);
        if (fname) g_out << "[Path]     " << fname << "\n";
    }

    g_out << std::string(72, '=') << "\n";

    const MonoTableInfo* tdef = g_mono.image_get_table_info(entry.image, MONO_TABLE_TYPEDEF);
    if (!tdef) {
        g_out << "  <no type table>\n";
        return;
    }

    int rows = g_mono.table_info_get_rows(tdef);
    g_out << "[Types]    " << rows << " type definitions\n";

    for (int i = 0; i < rows; i++) {
        uint32_t token = mono_token(MONO_TABLE_TYPEDEF, i);
        MonoClass* klass = g_mono.class_get(entry.image, token);
        if (klass) {
            DumpClass(klass);
        }
    }
}

// ── Output file path ───────────────────────────────────────────────
static std::string GetOutputPath() {
    char desktop[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", desktop, MAX_PATH)) {
        return std::string(desktop) + "\\Desktop\\albion_dump.txt";
    }
    return "C:\\albion_dump.txt";
}

// ── Worker thread ──────────────────────────────────────────────────
static DWORD WINAPI DumperThread(LPVOID /*param*/) {
    // Small delay to let the game finish initializing
    Sleep(3000);

    // Resolve Mono API
    if (!g_mono.Resolve()) {
        MessageBoxA(nullptr,
            "Failed to resolve Mono API.\n"
            "Make sure the game is fully loaded before injecting.\n\n"
            "Tried: mono-2.0-bdwgc.dll, mono-2.0-sgen.dll, mono.dll",
            "Albion Dumper - Error", MB_ICONERROR);
        return 1;
    }

    // Attach to Mono runtime
    MonoDomain* domain = g_mono.get_root_domain();
    if (!domain) {
        MessageBoxA(nullptr, "Failed to get Mono root domain.", "Albion Dumper - Error", MB_ICONERROR);
        return 1;
    }

    MonoThread* thread = g_mono.thread_attach(domain);

    // Collect all loaded assemblies
    g_assemblies.clear();
    g_mono.assembly_foreach(CollectAssembly, nullptr);

    // Sort by name for clean output
    std::sort(g_assemblies.begin(), g_assemblies.end(),
        [](const AssemblyEntry& a, const AssemblyEntry& b) { return a.name < b.name; });

    // Open output file
    std::string outPath = GetOutputPath();
    g_out.open(outPath, std::ios::out | std::ios::trunc);
    if (!g_out.is_open()) {
        MessageBoxA(nullptr, ("Failed to open: " + outPath).c_str(), "Albion Dumper - Error", MB_ICONERROR);
        if (thread && g_mono.thread_detach) g_mono.thread_detach(thread);
        return 1;
    }

    // Header
    auto now = std::time(nullptr);
    auto tm  = std::localtime(&now);

    g_out << "╔══════════════════════════════════════════════════════════════════════╗\n";
    g_out << "║           ALBION ONLINE — MONO OFFSET DUMP                         ║\n";
    g_out << "╚══════════════════════════════════════════════════════════════════════╝\n\n";
    g_out << "Timestamp:   " << std::put_time(tm, "%Y-%m-%d %H:%M:%S") << "\n";
    g_out << "Assemblies:  " << g_assemblies.size() << "\n";
    g_out << "Mono DLL:    " << (g_mono.hMono ? "resolved" : "???") << "\n";
    g_out << "Process ID:  " << GetCurrentProcessId() << "\n\n";

    g_out << "Legend:\n";
    g_out << "  [S] = Static field    offset = static area offset\n";
    g_out << "      = Instance field  offset = object instance offset\n";
    g_out << "  [P] = Property        { get; set; }\n";
    g_out << "  [M] = Method          address = JIT compiled native address\n";
    g_out << "  [T:0x...] = Metadata token\n\n";

    // Dump each assembly
    for (const auto& entry : g_assemblies) {
        DumpAssembly(entry);
    }

    // Footer
    g_out << "\n\n" << std::string(72, '=') << "\n";
    g_out << "DUMP COMPLETE\n";
    g_out << "  Total Classes:  " << g_totalClasses << "\n";
    g_out << "  Total Fields:   " << g_totalFields  << "\n";
    g_out << "  Total Methods:  " << g_totalMethods << "\n";
    g_out << std::string(72, '=') << "\n";

    g_out.close();

    // Detach from Mono
    if (thread && g_mono.thread_detach)
        g_mono.thread_detach(thread);

    // Notify user
    std::string msg = "Dump complete!\n\n"
                      "Classes: " + std::to_string(g_totalClasses) + "\n"
                      "Fields:  " + std::to_string(g_totalFields)  + "\n"
                      "Methods: " + std::to_string(g_totalMethods) + "\n\n"
                      "Output: " + outPath;
    MessageBoxA(nullptr, msg.c_str(), "Albion Dumper", MB_ICONINFORMATION);

    // Self-unload (optional — comment out to keep DLL loaded)
    FreeLibraryAndExitThread(GetModuleHandleA("dumper.dll"), 0);
    return 0;
}

// ── DLL entry point ────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, DumperThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
