#pragma once
#include "common.hpp"

namespace func::esp {

    // WEAPON PILL (RENDERED BELOW PLAYER FEET)
    inline void drawWeapon(ImDrawList* dl, const PlayerData& p, ImVec2 foot) {
        if (!cfg::esp::weapon || p.weaponType < 0) return;

        static const char* kNames[] = {
            "AR","SNIPER","SMG","SHOTGUN","PISTOL","MELEE",
            "GRENADE","LMG","RIFLE","MARKSMAN","SPECIAL"
        };
        const char* wn = (p.weaponType < (int)(sizeof(kNames)/sizeof(*kNames)))
                         ? kNames[p.weaponType] : "WPN";

        const float fs   = ESP_FONT_SIZE - 2.f;
        const float padX = 6.f;
        const float padY = 3.f;

        ImVec2 ts = ImGui::CalcTextSize(wn);
        float pw = ts.x + padX * 2.f;
        float ph = fs + padY * 2.f;
        float px = foot.x - pw * 0.5f;
        float py = foot.y + 4.f;

        ImU32 wCol = toU32(cfg::esp::weapon_col);
        ImU32 bg   = p.isVisible ? IM_COL32(0, 0, 0, 210) : IM_COL32(0, 0, 0, 150);

        dl->AddRectFilled({px + 1, py + 1}, {px + pw + 1, py + ph + 1}, C_SHADOW, 5.f);
        dl->AddRectFilled({px, py}, {px + pw, py + ph}, bg, 5.f);
        dl->AddRect({px, py}, {px + pw, py + ph}, C_BORDER, 5.f, 0, 1.f);
        dl->AddText(ImGui::GetFont(), fs, {px + padX, py + padY}, wCol, wn);
    }

} // namespace func::esp
