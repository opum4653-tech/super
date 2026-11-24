// ============================================================================
// GLOBAL SYSTEM METRICS & SHARED TIMING STATE
// ============================================================================
// Project: crack_master (External Native Mod & Overlay Suite)
//
// RELATIONSHIPS & CONSUMERS:
// - global_anim_time: Read by ui/menu.cpp and func/esp/*.hpp for dynamic UI glow & pulse.
// - g_sw, g_sh: Screen width and height dynamically populated by start_overlay()
//   and display_manager. Read across the entire math, ESP, aimbot, and menu pipelines.
// ============================================================================

float global_anim_time = 0.0f; // Accumulates real frame deltas only
float g_sw = 1920.0f;          // Screen Width in pixels (landscape max dimension)
float g_sh = 1080.0f;          // Screen Height in pixels (landscape min dimension)

