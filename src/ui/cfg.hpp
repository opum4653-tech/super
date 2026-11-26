#pragma once
// ============================================================================
// CENTRALIZED RUNTIME CONFIGURATION & UI STATE STORE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Holds all runtime configuration flags, sliders, and color settings.
// Read concurrently by ESP renderers (src/func/esp/) and Aimbot threads (src/func/aim/).
// Modified directly by the ImGui user interface (src/ui/menu.cpp).
//
// HIGHLIGHTS:
// - master_enabled: Single master toggle that enables/disables the entire feature set
//   without resetting the user's specific sub-feature preferences.
// - any_feature_enabled(): Gate used by data::update() to sleep/skip memory scraping
//   when all features are deactivated.
// ============================================================================

#include "imgui.h"
#include <condition_variable>
#include <cstdint>
#include <mutex>

extern float g_sw;
extern float g_sh;

namespace cfg {

    inline bool master_enabled = false; // Global Master Switch: controls all features, preserves selection state
    inline bool log_shield     = false; // UID-scoped iptables DROP on secondary log/crash/tracker domains

namespace esp {
// Master
    inline bool  box              = false;
    inline int   box_type         = 1; // 1 = Full Chaturbhuj (Rectangular) Box
    inline bool  name             = false;
    inline bool  health           = false;
    inline bool  armor            = false;
    inline bool  distance         = false;
    inline bool  weapon           = false;
    inline bool  bot_tag          = false;
    inline bool  ignore_bots      = false;
    inline bool  tracer           = false;
    inline int   tracer_pos       = 0; // 0 = Top, 1 = Center / Crosshair, 2 = Bottom
    inline bool  box_filled       = false; // Subtle filled background box
    inline bool  crosshair        = false;
    inline bool  firing_ind       = false;
    inline bool  loot             = false;

// Setting
    inline float max_range        = 0.0f;  // 0.0f = Official Game Range (Uncapped)
    inline float loot_range       = 0.0f;  // 0.0f = Official Game Range (Uncapped)
    inline float box_rounding     = 3.f;
    inline float box_thickness    = 2.0f;
    inline float tracer_thickness = 1.2f;
    inline float bracket_frac     = 0.28f;

// Warna
    inline ImVec4 box_col         = ImVec4(0.0f, 0.90f, 1.0f, 1.0f);
    inline ImVec4 box_visible     = ImVec4(0.30f, 1.0f, 0.55f, 1.0f);
    inline ImVec4 name_col        = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    inline ImVec4 distance_col    = ImVec4(0.95f, 0.95f, 0.30f, 1.0f);
    inline ImVec4 weapon_col      = ImVec4(1.0f, 0.85f, 0.30f, 1.0f);
}

namespace aim {
    inline bool  enable           = false; // Aimbot Headshot (Free Fire MAX)
    inline bool  zero_write       = true;  // Default ON: 100% Read-Only + uinput Touch (Anti-Ban)
    inline bool  show_fov         = false; // Render FOV Circle
    inline float aim_fov          = 45.0f;  // AimFov: head-carry circle (0 = use pixel radius)
    inline float fov_radius       = 200.0f; // Fallback pixel radius when AimFov is 0

    inline float head_bias        = 0.0f;
    inline int   trigger_mode     = 0; // 0 = When Firing, 1 = Always In FOV
    inline int   priority_mode    = 0; // 0 = Crosshair Distance, 1 = Low HP, 2 = 3D Distance
    inline bool  ignore_bots      = false;

    // Touch drag strength for the head carry. Scaled by AimFov and by how far
    // inside the circle the head is.
    inline float pull_step_px     = 22.0f;

    inline float get_fov_radius() {
        if (aim_fov > 0.0f && g_sw > 0.0f) {
            return (aim_fov / 180.0f) * (g_sw * 0.55f);
        }
        if (fov_radius > 0.0f) {
            return fov_radius;
        }
        // Neither configured: 25% of screen width (~270 px on a 1080-wide view).
        return (g_sw > 0.0f) ? (g_sw * 0.25f) : 250.0f;
    }
}

inline bool any_feature_enabled() {
    if (!master_enabled) return false;
    return aim::enable || esp::box || esp::health || esp::armor ||
           esp::name || esp::distance || esp::weapon ||
           esp::tracer || esp::loot || esp::crosshair || esp::firing_ind;
}

// ---------------------------------------------------------------------------
// THREAD-SAFE RUNTIME SNAPSHOT
// UI writes the plain values above on the render thread once per frame.
// Worker threads (scraper / aim) read this immutable snapshot only, so no
// bool/float is ever torn between an ImGui widget and a background loop.
// ---------------------------------------------------------------------------
struct RuntimeConfig {
    bool  master            = false;
    bool  aimEnable         = false;
    bool  aimZeroWrite      = true;
    float aimPullStepPx     = 0.0f;
    bool  aimShowFov        = false;
    bool  aimIgnoreBots     = false;
    int   aimTriggerMode    = 0;
    int   aimPriorityMode   = 0;
    float aimFovRadius      = 200.0f;
    float aimHeadBias       = 0.0f;
    float espMaxRange       = 0.0f;
    float espLootRange      = 0.0f;
    bool  espBox            = false;
    bool  espName           = false;
    bool  espHealth         = false;
    bool  espArmor          = false;
    bool  espDistance       = false;
    bool  espWeapon         = false;
    bool  espTracer         = false;
    bool  espLoot           = false;
    bool  espCrosshair      = false;
    bool  espFiringInd      = false;
    bool  espBotTag         = false;
    bool  anyFeature        = false;
    float screenW           = 1920.0f;
    float screenH           = 1080.0f;

    bool equals(const RuntimeConfig& o) const {
        return master == o.master &&
               aimEnable == o.aimEnable &&
               aimZeroWrite == o.aimZeroWrite &&
               aimPullStepPx == o.aimPullStepPx &&
               aimShowFov == o.aimShowFov &&
               aimIgnoreBots == o.aimIgnoreBots &&
               aimTriggerMode == o.aimTriggerMode &&
               aimPriorityMode == o.aimPriorityMode &&
               aimFovRadius == o.aimFovRadius &&
               aimHeadBias == o.aimHeadBias &&
               espMaxRange == o.espMaxRange &&
               espLootRange == o.espLootRange &&
               espBox == o.espBox &&
               espName == o.espName &&
               espHealth == o.espHealth &&
               espArmor == o.espArmor &&
               espDistance == o.espDistance &&
               espWeapon == o.espWeapon &&
               espTracer == o.espTracer &&
               espLoot == o.espLoot &&
               espCrosshair == o.espCrosshair &&
               espFiringInd == o.espFiringInd &&
               espBotTag == o.espBotTag &&
               anyFeature == o.anyFeature &&
               screenW == o.screenW &&
               screenH == o.screenH;
    }
};

inline RuntimeConfig             g_runtime_snapshot{};
inline std::mutex                g_runtime_mutex;
inline std::condition_variable   g_state_cv;
inline uint64_t                  g_state_generation = 0;

// Called by the scraper after a fresh real-time entity snapshot. No timer:
// workers wake only on actual state changes.
inline void notify_state_change() {
    std::lock_guard<std::mutex> lk(g_runtime_mutex);
    ++g_state_generation;
    g_state_cv.notify_all();
}

inline void publish_runtime() {
    RuntimeConfig s;
    s.master          = master_enabled;
    s.aimEnable       = aim::enable;
    s.aimZeroWrite    = aim::zero_write;
    s.aimPullStepPx   = aim::pull_step_px;
    s.aimShowFov      = aim::show_fov;
    s.aimIgnoreBots   = aim::ignore_bots;
    s.aimTriggerMode  = aim::trigger_mode;
    s.aimPriorityMode = aim::priority_mode;
    s.aimHeadBias     = aim::head_bias;
    s.espMaxRange     = esp::max_range;
    s.espLootRange    = esp::loot_range;
    s.espBox          = esp::box;
    s.espName         = esp::name;
    s.espHealth       = esp::health;
    s.espArmor        = esp::armor;
    s.espDistance     = esp::distance;
    s.espWeapon       = esp::weapon;
    s.espTracer       = esp::tracer;
    s.espLoot         = esp::loot;
    s.espCrosshair    = esp::crosshair;
    s.espFiringInd    = esp::firing_ind;
    s.espBotTag       = esp::bot_tag;
    s.anyFeature      = any_feature_enabled();
    s.screenW         = g_sw;
    s.screenH         = g_sh;

    if (aim::aim_fov > 0.0f && g_sw > 0.0f) {
        s.aimFovRadius = (aim::aim_fov / 180.0f) * (g_sw * 0.55f);
    } else if (aim::fov_radius > 0.0f) {
        s.aimFovRadius = aim::fov_radius;
    } else {
        s.aimFovRadius = (g_sw > 0.0f) ? (g_sw * 0.25f) : 250.0f;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_runtime_mutex);
        changed = !s.equals(g_runtime_snapshot);
        g_runtime_snapshot = s;
        if (changed) ++g_state_generation;
    }
    if (changed) g_state_cv.notify_all();
}

inline RuntimeConfig runtime() {
    std::lock_guard<std::mutex> lk(g_runtime_mutex);
    return g_runtime_snapshot;
}

}

