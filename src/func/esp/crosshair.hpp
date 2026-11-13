#pragma once
#include "common.hpp"

namespace func::esp {

    // CROSSHAIR & CENTRAL RETICLE DOT
    inline void drawCrosshair(ImDrawList* dl) {
        if (!cfg::esp::crosshair) return;

        float cx = g_sw * 0.5f, cy = g_sh * 0.5f;
        float gap = 6.f, len = 10.f;
        ImU32 c = IM_COL32(220, 40, 40, 240);

        // 4 crosshair arms
        dl->AddLine({cx - gap - len, cy}, {cx - gap, cy}, c, 1.8f);
        dl->AddLine({cx + gap, cy}, {cx + gap + len, cy}, c, 1.8f);
        dl->AddLine({cx, cy - gap - len}, {cx, cy - gap}, c, 1.8f);
        dl->AddLine({cx, cy + gap}, {cx, cy + gap + len}, c, 1.8f);

        // Official center aim dot
        dl->AddCircleFilled({cx, cy}, 1.8f, c);
        dl->AddCircle({cx, cy}, 3.0f, IM_COL32(255, 255, 255, 180), 12, 1.0f);
    }

} // namespace func::esp
