/*
 * radar.cpp â€” Albion Online External GDI Radar Overlay
 * Kernel Driver (MmCopyVirtualMemory) tabanlÄ± IL2CPP Entity Radar
 * 
 * Pointer zinciri (txt dosyasÄ±ndan):
 *   Il2CppClass* klass = read(gameAssemblyBase + off::client)  // drr_TypeInfo ptr
 *   staticFields       = read(klass + 0xB8)                    // Il2CppClass->static_fields
 *   singletonInstance   = read(staticFields + 0x0)              // static drr a (offset 0x0)
 *   localPlayer         = read(singletonInstance + 0x18)        // LocalPlayerCharacterView c
 *   entityHashtable     = read(singletonInstance + 0x70)        // Hashtable n (entity container)
 */

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <dwmapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")

// D3D11 Globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
#include <cstdio>
#include <cstdint>

#include "../kernel_radar/shared/ioctls.h"

// ── Offsetler & Class Hashleri (IL2CPP) ─────────────────────────────
namespace Config {
    constexpr float RadarSize       = 300.0f;
    constexpr float DefaultZoom     = 2.5f;

    namespace Offsets {
        // drr_TypeInfo adresi (script.json'dan: 80323912 decimal = 0x4C9A548)
        constexpr uintptr_t DrrTypeInfo_Hint = 0x4C9A548;

        // IL2CPP class yapısı
        constexpr uintptr_t Il2CppClass_StaticFields = 0xB8;  // Il2CppClass->static_fields pointer

        // drr singleton instance field offsetleri (dump.cs'ten)
        constexpr uintptr_t Singleton_Instance = 0x0;   // static drr a; singleton
        constexpr uintptr_t Singleton_LocalPlayer = 0x18; // LocalPlayerCharacterView c
        constexpr uintptr_t Singleton_EntityTable = 0x70; // Hashtable n (entity container)

        // LocalPlayerCharacterView offsetleri
        constexpr uintptr_t LocalPlayer_ActorCameraController = 0x2D8; // ActorCameraController g;
        constexpr uintptr_t LocalPlayer_Camera = 0x2F8;                // Camera k;

        // ActorCameraController offsetleri
        constexpr uintptr_t ActorCamera_Zoom = 0xE4; // float Zoom;

        // EntityView offsetleri
        constexpr uintptr_t EntityView_Entity        = 0x20;    // SimulationObjectView -> cft a;
        constexpr uintptr_t EntityView_Transform     = 0x28;    // SimulationObjectView -> Transform b;
        constexpr uintptr_t EntityView_Speed         = 0x15C;   // movement speed

        // Entity (FightingObject / PlayerCharacter) offsetleri
        constexpr uintptr_t Entity_Rotation    = 0x38;
        constexpr uintptr_t Entity_Position2D  = 0x3C;  // cfw -> Vector2 h;
        constexpr uintptr_t Entity_HealthBar   = 0x80;  // healthBar* -> +0x10 current, +0x18 max
        constexpr uintptr_t Entity_PartyGuid   = 0x1E8; // Guid agr (Party / Group Guid)
        constexpr uintptr_t Entity_GuildGuid   = 0x308; // Guid (Guild Guid)
        constexpr uintptr_t Entity_GuildName   = 0x318; // System.String (Guild Name)
        constexpr uintptr_t Entity_AllianceName= 0x338; // System.String (Alliance Name)
        constexpr uintptr_t Entity_PlayerName  = 0x348; // System.String (UTF-16)
        constexpr uintptr_t Entity_EquipmentList = 0x2C0; // List<Equipment*> (10 slots)

        // Equipment item offsetleri
        constexpr uintptr_t Equipment_Name     = 0x20;  // System.String or char*
        constexpr uintptr_t Equipment_Tier     = 0x28;  // int (T1..T8)
        constexpr uintptr_t Equipment_Enchant  = 0x38;  // int (0..4)
        constexpr uintptr_t Equipment_Slot     = 0x218; // int (0-7)

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
    inline float DefaultRadius  = 200.0f; // Ekranda kapladığı makul boyut
    inline float MaxDistance    = 150.0f; // Oyun içi makul max tarama mesafesi (sunucunun yolladığı maksimum civarı)
    inline float RadarBgAlpha   = 0.25f;  // Radar arkaplan saydamlığı (0 = tamamen transparan/görünmez, 1 = koyu opak)
    inline bool  DrawRadarBg    = true;   // Arkaplan dairesi çizilsin mi
    inline bool  DrawRadarGrid  = true;   // Izgara çizgileri çizilsin mi
    inline bool  ShowMobs       = false;  // Canavarları (Mob) radarda gizle/göster (Varsayılan: Kapalı)
    inline bool  ShowBosses     = true;   // Boss / Elit Canavarları gizle/göster (Varsayılan: AÇIK)
    inline bool  EnableGankHud  = true;   // 3. Kilitli Hedef Gank HUD paneli
    inline bool  EnableLeadLine = true;   // 2. Önünü kesme rota tahmin çizgisi
    inline bool  EnableHpBars   = true;   // 1. Can barı (% HP) ve Low HP tespiti
    inline bool  EnableIPDisplay= true;   // Eşya Gücü (Item Power - IP) & Silah Göstergesi
    inline bool  EnableWhaleAlert= true;  // 8.3/8.4 Yüksek Değerli Hedef (Whale) Mor Parlaması
    inline float LeadTime       = 2.5f;   // Rota tahmini kaç saniye ilerisi (saniye)
    constexpr int RadarPosX     = 40;
    constexpr int RadarPosY     = 40;
}

struct Vector2D { float x, y; };
struct Vector3D { float x, y, z; };

struct AlbionGuid {
    uint64_t low = 0;
    uint64_t high = 0;

    bool IsZero() const {
        return low == 0 && high == 0;
    }
    bool operator==(const AlbionGuid& o) const {
        return low == o.low && high == o.high;
    }
    bool operator!=(const AlbionGuid& o) const {
        return !(*this == o);
    }
};

struct RadarEntity {
    std::string name;
    std::string guildName;
    std::string allianceName;
    float posX;
    float posY;
    float distance;
    int entityType; // 1: Player, 3: Mob, 5: Boss
    bool isLocal;
    float velocity;
    Vector2D dir;       // Hareket yönü vektörü (normalize)
    Vector2D predPos;   // Tahmini önünü kesme konumu (Lead Position)
    bool isMounted;
    bool isWhitelisted;
    bool isPartyMember;
    float curHealth;
    float maxHealth;
    float healthPercent;
    bool isLowHealth;   // Canı %40'tan azsa
    int estimatedIP;    // Ortalama Item Power (IP)
    std::string weaponName; // Silah İsmi (Örn: T8.3 Bloodletter)
    bool isWhale;       // IP >= 1400 Yüksek Değerli Hedef (Whale)
};

// ── Kapanma Hızı Takipçisi (Closing Speed Tracker) ─────────────────
struct TargetClosingTracker {
    float lastDistance = 0.0f;
    std::chrono::steady_clock::time_point lastTime;
    float closingSpeed = 0.0f; // Pozitif = yaklaşıyor, Negatif = uzaklaşıyor
};
static std::unordered_map<std::string, TargetClosingTracker> g_closingTrackers;

// ── Whitelist & Parti Yönetimi ──────────────────────────────────────
enum class WhitelistSource {
    MANUAL,
    PARTY,
    GUILD,
    ALLIANCE
};

struct WhitelistEntry {
    std::string name;
    WhitelistSource source;
    std::chrono::steady_clock::time_point addedTime;
};

static std::unordered_map<std::string, WhitelistEntry> g_whitelist;
static bool g_autoPartyWhitelist = true;
static bool g_autoGuildWhitelist = false;
static bool g_autoAllianceWhitelist = false;
static bool g_showFriendlyESP = false;
static bool g_partyAlertSound = false;

static std::string ToLowerStr(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return lower;
}

static bool IsWhitelisted(const std::string& name) {
    if (name.empty() || name == "Player" || name == "Self") return false;
    std::string key = ToLowerStr(name);
    return g_whitelist.find(key) != g_whitelist.end();
}

static void AddToWhitelist(const std::string& name, WhitelistSource source = WhitelistSource::MANUAL) {
    if (name.empty() || name == "Player" || name == "Self") return;
    std::string key = ToLowerStr(name);
    if (g_whitelist.find(key) == g_whitelist.end()) {
        g_whitelist[key] = { name, source, std::chrono::steady_clock::now() };
    }
}

static void RemoveFromWhitelist(const std::string& name) {
    std::string key = ToLowerStr(name);
    g_whitelist.erase(key);
}

static void ClearWhitelist() {
    g_whitelist.clear();
}

// â”€â”€ Kernel Bellek Okuyucu â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

    // Bellek yazma (zoom hack iÃ§in)
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

// ── Pointer geçerlilik kontrolü ──────────────────────────────────────
static bool IsValidPtr(uintptr_t ptr) {
    return ptr > 0x10000000000ULL && ptr < 0x7FFFFFFFFFFFULL;
}

// ── Eşya Gücü (Item Power - IP) & Silah Ayrıştırma ───────────────────
struct EquipmentInfo {
    int averageIP = 0;
    std::string mainWeapon = "";
    int weaponTier = 0;
    int weaponEnchant = 0;
    int totalItems = 0;
};

static std::string FormatWeaponName(const std::string& rawName, int tier, int enchant) {
    std::string prefix = "";
    if (tier > 0) {
        prefix = "T" + std::to_string(tier) + "." + std::to_string(enchant) + " ";
    }

    if (rawName.empty()) {
        return prefix.empty() ? "Silah" : (prefix + "Silah");
    }

    std::string lower = ToLowerStr(rawName);
    std::string wName = "";

    if (lower.find("bloodletter") != std::string::npos || (lower.find("dagger") != std::string::npos && lower.find("hell") != std::string::npos)) wName = "Bloodletter";
    else if (lower.find("deathgivers") != std::string::npos) wName = "Deathgivers";
    else if (lower.find("daggerpair") != std::string::npos || lower.find("claws") != std::string::npos) wName = "Claws";
    else if (lower.find("dagger") != std::string::npos) wName = "Dagger";
    else if (lower.find("doublebladed") != std::string::npos || (lower.find("quarterstaff") != std::string::npos && lower.find("hell") != std::string::npos)) wName = "Double Bladed";
    else if (lower.find("quarterstaff") != std::string::npos || lower.find("ironclad") != std::string::npos) wName = "Quarterstaff";
    else if (lower.find("battleaxe") != std::string::npos) wName = "Battleaxe";
    else if (lower.find("bearpaws") != std::string::npos || (lower.find("axe") != std::string::npos && lower.find("hell") != std::string::npos)) wName = "Bear Paws";
    else if (lower.find("greataxe") != std::string::npos) wName = "Greataxe";
    else if (lower.find("axe") != std::string::npos) wName = "Axe";
    else if (lower.find("carving") != std::string::npos) wName = "Carving Sword";
    else if (lower.find("clarent") != std::string::npos) wName = "Clarent Blade";
    else if (lower.find("dualswords") != std::string::npos) wName = "Dual Swords";
    else if (lower.find("broadsword") != std::string::npos || lower.find("claymore") != std::string::npos) wName = "Claymore";
    else if (lower.find("sword") != std::string::npos) wName = "Sword";
    else if (lower.find("cursed") != std::string::npos || lower.find("curse") != std::string::npos) wName = "Cursed Staff";
    else if (lower.find("wildfire") != std::string::npos || lower.find("fire") != std::string::npos) wName = "Fire Staff";
    else if (lower.find("frost") != std::string::npos || lower.find("blizzard") != std::string::npos || lower.find("ice") != std::string::npos) wName = "Frost Staff";
    else if (lower.find("badon") != std::string::npos || (lower.find("bow") != std::string::npos && lower.find("keeper") != std::string::npos)) wName = "Bow of Badon";
    else if (lower.find("warbow") != std::string::npos) wName = "Warbow";
    else if (lower.find("bow") != std::string::npos) wName = "Bow";
    else if (lower.find("crossbow") != std::string::npos || lower.find("boltcasters") != std::string::npos) wName = "Crossbow";
    else if (lower.find("spear") != std::string::npos || lower.find("trident") != std::string::npos || lower.find("glaive") != std::string::npos) wName = "Spear";
    else if (lower.find("mace") != std::string::npos) wName = "Mace";
    else if (lower.find("hammer") != std::string::npos || lower.find("great_hammer") != std::string::npos) wName = "Hammer";
    else if (lower.find("holy") != std::string::npos || lower.find("divine") != std::string::npos) wName = "Holy Staff";
    else if (lower.find("nature") != std::string::npos || lower.find("druid") != std::string::npos) wName = "Nature Staff";
    else if (lower.find("shapeshifter") != std::string::npos || lower.find("prowler") != std::string::npos) wName = "Shapeshifter";
    else {
        std::string cleaned = rawName;
        for (char& c : cleaned) if (c == '_') c = ' ';
        wName = cleaned;
    }

    return prefix + wName;
}

static EquipmentInfo ReadPlayerEquipmentAndIP(uintptr_t entityObj, float maxHealth) {
    EquipmentInfo info;
    info.averageIP = 0;
    
    // 1. IL2CPP List<Equipment*> Okuma (Entity + 0x2C0)
    uintptr_t eqListPtr = mem.Read<uintptr_t>(entityObj + Config::Offsets::Entity_EquipmentList);
    int totalIPSum = 0;
    int combatItemCount = 0;

    if (IsValidPtr(eqListPtr)) {
        uintptr_t itemsArray = mem.Read<uintptr_t>(eqListPtr + 0x10);
        int listCount = mem.Read<int>(eqListPtr + 0x18);
        
        if (IsValidPtr(itemsArray) && listCount > 0 && listCount <= 16) {
            for (int i = 0; i < listCount; i++) {
                uintptr_t itemPtr = mem.Read<uintptr_t>(itemsArray + 0x20 + (i * 8));
                if (!IsValidPtr(itemPtr)) continue;
                
                int tier = mem.Read<int>(itemPtr + Config::Offsets::Equipment_Tier);
                int enchant = mem.Read<int>(itemPtr + Config::Offsets::Equipment_Enchant);
                int slot = mem.Read<int>(itemPtr + Config::Offsets::Equipment_Slot);
                
                if (tier >= 1 && tier <= 8) {
                    if (enchant < 0 || enchant > 4) enchant = 0;
                    
                    int itemIP = (tier * 100) + (enchant * 100) + 300;
                    totalIPSum += itemIP;
                    combatItemCount++;
                    info.totalItems++;

                    uintptr_t namePtr = mem.Read<uintptr_t>(itemPtr + Config::Offsets::Equipment_Name);
                    std::string rawItemName = "";
                    if (IsValidPtr(namePtr)) {
                        rawItemName = mem.ReadSystemString(namePtr);
                        if (rawItemName.empty()) rawItemName = mem.ReadString(namePtr, 48);
                    }
                    
                    std::string lower = ToLowerStr(rawItemName);
                    if (lower.find("main") != std::string::npos || lower.find("2h") != std::string::npos || lower.find("weapon") != std::string::npos || slot == 3 || info.mainWeapon.empty()) {
                        if (info.mainWeapon.empty() || lower.find("main") != std::string::npos || lower.find("2h") != std::string::npos) {
                            info.mainWeapon = FormatWeaponName(rawItemName, tier, enchant);
                            info.weaponTier = tier;
                            info.weaponEnchant = enchant;
                        }
                    }
                }
            }
        }
    }

    // IP Hesaplama / Akıllı IP Motoru
    if (combatItemCount >= 2) {
        info.averageIP = totalIPSum / combatItemCount;
    } else if (maxHealth > 0.0f) {
        // Can bazlı hassas IP hesaplama
        float baseHp = 2200.0f;
        float estimated = 700.0f + (maxHealth - baseHp) * 0.45f;
        if (estimated < 600.0f) estimated = 600.0f;
        if (estimated > 2200.0f) estimated = 2200.0f;
        info.averageIP = static_cast<int>(estimated);
        if (info.mainWeapon.empty()) {
            if (info.averageIP >= 1400) info.mainWeapon = "T8.3+ Yüksek Seviye Set";
            else if (info.averageIP >= 1150) info.mainWeapon = "T7/T8 PvP Seti";
            else if (info.averageIP >= 900) info.mainWeapon = "T5/T6 Set";
            else info.mainWeapon = "T4.1 Hafif Set";
        }
    } else {
        info.averageIP = 900;
        if (info.mainWeapon.empty()) info.mainWeapon = "Standart Set";
    }

    return info;
}

// ── Entity tipi belirleme (IL2CPP class hash) ──────────────────────────
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

// â”€â”€ Overlay / GDI TanÄ±mlamalarÄ± â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static HWND g_hwnd = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// â”€â”€ DÃ¶nÃ¼ÅŸtÃ¼rÃ¼cÃ¼ Matematik â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static POINT WorldToCircularRadarGDI(float localX, float localY, float targetX, float targetY, POINT center, float radius, float maxDist, float margin = 12.0f) {
    float dx = targetX - localX;
    float dy = targetY - localY;
    
    float angle = std::atan2(dy, dx);
    angle -= 0.785398f; // -45 derece (izometrik kamera)
    
    float dist = std::sqrt(dx * dx + dy * dy);
    float usableRadius = radius - margin; // Dot+label radardan taşmasın
    float normDist = dist / maxDist;
    if (normDist > 1.0f) normDist = 1.0f;
    
    int radarX = center.x + static_cast<int>(normDist * usableRadius * std::cos(angle));
    int radarY = center.y - static_cast<int>(normDist * usableRadius * std::sin(angle));
    return { radarX, radarY };
}

// ── Main Loop ──────────────────────────────────────────────────────────
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

    // ImGui / D3D11 Transparent Overlay Window (Maskelenmiş Class & Window Name)
    WNDCLASSEX wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, "DiscordOverlay_Host", nullptr };
    RegisterClassEx(&wc);
    g_hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, "Discord In-Game Overlay", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr);
    
    // Siyah (0,0,0) rengi transparan yap
    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
    
    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass("DiscordOverlay_Host", wc.hInstance);
        return 1;
    }
    
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    std::cout << "[*] ImGui + DX11 Overlay baslatildi!" << std::endl;
    std::cout << "[*] INSERT tusuna basarak menuyu acip kapatabilir ve radari tasiyabilirsiniz." << std::endl;

    MSG msg;
    
    // off::client bulma â€” iki aÅŸamalÄ±:
    // 1. Ã–nce script.json'dan bilinen hint adresi dene
    // 2. BaÅŸarÄ±sÄ±zsa, auto-scanner ile tara
    static uintptr_t ClientTypeInfoAddr = 0;     // GameAssembly.dll + offset â†’ Il2CppClass* pointer
    static uintptr_t SingletonInstance = 0;       // drr singleton instance

    while (true) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) goto cleanup;
        }

        // TOPMOST kontrolÃ¼
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        // â”€â”€ AÅAMA 0: Client TypeInfo adresini bul â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (ClientTypeInfoAddr == 0) {
            std::cout << "[*] Client singleton araniyor..." << std::endl;

            // YÃ¶ntem 1: Bilinen hint adresini dene
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

            // YÃ¶ntem 2: Auto-scanner (hint baÅŸarÄ±sÄ±zsa)
            if (ClientTypeInfoAddr == 0) {
                std::cout << "[*] Hint basarisiz, auto-scanner baslatiyor... (yavas olabilir)" << std::endl;
                if (logFile) { fprintf(logFile, "Hint failed, starting auto-scan...\n"); fflush(logFile); }
                
                // IL2CPP TypeInfo pointer tablosu genelde GameAssembly.dll'in .data section'Ä±nda bulunur
                // Makul aralÄ±k: 0x4000000 - 0x6000000 (64-96MB offset)
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
                    
                    // Ek doÄŸrulama: Entity tablosunun (Hashtable) count'u mantÄ±klÄ± mÄ±?
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

        // â”€â”€ AÅAMA 1: Singleton instance'Ä± yeniden oku (her frame) â”€â”€â”€â”€â”€â”€
        {
            uintptr_t klass = mem.Read<uintptr_t>(ClientTypeInfoAddr);
            uintptr_t staticFields = mem.Read<uintptr_t>(klass + Config::Offsets::Il2CppClass_StaticFields);
            SingletonInstance = mem.Read<uintptr_t>(staticFields + Config::Offsets::Singleton_Instance);
        }

        std::vector<RadarEntity> entities;
        float myPosX = 0.0f, myPosY = 0.0f;
        AlbionGuid localPartyGuid{};
        std::string localGuildName = "";
        std::string localAllianceName = "";

        if (IsValidPtr(SingletonInstance)) {
            // ── Local Player pozisyonu ve Parti/Guild Bilgileri oku ──
            uintptr_t localPlayerPtr = mem.Read<uintptr_t>(SingletonInstance + Config::Offsets::Singleton_LocalPlayer);
            if (IsValidPtr(localPlayerPtr)) {
                uintptr_t localEntityObj = mem.Read<uintptr_t>(localPlayerPtr + Config::Offsets::EntityView_Entity);
                if (IsValidPtr(localEntityObj)) {
                    Vector2D localPos = mem.Read<Vector2D>(localEntityObj + Config::Offsets::Entity_Position2D);
                    myPosX = localPos.x;
                    myPosY = localPos.y; // Unity 2D (x,y) yatay düzlem

                    // Local Player Party GUID
                    localPartyGuid = mem.Read<AlbionGuid>(localEntityObj + Config::Offsets::Entity_PartyGuid);

                    // Local Player Guild & Alliance
                    uintptr_t localGuildPtr = mem.Read<uintptr_t>(localEntityObj + Config::Offsets::Entity_GuildName);
                    if (IsValidPtr(localGuildPtr)) {
                        localGuildName = mem.ReadSystemString(localGuildPtr);
                    }
                    uintptr_t localAllyPtr = mem.Read<uintptr_t>(localEntityObj + Config::Offsets::Entity_AllianceName);
                    if (IsValidPtr(localAllyPtr)) {
                        localAllianceName = mem.ReadSystemString(localAllyPtr);
                    }
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
                        fprintf(logFile, "Singleton: 0x%llX, LocalPlayer: 0x%llX (pos: %.1f, %.1f), PartyGuid: %llX-%llX\n",
                                (unsigned long long)SingletonInstance, (unsigned long long)localPlayerPtr, myPosX, myPosY,
                                (unsigned long long)localPartyGuid.low, (unsigned long long)localPartyGuid.high);
                        fprintf(logFile, "EntityTable: 0x%llX, Buckets: 0x%llX, Count: %d, BucketsLen: %d\n",
                                (unsigned long long)entityTable, (unsigned long long)bucketsArray, htCount, bucketsLen);
                        fflush(logFile);
                    }

                    int found = 0;
                    for (int i = 0; i < bucketsLen; i++) {
                        // Bucket: { key (8), val (8), hash_coll (4 + padding) }
                        // IL2CPP array: data starts at +0x20
                        uintptr_t bucketAddr = bucketsArray + 0x20 + (i * Config::Hashtable::BucketStride);
                        uintptr_t key = mem.Read<uintptr_t>(bucketAddr + Config::Hashtable::Bucket_Key);
                        uintptr_t val = mem.Read<uintptr_t>(bucketAddr + Config::Hashtable::Bucket_Val);
                        
                        if (!IsValidPtr(key) || !IsValidPtr(val)) continue;
                        
                        found++;
                        
                        // val: EntityView* veya entity container
                        uintptr_t entityView = val;
                        
                        // Entity tipi kontrol
                        int eType = GetEntityType(entityView);
                        
                        // Mob & Boss filtresi kapalıysa hiç işlem yapma (CPU/Memory tasarrufu)
                        if (eType == 3 && !RadarSettings::ShowMobs && !RadarSettings::ShowBosses) continue;
                        if (eType != 1 && eType != 3 && eType != 4) continue; // Sadece oyuncular, kendi karakterimiz ve moblar
                        
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
                            std::string guildName = "";
                            std::string allianceName = "";
                            bool isPartyMember = false;
                            bool isWhitelisted = false;
                            float curHealth = 0.0f;
                            float maxHealth = 0.0f;
                            float healthPercent = 100.0f;
                            bool isLowHealth = false;
                            
                            // ── Can Barı & Low HP Tespiti (Oyuncular ve Canavarlar) ──
                            uintptr_t healthBarPtr = mem.Read<uintptr_t>(entityObj + Config::Offsets::Entity_HealthBar);
                            if (IsValidPtr(healthBarPtr)) {
                                curHealth = mem.Read<float>(healthBarPtr + 0x10);
                                maxHealth = mem.Read<float>(healthBarPtr + 0x18);
                                if (maxHealth > 0.0f && curHealth >= 0.0f) {
                                    healthPercent = (curHealth / maxHealth) * 100.0f;
                                    if (healthPercent > 100.0f) healthPercent = 100.0f;
                                    if (healthPercent < 40.0f && curHealth > 0.0f) {
                                        isLowHealth = true;
                                    }
                                }
                                // 3500'den fazla canı olan yaratıkları Boss/Elit olarak işaretle
                                if (eType == 3 && (maxHealth >= 3500.0f || curHealth >= 3500.0f)) {
                                    eType = 5; // Boss tipi
                                }
                            }

                            // Filtre kontrolleri (Boss değilse ve ShowMobs kapalıysa veya Boss ise ve ShowBosses kapalıysa)
                            if (eType == 3 && !RadarSettings::ShowMobs) continue;
                            if (eType == 5 && !RadarSettings::ShowBosses) continue;
                            
                            switch (eType) {
                                case 1: name = "Player"; break;
                                case 2: name = "Resource"; break;
                                case 3: name = "Mob"; break;
                                case 4: name = "Self"; break;
                                case 5: name = "Boss"; break;
                                default: name = "Entity"; break;
                            }
                            
                            // Remote player veya Boss ise isim bilgisini oku
                            if (eType == 1 || eType == 5) {
                                uintptr_t namePtr = mem.Read<uintptr_t>(entityObj + Config::Offsets::Entity_PlayerName);
                                if (IsValidPtr(namePtr)) {
                                    std::string entName = mem.ReadSystemString(namePtr);
                                    if (!entName.empty()) name = entName;
                                }
                            }

                            // Remote player ise ek parti/guild bilgilerini oku
                            if (eType == 1) {

                                // Party GUID Kontrolü
                                AlbionGuid remotePartyGuid = mem.Read<AlbionGuid>(entityObj + Config::Offsets::Entity_PartyGuid);
                                if (!localPartyGuid.IsZero() && !remotePartyGuid.IsZero() && localPartyGuid == remotePartyGuid) {
                                    isPartyMember = true;
                                }

                                // Guild & Alliance İsimleri
                                uintptr_t gPtr = mem.Read<uintptr_t>(entityObj + Config::Offsets::Entity_GuildName);
                                if (IsValidPtr(gPtr)) guildName = mem.ReadSystemString(gPtr);

                                uintptr_t aPtr = mem.Read<uintptr_t>(entityObj + Config::Offsets::Entity_AllianceName);
                                if (IsValidPtr(aPtr)) allianceName = mem.ReadSystemString(aPtr);

                                // Whitelist Kontrolü & Otomatik Parti Whitelist
                                if (isPartyMember) {
                                    isWhitelisted = true;
                                    if (g_autoPartyWhitelist && !name.empty() && name != "Player") {
                                        AddToWhitelist(name, WhitelistSource::PARTY);
                                    }
                                }

                                if (g_autoGuildWhitelist && !localGuildName.empty() && !guildName.empty() && _stricmp(localGuildName.c_str(), guildName.c_str()) == 0) {
                                    isWhitelisted = true;
                                    if (!name.empty() && name != "Player") {
                                        AddToWhitelist(name, WhitelistSource::GUILD);
                                    }
                                }

                                if (g_autoAllianceWhitelist && !localAllianceName.empty() && !allianceName.empty() && _stricmp(localAllianceName.c_str(), allianceName.c_str()) == 0) {
                                    isWhitelisted = true;
                                    if (!name.empty() && name != "Player") {
                                        AddToWhitelist(name, WhitelistSource::ALLIANCE);
                                    }
                                }

                                if (IsWhitelisted(name)) {
                                    isWhitelisted = true;
                                }
                            }
                            
                            // ── Hız, Yön ve Önünü Kesme (Lead Vector) Analizi ──
                            float currentVelocity = 0.0f;
                            bool mounted = false;
                            Vector2D dir = { 0.0f, 0.0f };
                            Vector2D predPos = pos;
                            
                            if (eType == 1) { // Sadece oyuncular için hız ve yön ölçümü
                                static std::unordered_map<std::string, std::pair<Vector2D, std::chrono::steady_clock::time_point>> lastPosMap;
                                static std::unordered_map<std::string, float> smoothSpeeds;
                                static std::unordered_map<std::string, Vector2D> smoothDirs;
                                auto now = std::chrono::steady_clock::now();
                                
                                std::string trackKey = name.empty() ? std::to_string(entityObj) : name;
                                
                                if (lastPosMap.find(trackKey) != lastPosMap.end()) {
                                    auto lastData = lastPosMap[trackKey];
                                    std::chrono::duration<float> elapsed = now - lastData.second;
                                    if (elapsed.count() > 0.15f) { // Her 0.15 saniyede bir ölçüm
                                        float tdx = pos.x - lastData.first.x;
                                        float tdy = pos.y - lastData.first.y;
                                        float distMoved = std::sqrt(tdx*tdx + tdy*tdy);
                                        float rawVelocity = distMoved / elapsed.count();
                                        
                                        // Yumuşatılmış hız (Smooth Speed)
                                        float currentSmooth = smoothSpeeds[trackKey];
                                        currentSmooth = (currentSmooth * 0.6f) + (rawVelocity * 0.4f);
                                        smoothSpeeds[trackKey] = currentSmooth;
                                        currentVelocity = currentSmooth;

                                        // Yön vektörü (Direction Vector)
                                        if (distMoved > 0.2f) {
                                            Vector2D rawDir = { tdx / distMoved, tdy / distMoved };
                                            Vector2D curDir = smoothDirs[trackKey];
                                            curDir.x = (curDir.x * 0.5f) + (rawDir.x * 0.5f);
                                            curDir.y = (curDir.y * 0.5f) + (rawDir.y * 0.5f);
                                            float dirLen = std::sqrt(curDir.x * curDir.x + curDir.y * curDir.y);
                                            if (dirLen > 0.001f) {
                                                curDir.x /= dirLen;
                                                curDir.y /= dirLen;
                                            }
                                            smoothDirs[trackKey] = curDir;
                                            dir = curDir;
                                        } else {
                                            dir = smoothDirs[trackKey];
                                        }
                                        
                                        lastPosMap[trackKey] = {pos, now};
                                    } else {
                                        currentVelocity = smoothSpeeds[trackKey];
                                        dir = smoothDirs[trackKey];
                                    }
                                } else {
                                    lastPosMap[trackKey] = {pos, now};
                                    smoothSpeeds[trackKey] = 0.0f;
                                    smoothDirs[trackKey] = { 0.0f, 0.0f };
                                }
                                
                                // Albion'da normal yürüme ~4.8, binekli hız 7.0 ve üzeridir
                                mounted = (currentVelocity > 6.8f);

                                // 2. Önünü kesme konumu tahmini (Predictive Lead Position)
                                if (currentVelocity > 1.5f && (dir.x != 0.0f || dir.y != 0.0f)) {
                                    predPos.x = pos.x + (dir.x * currentVelocity * RadarSettings::LeadTime);
                                    predPos.y = pos.y + (dir.y * currentVelocity * RadarSettings::LeadTime);
                                }
                            }

                            // ── Eşya Gücü (Item Power - IP) & Silah Analizi ──
                            int estimatedIP = 0;
                            std::string weaponName = "";
                            bool isWhale = false;
                            if (eType == 1) {
                                EquipmentInfo eqInfo = ReadPlayerEquipmentAndIP(entityObj, maxHealth);
                                estimatedIP = eqInfo.averageIP;
                                weaponName = eqInfo.mainWeapon;
                                isWhale = (estimatedIP >= 1400);
                            }
                            
                            entities.push_back({name, guildName, allianceName, pos.x, pos.y, distance, eType, (eType == 4), currentVelocity, dir, predPos, mounted, isWhitelisted, isPartyMember, curHealth, maxHealth, healthPercent, isLowHealth, estimatedIP, weaponName, isWhale});
                        }
                    }
                    
                    if (shouldLog && logFile) {
                        fprintf(logFile, "Total entities in range: %zu\n", entities.size());
                        fflush(logFile);
                    }
                }
            }

        }

        // ── Oyuncu Yakınlık Uyarı Sesi (Sadece Düşmanlar için) ─────────
        {
            static auto lastAlertTime = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            constexpr float ALERT_DISTANCE = 40.0f; // metre
            constexpr int   ALERT_COOLDOWN_MS = 3000; // 3 saniye cooldown
            
            bool shouldAlert = false;
            for (const auto& e : entities) {
                if (e.entityType == 1 && e.distance < ALERT_DISTANCE) {
                    if (e.isWhitelisted) {
                        if (g_partyAlertSound) shouldAlert = true;
                    } else {
                        shouldAlert = true; // Gerçek düşman yaklaştığında alarm ver
                        break;
                    }
                }
            }
            if (shouldAlert) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAlertTime).count();
                if (elapsed >= ALERT_COOLDOWN_MS) {
                    Beep(1200, 150); // Kısa tiz bip
                    Beep(1500, 150); // İkinci bip (dikkat çekici)
                    lastAlertTime = now;
                }
            }
        }

        // ── ImGui & D3D11 Çizim ──────────────────────────────────────────
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        static bool showMenu = false;
        static bool isClickThrough = true;

        if (GetAsyncKeyState(VK_INSERT) & 1) {
            showMenu = !showMenu;
            isClickThrough = !showMenu;
            if (isClickThrough) {
                SetWindowLong(g_hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
            } else {
                SetWindowLong(g_hwnd, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
            }
        }

        if (showMenu) {
            ImGui::SetNextWindowSize(ImVec2(500, 580), ImGuiCond_FirstUseEver);
            ImGui::Begin("Albion Radar & Gank Assist", &showMenu);
            
            if (ImGui::BeginTabBar("RadarTabs")) {
                if (ImGui::BeginTabItem("Görünüm & Şeffaflık")) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.8f, 1.0f), "Radar Boyut ve Şeffaflık Ayarları");
                    ImGui::Separator();
                    
                    static float fRadius = RadarSettings::DefaultRadius;
                    static float fMaxDist = RadarSettings::MaxDistance;
                    if (ImGui::SliderFloat("Radar Boyutu", &fRadius, 100.0f, 500.0f)) {
                        RadarSettings::DefaultRadius = fRadius;
                    }
                    if (ImGui::SliderFloat("Maksimum Menzil (m)", &fMaxDist, 50.0f, 300.0f)) {
                        RadarSettings::MaxDistance = fMaxDist;
                    }
                    
                    ImGui::SliderFloat("Arkaplan Şeffaflığı (Alpha)", &RadarSettings::RadarBgAlpha, 0.0f, 1.0f, "%.2f (0=Tam Saydam)");
                    ImGui::Checkbox("Radar Arka Plan Çemberini Çiz", &RadarSettings::DrawRadarBg);
                    ImGui::Checkbox("Radar Izgara Çizgilerini Çiz", &RadarSettings::DrawRadarGrid);

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.8f, 1.0f), "Varlık (Entity) Filtreleri");
                    ImGui::Separator();
                    ImGui::Checkbox("Canavarları Göster (Mobs)", &RadarSettings::ShowMobs);
                    ImGui::Checkbox("Boss / Elit Yaratıkları Göster", &RadarSettings::ShowBosses);

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.8f, 1.0f), "ESP ve Ses Seçenekleri");
                    ImGui::Separator();
                    ImGui::Checkbox("Dost Oyuncular İçin Yeşil ESP Çizgisi Göster", &g_showFriendlyESP);
                    ImGui::Checkbox("Whitelist'teki Kişiler İçin Sesli Uyarı Çal", &g_partyAlertSound);
                    
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Gank Modülü & IP")) {
                    ImGui::TextColored(ImVec4(0.9f, 0.2f, 1.0f, 1.0f), "Eşya Gücü (Item Power - IP) ve Silah Tespiti");
                    ImGui::Separator();
                    ImGui::Checkbox("Eşya Gücü (IP) ve Silah Bilgisini Göster", &RadarSettings::EnableIPDisplay);
                    ImGui::Checkbox("8.3 / 8.4 Yüksek Değerli Hedefleri (Whale) Mor Parlama ile Vurgula", &RadarSettings::EnableWhaleAlert);

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "1. Can Barı & Düşük Can (Low HP) Tespiti");
                    ImGui::Separator();
                    ImGui::Checkbox("Can Barı ve % HP Değerini Göster", &RadarSettings::EnableHpBars);
                    ImGui::TextDisabled("Canı %%40 altındaki kolay hedefler parlayan altın rengiyle vurgulanır.");

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "2. Önünü Kesme & Rota Tahmin Çizgisi");
                    ImGui::Separator();
                    ImGui::Checkbox("Kaçış Rota Çizgisini & Kestirme Noktasını Çiz", &RadarSettings::EnableLeadLine);
                    ImGui::SliderFloat("Rota Tahmin Süresi (sn)", &RadarSettings::LeadTime, 1.0f, 5.0f, "%.1f saniye ilerisi");

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "3. Kilitli Hedef Gank HUD Paneli");
                    ImGui::Separator();
                    ImGui::Checkbox("Gank Target HUD Kutusunu Göster", &RadarSettings::EnableGankHud);
                    ImGui::TextDisabled("En yakın / en kolay av hedefini kilitler; Silah, IP, Can, Kapanma Hızı ve Yakalama Süresini gösterir.");

                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Whitelist & Party")) {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Otomatik Whitelist Kuralları");
                    ImGui::Separator();
                    
                    ImGui::Checkbox("Partideki Kişileri Otomatik Whitelist'e Ekle", &g_autoPartyWhitelist);
                    ImGui::Checkbox("Aynı Loncadakileri Otomatik Ekle (Guild)", &g_autoGuildWhitelist);
                    ImGui::Checkbox("Aynı İttifaktakileri Otomatik Ekle (Alliance)", &g_autoAllianceWhitelist);
                    
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Manuel Oyuncu Ekle");
                    ImGui::Separator();
                    
                    static char manualNameInput[64] = "";
                    ImGui::InputText("Oyuncu İsmi", manualNameInput, sizeof(manualNameInput));
                    ImGui::SameLine();
                    if (ImGui::Button("Ekle") && strlen(manualNameInput) > 0) {
                        AddToWhitelist(manualNameInput, WhitelistSource::MANUAL);
                        manualNameInput[0] = '\0';
                    }
                    
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Aktif Whitelist (%zu Kişi)", g_whitelist.size());
                    ImGui::SameLine();
                    if (ImGui::Button("Tümünü Temizle")) {
                        ClearWhitelist();
                    }
                    ImGui::Separator();
                    
                    ImGui::BeginChild("WhitelistListChild", ImVec2(0, 130), true);
                    if (g_whitelist.empty()) {
                        ImGui::TextDisabled("Whitelist boş. Partiye katıldığınızda veya manuel eklediğinizde burada görünür.");
                    } else {
                        std::vector<std::string> toRemove;
                        for (const auto& item : g_whitelist) {
                            const std::string& key = item.first;
                            const WhitelistEntry& entry = item.second;
                            ImGui::PushID(key.c_str());
                            
                            const char* tag = "[M]";
                            if (entry.source == WhitelistSource::PARTY) tag = "[PARTİ]";
                            else if (entry.source == WhitelistSource::GUILD) tag = "[GUILD]";
                            else if (entry.source == WhitelistSource::ALLIANCE) tag = "[ALLY]";
                            
                            ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.4f, 1.0f), "%s %s", tag, entry.name.c_str());
                            ImGui::SameLine(ImGui::GetWindowWidth() - 75);
                            if (ImGui::SmallButton("Sil")) {
                                toRemove.push_back(key);
                            }
                            ImGui::PopID();
                        }
                        for (const auto& k : toRemove) {
                            g_whitelist.erase(k);
                        }
                    }
                    ImGui::EndChild();
                    
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Çevredeki Oyuncular (Hızlı Ekle/Çıkar)");
                    ImGui::Separator();
                    
                    ImGui::BeginChild("NearbyPlayersChild", ImVec2(0, 120), true);
                    bool foundAnyNearby = false;
                    for (const auto& entity : entities) {
                        if (entity.entityType == 1) {
                            foundAnyNearby = true;
                            ImGui::PushID(entity.name.c_str());
                            
                            if (entity.isWhitelisted) {
                                ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.4f, 1.0f), "[DOST] %s (%.0fm)", entity.name.c_str(), entity.distance);
                                ImGui::SameLine(ImGui::GetWindowWidth() - 75);
                                if (ImGui::SmallButton("Çıkar")) {
                                    RemoveFromWhitelist(entity.name);
                                }
                            } else {
                                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[DÜŞMAN] %s (%.0fm)", entity.name.c_str(), entity.distance);
                                ImGui::SameLine(ImGui::GetWindowWidth() - 75);
                                if (ImGui::SmallButton("+ Ekle")) {
                                    AddToWhitelist(entity.name, WhitelistSource::MANUAL);
                                }
                            }
                            ImGui::PopID();
                        }
                    }
                    if (!foundAnyNearby) {
                        ImGui::TextDisabled("Şu anda çevrede oyuncu bulunamadı.");
                    }
                    ImGui::EndChild();
                    
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            
            ImGui::End();
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImVec2 center(RadarSettings::RadarPosX + RadarSettings::DefaultRadius, RadarSettings::RadarPosY + RadarSettings::DefaultRadius);
        
        // ── Radar Arka Planı & Transparanlık ─────────────────────────────
        if (RadarSettings::DrawRadarBg && RadarSettings::RadarBgAlpha > 0.001f) {
            int bgAlpha = static_cast<int>(RadarSettings::RadarBgAlpha * 255.0f);
            drawList->AddCircleFilled(center, RadarSettings::DefaultRadius, IM_COL32(15, 18, 25, bgAlpha), 64);
            drawList->AddCircle(center, RadarSettings::DefaultRadius, IM_COL32(0, 255, 180, (int)(RadarSettings::RadarBgAlpha * 200.0f + 55.0f)), 64, 2.0f);
        } else {
            // Tam transparan modda sadece ince çember sınırı çiz
            drawList->AddCircle(center, RadarSettings::DefaultRadius, IM_COL32(0, 255, 180, 110), 64, 1.5f);
        }
        
        // Grid ve İç Çember
        if (RadarSettings::DrawRadarGrid && RadarSettings::RadarBgAlpha > 0.001f) {
            int gridAlpha = static_cast<int>(RadarSettings::RadarBgAlpha * 180.0f);
            drawList->AddLine(ImVec2(center.x - RadarSettings::DefaultRadius, center.y), ImVec2(center.x + RadarSettings::DefaultRadius, center.y), IM_COL32(40, 60, 80, gridAlpha), 1.0f);
            drawList->AddLine(ImVec2(center.x, center.y - RadarSettings::DefaultRadius), ImVec2(center.x, center.y + RadarSettings::DefaultRadius), IM_COL32(40, 60, 80, gridAlpha), 1.0f);
            drawList->AddCircle(center, RadarSettings::DefaultRadius / 2, IM_COL32(40, 60, 80, gridAlpha), 64, 1.0f);
        }

        // Kendi merkezimiz
        drawList->AddCircleFilled(center, 4.0f, IM_COL32(0, 220, 255, 255));

        // Zaman tabanlı pulsing animasyonu
        static auto animStartTime = std::chrono::steady_clock::now();
        auto curTime = std::chrono::steady_clock::now();
        float animElapsed = std::chrono::duration<float>(curTime - animStartTime).count();
        float pulse01 = (std::sin(animElapsed * 4.0f) + 1.0f) / 2.0f; // 0..1 arası

        // Entity'leri çiz
        for (const auto& entity : entities) {
            if (entity.entityType == 3 && !RadarSettings::ShowMobs) continue;
            if (entity.entityType == 5 && !RadarSettings::ShowBosses) continue;
            if (entity.entityType != 1 && entity.entityType != 3 && entity.entityType != 5) continue;

            POINT dotPosGDI = WorldToCircularRadarGDI(myPosX, myPosY, entity.posX, entity.posY, { static_cast<LONG>(center.x), static_cast<LONG>(center.y) }, RadarSettings::DefaultRadius, RadarSettings::MaxDistance);
            ImVec2 dotPos(static_cast<float>(dotPosGDI.x), static_cast<float>(dotPosGDI.y));

            ImU32 color;
            float dotSize;
            switch (entity.entityType) {
                case 1: {
                    if (entity.isWhitelisted) {
                        // Whitelist / Parti Üyesi -> Yeşil / Spring Green
                        color = entity.isPartyMember ? IM_COL32(0, 255, 127, 255) : IM_COL32(50, 220, 90, 255);
                        dotSize = 5.5f;

                        if (g_showFriendlyESP) {
                            float traceDx = dotPos.x - center.x;
                            float traceDy = dotPos.y - center.y;
                            float traceDist = std::sqrt(traceDx * traceDx + traceDy * traceDy);
                            ImVec2 clippedEnd = dotPos;
                            if (traceDist > RadarSettings::DefaultRadius - 2.0f) {
                                float scale = (RadarSettings::DefaultRadius - 2.0f) / traceDist;
                                clippedEnd.x = center.x + traceDx * scale;
                                clippedEnd.y = center.y + traceDy * scale;
                            }
                            drawList->AddLine(center, clippedEnd, IM_COL32(0, 255, 127, 120), 1.2f);
                        }
                    } else {
                        // Düşman Oyuncu
                        if (RadarSettings::EnableWhaleAlert && entity.isWhale) {
                            // 8.3 / 8.4 Yüksek Değerli Hedef (Whale) -> Parlayan Neon Mor / Pembe
                            color = IM_COL32(255, 0, 220, 255);
                            dotSize = 7.5f;
                            drawList->AddCircle(dotPos, dotSize + 4.0f + (pulse01 * 3.0f), IM_COL32(255, 0, 220, 240), 16, 2.0f);
                            drawList->AddCircle(dotPos, dotSize + 8.0f + (pulse01 * 2.0f), IM_COL32(255, 80, 240, 120), 16, 1.0f);
                        } else if (entity.isLowHealth) {
                            // 1. Düşük Canlı Kolay Av -> Parlayan Altın-Kırmızı
                            color = IM_COL32(255, 160 + static_cast<int>(pulse01 * 80), 0, 255);
                            dotSize = 7.0f;
                            drawList->AddCircle(dotPos, dotSize + 3.0f + (pulse01 * 2.0f), IM_COL32(255, 200, 0, 220), 12, 1.5f);
                        } else if (entity.estimatedIP >= 1150) {
                            // Standart T7/T8 PvP -> Kırmızı
                            color = IM_COL32(255, 60, 60, 255); 
                            dotSize = 6.0f;
                        } else {
                            // Düşük IP / 4.1 Hafif Set -> Turuncu/Sarı
                            color = IM_COL32(255, 140, 50, 255);
                            dotSize = 5.5f;
                        }

                        // Takip çizgisi — radar dairesi içinde kliplenmiş
                        float traceDx = dotPos.x - center.x;
                        float traceDy = dotPos.y - center.y;
                        float traceDist = std::sqrt(traceDx * traceDx + traceDy * traceDy);
                        ImVec2 clippedEnd = dotPos;
                        if (traceDist > RadarSettings::DefaultRadius - 2.0f) {
                            float scale = (RadarSettings::DefaultRadius - 2.0f) / traceDist;
                            clippedEnd.x = center.x + traceDx * scale;
                            clippedEnd.y = center.y + traceDy * scale;
                        }
                        ImU32 lineCol = entity.isWhale ? IM_COL32(255, 0, 220, 180) : (entity.isLowHealth ? IM_COL32(255, 180, 0, 180) : IM_COL32(255, 60, 60, 150));
                        drawList->AddLine(center, clippedEnd, lineCol, entity.isWhale ? 2.0f : 1.5f);

                        // 2. Önünü Kesme / Rota Tahmin Çizgisi (Predictive Lead Line)
                        if (RadarSettings::EnableLeadLine && entity.velocity > 1.8f && (entity.predPos.x != entity.posX || entity.predPos.y != entity.posY)) {
                            POINT predDotGDI = WorldToCircularRadarGDI(myPosX, myPosY, entity.predPos.x, entity.predPos.y, { static_cast<LONG>(center.x), static_cast<LONG>(center.y) }, RadarSettings::DefaultRadius, RadarSettings::MaxDistance);
                            ImVec2 predDot(static_cast<float>(predDotGDI.x), static_cast<float>(predDotGDI.y));

                            // Rota Tahmin Çizgisi (Sarı/Altın)
                            drawList->AddLine(dotPos, predDot, IM_COL32(255, 220, 0, 180), 1.5f);

                            // Kestirme Noktası (Elmas Deseni)
                            float dSize = 3.5f;
                            ImVec2 diamond[4] = {
                                ImVec2(predDot.x, predDot.y - dSize),
                                ImVec2(predDot.x + dSize, predDot.y),
                                ImVec2(predDot.x, predDot.y + dSize),
                                ImVec2(predDot.x - dSize, predDot.y)
                            };
                            drawList->AddPolyline(diamond, 4, IM_COL32(255, 220, 0, 255), ImDrawFlags_Closed, 1.5f);
                        }
                    }
                    break;
                }
                case 3: 
                    color = IM_COL32(255, 165, 0, 255); 
                    dotSize = 4.0f;
                    break;    // Mob = turuncu
                case 5: 
                    color = IM_COL32(255, 0, 255, 255); // Boss = Mor
                    dotSize = 8.0f; 
                    // Boss için kafatası tarzı kalın çerçeve
                    drawList->AddCircle(dotPos, dotSize + 2.0f, IM_COL32(255, 0, 255, 255), 12, 2.0f);
                    break;
                default: 
                    color = IM_COL32(200, 200, 200, 255); 
                    dotSize = 4.0f;
                    break;
            }

            // Binekli (Mounted) oyuncular için üçgen, yayalar için daire çizimi
            if (entity.entityType == 1 && entity.isMounted) {
                ImVec2 p1(dotPos.x, dotPos.y - dotSize - 2.0f);
                ImVec2 p2(dotPos.x - dotSize - 2.0f, dotPos.y + dotSize + 2.0f);
                ImVec2 p3(dotPos.x + dotSize + 2.0f, dotPos.y + dotSize + 2.0f);
                drawList->AddTriangleFilled(p1, p2, p3, color);
                drawList->AddTriangle(p1, p2, p3, IM_COL32(0, 0, 0, 255), 1.0f);
            } else {
                drawList->AddCircleFilled(dotPos, dotSize, color);
                drawList->AddCircle(dotPos, dotSize, IM_COL32(0, 0, 0, 255), 12, 1.0f);
            }

            // 1. Radarda Mini Can Barı (% HP) - Oyuncular ve Bosslar için
            if (RadarSettings::EnableHpBars && (entity.entityType == 1 || entity.entityType == 5) && entity.maxHealth > 0.0f) {
                float hpBarW = (entity.entityType == 5) ? 28.0f : 22.0f;
                float hpBarH = 3.0f;
                float hpBarX = dotPos.x - (hpBarW / 2.0f);
                float hpBarY = dotPos.y + dotSize + 3.0f;
                float hpRatio = entity.healthPercent / 100.0f;
                if (hpRatio < 0.0f) hpRatio = 0.0f;
                if (hpRatio > 1.0f) hpRatio = 1.0f;

                drawList->AddRectFilled(ImVec2(hpBarX, hpBarY), ImVec2(hpBarX + hpBarW, hpBarY + hpBarH), IM_COL32(10, 10, 10, 200));
                
                ImU32 hpBarCol = (entity.entityType == 5) ? IM_COL32(220, 50, 255, 255) : IM_COL32(50, 220, 80, 255);
                if (entity.healthPercent <= 35.0f) hpBarCol = IM_COL32(255, 40, 40, 255);
                else if (entity.healthPercent <= 60.0f && entity.entityType != 5) hpBarCol = IM_COL32(255, 200, 0, 255);

                drawList->AddRectFilled(ImVec2(hpBarX, hpBarY), ImVec2(hpBarX + (hpBarW * hpRatio), hpBarY + hpBarH), hpBarCol);
            }

            // İsim, IP, Silah ve mesafe etiketi
            char label[160];
            if (entity.entityType == 5) {
                if (entity.maxHealth > 0.0f) {
                    snprintf(label, sizeof(label), "[BOSS] %s (%.0f HP | %dm)", entity.name.c_str(), entity.curHealth > 0 ? entity.curHealth : entity.maxHealth, (int)entity.distance);
                } else {
                    snprintf(label, sizeof(label), "[BOSS] %s (%dm)", entity.name.c_str(), (int)entity.distance);
                }
            } else if (entity.entityType == 1) {
                const char* prefix = "";
                if (entity.isPartyMember) prefix = "[PARTİ] ";
                else if (entity.isWhitelisted) prefix = "[DOST] ";
                else if (entity.isWhale) prefix = "[8.3 BALİNA] ";
                else if (entity.isLowHealth) prefix = "[KOLAY AV] ";
                else if (!entity.isMounted) prefix = "[BİNEKSİZ] ";
                else if (entity.isMounted) prefix = "[MOUNTED] ";

                if (RadarSettings::EnableIPDisplay && entity.estimatedIP > 0) {
                    if (!entity.weaponName.empty()) {
                        snprintf(label, sizeof(label), "%s%s [%d IP | %s] (%dm)", prefix, entity.name.c_str(), entity.estimatedIP, entity.weaponName.c_str(), (int)entity.distance);
                    } else {
                        snprintf(label, sizeof(label), "%s%s [%d IP] (%dm)", prefix, entity.name.c_str(), entity.estimatedIP, (int)entity.distance);
                    }
                } else if (RadarSettings::EnableHpBars && entity.maxHealth > 0.0f) {
                    snprintf(label, sizeof(label), "%s%s (%.0f%%, %dm)", prefix, entity.name.c_str(), entity.healthPercent, (int)entity.distance);
                } else {
                    snprintf(label, sizeof(label), "%s%s (%dm)", prefix, entity.name.c_str(), (int)entity.distance);
                }
            } else {
                snprintf(label, sizeof(label), "%s (%dm)", entity.name.c_str(), (int)entity.distance);
            }
            drawList->AddText(ImVec2(dotPos.x + dotSize + 3, dotPos.y - 7), color, label);
        }

        // ── 3. Kilitli Hedef Gank HUD Paneli (Gank Target Box) ───────────
        if (RadarSettings::EnableGankHud) {
            const RadarEntity* lockedEnemy = nullptr;
            float bestTargetScore = 99999.0f;
            auto nowTime = std::chrono::steady_clock::now();

            for (const auto& entity : entities) {
                if (entity.entityType == 1 && !entity.isWhitelisted) {
                    // Skorlama: 8.3 hedeflere, düşük cana ve yakın mesafeye öncelik ver
                    float score = entity.distance;
                    if (entity.isWhale) score *= 0.35f;     // Yüksek değerli hedef en yüksek öncelik
                    if (entity.isLowHealth) score *= 0.45f; // Düşük can öncelik
                    if (!entity.isMounted) score *= 0.75f;  // Bineksiz öncelik
                    if (score < bestTargetScore) {
                        bestTargetScore = score;
                        lockedEnemy = &entity;
                    }
                }
            }

            if (lockedEnemy) {
                float screenW = (float)GetSystemMetrics(SM_CXSCREEN);
                ImVec2 hudPos(screenW - 330.0f, 45.0f);
                ImVec2 hudSize(305.0f, 185.0f);

                // Kapanma hızı (Closing Speed) hesaplama
                std::string tKey = lockedEnemy->name.empty() ? std::to_string(reinterpret_cast<uintptr_t>(lockedEnemy)) : lockedEnemy->name;
                auto& tracker = g_closingTrackers[tKey];
                if (tracker.lastDistance > 0.0f) {
                    std::chrono::duration<float> dt = nowTime - tracker.lastTime;
                    if (dt.count() > 0.25f) {
                        float distChange = tracker.lastDistance - lockedEnemy->distance; // pozitif = yaklaşıyor
                        float rawClosing = distChange / dt.count();
                        tracker.closingSpeed = (tracker.closingSpeed * 0.7f) + (rawClosing * 0.3f);
                        tracker.lastDistance = lockedEnemy->distance;
                        tracker.lastTime = nowTime;
                    }
                } else {
                    tracker.lastDistance = lockedEnemy->distance;
                    tracker.lastTime = nowTime;
                    tracker.closingSpeed = 0.0f;
                }

                // HUD Arka Planı (Şeffaf glassmorphism kutu)
                int hudAlpha = static_cast<int>(RadarSettings::RadarBgAlpha * 220.0f + 30.0f);
                drawList->AddRectFilled(hudPos, ImVec2(hudPos.x + hudSize.x, hudPos.y + hudSize.y), IM_COL32(10, 14, 20, hudAlpha), 8.0f);
                
                ImU32 borderCol = lockedEnemy->isWhale ? IM_COL32(255, 0, 220, 255) : (lockedEnemy->isLowHealth ? IM_COL32(255, 180, 0, 240) : IM_COL32(255, 50, 50, 200));
                drawList->AddRect(hudPos, ImVec2(hudPos.x + hudSize.x, hudPos.y + hudSize.y), borderCol, 8.0f, 0, 1.8f);

                // Başlık & İsim
                const char* headerStatus = "HEDEF";
                if (lockedEnemy->isWhale) headerStatus = "8.3 BALİNA (WHALE)";
                else if (lockedEnemy->isLowHealth) headerStatus = "KOLAY AV (LOW HP)";
                else if (!lockedEnemy->isMounted) headerStatus = "BİNEKSİZ AV";

                char headerText[64];
                snprintf(headerText, sizeof(headerText), "TARGET FOCUS [ %s ]", headerStatus);
                drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + 8), borderCol, headerText);

                char nameLine[128];
                if (!lockedEnemy->guildName.empty()) {
                    snprintf(nameLine, sizeof(nameLine), "%s [%s]", lockedEnemy->name.c_str(), lockedEnemy->guildName.c_str());
                } else {
                    snprintf(nameLine, sizeof(nameLine), "%s", lockedEnemy->name.c_str());
                }
                drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + 25), IM_COL32(255, 255, 255, 255), nameLine);

                // Silah ve IP Gösterimi
                if (RadarSettings::EnableIPDisplay && lockedEnemy->estimatedIP > 0) {
                    char wepLine[128];
                    snprintf(wepLine, sizeof(wepLine), "Silah: %s", lockedEnemy->weaponName.c_str());
                    drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + 42), IM_COL32(0, 230, 255, 255), wepLine);

                    char ipLine[128];
                    ImU32 ipBadgeCol = lockedEnemy->isWhale ? IM_COL32(255, 0, 220, 255) : (lockedEnemy->estimatedIP >= 1150 ? IM_COL32(255, 120, 50, 255) : IM_COL32(50, 255, 120, 255));
                    snprintf(ipLine, sizeof(ipLine), "Eşya Gücü: ~%d IP [%s]", lockedEnemy->estimatedIP, lockedEnemy->isWhale ? "8.3+ WHALE" : (lockedEnemy->estimatedIP >= 1150 ? "T7/T8 PvP" : "4.1 KOLAY"));
                    drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + 58), ipBadgeCol, ipLine);
                }

                // Binek Durumu
                float curY = (RadarSettings::EnableIPDisplay && lockedEnemy->estimatedIP > 0) ? 75.0f : 45.0f;
                if (!lockedEnemy->isMounted) {
                    drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + curY), IM_COL32(50, 255, 120, 255), "[ BİNEKSİZ / YAYA ]");
                } else {
                    char mountText[64];
                    snprintf(mountText, sizeof(mountText), "[ BİNEKLİ - %.1f m/s ]", lockedEnemy->velocity);
                    drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + curY), IM_COL32(255, 200, 50, 255), mountText);
                }

                // 1. Can Barı
                float barX = hudPos.x + 12;
                float barY = hudPos.y + curY + 18.0f;
                float barW = hudSize.x - 24;
                float barH = 15.0f;
                float hpRatio = lockedEnemy->healthPercent / 100.0f;
                if (hpRatio < 0.0f) hpRatio = 0.0f;
                if (hpRatio > 1.0f) hpRatio = 1.0f;

                drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(30, 30, 30, 220), 4.0f);
                
                ImU32 hpBarCol;
                if (lockedEnemy->healthPercent > 60.0f) hpBarCol = IM_COL32(50, 220, 80, 255);
                else if (lockedEnemy->healthPercent > 35.0f) hpBarCol = IM_COL32(255, 200, 0, 255);
                else hpBarCol = IM_COL32(255, 40, 40, 255);

                if (hpRatio > 0.0f) {
                    drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + (barW * hpRatio), barY + barH), hpBarCol, 4.0f);
                }
                drawList->AddRect(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(100, 100, 100, 180), 4.0f);

                char hpStr[64];
                if (lockedEnemy->maxHealth > 0.0f) {
                    snprintf(hpStr, sizeof(hpStr), "%.0f / %.0f HP (%.0f%%)", lockedEnemy->curHealth, lockedEnemy->maxHealth, lockedEnemy->healthPercent);
                } else {
                    snprintf(hpStr, sizeof(hpStr), "Can: %.0f%%", lockedEnemy->healthPercent);
                }
                drawList->AddText(ImVec2(barX + 8, barY), IM_COL32(255, 255, 255, 255), hpStr);

                // Mesafe & Kapanma Hızı
                float statY = barY + 20.0f;
                char distLine[64];
                snprintf(distLine, sizeof(distLine), "Mesafe: %.1fm", lockedEnemy->distance);
                drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + statY), IM_COL32(220, 220, 220, 255), distLine);

                char speedLine[128];
                ImU32 speedCol;
                if (tracker.closingSpeed > 0.3f) {
                    snprintf(speedLine, sizeof(speedLine), "Kapanma: +%.1fm/s (YAKLAŞIYORSUN)", tracker.closingSpeed);
                    speedCol = IM_COL32(50, 255, 120, 255);
                } else if (tracker.closingSpeed < -0.3f) {
                    snprintf(speedLine, sizeof(speedLine), "Kapanma: %.1fm/s (UZAKLAŞIYOR)", tracker.closingSpeed);
                    speedCol = IM_COL32(255, 80, 80, 255);
                } else {
                    snprintf(speedLine, sizeof(speedLine), "Kapanma: 0.0m/s (SABİT)");
                    speedCol = IM_COL32(200, 200, 200, 255);
                }
                drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + statY + 16.0f), speedCol, speedLine);

                // Tahmini Yakalama Süresi
                if (tracker.closingSpeed > 0.5f) {
                    float timeToCatch = lockedEnemy->distance / tracker.closingSpeed;
                    char catchStr[64];
                    snprintf(catchStr, sizeof(catchStr), "Tahmini Yakalama: ~%.1f sn", timeToCatch);
                    drawList->AddText(ImVec2(hudPos.x + 12, hudPos.y + statY + 32.0f), IM_COL32(0, 255, 200, 255), catchStr);
                }
            }
        }

        // ── Ekran Ortasından ESP Çizgileri (Oyuncuların yönünde) ─────────
        {
            float screenW = (float)GetSystemMetrics(SM_CXSCREEN);
            float screenH = (float)GetSystemMetrics(SM_CYSCREEN);
            ImVec2 screenCenter(screenW / 2.0f, screenH / 2.0f);
            
            for (const auto& entity : entities) {
                if (entity.entityType != 1) continue; // Sadece oyuncular
                
                // Whitelisted / Parti üyesi ise ve friendly ESP kapalıysa çizgi çizme
                if (entity.isWhitelisted && !g_showFriendlyESP) continue;

                float dx = entity.posX - myPosX;
                float dy = entity.posY - myPosY;
                float angle = std::atan2(dy, dx);
                angle -= 0.785398f; // İzometrik kamera düzeltmesi (-45°)
                
                // Mesafeye göre çizgi uzunluğu (yakın = uzun, uzak = kısa)
                float maxLineLen = screenH * 0.4f;
                float normDist = entity.distance / RadarSettings::MaxDistance;
                if (normDist > 1.0f) normDist = 1.0f;
                float lineLen = maxLineLen * (1.0f - normDist * 0.5f);
                
                ImVec2 lineEnd(
                    screenCenter.x + lineLen * std::cos(angle),
                    screenCenter.y - lineLen * std::sin(angle)
                );
                
                ImU32 rayColor;
                ImU32 arrowColor;
                ImU32 textColor;

                if (entity.isWhitelisted) {
                    rayColor = IM_COL32(50, 255, 120, 190);
                    arrowColor = IM_COL32(50, 255, 120, 220);
                    textColor = IM_COL32(80, 255, 140, 255);
                } else if (entity.isWhale) {
                    rayColor = IM_COL32(255, 0, 220, 240);
                    arrowColor = IM_COL32(255, 0, 220, 255);
                    textColor = IM_COL32(255, 100, 240, 255);
                } else if (entity.isLowHealth) {
                    rayColor = IM_COL32(255, 180, 0, 230);
                    arrowColor = IM_COL32(255, 200, 0, 255);
                    textColor = IM_COL32(255, 210, 40, 255);
                } else {
                    rayColor = IM_COL32(255, 50, 50, 200);
                    arrowColor = IM_COL32(255, 50, 50, 220);
                    textColor = IM_COL32(255, 80, 80, 255);
                }

                // Çizgi
                drawList->AddLine(screenCenter, lineEnd, rayColor, entity.isWhitelisted ? 1.8f : (entity.isWhale ? 3.5f : (entity.isLowHealth ? 3.0f : 2.5f)));
                
                // Çizgi ucuna ok başı (yön belirteci)
                float arrowAngle = std::atan2(lineEnd.y - screenCenter.y, lineEnd.x - screenCenter.x);
                float arrowSize = entity.isWhale ? 14.0f : (entity.isLowHealth ? 12.0f : 10.0f);
                ImVec2 arrow1(
                    lineEnd.x - arrowSize * std::cos(arrowAngle - 0.4f),
                    lineEnd.y - arrowSize * std::sin(arrowAngle - 0.4f)
                );
                ImVec2 arrow2(
                    lineEnd.x - arrowSize * std::cos(arrowAngle + 0.4f),
                    lineEnd.y - arrowSize * std::sin(arrowAngle + 0.4f)
                );
                drawList->AddTriangleFilled(lineEnd, arrow1, arrow2, arrowColor);
                
                // Uç noktaya isim + IP + mesafe etiketi
                char espLabel[140];
                const char* prefix = "";
                if (entity.isPartyMember) prefix = "[PARTİ] ";
                else if (entity.isWhitelisted) prefix = "[DOST] ";
                else if (entity.isWhale) prefix = "[8.3 BALİNA] ";
                else if (entity.isLowHealth) prefix = "[KOLAY AV] ";
                else if (!entity.isMounted) prefix = "[BİNEKSİZ] ";
                else if (entity.isMounted) prefix = "[MOUNTED] ";

                if (RadarSettings::EnableIPDisplay && entity.estimatedIP > 0) {
                    snprintf(espLabel, sizeof(espLabel), "%s%s [%d IP] (%dm)", prefix, entity.name.c_str(), entity.estimatedIP, (int)entity.distance);
                } else if (RadarSettings::EnableHpBars && entity.maxHealth > 0.0f) {
                    snprintf(espLabel, sizeof(espLabel), "%s%s (%.0f%% HP, %dm)", prefix, entity.name.c_str(), entity.healthPercent, (int)entity.distance);
                } else {
                    snprintf(espLabel, sizeof(espLabel), "%s%s (%dm)", prefix, entity.name.c_str(), (int)entity.distance);
                }
                drawList->AddText(ImVec2(lineEnd.x + 12, lineEnd.y - 7), textColor, espLabel);
            }
        }

        // ── Boss Yön Çizgisi (Sadece Boss varsa ekranda gösterilir) ──
        {
            float screenW = (float)GetSystemMetrics(SM_CXSCREEN);
            float screenH = (float)GetSystemMetrics(SM_CYSCREEN);
            ImVec2 screenCenter(screenW / 2.0f, screenH / 2.0f);
            
            // En yakın boss'u bul
            const RadarEntity* closestBoss = nullptr;
            float minBossDist = 99999.0f;
            
            for (const auto& entity : entities) {
                if (entity.entityType == 5 && entity.distance < minBossDist) {
                    minBossDist = entity.distance;
                    closestBoss = &entity;
                }
            }
            
            if (closestBoss) {
                // Pulsing efekti için zaman bazlı alpha
                static auto startTime = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                float elapsed = std::chrono::duration<float>(now - startTime).count();
                float pulse = (std::sin(elapsed * 3.0f) + 1.0f) / 2.0f; // 0..1 arası salınım
                int pulseAlpha = 120 + (int)(pulse * 135.0f); // 120..255
                
                ImU32 lineColor = IM_COL32(255, 200, 0, (int)(pulseAlpha * 0.8f));   // Altın çizgi
                ImU32 arrowColor = IM_COL32(255, 215, 0, pulseAlpha);                 // Altın ok
                ImU32 glowColor = IM_COL32(255, 200, 0, (int)(pulse * 100.0f));      // Altın glow
                
                float dx = closestBoss->posX - myPosX;
                float dy = closestBoss->posY - myPosY;
                float angle = std::atan2(dy, dx);
                angle -= 0.785398f; // İzometrik -45°
                
                float maxLineLen = screenH * 0.35f;
                float normDist = closestBoss->distance / RadarSettings::MaxDistance;
                if (normDist > 1.0f) normDist = 1.0f;
                float lineLen = maxLineLen * (1.0f - normDist * 0.3f);
                
                ImVec2 lineEnd(
                    screenCenter.x + lineLen * std::cos(angle),
                    screenCenter.y - lineLen * std::sin(angle)
                );
                
                // Kesikli görünüm efekti — ana çizgi
                drawList->AddLine(screenCenter, lineEnd, lineColor, 2.0f);
                
                // Glow halkası (pulsing)
                drawList->AddCircle(lineEnd, 14.0f, glowColor, 16, 2.0f);
                drawList->AddCircle(lineEnd, 18.0f, glowColor, 16, 1.0f);
                
                // Ok başı
                float arrowAngle = std::atan2(lineEnd.y - screenCenter.y, lineEnd.x - screenCenter.x);
                float arrowSize = 12.0f;
                ImVec2 a1(lineEnd.x - arrowSize * std::cos(arrowAngle - 0.35f),
                          lineEnd.y - arrowSize * std::sin(arrowAngle - 0.35f));
                ImVec2 a2(lineEnd.x - arrowSize * std::cos(arrowAngle + 0.35f),
                          lineEnd.y - arrowSize * std::sin(arrowAngle + 0.35f));
                drawList->AddTriangleFilled(lineEnd, a1, a2, arrowColor);
                
                // Etiket
                char farmLabel[128];
                snprintf(farmLabel, sizeof(farmLabel), "[BOSS] %s (%dm)", closestBoss->name.c_str(), (int)closestBoss->distance);
                drawList->AddText(ImVec2(lineEnd.x + 20, lineEnd.y - 8), arrowColor, farmLabel);
            }
        }

        // Entity sayısı ve Whitelist bilgisi
        char infoText[128];
        snprintf(infoText, sizeof(infoText), "Entities: %zu | Whitelist: %zu", entities.size(), g_whitelist.size());
        drawList->AddText(ImVec2(RadarSettings::RadarPosX, RadarSettings::RadarPosY + (RadarSettings::DefaultRadius * 2) + 10), IM_COL32(0, 255, 180, 255), infoText);
        
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // Tamamen saydam D3D arka planı
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // VSync açık
    }

cleanup:
    if (logFile) fclose(logFile);
    mem.Close();
    return 0;
}
