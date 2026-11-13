#pragma once
#include "common.hpp"

namespace func::esp {

    // TRACER LINE (ORIGINS: 0 = Top, 1 = Center / Reticle Dot, 2 = Bottom)
    inline void drawTracer(ImDrawList* dl, const PlayerData& p, ImU32 colBase) {
        if (!cfg::esp::tracer) return;

        // Strictly draw tracer ONLY when head point is within visible screen bounds
        if (p.Head.z < 0.5f || p.Head.x < 0.f || p.Head.x > g_sw ||
            p.Head.y < 0.f || p.Head.y > g_sh) {
            return;
        }

        ImVec2 lineStart;
        if (cfg::esp::tracer_pos == 1) {
            // Screen Center / Official Reticle Dot Origin
            lineStart = ImVec2(g_sw * 0.5f, g_sh * 0.5f);
        } else if (cfg::esp::tracer_pos == 2) {
            // Screen Bottom Origin
            lineStart = ImVec2(g_sw * 0.5f, g_sh);
        } else {
            // Screen Top Origin (Default)
            lineStart = ImVec2(g_sw * 0.5f, 0.f);
        }

        ImVec2 head(p.Head.x, p.Head.y);
        float aa = ImClamp(1.f - p.distance / cfg::esp::max_range, 0.25f, 1.f);
        float tThick = cfg::esp::tracer_thickness;

        // Dynamic laser line with soft ambient glow
        dl->AddLine(lineStart, head, withAlpha(colBase, 0.25f * aa), tThick + 1.4f);
        dl->AddLine(lineStart, head, withAlpha(colBase, 0.95f * aa), tThick);

        // If originating from center reticle dot, draw a small central glow dot
        if (cfg::esp::tracer_pos == 1) {
            dl->AddCircleFilled(lineStart, 2.5f, withAlpha(colBase, 0.85f));
            dl->AddCircle(lineStart, 4.0f, withAlpha(colBase, 0.40f), 12, 1.0f);
        }
    }

} // namespace func::esp
