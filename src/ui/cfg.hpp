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

extern float g_sw;

namespace cfg {

    inline bool master_enabled = false; // Global Master Switch: controls all features, preserves selection state

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
    inline float max_range        = 250.f;
    inline float loot_range       = 150.f;
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
    inline bool  enable           = false; // Target Lock Reticle (Visual Assist, Zero Fake Damage)
    inline bool  show_fov         = false; // Render FOV Circle
    inline float aim_fov          = 90.0f;  // Default 90 deg acquisition FOV
    inline float fov_radius       = 200.0f; // Default 200px screen radius
    inline float smooth           = 0.0f;
    inline float head_bias        = 0.0f;
    inline int   trigger_mode     = 0; // 0 = When Firing, 1 = Always In FOV
    inline int   priority_mode    = 0; // 0 = Crosshair Distance, 1 = Low HP, 2 = 3D Distance
    inline bool  ignore_bots      = false;
    inline float max_distance     = 0.0f;  // 0.0f default: unlimited (500m)

    // Resolves the aim acquisition radius in screen pixels.
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

namespace motion {
    inline bool  air_flight    = false; // Air Flight elevation modifier
    inline float flight_height = 0.0f;  // 0.0f default: fully controlled from menu slider
    inline bool  phantom_mode  = false; // Phantom Mode entity visibility
}

inline bool any_feature_enabled() {
    if (!master_enabled) return false;
    return aim::enable || esp::box || esp::health || esp::armor ||
           esp::name || esp::distance || esp::weapon ||
           esp::tracer || esp::loot || esp::crosshair || esp::firing_ind ||
           motion::air_flight || motion::phantom_mode;
}

}

