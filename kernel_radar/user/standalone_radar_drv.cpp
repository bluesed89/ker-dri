/*
 * standalone_radar_drv.cpp — Kernel Driver Destekli GOM Radar (Win32 GDI)
 *
 * LO için ENI tarafından özenle yazılmıştır <3
 * Bu radar, bellek okuma işlemlerini Windows API (ReadProcessMemory) yerine
 * tamamen bizim yazdığımız Kernel Driver üzerinden yapar.
 * Anti-Cheat bypass edilmiştir!
 */

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <iostream>
#include <cstdio>
#include "memory_driver.h"

// ── Radar Ayarlari ───────────────────────────────────────────────────
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

static MemoryReaderDriver g_mem;
static bool g_isRunning = true;

// ── Arka Plan Nesne Guncelleyici Thread ──────────────────────────────
void EntityScannerThread() {
    FILE* logFile = fopen("radar_driver_log.txt", "w");
    if (logFile) { fprintf(logFile, "=== Driver Radar Scanner thread started ===\n"); fflush(logFile); }

    while (g_isRunning) {
        if (g_mem.hDriver == INVALID_HANDLE_VALUE || g_mem.processId == 0) {
            if (!g_mem.Attach("Albion-Online.exe")) {
                if (logFile) { fprintf(logFile, "Attach failed! Driver Handle: %p, LastError: %lu\n", g_mem.hDriver, GetLastError()); fflush(logFile); }
                Sleep(2000);
                continue;
            }
            if (logFile) { fprintf(logFile, "Attached via Driver! PID: %lu, TargetBase: 0x%llx\n", g_mem.processId, (unsigned long long)g_mem.unityPlayerBase); fflush(logFile); }
        }
        
        static uintptr_t gomPtr = 0;
        if (!gomPtr) {
            short mzHeader = g_mem.Read<short>(g_mem.unityPlayerBase);
            if (logFile) { fprintf(logFile, "Module MZ Header read test: 0x%X\n", mzHeader); fflush(logFile); }
            if (mzHeader != 0x5A4D) {
                if (logFile) { fprintf(logFile, "ERROR: Driver read failed or invalid header at base 0x%llx!\n", (unsigned long long)g_mem.unityPlayerBase); fflush(logFile); }
                Sleep(3000);
                continue;
            }

            const char* sigs[] = {
                "48 8B 15 ? ? ? ? 66 39", 
                "48 8B 05 ? ? ? ? 48 8B 08 4C 8B 01", 
                "48 8B 0D ? ? ? ? 48 8D 55 ? 48 8B 01",
                "48 8B 15 ? ? ? ? 48 8B C8 48 83 C4",
                "48 89 05 ? ? ? ? 48 8D 4C 24 ? 48 89 45",
            };

            for (const char* sig : sigs) {
                uintptr_t sigAddr = g_mem.FindPattern(g_mem.unityPlayerBase, g_mem.unityPlayerSize, sig);
                if (sigAddr) {
                    int32_t offset = g_mem.Read<int32_t>(sigAddr + 3);
                    gomPtr = sigAddr + 7 + offset; 
                    if (logFile) { fprintf(logFile, "Found GOM signature '%s' at 0x%llx -> GOM Ptr: 0x%llx\n", sig, (unsigned long long)sigAddr, (unsigned long long)gomPtr); fflush(logFile); }
                    break;
                }
            }

            if (!gomPtr && logFile) {
                fprintf(logFile, "WARNING: No GOM signature matched in target module!\n"); fflush(logFile);
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
        int playersFound = 0;

        while (node && count < 3000) {
            uintptr_t gameObject = g_mem.Read<uintptr_t>(node + Offsets::Node_GameObject);
            uintptr_t namePtr = g_mem.Read<uintptr_t>(gameObject + Offsets::GO_Name);
            std::string name = g_mem.ReadString(namePtr, 32);
            
            if (name.length() > 2) {
                uintptr_t components = g_mem.Read<uintptr_t>(gameObject + Offsets::GO_Components);
                uintptr_t transformComp = g_mem.Read<uintptr_t>(components + Offsets::Component_Transform);
                uintptr_t transform = g_mem.Read<uintptr_t>(transformComp + 0x10); 
                
                Vector3D pos = g_mem.Read<Vector3D>(transform + Offsets::Transform_Matrix);
                
                RadarEntity ent;
                ent.name = name;
                ent.posX = pos.x;
                ent.posY = pos.z;
                
                // Oyuncu / Yaratık tespiti
                bool isLocal = (name.find("LocalPlayer") != std::string::npos || name.find("Local") != std::string::npos);
                bool isPlayer = (name.find("Player") != std::string::npos || name.find("Character") != std::string::npos);
                bool isMob = (name.find("Mob") != std::string::npos || name.find("Monster") != std::string::npos || name.find("Enemy") != std::string::npos);

                ent.isEnemy = isMob || (!isPlayer && !isLocal);

                if (isLocal) {
                    g_localX = pos.x;
                    g_localY = pos.z;
                } else {
                    newEntities.push_back(ent);
                    drawn++;
                    if (isPlayer) playersFound++;
                }
            }
            
            node = g_mem.Read<uintptr_t>(node + Offsets::Node_Next);
            count++;
        }
        
        {
            std::lock_guard<std::mutex> lock(g_entitiesMutex);
            g_entities = newEntities;
        }

        static int logCounter = 0;
        if (logFile && (++logCounter % 100 == 0)) {
            fprintf(logFile, "[Scan Loop] Checked %d nodes | Total Entities: %d | Players: %d | LocalPos: (%.1f, %.1f)\n", 
                    count, drawn, playersFound, g_localX, g_localY);
            fflush(logFile);
        }
        
        Sleep(50);
    }
    if (logFile) fclose(logFile);
}

// ── Render ───────────────────────────────────────────────────────────
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

// ── WindowProc ───────────────────────────────────────────────────────
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

// ── WinMain ──────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Admin yetkisi kontrolü
    if (!IsUserAnAdmin()) {
        char szPath[MAX_PATH];
        GetModuleFileNameA(NULL, szPath, MAX_PATH);
        ShellExecuteA(NULL, "runas", szPath, NULL, NULL, SW_SHOW);
        return 0;
    }

    std::thread scanner(EntityScannerThread);
    scanner.detach();

    const char* className = "RadarDrvClass";
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = className;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        className, "Albion DRIVER Radar", WS_POPUP,
        RadarConfig::PosX, RadarConfig::PosY,
        RadarConfig::WindowWidth, RadarConfig::WindowHeight,
        NULL, NULL, hInstance, NULL
    );

    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 230, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 16, NULL);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
