#pragma once
#include "common.hpp"

namespace func::esp {

    // FIRING INDICATOR (PULSE WARNING WHEN ENEMY IS SHOOTING)
    inline void drawFiring(ImDrawList* dl, const PlayerData& p, float cx, float ty) {
        if (!cfg::esp::firing_ind || !p.isFiring) return;

        float t = ImGui::GetTime();
        float pulse = 0.5f + 0.5f * sinf(t * 12.f);
        float r = 3.f + pulse * 1.2f;
        ImVec2 pos(cx, ty - 20.f);

        dl->AddCircleFilled(pos, r + 4.f, IM_COL32(255, 200, 40, (int)(70 * pulse)));
        dl->AddCircleFilled(pos, r, IM_COL32(255, 200, 40, 255));
        dl->AddCircle(pos, r + 1.f, IM_COL32(255, 255, 255, 210), 16, 1.2f);
    }

} // namespace func::esp
