/*
 * radar.cpp — Albion Online External GDI Radar Overlay
 * Kernel Driver (MmCopyVirtualMemory) tabanlı IL2CPP Entity Radar
 * 
 * Pointer zinciri (txt dosyasından):
 *   Il2CppClass* klass = read(gameAssemblyBase + off::client)  // drr_TypeInfo ptr
 *   staticFields       = read(klass + 0xB8)                    // Il2CppClass->static_fields
 *   singletonInstance   = read(staticFields + 0x0)              // static drr a (offset 0x0)
 *   localPlayer         = read(singletonInstance + 0x18)        // LocalPlayerCharacterView c
 *   entityHashtable     = read(singletonInstance + 0x70)        // Hashtable n (entity container)
 */

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "../kernel_radar/shared/ioctls.h"

// ── Offsetler & Class Hashleri (IL2CPP) ─────────────────────────────
namespace Config {
    constexpr float RadarSize       = 300.0f;
    constexpr float DefaultZoom     = 2.5f;

    namespace Offsets {
        // drr_TypeInfo adresi (script.json'dan: 80323912 decimal = 0x4C9A548)
        // Bu dump zamanındaki adres — oyun güncellenmiş olabilir, auto-scanner yedek olarak çalışır
        constexpr uintptr_t DrrTypeInfo_Hint = 0x4C9A548;

        // IL2CPP class yapısı
        constexpr uintptr_t Il2CppClass_StaticFields = 0xB8;  // Il2CppClass->static_fields pointer

        // drr singleton instance field offsetleri (dump.cs'ten)
        constexpr uintptr_t Singleton_Instance = 0x0;   // static drr a; singleton
        constexpr uintptr_t Singleton_LocalPlayer = 0x18; // LocalPlayerCharacterView c
        constexpr uintptr_t Singleton_EntityTable = 0x70; // Hashtable n (entity container)

        // EntityView offsetleri
        constexpr uintptr_t EntityView_Entity        = 0x20;    // SimulationObjectView -> cft a;
        constexpr uintptr_t EntityView_Transform     = 0x28;    // SimulationObjectView -> Transform b;
        constexpr uintptr_t EntityView_Speed         = 0x15C;   // movement speed

        // Entity (FightingObject) offsetleri
        constexpr uintptr_t Entity_Rotation    = 0x38;
        constexpr uintptr_t Entity_Position2D  = 0x3C;  // cfw -> Vector2 h;
        constexpr uintptr_t Entity_HealthBar   = 0x80;  // healthBar* -> +0x10 current, +0x18 max
        constexpr uintptr_t Entity_PlayerName  = 0x348; // System.String (UTF-16)

        // IL2CPP vtable class hash (entity tipi belirleme)
        constexpr uintptr_t ClassHash_Step1 = 0x0;   // EntityView -> vtable
        constexpr uintptr_t ClassHash_Step2 = 0x10;  // vtable -> class info
        constexpr uintptr_t ClassHash_Step3 = 0x0;   // class info -> name hash
    }

    namespace ClassHashes {
        // Class isimlerinin ilk 8 byte'ı (little-endian) — txt dosyasından
        constexpr uint64_t RemotePlayerView      = 0x6C5065746F6D6552ULL; // "RemotePl"
        constexpr uint64_t LocalPlayerView       = 0x6C506C61636F4CULL;   // "LocalPl"
        constexpr uint64_t HarvestableObjectView = 0x6176726148ULL;       // "Harva..."
        constexpr uint64_t MobView               = 0x77656956626F4DULL;   // "MobView"
    }

    // Hashtable bucket yapısı (System.Collections.Hashtable)
    namespace Hashtable {
        constexpr uintptr_t Buckets = 0x10;     // bucket[] _buckets
        constexpr uintptr_t Count   = 0x18;     // int _count
        // bucket yapısı: { object key, object val, int hash_col } = 24 bytes (3 * 8 aligned)
        constexpr size_t BucketStride = 0x18;   // 24 bytes per bucket
        constexpr uintptr_t Bucket_Key = 0x0;
        constexpr uintptr_t Bucket_Val = 0x8;
        constexpr uintptr_t Bucket_HashCol = 0x10;
    }
}

namespace RadarSettings {
    constexpr float DefaultRadius  = 200.0f; // Ekranda kapladığı makul boyut
    constexpr float MaxDistance    = 150.0f; // Oyun içi makul max tarama mesafesi (sunucunun yolladığı maksimum civarı)
    constexpr int RadarPosX        = 40;
    constexpr int RadarPosY        = 40;
}

struct Vector2D { float x, y; };
struct Vector3D { float x, y, z; };

struct RadarEntity {
    std::string name;
    float posX;
    float posY;
    float distance;
    int entityType; // 0=unknown, 1=remote player, 2=harvestable, 3=mob, 4=self
    bool isLocalPlayer;
};

// ── Kernel Bellek Okuyucu ──────────────────────────────────────────
class KernelMemoryReader {
private:
    HANDLE hDriver = INVALID_HANDLE_VALUE;
public:
    DWORD processId = 0;
    uintptr_t gameAssemblyBase = 0;
    uintptr_t unityPlayerBase = 0;

    bool Attach(const char* processName) {
        hDriver = CreateFileA(USER_DEVICE_LINK, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDriver == INVALID_HANDLE_VALUE) return false;

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

        gameAssemblyBase = GetModuleBase(L"GameAssembly.dll");
        unityPlayerBase = GetModuleBase(L"UnityPlayer.dll");
        return gameAssemblyBase != 0;
    }

    uintptr_t GetModuleBase(const wchar_t* moduleName) {
        MODULE_BASE_REQUEST req = {0};
        req.ProcessId = processId;
        wcscpy_s(req.ModuleName, moduleName);
        DWORD bytes = 0;
        if (DeviceIoControl(hDriver, IOCTL_GET_MODULE_BASE, &req, sizeof(req), &req, sizeof(req), &bytes, nullptr)) {
            return req.BaseAddress;
        }
        return 0;
    }

    template <typename T>
    T Read(uintptr_t address) {
        T value{};
        if (address == 0) return value;
        READ_MEMORY_REQUEST req = {0};
        req.ProcessId = processId;
        req.Address = address;
        req.Size = sizeof(T);
        req.OutputBuffer = (ULONGLONG)&value;
        DWORD bytes = 0;
        DeviceIoControl(hDriver, IOCTL_READ_MEMORY, &req, sizeof(req), &req, sizeof(req), &bytes, nullptr);
        return value;
    }

    // Bellek yazma (zoom hack için)
    template <typename T>
    bool Write(uintptr_t address, T value) {
        if (address == 0) return false;
        WRITE_MEMORY_REQUEST req = {0};
        req.ProcessId = processId;
        req.Address = address;
        req.Size = sizeof(T);
        req.InputBuffer = (ULONGLONG)&value;
        DWORD bytes = 0;
        return DeviceIoControl(hDriver, IOCTL_WRITE_MEMORY, &req, sizeof(req), &req, sizeof(req), &bytes, nullptr);
    }

    std::string ReadString(uintptr_t address, size_t maxLen = 64) {
        if (address == 0) return "";
        std::vector<char> buf(maxLen + 1, 0);
        READ_MEMORY_REQUEST req = {0};
        req.ProcessId = processId;
        req.Address = address;
        req.Size = maxLen;
        req.OutputBuffer = (ULONGLONG)buf.data();
        DWORD bytes = 0;
        DeviceIoControl(hDriver, IOCTL_READ_MEMORY, &req, sizeof(req), &req, sizeof(req), &bytes, nullptr);
        return std::string(buf.data());
    }

    // UTF-16 string okuma (System.String)
    std::string ReadSystemString(uintptr_t stringPtr) {
        if (stringPtr == 0) return "";
        // System.String: +0x10 = length (int), +0x14 = chars (wchar_t[])
        int len = Read<int>(stringPtr + 0x10);
        if (len <= 0 || len > 128) return "";
        std::vector<wchar_t> wbuf(len + 1, 0);
        READ_MEMORY_REQUEST req = {0};
        req.ProcessId = processId;
        req.Address = stringPtr + 0x14;
        req.Size = len * 2;
        req.OutputBuffer = (ULONGLONG)wbuf.data();
        DWORD bytes = 0;
        DeviceIoControl(hDriver, IOCTL_READ_MEMORY, &req, sizeof(req), &req, sizeof(req), &bytes, nullptr);
        // wchar -> char conversion (basit ASCII)
        std::string result;
        for (int i = 0; i < len && wbuf[i]; i++) {
            result += (char)(wbuf[i] < 128 ? wbuf[i] : '?');
        }
        return result;
    }

    void Close() {
        if (hDriver != INVALID_HANDLE_VALUE) {
            CloseHandle(hDriver);
            hDriver = INVALID_HANDLE_VALUE;
        }
    }
};

KernelMemoryReader mem;

// ── Pointer geçerlilik kontrolü ──────────────────────────────────
static bool IsValidPtr(uintptr_t ptr) {
    return ptr > 0x10000000000ULL && ptr < 0x7FFFFFFFFFFFULL;
}

// ── Entity tipi belirleme (IL2CPP class hash) ────────────────────
static int GetEntityType(uintptr_t entityView) {
    if (entityView == 0) return 0;
    uintptr_t step1 = mem.Read<uintptr_t>(entityView + Config::Offsets::ClassHash_Step1);
    if (!IsValidPtr(step1)) return 0;
    uintptr_t step2 = mem.Read<uintptr_t>(step1 + Config::Offsets::ClassHash_Step2);
    if (!IsValidPtr(step2)) return 0;
    uint64_t klass = mem.Read<uint64_t>(step2 + Config::Offsets::ClassHash_Step3);

    if (klass == Config::ClassHashes::RemotePlayerView)      return 1;
    if (klass == Config::ClassHashes::HarvestableObjectView) return 2;
    if (klass == Config::ClassHashes::MobView)               return 3;
    if (klass == Config::ClassHashes::LocalPlayerView)        return 4;
    return 0;
}

// ── Overlay / GDI Tanımlamaları ──────────────────────────────────
static HWND g_hwnd = nullptr;

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ── Dönüştürücü Matematik ────────────────────────────────────────────
static POINT WorldToCircularRadarGDI(float localX, float localY, float targetX, float targetY, POINT center, float radius, float maxDist) {
    float dx = targetX - localX;
    float dy = targetY - localY;
    
    // İzometrik kamera düzeltmesi (Albion yaklaşık 45 derece eğiktir)
    // Kameranın bakış açısını radarın üst noktasına (Kuzey) eşitlemek için:
    // radyan cinsinden 45 derece döndürüyoruz (PI / 4 = 0.785398f)
    // Eğer sağ-sol ters ise bu açıyı +0.785398f veya -0.785398f olarak değiştirebiliriz.
    // Albion kamerası için genellikle -45 derece (veya -135) dönüş gerekir.
    float angle = std::atan2(dy, dx);
    angle -= 0.785398f; // -45 derece
    
    float dist = std::sqrt(dx * dx + dy * dy);
    float normDist = dist / maxDist;
    if (normDist > 1.0f) normDist = 1.0f;
    
    int radarX = center.x + static_cast<int>(normDist * radius * std::cos(angle));
    int radarY = center.y - static_cast<int>(normDist * radius * std::sin(angle)); // GDI'de Y aşağı doğru artar
    return { radarX, radarY };
}

// ── Main Loop ────────────────────────────────────────────────────────
int main() {
    // Log dosyası aç
    FILE* logFile = fopen("radar_debug.txt", "w");
    if (logFile) { fprintf(logFile, "=== Albion Radar Debug Log ===\n"); fflush(logFile); }

    std::cout << "[*] Albion Online bekleniyor..." << std::endl;
    while (!mem.Attach("Albion-Online.exe")) { Sleep(1000); }
    std::cout << "[+] Process Baglandi! PID: " << mem.processId << std::endl;
    std::cout << "[+] GameAssembly.dll Base: 0x" << std::hex << mem.gameAssemblyBase << std::dec << std::endl;
    std::cout << "[+] UnityPlayer.dll Base: 0x" << std::hex << mem.unityPlayerBase << std::dec << std::endl;

    if (logFile) { 
        fprintf(logFile, "PID: %lu\nGameAssembly: 0x%llX\nUnityPlayer: 0x%llX\n", 
                mem.processId, (unsigned long long)mem.gameAssemblyBase, (unsigned long long)mem.unityPlayerBase);
        fflush(logFile); 
    }

    // GDI Transparent Overlay Window
    WNDCLASSEX wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, (HBRUSH)CreateSolidBrush(RGB(0,0,0)), nullptr, "AlbionRadar", nullptr };
    RegisterClassEx(&wc);
    g_hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, "AlbionRadar Overlay", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr);
    
    // Alpha (Transparanlık) değeri eklendi:
    // RGB(0,0,0) (siyah) olan yerler LWA_COLORKEY ile tamamen transparan oluyor,
    // ancak LWA_ALPHA kullanarak pencereye genel bir saydamlık (örneğin 128 / 255 = %50) veriyoruz.
    // Hem COLORKEY (siyahları sil) hem ALPHA (saydamlaştır) için ikisini bitwise OR ile birleştirdik.
    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 160, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);

    std::cout << "[*] GDI Radar Overlay baslatildi!" << std::endl;

    bool zoomHackEnabled = false;
    MSG msg;
    
    // off::client bulma — iki aşamalı:
    // 1. Önce script.json'dan bilinen hint adresi dene
    // 2. Başarısızsa, auto-scanner ile tara
    static uintptr_t ClientTypeInfoAddr = 0;     // GameAssembly.dll + offset → Il2CppClass* pointer
    static uintptr_t SingletonInstance = 0;       // drr singleton instance

    while (true) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) goto cleanup;
        }

        // TOPMOST kontrolü
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        // ── AŞAMA 0: Client TypeInfo adresini bul ──────────────────────
        if (ClientTypeInfoAddr == 0) {
            std::cout << "[*] Client singleton araniyor..." << std::endl;

            // Yöntem 1: Bilinen hint adresini dene
            uintptr_t hintAddr = mem.gameAssemblyBase + Config::Offsets::DrrTypeInfo_Hint;
            uintptr_t testKlass = mem.Read<uintptr_t>(hintAddr);
            
            if (logFile) { 
                fprintf(logFile, "\n--- Client Search ---\n");
                fprintf(logFile, "Hint addr: 0x%llX -> klass: 0x%llX\n", 
                        (unsigned long long)hintAddr, (unsigned long long)testKlass);
                fflush(logFile); 
            }
            
            if (IsValidPtr(testKlass)) {
                uintptr_t testStaticFields = mem.Read<uintptr_t>(testKlass + Config::Offsets::Il2CppClass_StaticFields);
                if (logFile) { fprintf(logFile, "  staticFields: 0x%llX\n", (unsigned long long)testStaticFields); fflush(logFile); }
                
                if (IsValidPtr(testStaticFields)) {
                    uintptr_t testInstance = mem.Read<uintptr_t>(testStaticFields + Config::Offsets::Singleton_Instance);
                    if (logFile) { fprintf(logFile, "  singleton: 0x%llX\n", (unsigned long long)testInstance); fflush(logFile); }
                    
                    if (IsValidPtr(testInstance)) {
                        uintptr_t testLP = mem.Read<uintptr_t>(testInstance + Config::Offsets::Singleton_LocalPlayer);
                        uintptr_t testET = mem.Read<uintptr_t>(testInstance + Config::Offsets::Singleton_EntityTable);
                        if (logFile) { 
                            fprintf(logFile, "  localPlayer: 0x%llX, entityTable: 0x%llX\n", 
                                    (unsigned long long)testLP, (unsigned long long)testET); 
                            fflush(logFile); 
                        }
                        
                        if (IsValidPtr(testLP) && IsValidPtr(testET)) {
                            ClientTypeInfoAddr = hintAddr;
                            SingletonInstance = testInstance;
                            std::cout << "[+] HINT ADRESI CALISTI! ClientTypeInfo: 0x" << std::hex << ClientTypeInfoAddr << std::dec << std::endl;
                            if (logFile) { fprintf(logFile, "SUCCESS via hint!\n"); fflush(logFile); }
                        }
                    }
                }
            }

            // Yöntem 2: Auto-scanner (hint başarısızsa)
            if (ClientTypeInfoAddr == 0) {
                std::cout << "[*] Hint basarisiz, auto-scanner baslatiyor... (yavas olabilir)" << std::endl;
                if (logFile) { fprintf(logFile, "Hint failed, starting auto-scan...\n"); fflush(logFile); }
                
                // IL2CPP TypeInfo pointer tablosu genelde GameAssembly.dll'in .data section'ında bulunur
                // Makul aralık: 0x4000000 - 0x6000000 (64-96MB offset)
                for (uintptr_t offset = 0x4000000; offset < 0x6000000; offset += 8) {
                    uintptr_t addr = mem.gameAssemblyBase + offset;
                    uintptr_t testKlass2 = mem.Read<uintptr_t>(addr);
                    
                    if (!IsValidPtr(testKlass2)) continue;
                    
                    uintptr_t testSF = mem.Read<uintptr_t>(testKlass2 + 0xB8);
                    if (!IsValidPtr(testSF)) continue;
                    
                    uintptr_t testInst = mem.Read<uintptr_t>(testSF + 0x0);
                    if (!IsValidPtr(testInst)) continue;
                    
                    uintptr_t testLP2 = mem.Read<uintptr_t>(testInst + 0x18);
                    uintptr_t testET2 = mem.Read<uintptr_t>(testInst + 0x70);
                    
                    if (!IsValidPtr(testLP2) || !IsValidPtr(testET2)) continue;
                    
                    uintptr_t testLP2_entity = mem.Read<uintptr_t>(testLP2 + Config::Offsets::EntityView_Entity);
                    if (!IsValidPtr(testLP2_entity)) continue;

                    Vector2D testPos = mem.Read<Vector2D>(testLP2_entity + Config::Offsets::Entity_Position2D);
                    if (testPos.x == 0.0f && testPos.y == 0.0f) continue;
                    if (testPos.x < -100000.0f || testPos.x > 100000.0f) continue;
                    if (testPos.y < -100000.0f || testPos.y > 100000.0f) continue;
                    
                    // Ek doğrulama: Entity tablosunun (Hashtable) count'u mantıklı mı?
                    int htCount = mem.Read<int>(testET2 + 0x18); // Hashtable._count
                    if (htCount <= 0 || htCount > 10000) continue;
                    
                    ClientTypeInfoAddr = addr;
                    SingletonInstance = testInst;
                    std::cout << "[+] AUTO-SCANNER BULDU! Offset: 0x" << std::hex << offset 
                              << " TestPos: " << testPos.x << ", " << testPos.y 
                              << " EntityCount: " << std::dec << htCount << std::endl;
                    if (logFile) { 
                        fprintf(logFile, "AUTO-SCAN FOUND at offset 0x%llX, pos=(%.1f, %.1f), htCount=%d\n", 
                                (unsigned long long)offset, testPos.x, testPos.y, htCount); 
                        fflush(logFile); 
                    }
                    break;
                }
            }

            if (ClientTypeInfoAddr == 0) {
                std::cout << "[-] Client bulunamadi! Oyuna tam giris yaptiginizdan emin olun." << std::endl;
                Sleep(3000);
                continue;
            }
        }

        // ── AŞAMA 1: Singleton instance'ı yeniden oku (her frame) ──────
        {
            uintptr_t klass = mem.Read<uintptr_t>(ClientTypeInfoAddr);
            uintptr_t staticFields = mem.Read<uintptr_t>(klass + Config::Offsets::Il2CppClass_StaticFields);
            SingletonInstance = mem.Read<uintptr_t>(staticFields + Config::Offsets::Singleton_Instance);
        }

        std::vector<RadarEntity> entities;
        float myPosX = 0.0f, myPosY = 0.0f;

        if (IsValidPtr(SingletonInstance)) {
            // ── Local Player pozisyonu oku ──
            uintptr_t localPlayerPtr = mem.Read<uintptr_t>(SingletonInstance + Config::Offsets::Singleton_LocalPlayer);
            if (IsValidPtr(localPlayerPtr)) {
                uintptr_t localEntityObj = mem.Read<uintptr_t>(localPlayerPtr + Config::Offsets::EntityView_Entity);
                if (IsValidPtr(localEntityObj)) {
                    Vector2D localPos = mem.Read<Vector2D>(localEntityObj + Config::Offsets::Entity_Position2D);
                    myPosX = localPos.x;
                    myPosY = localPos.y; // Unity 2D (x,y) yatay düzlem
                }
            }

            // ── Entity Tablosu (Hashtable) oku ──
            uintptr_t entityTable = mem.Read<uintptr_t>(SingletonInstance + Config::Offsets::Singleton_EntityTable);
            if (IsValidPtr(entityTable)) {
                // Hashtable yapısı: _buckets (0x10) = bucket[], _count (0x18) = int
                uintptr_t bucketsArray = mem.Read<uintptr_t>(entityTable + Config::Hashtable::Buckets);
                int htCount = mem.Read<int>(entityTable + Config::Hashtable::Count);
                
                if (IsValidPtr(bucketsArray) && htCount > 0 && htCount < 10000) {
                    // C# Array header: +0x10 = length, +0x20 = data start (IL2CPP array)
                    int bucketsLen = mem.Read<int>(bucketsArray + 0x18);
                    if (bucketsLen <= 0 || bucketsLen > 50000) bucketsLen = htCount * 2;
                    
                    static int logCounter = 0;
                    bool shouldLog = (logCounter++ % 300 == 0); // Her ~5 saniyede bir logla
                    
                    if (shouldLog && logFile) {
                        fprintf(logFile, "\n--- Frame %d ---\n", logCounter);
                        fprintf(logFile, "Singleton: 0x%llX, LocalPlayer: 0x%llX (pos: %.1f, %.1f)\n",
                                (unsigned long long)SingletonInstance, (unsigned long long)localPlayerPtr, myPosX, myPosY);
                        fprintf(logFile, "EntityTable: 0x%llX, Buckets: 0x%llX, Count: %d, BucketsLen: %d\n",
                                (unsigned long long)entityTable, (unsigned long long)bucketsArray, htCount, bucketsLen);
                        fflush(logFile);
                    }

                    int found = 0;
                    for (int i = 0; i < bucketsLen && found < htCount + 10; i++) {
                        // Bucket: { key (8), val (8), hash_coll (4 + padding) }
                        // IL2CPP array: data starts at +0x20
                        uintptr_t bucketAddr = bucketsArray + 0x20 + (i * Config::Hashtable::BucketStride);
                        uintptr_t key = mem.Read<uintptr_t>(bucketAddr + Config::Hashtable::Bucket_Key);
                        uintptr_t val = mem.Read<uintptr_t>(bucketAddr + Config::Hashtable::Bucket_Val);
                        
                        if (!IsValidPtr(key) || !IsValidPtr(val)) continue;
                        
                        found++;
                        
                        // val muhtemelen EntityView* veya entity container
                        uintptr_t entityView = val;
                        
                        // Entity tipi kontrol
                        int eType = GetEntityType(entityView);
                        
                        // Pozisyon oku
                        uintptr_t entityObj = mem.Read<uintptr_t>(entityView + Config::Offsets::EntityView_Entity);
                        if (!IsValidPtr(entityObj)) continue;
                        
                        Vector2D pos = mem.Read<Vector2D>(entityObj + Config::Offsets::Entity_Position2D);
                        
                        if (shouldLog && logFile && found <= 5) {
                            fprintf(logFile, "  Bucket[%d] key=0x%llX val=0x%llX type=%d pos=(%.1f, %.1f)\n",
                                    i, (unsigned long long)key, (unsigned long long)val, eType, pos.x, pos.y);
                            fflush(logFile);
                        }
                        
                        // Koordinat geçerlilik kontrolü
                        if (pos.x == 0.0f && pos.y == 0.0f) continue;
                        if (pos.x < -100000.0f || pos.x > 100000.0f) continue;
                        if (pos.y < -100000.0f || pos.y > 100000.0f) continue;
                        
                        float dx = pos.x - myPosX;
                        float dy = pos.y - myPosY;
                        float distance = std::sqrt(dx*dx + dy*dy);
                        
                        if (distance < RadarSettings::MaxDistance && distance > 0.5f) {
                            std::string name;
                            switch (eType) {
                                case 1: name = "Player"; break;
                                case 2: name = "Resource"; break;
                                case 3: name = "Mob"; break;
                                case 4: name = "Self"; break;
                                default: name = "Entity"; break;
                            }
                            
                            // Remote player ise isim okumayı dene
                            if (eType == 1) {
                                uintptr_t namePtr = mem.Read<uintptr_t>(entityObj + Config::Offsets::Entity_PlayerName);
                                if (IsValidPtr(namePtr)) {
                                    std::string playerName = mem.ReadSystemString(namePtr);
                                    if (!playerName.empty()) name = playerName;
                                }
                            }
                            
                            entities.push_back({name, pos.x, pos.y, distance, eType, (eType == 4)});
                        }
                    }
                    
                    if (shouldLog && logFile) {
                        fprintf(logFile, "Total entities in range: %zu\n", entities.size());
                        fflush(logFile);
                    }
                }
            }

            // ── Hotkey: F1 = Zoom Hack ──
            if (GetAsyncKeyState(VK_F1) & 1) {
                zoomHackEnabled = !zoomHackEnabled;
                std::cout << "[!] Zoom Hack: " << (zoomHackEnabled ? "ACIK" : "KAPALI") << std::endl;
            }
        }

        // ── GDI Çizim ──────────────────────────────────────────────────
        HDC hdc = GetDC(g_hwnd);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdc, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        SelectObject(memDC, hBitmap);

        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        RECT rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        FillRect(memDC, &rect, bgBrush);
        DeleteObject(bgBrush);

        POINT center = { RadarSettings::RadarPosX + (int)RadarSettings::DefaultRadius, RadarSettings::RadarPosY + (int)RadarSettings::DefaultRadius };

        // Radar arka plan dairesi
        HBRUSH darkBrush = CreateSolidBrush(RGB(15, 18, 25));
        HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(0, 255, 180));
        SelectObject(memDC, darkBrush);
        SelectObject(memDC, borderPen);
        Ellipse(memDC, center.x - (int)RadarSettings::DefaultRadius, center.y - (int)RadarSettings::DefaultRadius, 
                center.x + (int)RadarSettings::DefaultRadius, center.y + (int)RadarSettings::DefaultRadius);
        DeleteObject(borderPen);
        DeleteObject(darkBrush);

        // Grid çizgileri
        HPEN gridPen = CreatePen(PS_DOT, 1, RGB(40, 60, 80));
        SelectObject(memDC, gridPen);
        MoveToEx(memDC, center.x - (int)RadarSettings::DefaultRadius, center.y, NULL);
        LineTo(memDC, center.x + (int)RadarSettings::DefaultRadius, center.y);
        MoveToEx(memDC, center.x, center.y - (int)RadarSettings::DefaultRadius, NULL);
        LineTo(memDC, center.x, center.y + (int)RadarSettings::DefaultRadius);
        // İç çember (yarı mesafe)
        SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Ellipse(memDC, center.x - (int)(RadarSettings::DefaultRadius/2), center.y - (int)(RadarSettings::DefaultRadius/2), 
                center.x + (int)(RadarSettings::DefaultRadius/2), center.y + (int)(RadarSettings::DefaultRadius/2));
        DeleteObject(gridPen);

        // Kendi merkezimiz (cyan)
        HBRUSH selfBrush = CreateSolidBrush(RGB(0, 220, 255));
        SelectObject(memDC, selfBrush);
        Ellipse(memDC, center.x - 4, center.y - 4, center.x + 4, center.y + 4);
        DeleteObject(selfBrush);

        // Font
        SetBkMode(memDC, TRANSPARENT);
        HFONT hFont = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(memDC, hFont);

        // Entity'leri çiz
        for (const auto& entity : entities) {
            // Sadece Mob(3) ve Player(1) gösterilecek. Resource (2) elendi.
            if (entity.entityType != 1 && entity.entityType != 3) continue;

            POINT dotPos = WorldToCircularRadarGDI(myPosX, myPosY, entity.posX, entity.posY, center, RadarSettings::DefaultRadius, RadarSettings::MaxDistance);

            // Renk: tip bazlı
            COLORREF color;
            switch (entity.entityType) {
                case 1: color = RGB(255, 60, 60); break;    // Player = kırmızı
                case 3: color = RGB(255, 165, 0); break;    // Mob = turuncu
                default: color = RGB(200, 200, 200); break; // Unknown = gri (artık buraya düşmemesi lazım)
            }

            HBRUSH dotBrush = CreateSolidBrush(color);
            HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            SelectObject(memDC, dotBrush);
            SelectObject(memDC, dotPen);
            
            int dotSize = (entity.entityType == 1) ? 7 : 5; // Oyuncular büyük, moblar küçük
            Ellipse(memDC, dotPos.x - dotSize, dotPos.y - dotSize, dotPos.x + dotSize, dotPos.y + dotSize);

            // İsim ve mesafe etiketi
            SetTextColor(memDC, color);
            std::string label = entity.name + " (" + std::to_string((int)entity.distance) + "m)";
            TextOutA(memDC, dotPos.x + dotSize + 3, dotPos.y - 7, label.c_str(), (int)label.length());

            DeleteObject(dotBrush);
            DeleteObject(dotPen);
        }

        // Entity sayısı göstergesi (sol alt)
        SetTextColor(memDC, RGB(0, 255, 180));
        std::string infoText = "Entities: " + std::to_string(entities.size());
        TextOutA(memDC, RadarSettings::RadarPosX, RadarSettings::RadarPosY + (int)(RadarSettings::DefaultRadius * 2) + 10, 
                 infoText.c_str(), (int)infoText.length());
        
        // Zoom hack durumu
        if (zoomHackEnabled) {
            SetTextColor(memDC, RGB(255, 255, 0));
            const char* zoomText = "ZOOM: ON";
            TextOutA(memDC, RadarSettings::RadarPosX, RadarSettings::RadarPosY + (int)(RadarSettings::DefaultRadius * 2) + 25, 
                     zoomText, (int)strlen(zoomText));
        }

        SelectObject(memDC, hOldFont);
        DeleteObject(hFont);

        BitBlt(hdc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), memDC, 0, 0, SRCCOPY);
        DeleteObject(hBitmap);
        DeleteDC(memDC);
        ReleaseDC(g_hwnd, hdc);

        Sleep(16); // ~60 FPS
    }

cleanup:
    if (logFile) fclose(logFile);
    mem.Close();
    return 0;
}
