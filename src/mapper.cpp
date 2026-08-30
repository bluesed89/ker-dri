/*
 * mapper.cpp — Manual Mapping Injector for Albion Online
 *
 * LoadLibrary kullanmadan DLL'i hedef processe map'ler.
 * PE header'ları parse eder, section'ları yazar, relocation ve import'ları
 * çözer, shellcode ile DllMain'i çağırır.
 *
 * Avantajlar:
 *   - Modül listesinde görünmez (PEB->Ldr'de kayıt yok)
 *   - EAC'nin basit modül taramasını bypass eder
 *   - PE header'ları silerek tespit zorlaşır
 *
 * Usage:  mapper.exe [path_to_dumper.dll]
 * Build:  cmake --build build --config Release
 */

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>

namespace fs = std::filesystem;

// ── Console colors ─────────────────────────────────────────────────
enum Color { RED = 12, GREEN = 10, YELLOW = 14, CYAN = 11, WHITE = 15, GRAY = 8 };

static void SetColor(Color c) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), c);
}

static void Print(Color c, const char* prefix, const std::string& msg) {
    SetColor(c);
    std::cout << prefix;
    SetColor(WHITE);
    std::cout << " " << msg << std::endl;
}

#define LOG_OK(msg)    Print(GREEN,  "[+]", msg)
#define LOG_INFO(msg)  Print(CYAN,   "[*]", msg)
#define LOG_WARN(msg)  Print(YELLOW, "[!]", msg)
#define LOG_ERR(msg)   Print(RED,    "[-]", msg)
#define LOG_DBG(msg)   Print(GRAY,   "[~]", msg)

// ── Process finder ─────────────────────────────────────────────────
static const char* TARGET_NAMES[] = {
    "Albion-Online.exe",
    "AlbionOnline.exe",
    "Albion Online.exe",
};

struct ProcessInfo {
    DWORD pid = 0;
    std::string name;
};

static ProcessInfo FindTarget() {
    ProcessInfo result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe)) {
        do {
            for (auto target : TARGET_NAMES) {
                if (_stricmp(pe.szExeFile, target) == 0) {
                    result.pid  = pe.th32ProcessID;
                    result.name = pe.szExeFile;
                    CloseHandle(snap);
                    return result;
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

// ── Elevation check ────────────────────────────────────────────────
static bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev = {};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size))
            elevated = elev.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// ── SeDebugPrivilege aktif et ──────────────────────────────────────
// Bu olmadan Windows korumalı process'lere (EAC korumalı oyunlar gibi)
// tam erişim sağlayamaz. Yönetici olsan bile lazım.
static bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        LOG_ERR("Token acilamadi (err: " + std::to_string(GetLastError()) + ")");
        return false;
    }

    LUID luid;
    if (!LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &luid)) {
        LOG_ERR("SeDebugPrivilege bulunamadi");
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        LOG_ERR("AdjustTokenPrivileges basarisiz (err: " + std::to_string(GetLastError()) + ")");
        CloseHandle(token);
        return false;
    }

    DWORD err = GetLastError();
    CloseHandle(token);

    if (err == ERROR_NOT_ALL_ASSIGNED) {
        LOG_ERR("SeDebugPrivilege aktif edilemedi — yonetici olarak calistir!");
        return false;
    }

    LOG_OK("SeDebugPrivilege aktif edildi");
    return true;
}

// ── Get module base in remote process ──────────────────────────────
static uintptr_t GetRemoteModuleBase(DWORD pid, const char* moduleName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);

    uintptr_t base = 0;
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, moduleName) == 0) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// ═══════════════════════════════════════════════════════════════════
//  MANUAL MAPPER
// ═══════════════════════════════════════════════════════════════════

class ManualMapper {
public:
    bool Map(DWORD pid, const std::string& dllPath) {
        // 1. Read DLL from disk
        if (!ReadDll(dllPath)) return false;

        // 2. Open target process with full access
        hProc_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!hProc_) {
            DWORD err = GetLastError();
            LOG_ERR("OpenProcess basarisiz (err: " + std::to_string(err) + ")");
            if (err == 5) LOG_ERR("Erisim reddedildi — yonetici + SeDebugPrivilege gerekli!");
            if (err == 87) LOG_ERR("Gecersiz parametre — PID yanlis olabilir");
            return false;
        }
        LOG_OK("Process acildi (PROCESS_ALL_ACCESS)");

        // 3. Allocate memory in target process
        if (!AllocateRemote()) { Cleanup(); return false; }

        // 4. Map PE sections
        if (!MapSections()) { Cleanup(); return false; }

        // 5. Process base relocations
        if (!ProcessRelocations()) { Cleanup(); return false; }

        // 6. Resolve imports
        if (!ResolveImports()) { Cleanup(); return false; }

        // 7. Write the mapped image to target
        if (!WriteToTarget()) { Cleanup(); return false; }

        // 8. Execute DllMain via shellcode
        if (!ExecuteEntry()) { Cleanup(); return false; }

        // 9. Erase PE headers (stealth)
        EraseHeaders();

        LOG_OK("Manual mapping tamamlandı!");
        CloseHandle(hProc_);
        return true;
    }

private:
    HANDLE               hProc_      = nullptr;
    std::vector<uint8_t> rawDll_;               // DLL dosyası (diskten okunan)
    std::vector<uint8_t> mappedImage_;           // Map'lenmiş imaj (section'lar yerleştirilmiş)
    uintptr_t            remoteBase_ = 0;        // Hedef process'teki tahsis adresi

    PIMAGE_DOS_HEADER    dosHeader_  = nullptr;
    PIMAGE_NT_HEADERS    ntHeaders_  = nullptr;

    // ── Read DLL from disk ─────────────────────────────────────────
    bool ReadDll(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            LOG_ERR("DLL okunamadı: " + path);
            return false;
        }

        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        rawDll_.resize(fileSize);
        file.read(reinterpret_cast<char*>(rawDll_.data()), fileSize);
        file.close();

        // Validate PE
        dosHeader_ = reinterpret_cast<PIMAGE_DOS_HEADER>(rawDll_.data());
        if (dosHeader_->e_magic != IMAGE_DOS_SIGNATURE) {
            LOG_ERR("Geçersiz DOS header (MZ bulunamadı)");
            return false;
        }

        ntHeaders_ = reinterpret_cast<PIMAGE_NT_HEADERS>(rawDll_.data() + dosHeader_->e_lfanew);
        if (ntHeaders_->Signature != IMAGE_NT_SIGNATURE) {
            LOG_ERR("Geçersiz PE header (PE signature bulunamadı)");
            return false;
        }

        if (ntHeaders_->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
            LOG_ERR("DLL x64 değil — Albion 64-bit process, DLL de 64-bit olmalı");
            return false;
        }

        LOG_OK("DLL okundu: " + std::to_string(fileSize) + " bytes, "
               + std::to_string(ntHeaders_->FileHeader.NumberOfSections) + " section");
        LOG_DBG("ImageSize: 0x" + ToHex(ntHeaders_->OptionalHeader.SizeOfImage));
        LOG_DBG("EntryPoint RVA: 0x" + ToHex(ntHeaders_->OptionalHeader.AddressOfEntryPoint));

        return true;
    }

    // ── Allocate in target process ─────────────────────────────────
    bool AllocateRemote() {
        DWORD imageSize = ntHeaders_->OptionalHeader.SizeOfImage;

        // Try preferred base first
        uintptr_t preferred = ntHeaders_->OptionalHeader.ImageBase;
        remoteBase_ = reinterpret_cast<uintptr_t>(
            VirtualAllocEx(hProc_, reinterpret_cast<LPVOID>(preferred),
                           imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

        // If preferred base is taken, let OS choose
        if (!remoteBase_) {
            remoteBase_ = reinterpret_cast<uintptr_t>(
                VirtualAllocEx(hProc_, nullptr, imageSize,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        }

        if (!remoteBase_) {
            DWORD err = GetLastError();
            LOG_ERR("VirtualAllocEx basarisiz (err: " + std::to_string(err) + ")");
            if (err == 5)   LOG_ERR("Erisim reddedildi — handle yeterli izne sahip degil");
            if (err == 87)  LOG_ERR("Gecersiz parametre");
            if (err == 487) LOG_ERR("Adres cakismasi — baska bir adres denenecek");
            return false;
        }

        LOG_OK("Bellek tahsis edildi: 0x" + ToHex(remoteBase_) + " (" + std::to_string(imageSize) + " bytes)");

        // Prepare mapped image buffer
        mappedImage_.resize(imageSize, 0);

        return true;
    }

    // ── Map sections ───────────────────────────────────────────────
    bool MapSections() {
        // Copy headers
        memcpy(mappedImage_.data(), rawDll_.data(), ntHeaders_->OptionalHeader.SizeOfHeaders);

        // Map each section
        auto* section = IMAGE_FIRST_SECTION(ntHeaders_);
        for (WORD i = 0; i < ntHeaders_->FileHeader.NumberOfSections; i++, section++) {
            if (section->SizeOfRawData == 0) continue;

            // Source: raw file offset
            // Dest: mapped virtual address
            memcpy(
                mappedImage_.data() + section->VirtualAddress,
                rawDll_.data() + section->PointerToRawData,
                min(section->SizeOfRawData, section->Misc.VirtualSize));

            char name[9] = {};
            memcpy(name, section->Name, 8);
            LOG_DBG("Section: " + std::string(name)
                    + " → 0x" + ToHex(section->VirtualAddress)
                    + " (" + std::to_string(section->SizeOfRawData) + " bytes)");
        }

        LOG_OK(std::to_string(ntHeaders_->FileHeader.NumberOfSections) + " section map'lendi");
        return true;
    }

    // ── Process base relocations ───────────────────────────────────
    bool ProcessRelocations() {
        // Delta between preferred and actual base
        intptr_t delta = static_cast<intptr_t>(remoteBase_) -
                         static_cast<intptr_t>(ntHeaders_->OptionalHeader.ImageBase);

        if (delta == 0) {
            LOG_INFO("Tercih edilen base adrese yüklendi — relocation gerekmedi");
            return true;
        }

        LOG_INFO("Base delta: 0x" + ToHex(static_cast<uintptr_t>(delta > 0 ? delta : -delta))
                 + (delta > 0 ? " (ileri)" : " (geri)"));

        auto& relocDir = ntHeaders_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (relocDir.VirtualAddress == 0 || relocDir.Size == 0) {
            if (delta != 0) {
                LOG_ERR("Relocation tablosu yok ama delta != 0 — bu DLL relocate edilemez!");
                return false;
            }
            return true;
        }

        auto* relocBlock = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
            mappedImage_.data() + relocDir.VirtualAddress);

        int fixupCount = 0;

        while (relocBlock->VirtualAddress != 0 && relocBlock->SizeOfBlock != 0) {
            DWORD numEntries = (relocBlock->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            auto* entries = reinterpret_cast<WORD*>(reinterpret_cast<uint8_t*>(relocBlock) + sizeof(IMAGE_BASE_RELOCATION));

            for (DWORD i = 0; i < numEntries; i++) {
                int type   = entries[i] >> 12;
                int offset = entries[i] & 0x0FFF;

                if (type == IMAGE_REL_BASED_DIR64) {
                    // 64-bit relocation
                    auto* patchAddr = reinterpret_cast<uint64_t*>(
                        mappedImage_.data() + relocBlock->VirtualAddress + offset);
                    *patchAddr += delta;
                    fixupCount++;
                }
                else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    // 32-bit relocation (rare in x64)
                    auto* patchAddr = reinterpret_cast<uint32_t*>(
                        mappedImage_.data() + relocBlock->VirtualAddress + offset);
                    *patchAddr += static_cast<uint32_t>(delta);
                    fixupCount++;
                }
                else if (type == IMAGE_REL_BASED_ABSOLUTE) {
                    // Padding — skip
                }
            }

            // Next block
            relocBlock = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
                reinterpret_cast<uint8_t*>(relocBlock) + relocBlock->SizeOfBlock);
        }

        LOG_OK(std::to_string(fixupCount) + " relocation düzeltildi");
        return true;
    }

    // ── Resolve imports ────────────────────────────────────────────
    bool ResolveImports() {
        auto& importDir = ntHeaders_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
            LOG_INFO("Import tablosu yok — bağımlılık yok");
            return true;
        }

        auto* importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
            mappedImage_.data() + importDir.VirtualAddress);

        int dllCount = 0;
        int funcCount = 0;

        while (importDesc->Name != 0) {
            const char* dllName = reinterpret_cast<const char*>(
                mappedImage_.data() + importDesc->Name);

            // Load the import DLL in our own process to get function addresses
            // System DLLs (kernel32, user32, etc.) have the same base across processes
            // on the same boot session due to ASLR being per-boot, not per-process
            HMODULE hLocalMod = LoadLibraryA(dllName);
            if (!hLocalMod) {
                LOG_ERR("Import DLL yüklenemedi: " + std::string(dllName));

                // Try loading it in the remote process first
                if (!LoadRemoteDll(dllName)) {
                    LOG_ERR("Uzak process'te de yüklenemedi: " + std::string(dllName));
                    return false;
                }
                hLocalMod = LoadLibraryA(dllName);
                if (!hLocalMod) return false;
            }

            uintptr_t localModBase  = reinterpret_cast<uintptr_t>(hLocalMod);
            uintptr_t remoteModBase = GetRemoteModuleBase(GetProcessId(hProc_), dllName);

            if (!remoteModBase) {
                // Module not in remote process, load it there
                if (!LoadRemoteDll(dllName)) {
                    LOG_ERR("Import DLL uzak process'e yüklenemedi: " + std::string(dllName));
                    return false;
                }
                remoteModBase = GetRemoteModuleBase(GetProcessId(hProc_), dllName);
                if (!remoteModBase) {
                    LOG_ERR("Import DLL base bulunamadı: " + std::string(dllName));
                    return false;
                }
            }

            // Walk IAT (Import Address Table)
            auto* thunkRef = reinterpret_cast<PIMAGE_THUNK_DATA>(
                mappedImage_.data() + importDesc->OriginalFirstThunk);
            auto* iatEntry = reinterpret_cast<PIMAGE_THUNK_DATA>(
                mappedImage_.data() + importDesc->FirstThunk);

            // If OriginalFirstThunk is 0, use FirstThunk
            if (importDesc->OriginalFirstThunk == 0) {
                thunkRef = iatEntry;
            }

            while (thunkRef->u1.AddressOfData != 0) {
                uintptr_t funcAddr = 0;

                if (IMAGE_SNAP_BY_ORDINAL64(thunkRef->u1.Ordinal)) {
                    // Import by ordinal
                    WORD ordinal = IMAGE_ORDINAL64(thunkRef->u1.Ordinal);
                    uintptr_t localAddr = reinterpret_cast<uintptr_t>(
                        GetProcAddress(hLocalMod, MAKEINTRESOURCEA(ordinal)));
                    if (!localAddr) {
                        LOG_ERR("Ordinal çözülemedi: " + std::string(dllName) + " #" + std::to_string(ordinal));
                        return false;
                    }
                    // Translate local → remote
                    funcAddr = localAddr - localModBase + remoteModBase;
                }
                else {
                    // Import by name
                    auto* importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                        mappedImage_.data() + thunkRef->u1.AddressOfData);
                    const char* funcName = importByName->Name;

                    uintptr_t localAddr = reinterpret_cast<uintptr_t>(
                        GetProcAddress(hLocalMod, funcName));
                    if (!localAddr) {
                        LOG_ERR("Fonksiyon çözülemedi: " + std::string(dllName) + "!" + funcName);
                        return false;
                    }
                    // Translate local → remote
                    funcAddr = localAddr - localModBase + remoteModBase;
                }

                // Write resolved address to IAT
                iatEntry->u1.Function = funcAddr;

                thunkRef++;
                iatEntry++;
                funcCount++;
            }

            LOG_DBG("Import: " + std::string(dllName));
            dllCount++;
            importDesc++;
        }

        LOG_OK(std::to_string(dllCount) + " DLL, " + std::to_string(funcCount) + " fonksiyon çözüldü");
        return true;
    }

    // ── Load a DLL in the remote process ───────────────────────────
    bool LoadRemoteDll(const char* dllName) {
        size_t nameLen = strlen(dllName) + 1;

        LPVOID remoteMem = VirtualAllocEx(hProc_, nullptr, nameLen,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteMem) return false;

        WriteProcessMemory(hProc_, remoteMem, dllName, nameLen, nullptr);

        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        FARPROC pLoadLib  = GetProcAddress(hKernel32, "LoadLibraryA");

        HANDLE hThread = CreateRemoteThread(hProc_, nullptr, 0,
            (LPTHREAD_START_ROUTINE)pLoadLib, remoteMem, 0, nullptr);

        if (!hThread) {
            VirtualFreeEx(hProc_, remoteMem, 0, MEM_RELEASE);
            return false;
        }

        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
        VirtualFreeEx(hProc_, remoteMem, 0, MEM_RELEASE);

        return true;
    }

    // ── Write mapped image to target ───────────────────────────────
    bool WriteToTarget() {
        if (!WriteProcessMemory(hProc_, reinterpret_cast<LPVOID>(remoteBase_),
                                mappedImage_.data(), mappedImage_.size(), nullptr)) {
            LOG_ERR("WriteProcessMemory başarısız");
            return false;
        }
        LOG_OK("Mapped image hedef process'e yazıldı");
        return true;
    }

    // ── Execute DllMain via shellcode ──────────────────────────────
    bool ExecuteEntry() {
        DWORD entryRVA = ntHeaders_->OptionalHeader.AddressOfEntryPoint;
        if (entryRVA == 0) {
            LOG_WARN("EntryPoint RVA = 0 — DllMain yok, atlanıyor");
            return true;
        }

        uintptr_t entryAddr = remoteBase_ + entryRVA;
        LOG_INFO("EntryPoint: 0x" + ToHex(entryAddr));

        /*
         * x64 Shellcode — DllMain çağırıcı
         *
         * Yapı:
         *   LoaderData+0x00 = imageBase  (HINSTANCE)
         *   LoaderData+0x08 = entryPoint (DllMain adresi)
         *
         * Eşdeğer C kodu:
         *   typedef BOOL(WINAPI* DllMain_t)(HINSTANCE, DWORD, LPVOID);
         *   DllMain_t fn = (DllMain_t)(data->entryPoint);
         *   fn(data->imageBase, DLL_PROCESS_ATTACH, NULL);
         *   return 0;
         *
         * Assembly (x64, Microsoft fastcall):
         *   mov rax, [rcx+0x08]    ; rax = entryPoint
         *   push rcx               ; save data ptr
         *   mov rcx, [rcx+0x00]    ; rcx = imageBase (param 1: hinstDLL)
         *   mov edx, 1             ; edx = DLL_PROCESS_ATTACH (param 2)
         *   xor r8, r8             ; r8  = NULL (param 3: lpvReserved)
         *   sub rsp, 0x28          ; shadow space (32 bytes) + alignment
         *   call rax               ; call DllMain
         *   add rsp, 0x28          ; restore stack
         *   pop rcx                ; restore data ptr (not needed but clean)
         *   xor eax, eax           ; return 0
         *   ret
         */
        uint8_t shellcode[] = {
            0x48, 0x8B, 0x41, 0x08,             // mov rax, [rcx+0x08]
            0x51,                               // push rcx
            0x48, 0x8B, 0x09,                   // mov rcx, [rcx+0x00]
            0xBA, 0x01, 0x00, 0x00, 0x00,       // mov edx, 1
            0x4D, 0x31, 0xC0,                   // xor r8, r8
            0x48, 0x83, 0xEC, 0x28,             // sub rsp, 0x28
            0xFF, 0xD0,                         // call rax
            0x48, 0x83, 0xC4, 0x28,             // add rsp, 0x28
            0x59,                               // pop rcx
            0x31, 0xC0,                         // xor eax, eax
            0xC3                                // ret
        };

        // LoaderData structure
        struct LoaderData {
            uint64_t imageBase;
            uint64_t entryPoint;
        };

        LoaderData loaderData;
        loaderData.imageBase  = remoteBase_;
        loaderData.entryPoint = entryAddr;

        // Allocate space for shellcode + data in target
        size_t totalSize = sizeof(shellcode) + sizeof(LoaderData);
        LPVOID remoteShellcode = VirtualAllocEx(hProc_, nullptr, totalSize,
                                                 MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_EXECUTE_READWRITE);
        if (!remoteShellcode) {
            LOG_ERR("Shellcode için bellek tahsis edilemedi");
            return false;
        }

        // Layout: [shellcode][LoaderData]
        LPVOID remoteData = reinterpret_cast<uint8_t*>(remoteShellcode) + sizeof(shellcode);

        // Write shellcode
        WriteProcessMemory(hProc_, remoteShellcode, shellcode, sizeof(shellcode), nullptr);

        // Write loader data
        WriteProcessMemory(hProc_, remoteData, &loaderData, sizeof(loaderData), nullptr);

        // Create remote thread: entry = shellcode, param = remoteData
        HANDLE hThread = CreateRemoteThread(hProc_, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteShellcode),
            remoteData, 0, nullptr);

        if (!hThread) {
            LOG_ERR("CreateRemoteThread başarısız (err: " + std::to_string(GetLastError()) + ")");
            VirtualFreeEx(hProc_, remoteShellcode, 0, MEM_RELEASE);
            return false;
        }

        LOG_INFO("DllMain çağrılıyor, bekleniyor...");
        WaitForSingleObject(hThread, 15000);

        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);

        CloseHandle(hThread);

        // Don't free shellcode memory yet — DllMain may have spawned threads
        // VirtualFreeEx(hProc_, remoteShellcode, 0, MEM_RELEASE);

        LOG_OK("DllMain döndü (exit code: " + std::to_string(exitCode) + ")");
        return true;
    }

    // ── Erase PE headers for stealth ───────────────────────────────
    void EraseHeaders() {
        DWORD headerSize = ntHeaders_->OptionalHeader.SizeOfHeaders;
        std::vector<uint8_t> zeros(headerSize, 0);

        if (WriteProcessMemory(hProc_, reinterpret_cast<LPVOID>(remoteBase_),
                                zeros.data(), headerSize, nullptr)) {
            LOG_OK("PE header'ları silindi (stealth)");
        } else {
            LOG_WARN("PE header'ları silinemedi — ama dump yine çalışır");
        }
    }

    // ── Cleanup on failure ─────────────────────────────────────────
    void Cleanup() {
        if (remoteBase_ && hProc_) {
            VirtualFreeEx(hProc_, reinterpret_cast<LPVOID>(remoteBase_), 0, MEM_RELEASE);
            remoteBase_ = 0;
        }
        if (hProc_) {
            CloseHandle(hProc_);
            hProc_ = nullptr;
        }
    }

    // ── Hex formatter ──────────────────────────────────────────────
    static std::string ToHex(uintptr_t val) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(val));
        return buf;
    }
};

// ═══════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    SetConsoleTitle("Albion Online - Manual Map Injector");

    SetColor(CYAN);
    std::cout << R"(
    +====================================================+
    |     ALBION ONLINE - MANUAL MAP INJECTOR             |
    |     EAC Bypass | PE Header Erase | Stealth          |
    +====================================================+
    )" << std::endl;
    SetColor(WHITE);

    // Elevation check
    if (!IsElevated()) {
        LOG_WARN("Yonetici olarak calistirilmadi — enjeksiyon basarisiz olabilir.");
        LOG_WARN("Sag tik -> Yonetici olarak calistir\n");
    }

    // SeDebugPrivilege — process bellegine tam erisim icin gerekli
    if (!EnableDebugPrivilege()) {
        LOG_WARN("SeDebugPrivilege aktif edilemedi — devam ediliyor ama basarisiz olabilir...");
    }

    // DLL path
    std::string dllPath;
    if (argc > 1) {
        dllPath = argv[1];
    } else {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        fs::path exeDir = fs::path(exePath).parent_path();
        dllPath = (exeDir / "dumper.dll").string();
    }

    if (!fs::exists(dllPath)) {
        LOG_ERR("DLL bulunamadi: " + dllPath);
        LOG_ERR("dumper.dll'i mapper.exe'nin yanina koy veya yol arguman olarak ver.");
        std::cout << "\nCikmak icin Enter'a bas...";
        std::cin.get();
        return 1;
    }

    dllPath = fs::absolute(dllPath).string();
    LOG_OK("DLL: " + dllPath);

    // Find target
    LOG_INFO("Albion Online aranıyor...");
    ProcessInfo target = FindTarget();

    if (target.pid == 0) {
        LOG_ERR("Albion Online bulunamadi!");
        LOG_INFO("Once oyunu baslat, tam yuklenene kadar bekle, sonra bunu calistir.");
        std::cout << "\nCikmak icin Enter'a bas...";
        std::cin.get();
        return 1;
    }

    LOG_OK("Bulundu: " + target.name + " (PID: " + std::to_string(target.pid) + ")");

    // Manual map
    LOG_INFO("Manual mapping baslatiliyor...\n");
    ManualMapper mapper;

    if (mapper.Map(target.pid, dllPath)) {
        std::cout << std::endl;
        LOG_OK("============================================");
        LOG_OK("  DLL basariyla map'lendi!");
        LOG_OK("  Masaustunde albion_dump.txt'yi kontrol et");
        LOG_OK("============================================");
        LOG_INFO("Oyun penceresinde dump bitince MessageBox cikacak.");
    } else {
        std::cout << std::endl;
        LOG_ERR("Manual mapping basarisiz.");
        LOG_WARN("Kontrol et:");
        LOG_WARN("  1. Albion Online tam yuklu mu (giris ekranini gectin mi)");
        LOG_WARN("  2. Yonetici olarak calistiriyor musun");
        LOG_WARN("  3. DLL x64 olarak derlenmis mi");
    }

    std::cout << "\nCikmak icin Enter'a bas...";
    std::cin.get();
    return 0;
}
