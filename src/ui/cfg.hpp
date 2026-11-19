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
    inline int   read_interval_ms = 16;   // Read loop cadence (ms). Higher = lower syscall frequency.
    inline bool  enable           = false; // Traditional Aim Assist
    inline bool  silent           = false; // Aim Silent: direct ray trajectory steering, zero camera movement
    inline bool  scope_aim        = false; // Off by default. Enabling adds a write path on scope.
    inline bool  sniper_aim       = false; // Off by default. Enabling forces head lock, ignoring headshot_rate.
    inline bool  show_fov         = false;
    inline float aim_fov          = 35.0f;  // AimFov (0° - 180°), matching reference calculation
    inline float fov_radius       = 180.0f; // Fallback pixel radius
    inline float smooth           = 5.0f;   // Safe default: smooth lerp (1=snap, 10=slow). Range 3-8 looks natural.
    inline float headshot_rate    = 85.0f;  // Safe 85% default rate to prevent server heuristic ban
    inline float head_bias        = 0.08f;  // Vertical bias (+0.08m) for dead-center headshot alignment
    inline int   target_bone      = 0; // 0 = Head, 1 = Neck/Chest, 2 = Pelvis/Body
    inline int   trigger_mode     = 0; // 0 = When Firing, 1 = Always In FOV
    inline int   priority_mode    = 0; // 0 = Crosshair Distance, 1 = Low HP, 2 = 3D Distance
    inline bool  ignore_bots      = false;
    inline float max_distance     = 200.0f;

    inline float get_fov_radius() {
        if (aim_fov > 0.0f && g_sw > 0.0f) {
            return (aim_fov / 180.0f) * (g_sw * 0.55f);
        }
        return fov_radius;
    }
}

namespace motion {
    inline bool  air_flight    = false; // Air Flight elevation modifier
    inline float flight_height = 4.5f;  // Elevation height in meters
    inline bool  phantom_mode  = false; // Phantom Mode entity visibility
}

inline bool any_feature_enabled() {
    if (!master_enabled) return false;
    return aim::enable || aim::silent || esp::box || esp::health || esp::armor ||
           esp::name || esp::distance || esp::weapon ||
           esp::tracer || esp::loot || esp::crosshair || esp::firing_ind ||
           motion::air_flight || motion::phantom_mode;
}

}

