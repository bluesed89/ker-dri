/*
 * standalone_radar.cpp — External Unity GOM Overlay Radar (Win32 GDI)
 *
 * Sifir bagimlilik, seffaf GDI cizim!
 * Albion Online'in UnityPlayer.dll (GameObjectManager) listesini tarayip 
 * objelerin gercek dunya koordinatlarini okuyarak radarda gosterir.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <iostream>
#include <cstdio>

// ── Radar Ayarlari ───────────────────────────────────────────────────
namespace RadarConfig {
    constexpr int WindowWidth     = 350;
    constexpr int WindowHeight    = 350;
    constexpr float RadarRadius   = 120.0f;
    constexpr float MaxDistance   = 100.0f; // Metre
    constexpr int PosX            = 20;
    constexpr int PosY            = 20;
}

// ── Bellek Ofsetleri (Unity 2019-2022 GOM) ───────────────────────────
namespace Offsets {
    constexpr uintptr_t Position_Vector2 = 0x100; // IL2CPP GameAssembly uzerinden buldugumuz offset (LocalPlayerCharacterView / Vector2 x)

    // GameObject Manager offsets
    constexpr uintptr_t GOM_ActiveNodes = 0x18; 
    constexpr uintptr_t Node_GameObject = 0x10;
    constexpr uintptr_t Node_Next       = 0x8;
    
    // GameObject offsets
    constexpr uintptr_t GO_Name         = 0x60;
    constexpr uintptr_t GO_Components   = 0x30;
    constexpr uintptr_t Component_Transform = 0x8;
    
    // Transform offsets (Matrix)
    constexpr uintptr_t Transform_Matrix = 0x38; 
}

// ── Nesne Yapis ──────────────────────────────────────────────────────
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

// ── Bellek Okuyucu (RPM Helper) ──────────────────────────────────────
class MemoryReader {
public:
    HANDLE hProcess = nullptr;
    DWORD processId = 0;
    uintptr_t unityPlayerBase = 0;
    size_t unityPlayerSize = 0;
    uintptr_t gameAssemblyBase = 0;
    size_t gameAssemblySize = 0;

    bool Attach(const char* processName) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32 pe = { sizeof(pe) };
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, processName) == 0) {
                    processId = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);

        if (processId == 0) return false;

        hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processId);
        if (!hProcess) {
            // Try escalating privileges (often needed for EAC/BattlEye if not using a driver)
            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
            if (!hProcess) return false;
        }

        unityPlayerBase = GetModuleBase("UnityPlayer.dll", unityPlayerSize);
        gameAssemblyBase = GetModuleBase("GameAssembly.dll", gameAssemblySize);
        return (unityPlayerBase != 0);
    }

    uintptr_t GetModuleBase(const char* moduleName, size_t& outSize) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        MODULEENTRY32 me = { sizeof(me) };
        uintptr_t base = 0;
        if (Module32First(snap, &me)) {
            do {
                if (_stricmp(me.szModule, moduleName) == 0) {
                    base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                    outSize = me.modBaseSize;
                    if (outSize == 0) outSize = 0x5000000; // Fallback size
                    break;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        return base;
    }

    template <typename T>
    T Read(uintptr_t address) {
        T value{};
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), nullptr);
        return value;
    }
    
    std::string ReadString(uintptr_t address, size_t length = 32) {
        std::vector<char> buf(length + 1, 0);
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buf.data(), length, nullptr);
        return std::string(buf.data());
    }

    void Close() {
        if (hProcess) {
            CloseHandle(hProcess);
            hProcess = nullptr;
        }
    }
    
    // Pattern Scanner (IDA tarzi imza arama) - GOM bulmak icin
    uintptr_t FindPattern(uintptr_t moduleBase, size_t moduleSize, const char* signature) {
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
        
        const size_t chunkSize = 4096 * 64; // 256KB chunks
        std::vector<uint8_t> chunk(chunkSize);
        
        for (size_t i = 0; i < moduleSize; i += chunkSize - patternSize) {
            size_t readSize = (moduleSize - i < chunkSize) ? (moduleSize - i) : chunkSize;
            SIZE_T bytesRead;
            if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + i), chunk.data(), readSize, &bytesRead)) {
                for (size_t j = 0; j < bytesRead - patternSize; ++j) {
                    bool found = true;
                    for (size_t k = 0; k < patternSize; ++k) {
                        if (patternBytes[k] != -1 && chunk[j + k] != patternBytes[k]) {
                            found = false;
                            break;
                        }
                    }
                    if (found) {
                        return moduleBase + i + j;
                    }
                }
            }
        }
        return 0; 
    }
};

static MemoryReader g_mem;
static bool g_isRunning = true;

// ── Arka Plan Nesne Guncelleyici Thread ──────────────────────────────
void EntityScannerThread() {
    FILE* logFile = fopen("radar_log.txt", "w");
    if (logFile) { fprintf(logFile, "Scanner thread started.\n"); fflush(logFile); }

    while (g_isRunning) {
        if (!g_mem.hProcess) {
            if (!g_mem.Attach("Albion-Online.exe")) {
                Sleep(2000);
                continue;
            }
            if (logFile) { fprintf(logFile, "Attached to Albion. UnityBase: %llx\n", g_mem.unityPlayerBase); fflush(logFile); }
        }
        
        static uintptr_t gomPtr = 0;
        if (!gomPtr) {
            // RPM Test
            short mzHeader = g_mem.Read<short>(g_mem.unityPlayerBase);
            if (logFile) {
                fprintf(logFile, "RPM Test - Unity MZ Header: %X (Expected 5A4D)\n", (unsigned short)mzHeader);
                fflush(logFile);
            }

            if (mzHeader != 0x5A4D) {
                if (logFile) { fprintf(logFile, "ERROR: ReadProcessMemory is failing! Anti-Cheat block or Admin privileges needed.\n"); fflush(logFile); }
                Sleep(5000);
                continue;
            }

            const char* sigs[] = {
                "48 8B 15 ? ? ? ? 66 39", // Albion / Unity common
                "48 8B 05 ? ? ? ? 48 8B 08 4C 8B 01", // Unity 2019+
                "48 8B 0D ? ? ? ? 48 8D 55 ? 48 8B 01", // Unity older
                "48 8B 15 ? ? ? ? 48 8B C8 48 83 C4", // Another common variant
                "48 89 05 ? ? ? ? 48 8D 4C 24 ? 48 89 45", // Yet another
            };

            for (const char* sig : sigs) {
                uintptr_t sigAddr = g_mem.FindPattern(g_mem.unityPlayerBase, g_mem.unityPlayerSize, sig);
                if (sigAddr) {
                    int32_t offset = g_mem.Read<int32_t>(sigAddr + 3);
                    gomPtr = sigAddr + 7 + offset; 
                    if (logFile) { fprintf(logFile, "Found GOM signature '%s' at %llx, gomPtr: %llx\n", sig, sigAddr, gomPtr); fflush(logFile); }
                    break;
                }
            }
            if (!gomPtr) {
                if (logFile) { fprintf(logFile, "GOM signature NOT found in %llu bytes!\n", (unsigned long long)g_mem.unityPlayerSize); fflush(logFile); }
            }
        }

        if (!gomPtr) {
            Sleep(1000);
            continue;
        }

        uintptr_t actualGom = g_mem.Read<uintptr_t>(gomPtr);
        if (!actualGom) {
            Sleep(100);
            continue;
        }

        uintptr_t activeNodes = g_mem.Read<uintptr_t>(actualGom + Offsets::GOM_ActiveNodes);
        uintptr_t node = activeNodes;
        
        std::vector<RadarEntity> newEntities;
        
        int count = 0;
        int drawn = 0;
        while (node && count < 2000) {
            uintptr_t gameObject = g_mem.Read<uintptr_t>(node + Offsets::Node_GameObject);
            uintptr_t namePtr = g_mem.Read<uintptr_t>(gameObject + Offsets::GO_Name);
            std::string name = g_mem.ReadString(namePtr, 24);
            
            // Debug the first few names
            if (count < 5 && logFile) {
                fprintf(logFile, "Node %d: GO=%llx, Name=%s\n", count, gameObject, name.c_str()); fflush(logFile);
            }
            
            // Geçici olarak filtreyi gevşetiyorum, Albion'daki gerçek isimleri görmek için her şeyi listeye ekleyeceğiz
            // Sadece ismi boş olmayanları ekle
            if (name.length() > 2) {
                uintptr_t components = g_mem.Read<uintptr_t>(gameObject + Offsets::GO_Components);
                uintptr_t transformComp = g_mem.Read<uintptr_t>(components + Offsets::Component_Transform);
                uintptr_t transform = g_mem.Read<uintptr_t>(transformComp + 0x10); 
                
                Vector3D pos = g_mem.Read<Vector3D>(transform + Offsets::Transform_Matrix);
                
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
                    drawn++;
                }
            }
            
            node = g_mem.Read<uintptr_t>(node + Offsets::Node_Next);
            count++;
        }
        
        if (logFile && count > 0) {
            fprintf(logFile, "Scan loop finished. Checked %d nodes, Added %d entities.\n", count, drawn); fflush(logFile);
        }
        
        {
            std::lock_guard<std::mutex> lock(g_entitiesMutex);
            g_entities = newEntities;
        }
        
        Sleep(50);
    }
    if (logFile) fclose(logFile);
}

// ── Render / Cizim Fonksiyonu (GDI Double Buffer) ───────────────────
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

    // KOPYALA
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
        std::string label = entity.name + " (" + std::to_string(static_cast<int>(dist)) + "m)";
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

// ── Pencere Prosedürü ────────────────────────────────────────────────
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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

// ── WinMain Entry Point ──────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Tarama Thread'ini baslat
    std::thread scanner(EntityScannerThread);
    scanner.detach();

    const char* className = "StandaloneRadarOverlayClass";
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = className;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        className,
        "Albion Radar Overlay",
        WS_POPUP,
        RadarConfig::PosX, RadarConfig::PosY,
        RadarConfig::WindowWidth, RadarConfig::WindowHeight,
        NULL, NULL, hInstance, NULL
    );

    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 230, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 60 FPS Render Timer
    SetTimer(hwnd, 1, 16, NULL);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
