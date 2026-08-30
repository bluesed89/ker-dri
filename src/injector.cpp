/*
 * injector.cpp — Simple DLL Injector for Albion Online
 *
 * Finds the Albion-Online.exe process, injects dumper.dll via
 * LoadLibraryA + CreateRemoteThread. Run as administrator.
 *
 * Usage:  injector.exe [path_to_dumper.dll]
 *   - If no path given, looks for dumper.dll next to injector.exe
 *
 * Build as EXE (see CMakeLists.txt).
 */

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

// ── Console colors ─────────────────────────────────────────────────
enum Color { RED = 12, GREEN = 10, YELLOW = 14, CYAN = 11, WHITE = 15, GRAY = 8 };

static void SetColor(Color c) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), c);
}

static void Print(Color c, const char* prefix, const char* msg) {
    SetColor(c);
    std::cout << prefix;
    SetColor(WHITE);
    std::cout << " " << msg << std::endl;
}

#define LOG_OK(msg)    Print(GREEN,  "[+]", msg)
#define LOG_INFO(msg)  Print(CYAN,   "[*]", msg)
#define LOG_WARN(msg)  Print(YELLOW, "[!]", msg)
#define LOG_ERR(msg)   Print(RED,    "[-]", msg)

// ── Process enumeration ────────────────────────────────────────────

// Target process names (Albion uses different exe names across versions)
static const char* TARGET_NAMES[] = {
    "Albion-Online.exe",
    "AlbionOnline.exe",
    "Albion Online.exe",
};

struct ProcessInfo {
    DWORD pid;
    std::string name;
};

static ProcessInfo FindTargetProcess() {
    ProcessInfo result = { 0, "" };

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe)) {
        do {
            for (auto targetName : TARGET_NAMES) {
                if (_stricmp(pe.szExeFile, targetName) == 0) {
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
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size)) {
            elevated = elev.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// ── DLL Injection ──────────────────────────────────────────────────
static bool InjectDll(DWORD pid, const std::string& dllPath) {
    // Open target process
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!hProc) {
        LOG_ERR(("OpenProcess failed (err: " + std::to_string(GetLastError()) + ")").c_str());
        return false;
    }

    // Allocate memory in target for DLL path string
    size_t pathLen = dllPath.size() + 1;
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        LOG_ERR("VirtualAllocEx failed");
        CloseHandle(hProc);
        return false;
    }

    // Write DLL path
    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathLen, nullptr)) {
        LOG_ERR("WriteProcessMemory failed");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Resolve LoadLibraryA in kernel32
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLib = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!pLoadLib) {
        LOG_ERR("Failed to resolve LoadLibraryA");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Create remote thread
    HANDLE hThread = CreateRemoteThread(
        hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)pLoadLib,
        remoteMem, 0, nullptr);

    if (!hThread) {
        LOG_ERR(("CreateRemoteThread failed (err: " + std::to_string(GetLastError()) + ")").c_str());
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    LOG_INFO("Remote thread created, waiting for DLL to load...");

    // Wait for the remote thread to finish (LoadLibrary call)
    WaitForSingleObject(hThread, 10000);

    // Check if LoadLibrary returned non-null (success)
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    // Clean up
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (exitCode == 0) {
        LOG_ERR("LoadLibraryA returned NULL in target process (DLL failed to load)");
        return false;
    }

    return true;
}

// ── Main ───────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    SetConsoleTitle("Albion Online - Offset Dumper Injector");

    SetColor(CYAN);
    std::cout << R"(
    ╔══════════════════════════════════════════════════╗
    ║     ALBION ONLINE — OFFSET DUMPER INJECTOR       ║
    ╚══════════════════════════════════════════════════╝
    )" << std::endl;
    SetColor(WHITE);

    // Check elevation
    if (!IsElevated()) {
        LOG_WARN("Not running as Administrator — injection may fail.");
        LOG_WARN("Right-click -> Run as administrator if it doesn't work.\n");
    }

    // Resolve DLL path
    std::string dllPath;
    if (argc > 1) {
        dllPath = argv[1];
    } else {
        // Default: dumper.dll next to injector.exe
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        fs::path exeDir = fs::path(exePath).parent_path();
        dllPath = (exeDir / "dumper.dll").string();
    }

    // Verify DLL exists
    if (!fs::exists(dllPath)) {
        LOG_ERR(("DLL not found: " + dllPath).c_str());
        LOG_ERR("Place dumper.dll next to injector.exe, or pass path as argument.");
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    // Get absolute path (required for remote LoadLibrary)
    dllPath = fs::absolute(dllPath).string();
    LOG_OK(("DLL path: " + dllPath).c_str());

    // Find target process
    LOG_INFO("Searching for Albion Online process...");
    ProcessInfo target = FindTargetProcess();

    if (target.pid == 0) {
        LOG_ERR("Albion Online is not running!");
        LOG_INFO("Start the game first, wait for it to fully load, then run this.");
        std::cout << "\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    LOG_OK(("Found: " + target.name + " (PID: " + std::to_string(target.pid) + ")").c_str());

    // Inject
    LOG_INFO("Injecting DLL...");
    if (InjectDll(target.pid, dllPath)) {
        LOG_OK("DLL injected successfully!");
        LOG_OK("Check your Desktop for albion_dump.txt");
        LOG_INFO("A MessageBox will appear in the game window when the dump is complete.");
    } else {
        LOG_ERR("Injection failed.");
        LOG_WARN("Make sure:");
        LOG_WARN("  1. Albion Online is fully loaded (past login screen)");
        LOG_WARN("  2. You're running as Administrator");
        LOG_WARN("  3. No anti-cheat is blocking injection");
    }

    std::cout << "\nPress Enter to exit...";
    std::cin.get();
    return 0;
}
