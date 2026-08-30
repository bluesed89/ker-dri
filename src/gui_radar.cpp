/*
 * gui_radar.cpp — Standalone Circular Radar UI with ImGui & DirectX 11
 *
 * Sol üst köşede duran, yuvarlak (circular) radar arayüzü.
 * Oyuncuların ismi, mesafesi ve yönü radarda renkli noktalar ve metinlerle gösterilir.
 */

#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <vector>
#include <string>
#include <cmath>

// ── ImGui Headers (Assuming standard setup) ─────────────────────────
// Note: In production build, link with imgui, imgui_draw, imgui_widgets, imgui_impl_dx11, imgui_impl_win32
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ── Radar Veri Yapıları & Offsetler ──────────────────────────────────
namespace RadarSettings {
    constexpr float DefaultRadius  = 120.0f; // Radar yarıçapı (px)
    constexpr float MaxDistance    = 100.0f; // Metre cinsinden radar menzili
    constexpr ImVec2 RadarPos      = ImVec2(20.0f, 20.0f); // Sol üst köşe pozisyonu
}

struct RadarEntity {
    std::string name;
    float posX;
    float posY;
    float distance;
    bool isEnemy;
    bool isLocalPlayer;
};

// ── Dönüştürücü Matematik (World Position to 2D Circular Radar) ─────
static ImVec2 WorldToCircularRadar(float localX, float localY, float targetX, float targetY, ImVec2 center, float radius, float maxDist) {
    float dx = targetX - localX;
    float dy = targetY - localY;

    float dist = std::sqrt(dx * dx + dy * dy);
    
    // Normalleştirilmiş mesafe (0.0 ile 1.0 arası)
    float normDist = dist / maxDist;
    if (normDist > 1.0f) normDist = 1.0f; // Radar sınırında sabitle

    // Açı hesabı
    float angle = std::atan2(dy, dx);

    // Radar üzerindeki piksel konumu
    float radarX = center.x + (normDist * radius * std::cos(angle));
    float radarY = center.y + (normDist * radius * std::sin(angle));

    return ImVec2(radarX, radarY);
}

// ── ImGui Yuvarlak Radar Çizim Fonksiyonu ────────────────────────────
void RenderCircularRadar(const std::vector<RadarEntity>& entities, float localX, float localY) {
    ImGui::SetNextWindowPos(RadarSettings::RadarPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(RadarSettings::DefaultRadius * 2 + 40, RadarSettings::DefaultRadius * 2 + 60), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;

    ImGui::Begin("CircularRadarOverlay", nullptr, flags);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 center = ImVec2(p.x + RadarSettings::DefaultRadius + 10, p.y + RadarSettings::DefaultRadius + 10);
    float radius = RadarSettings::DefaultRadius;

    // 1. Radar Arka Planı (Yuvarlak & Koyu Şeffaf)
    drawList->AddCircleFilled(center, radius, IM_COL32(15, 15, 20, 200), 64);
    drawList->AddCircle(center, radius, IM_COL32(0, 255, 200, 255), 64, 2.0f); // Dış yeşil halka
    drawList->AddCircle(center, radius * 0.5f, IM_COL32(255, 255, 255, 40), 32, 1.0f); // İç halka

    // 2. Artı Göstergesi (Crosshair Grid)
    drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), IM_COL32(255, 255, 255, 40), 1.0f);
    drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), IM_COL32(255, 255, 255, 40), 1.0f);

    // 3. Merkez Noktası (Bizim Oyuncumuz)
    drawList->AddCircleFilled(center, 4.0f, IM_COL32(0, 255, 255, 255)); // Beyaz/Mavi merkez nokta

    // 4. Diğer Oyuncuları / Nesneleri Çiz
    for (const auto& entity : entities) {
        if (entity.isLocalPlayer) continue;

        ImVec2 dotPos = WorldToCircularRadar(localX, localY, entity.posX, entity.posY, center, radius, RadarSettings::MaxDistance);

        // Renk seçimi (Düşman: Kırmızı, Dost: Yeşil)
        ImU32 dotColor = entity.isEnemy ? IM_COL32(255, 50, 50, 255) : IM_COL32(50, 255, 50, 255);

        // Nokta çizimi
        drawList->AddCircleFilled(dotPos, 4.0f, dotColor);
        drawList->AddCircle(dotPos, 5.0f, IM_COL32(0, 0, 0, 255), 12, 1.0f); // Siyah kenarlık

        // İsim ve Mesafe Metni
        std::string label = entity.name + " (" + std::to_string(static_cast<int>(entity.distance)) + "m)";
        drawList->AddText(ImVec2(dotPos.x + 8.0f, dotPos.y - 6.0f), IM_COL32(255, 255, 255, 230), label.c_str());
    }

    ImGui::End();
}
