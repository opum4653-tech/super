#pragma once
// ============================================================================
// FOV CIRCLE & ACTIVE TARGET LOCK VISUALIZER
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Renders the visual aim trigger radius and active target acquisition indicators:
// - Outer FOV Ring: Displays exact screen pixel boundary where enemies will be acquired.
// - Dynamic Scaling: Radius calculated from cfg::aim::get_fov_radius().
// - Target Lock Ring: Red-and-white targeting reticle placed on target screen position
//   whenever an enemy is actively locked by the aim hysteresis engine.
// ============================================================================

#include "common.hpp"

namespace aimbot {

    // FOV CIRCLE & ACTIVE TARGET LOCK VISUALIZER
    // Scaled dynamically by cfg::aim::get_fov_radius()
    inline void render_fov(ImDrawList* dl) {

        if (!cfg::master_enabled) return;
        if ((!cfg::aim::enable && !cfg::aim::silent) || !cfg::aim::show_fov) return;
        if (!data::inMatch.load(std::memory_order_relaxed)) return;

        float cx = g_sw * 0.5f;
        float cy = g_sh * 0.5f;
        float r  = cfg::aim::get_fov_radius();

        // Smooth Orchid / White FOV circle (shows exact trigger zone)
        dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 255, 255, 45), 64, 1.0f);
        dl->AddCircle(ImVec2(cx, cy), r + 1.0f, IM_COL32(225, 0, 153, 160), 64, 1.5f);

        // If target is actively locked inside FOV, draw target lock ring (thread-safe snapshot)
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
