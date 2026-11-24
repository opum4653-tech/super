#pragma once
// ============================================================================
// FOV CIRCLE & HEAD GUIDANCE VISUALIZER
// ============================================================================
// The circle is the AimFov boundary. Enemies whose head falls inside it get
// head guidance; enemies outside it get nothing.
// ============================================================================

#include "common.hpp"

namespace aimbot {

    inline void render_fov(ImDrawList* dl) {

        const cfg::RuntimeConfig rc = cfg::runtime();
        if (!rc.master) return;
        if (!rc.aimEnable || !rc.aimShowFov) return;
        if (!data::inMatch.load(std::memory_order_relaxed)) return;

        float cx = rc.screenW * 0.5f;
        float cy = rc.screenH * 0.5f;
        float r  = rc.aimFovRadius;

        dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 196, 64, 70), 64, 1.2f);
        dl->AddCircle(ImVec2(cx, cy), r + 1.0f, IM_COL32(64, 230, 255, 210), 64, 2.0f);

        TargetInfo targetSnap;
        {
            std::lock_guard<std::mutex> lk(g_targetMutex);
            targetSnap = g_currentTarget;
        }
        if (targetSnap.valid && targetSnap.screenPos.z >= 0.001f) {
            dl->AddCircle(ImVec2(targetSnap.screenPos.x, targetSnap.screenPos.y),
                          10.0f, IM_COL32(255, 30, 80, 240), 16, 2.0f);
            dl->AddCircle(ImVec2(targetSnap.screenPos.x, targetSnap.screenPos.y),
                          3.5f, IM_COL32(255, 255, 255, 255), 12, 1.5f);
        }
    }

} // namespace aimbot
