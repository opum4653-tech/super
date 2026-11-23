// ============================================================================
// NATIVE IMGUI OVERLAY MENU & TOUCH CONTROLLER
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Implements the user-facing menu interface and touch handling:
// - Floating Logo ("CM"): Draggable, circular HUD button that toggles menu visibility.
// - Emerald Green Theme: Clean, modern palette with rounded cards and custom sliders.
// - Axis-Locked Touch Scrolling: Custom touch gesture discriminator preventing accidental
//   slider adjustments while scrolling through options.
// - Subsystems Orchestration:
//     * Calls func::esp::draw() to render the full ESP suite on the ImGui draw list.
//     * Calls aimbot::render_fov() to draw targeting rings.
//     * Handles safe exit with full reverse system restoration.
// ============================================================================

#include "menu.hpp"
#include "imgui.h"

#include "Android_draw/draw.h"
#include "Android_touch/TouchHelperA.h"
#include "other/settings_guard.hpp"
#include "other/memory.hpp"
#include "kpm.h"
#include "func/esp.hpp"
#include "func/aim.hpp"
#include "cfg.hpp"
#include "game/data.hpp"
#include <string>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>

using namespace ImGui;

// Forward-declare Dear ImGui's ClearActiveID to cancel active widgets during scroll
namespace ImGui {
    IMGUI_API void ClearActiveID();
}

extern std::atomic<bool> main_thread_flag;
static int activeTab = 1; // Default to Tab 1 (Visual) matching screenshot
static float ui_opacity = 1.0f;
static std::atomic<bool> menu_open{false};
static std::atomic<bool> g_should_exit{false};

// ============================================================================
// EMERALD / NEON GREEN PALETTE (MATCHING SCREENSHOT)
// ============================================================================
namespace theme_overlay {
    constexpr ImU32 ColTitleGreen     = IM_COL32(46, 255, 76, 255);    // Bright Neon Green #2EFF4C
    constexpr ImU32 ColActiveTab      = IM_COL32(0, 230, 30, 255);     // Solid Bright Green #00E61E
    constexpr ImU32 ColInactiveTabBg  = IM_COL32(18, 24, 18, 255);     // Dark Greenish Charcoal
    constexpr ImU32 ColInactiveTabBdr = IM_COL32(40, 56, 40, 255);     // Subtle Green-Gray Border
    constexpr ImU32 ColWindowBg       = IM_COL32(10, 11, 13, 252);     // Pitch Black
    constexpr ImU32 ColWindowBorder   = IM_COL32(28, 38, 28, 255);     // Subtle Dark Border
    constexpr ImU32 ColAccentBar      = IM_COL32(0, 255, 50, 255);     // Vivid Neon Green Vertical Accent
    constexpr ImU32 ColSwitchOnTrack  = IM_COL32(0, 230, 36, 255);     // Bright Green Track (ON)
    constexpr ImU32 ColSwitchOffTrack = IM_COL32(24, 42, 46, 255);     // Dark Slate Track (OFF)
    constexpr ImU32 ColSwitchThumb    = IM_COL32(255, 255, 255, 255);  // Pure White Knob
    constexpr ImU32 ColTextWhite      = IM_COL32(250, 250, 250, 255);  // Clean White Text
    constexpr ImU32 ColTextMuted      = IM_COL32(180, 195, 180, 255);  // Muted Light Silver
    constexpr ImU32 ColTopBtnBg       = IM_COL32(20, 26, 20, 255);     // Top Button BG
    constexpr ImU32 ColTopBtnBdr      = IM_COL32(48, 62, 48, 255);     // Top Button Border
}

// ----------------------------------------------------------------------------
// FLOATING CIRCLE LOGO
// ----------------------------------------------------------------------------
static void DrawCircularLogo(ImDrawList* dl, ImVec2 center, float radius, bool spinning = false) {
    (void)spinning;
    ImU32 colPrimary = theme_overlay::ColAccentBar;
    ImU32 colDarkBg  = IM_COL32(12, 16, 12, 245);
    ImU32 colWhite   = IM_COL32(255, 255, 255, 255);

    // 1. Base Dark Disc
    dl->AddCircleFilled(center, radius, colDarkBg, 64);

    // 2. Neon Green Outline
    dl->AddCircle(center, radius, colPrimary, 64, 2.0f);

    // 3. Center "CM" Brand Monogram
    ImFont* font = ImGui::GetFont();
    const char* txt = "CM";
    float fontSize = radius * 0.95f;
    ImVec2 tsz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, txt);
    ImVec2 txtPos = ImVec2(center.x - tsz.x * 0.5f, center.y - tsz.y * 0.5f);

    dl->AddText(font, fontSize, ImVec2(txtPos.x + 1.2f, txtPos.y + 1.2f), IM_COL32(0, 0, 0, 180), txt);
    dl->AddText(font, fontSize, txtPos, colWhite, txt);
}

// ----------------------------------------------------------------------------
// NATIVE TOUCH-SCROLL HANDLER (AXIS-LOCKED GESTURE DISCRIMINATION)
// Aligned with Kernel Project (refer/Kernel/Kernel/src/ui/menu.cpp)
// ----------------------------------------------------------------------------
static bool s_touch_scrolling_active = false;

static void handleTouchScroll() {
    ImGuiIO& io = ImGui::GetIO();
    enum GestureState { GESTURE_IDLE, GESTURE_HORIZONTAL, GESTURE_VERTICAL };
    static GestureState s_gesture = GESTURE_IDLE;
    static ImVec2 s_touch_start = ImVec2(0, 0);
    static ImGuiID s_active_child = 0;

    ImGuiID child_id = ImGui::GetID("##TouchScrollToken");

    if (io.MouseDown[0]) {
        if (s_gesture == GESTURE_IDLE) {
            // CRITICAL FIX: Allow gesture initiation even when touch starts over active controls (buttons, switches, sliders)
            if (s_active_child == 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                s_touch_start = io.MousePos;
                s_active_child = child_id;
            }
        }

        if (s_active_child == child_id) {
            float dx = io.MousePos.x - s_touch_start.x;
            float dy = io.MousePos.y - s_touch_start.y;
            float abs_dx = std::abs(dx);
            float abs_dy = std::abs(dy);

            if (s_gesture == GESTURE_IDLE) {
                if (abs_dx >= 5.0f && abs_dx > abs_dy * 1.1f) {
                    s_gesture = GESTURE_HORIZONTAL;
                } else if (abs_dy >= 5.0f && abs_dy > abs_dx * 1.1f) {
                    s_gesture = GESTURE_VERTICAL;
                    s_touch_scrolling_active = true;
                    ImGui::ClearActiveID(); // Cancel button/slider activation when drag scroll begins
                }
            }

            if (s_gesture == GESTURE_VERTICAL) {
                s_touch_scrolling_active = true;
                float delta_y = io.MouseDelta.y;
                if (std::abs(delta_y) > 0.01f) {
                    float cur_scroll = ImGui::GetScrollY();
                    float new_scroll = cur_scroll - delta_y;
                    float max_scroll = ImGui::GetScrollMaxY();
                    if (new_scroll < 0.0f) new_scroll = 0.0f;
                    if (new_scroll > max_scroll) new_scroll = max_scroll;
                    ImGui::SetScrollY(new_scroll);
                }
            }
        }
    } else {
        s_gesture = GESTURE_IDLE;
        s_active_child = 0;
        s_touch_scrolling_active = false;
    }
}

// ----------------------------------------------------------------------------
// CUSTOM WIDGET: TOP NAVIGATION TAB BUTTON
// ----------------------------------------------------------------------------
static bool DrawTopTabButton(const char* label, bool is_active, ImVec2 size, float ui_scale = 1.0f) {
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bMin = ImGui::GetItemRectMin();
    ImVec2 bMax = ImGui::GetItemRectMax();
    float rounding = 8.0f * ui_scale;

    ImFont* font = ImGui::GetFont();
    float fontSize = 18.0f * ui_scale;

    if (is_active) {
        // Solid vivid green background
        dl->AddRectFilled(bMin, bMax, theme_overlay::ColActiveTab, rounding);
        // Clean bold white text
        ImVec2 tsz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        ImVec2 tpos(bMin.x + (size.x - tsz.x) * 0.5f, bMin.y + (size.y - tsz.y) * 0.5f);
        dl->AddText(font, fontSize, tpos, theme_overlay::ColTextWhite, label);
    } else {
        // Dark olive background
        ImU32 bgCol = hovered ? IM_COL32(26, 36, 26, 255) : theme_overlay::ColInactiveTabBg;
        dl->AddRectFilled(bMin, bMax, bgCol, rounding);
        // Subtle border
        dl->AddRect(bMin, bMax, theme_overlay::ColInactiveTabBdr, rounding, 0, 1.2f);
        // Light silver text
        ImVec2 tsz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        ImVec2 tpos(bMin.x + (size.x - tsz.x) * 0.5f, bMin.y + (size.y - tsz.y) * 0.5f);
        dl->AddText(font, fontSize, tpos, hovered ? theme_overlay::ColTextWhite : theme_overlay::ColTextMuted, label);
    }

    return pressed;
}

// ----------------------------------------------------------------------------
// CUSTOM WIDGET: TOP-RIGHT SQUARE CONTROL BUTTON ( - )
// ----------------------------------------------------------------------------
static bool DrawHeaderSquareBtn(const char* id, const char* text, ImVec2 pos, float size, float ui_scale = 1.0f) {
    ImGui::SetCursorScreenPos(pos);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bMin = pos;
    ImVec2 bMax(pos.x + size, pos.y + size);
    float rounding = 6.0f * ui_scale;

    ImU32 bgCol = hovered ? IM_COL32(32, 42, 32, 255) : theme_overlay::ColTopBtnBg;
    dl->AddRectFilled(bMin, bMax, bgCol, rounding);
    dl->AddRect(bMin, bMax, theme_overlay::ColTopBtnBdr, rounding, 0, 1.2f);

    ImFont* font = ImGui::GetFont();
    float fontSize = size * 0.55f;
    ImVec2 tsz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    ImVec2 tpos(bMin.x + (size - tsz.x) * 0.5f, bMin.y + (size - tsz.y) * 0.5f);
    dl->AddText(font, fontSize, tpos, theme_overlay::ColTextWhite, text);

    return pressed;
}

// ----------------------------------------------------------------------------
// CUSTOM WIDGET: SECTION HEADER WITH VERTICAL NEON GREEN ACCENT PILL
// ----------------------------------------------------------------------------
static void DrawSectionHeader(ImDrawList* dl, const char* label, float ui_scale = 1.0f) {
    ImVec2 cur = ImGui::GetCursorScreenPos();
    float barW = 4.5f * ui_scale;
    float barH = 22.0f * ui_scale;
    float padY = 2.0f * ui_scale;

    // Draw vertical neon green accent pill
    dl->AddRectFilled(
        ImVec2(cur.x, cur.y + padY),
        ImVec2(cur.x + barW, cur.y + padY + barH),
        theme_overlay::ColAccentBar,
        2.5f
    );

    // Text starting cleanly after the accent bar
    ImFont* font = ImGui::GetFont();
    float fontSize = 18.0f * ui_scale;
    ImVec2 tsz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    ImVec2 tpos(cur.x + barW + 10.0f * ui_scale, cur.y + (barH + padY * 2.0f - tsz.y) * 0.5f);
    dl->AddText(font, fontSize, tpos, IM_COL32(250, 250, 250, 255), label);

    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + barH + 8.0f * ui_scale));
}

// ----------------------------------------------------------------------------
// CUSTOM WIDGET: PILL TOGGLE SWITCH (EXACT REPLICA OF USER SCREENSHOT)
// ----------------------------------------------------------------------------
static bool DrawToggleSwitch(const char* label, bool* v, float row_height = 46.0f, float ui_scale = 1.0f) {
    ImVec2 curPos = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;

    char btn_id[128];
    snprintf(btn_id, sizeof(btn_id), "##SwitchBtn_%s", label);

    bool pressed = ImGui::InvisibleButton(btn_id, ImVec2(availW, row_height));
    if (pressed && !s_touch_scrolling_active) {
        *v = !(*v);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1. Label on the left, vertically centered
    ImFont* font = ImGui::GetFont();
    float fontSize = 17.0f * ui_scale;
    ImVec2 tsz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    ImVec2 label_pos(curPos.x + 8.0f * ui_scale, curPos.y + (row_height - tsz.y) * 0.5f);
    dl->AddText(font, fontSize, label_pos, theme_overlay::ColTextWhite, label);

    // 2. Capsule / Pill switch on the right
    float switchW = 58.0f * ui_scale;
    float switchH = 30.0f * ui_scale;
    float switchRounding = switchH * 0.5f;
    float switchPadRight = 8.0f * ui_scale;

    ImVec2 sMin(curPos.x + availW - switchW - switchPadRight, curPos.y + (row_height - switchH) * 0.5f);
    ImVec2 sMax(sMin.x + switchW, sMin.y + switchH);

    // Track color (Solid Green if ON, Dark slate if OFF)
    ImU32 trackCol = *v ? theme_overlay::ColSwitchOnTrack : theme_overlay::ColSwitchOffTrack;
    dl->AddRectFilled(sMin, sMax, trackCol, switchRounding);

    // Subtle dark track border when OFF
    if (!(*v)) {
        dl->AddRect(sMin, sMax, IM_COL32(36, 56, 62, 220), switchRounding, 0, 1.2f);
    }

    // Circular white knob / thumb
    float knobRadius = switchRounding - 2.8f * ui_scale;
    float knobX = *v ? (sMax.x - switchRounding) : (sMin.x + switchRounding);
    float knobY = sMin.y + switchRounding;

    dl->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, theme_overlay::ColSwitchThumb, 32);

    return pressed;
}

// ============================================================================
// MAIN RENDERING PIPELINE
// ============================================================================
namespace ui::menu {

    void render() {
        // Render background overlays (ESP & Aimbot FOV)
        // Note: func::aim::update() runs decoupled in the dedicated aimbot worker thread
        func::esp::draw();
        func::aim::render_fov(ImGui::GetBackgroundDrawList());

        ImGuiIO& io = ImGui::GetIO();

        float realW = (io.DisplaySize.x > 0.0f) ? io.DisplaySize.x : (float)native_window_screen_x;
        float realH = (io.DisplaySize.y > 0.0f) ? io.DisplaySize.y : (float)native_window_screen_y;
        if (realW <= 0) realW = 2400.0f;
        if (realH <= 0) realH = 1080.0f;

        // Screen rotation & resolution tracker (aligned with Kernel project)
        static int last_orientation = -1;
        static float last_screen_w = 0.0f;
        static float last_screen_h = 0.0f;
        static bool menu_needs_reposition = false;
        static bool pill_needs_reposition = false;

        int current_orientation = (realW > realH) ? 1 : 0;
        bool screen_changed = (last_orientation != -1 && (current_orientation != last_orientation ||
                               std::abs(realW - last_screen_w) > 50.0f ||
                               std::abs(realH - last_screen_h) > 50.0f));

        if (screen_changed) {
            menu_needs_reposition = true;
            pill_needs_reposition = true;
        }
        last_orientation = current_orientation;
        last_screen_w = realW;
        last_screen_h = realH;

        // Dynamic responsive UI scaling (aligned with Kernel project: 1.0 at 1080p)
        float ui_scale = realH / 1080.0f;
        if (ui_scale < 0.70f) ui_scale = 0.70f;
        if (ui_scale > 1.35f) ui_scale = 1.35f;

        const float logoRadius = 46.0f * ui_scale;
        const float logoDiameter = logoRadius * 2.0f;
        const float margin = 14.0f * ui_scale;

        static ImVec2 s_circle_pos = ImVec2(-1.0f, -1.0f);
        if (s_circle_pos.x < 0.0f || pill_needs_reposition) {
            s_circle_pos = ImVec2(margin + logoRadius + 20.0f * ui_scale, realH * 0.25f);
            pill_needs_reposition = false;
        }

        // Viewport clamping
        s_circle_pos.x = std::max(margin + logoRadius, std::min(s_circle_pos.x, realW - logoRadius - margin));
        s_circle_pos.y = std::max(margin + logoRadius, std::min(s_circle_pos.y, realH - logoRadius - margin));

        static ImVec2 s_touch_start_pos = ImVec2(0, 0);
        static ImVec2 s_drag_offset = ImVec2(0, 0);
        static bool s_is_dragging = false;
        static bool s_moved_drag = false;

        // --------------------------------------------------------------------
        // STATE 1: FLOATING LOGO BALL (Rendered ONLY when menu is closed)
        // --------------------------------------------------------------------
        if (!menu_open) {
            ImVec2 winPos = ImVec2(s_circle_pos.x - logoRadius, s_circle_pos.y - logoRadius);
            ImVec2 winSize = ImVec2(logoDiameter, logoDiameter);

            // Register circular UI touch barrier matching Kernel reference
            Touch_AddUiCircle(s_circle_pos.x, s_circle_pos.y, logoRadius + 10.0f * ui_scale);

            ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);

            ImGuiWindowFlags circleFlags = ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoScrollWithMouse |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBackground |
                                           ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoBringToFrontOnFocus;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            if (ImGui::Begin("##FLOATING_CIRCLE", nullptr, circleFlags)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 center = s_circle_pos;
                const ImVec2 mouse = io.MousePos;

                ImGui::SetCursorPos(ImVec2(0, 0));
                bool clicked = ImGui::InvisibleButton("##CircleHitbox", winSize);
                const bool active  = ImGui::IsItemActive();
                const bool deactivated = ImGui::IsItemDeactivated();

                static bool s_was_activated = false;
                if (ImGui::IsItemActivated()) {
                    s_was_activated = true;
                    s_touch_start_pos = mouse;
                    s_drag_offset = ImVec2(mouse.x - s_circle_pos.x, mouse.y - s_circle_pos.y);
                    s_is_dragging = false;
                    s_moved_drag = false;
                }

                if (active && s_was_activated) {
                    float dx = mouse.x - s_touch_start_pos.x;
                    float dy = mouse.y - s_touch_start_pos.y;
                    const float dragSlop = 22.0f * ui_scale;
                    if (s_moved_drag || std::hypot(dx, dy) > dragSlop) {
                        s_is_dragging = true;
                        s_moved_drag = true;
                        s_circle_pos.x = std::clamp(mouse.x - s_drag_offset.x, margin + logoRadius, realW - logoRadius - margin);
                        s_circle_pos.y = std::clamp(mouse.y - s_drag_offset.y, margin + logoRadius, realH - logoRadius - margin);
                    }
                }

                // Smooth tap toggle without artificial startup delays
                static double s_last_toggle_time = 0.0;
                double cur_time = ImGui::GetTime();

                if (s_was_activated && (clicked || (deactivated && !s_moved_drag))) {
                    if (cur_time - s_last_toggle_time > 0.25) {
                        menu_open = true;
                        s_last_toggle_time = cur_time;
                    }
                    s_moved_drag = false;
                    s_is_dragging = false;
                    s_was_activated = false;
                } else if (deactivated) {
                    s_moved_drag = false;
                    s_is_dragging = false;
                    s_was_activated = false;
                }

                // Draw circular emblem with CM monogram
                DrawCircularLogo(dl, center, logoRadius, s_is_dragging);

                // Real-time daemon connection status dot
                bool connected = (proc::pid > 0 && proc::lib > 0);
                ImU32 stCol = connected ? IM_COL32(0, 240, 140, 255) : IM_COL32(255, 90, 90, 255);
                ImVec2 stDot(center.x + logoRadius * 0.68f, center.y - logoRadius * 0.68f);
                dl->AddCircleFilled(stDot, 6.0f, (stCol & 0x00FFFFFF) | (90 << 24));
                dl->AddCircleFilled(stDot, 4.0f, (stCol & 0x00FFFFFF) | (180 << 24));
                dl->AddCircleFilled(stDot, 2.5f, stCol);
            }
            ImGui::End();
            ImGui::PopStyleVar();

            // Menu is minimized, done for this frame!
            return;
        }

        // --------------------------------------------------------------------
        // STATE 2: FLOATING MENU WINDOW (Rendered ONLY when menu is open)
        // --------------------------------------------------------------------

        // ====================================================================
        // MAIN OVERLAY WINDOW — ENLARGED TOUCH GEOMETRY
        // ====================================================================
        const float headerH = 48.0f * ui_scale;
        const float winW = std::max(540.0f * ui_scale, std::min(700.0f * ui_scale, realW * 0.92f));
        const float winH = std::max(420.0f * ui_scale, std::min(580.0f * ui_scale, realH * 0.90f));

        static ImVec2 s_menu_pos = ImVec2(-1.0f, -1.0f);
        if (s_menu_pos.x < 0.0f || menu_needs_reposition) {
            s_menu_pos = ImVec2((realW - winW) * 0.5f, (realH - winH) * 0.5f);
            menu_needs_reposition = false;
        }

        // FULL SCREEN MOVEMENT FREEDOM (Drag anywhere across screen, matching Kernel project)
        const float minVisibleX = 80.0f * ui_scale;
        const float minVisibleY = headerH;
        const float minPosX = -(winW - minVisibleX);
        const float maxPosX = realW - minVisibleX;
        const float minPosY = 0.0f;
        const float maxPosY = realH - minVisibleY;
        s_menu_pos.x = std::clamp(s_menu_pos.x, minPosX, maxPosX);
        s_menu_pos.y = std::clamp(s_menu_pos.y, minPosY, maxPosY);

        ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
        ImGui::SetNextWindowPos(s_menu_pos, ImGuiCond_Always);

        // Register rectangular UI touch barrier matching Kernel reference
        Touch_AddUiRect(s_menu_pos.x, s_menu_pos.y, winW, winH);

        // Styling: Pure Pitch Black & Clean Green Accents
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.039f, 0.043f, 0.051f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.11f, 0.15f, 0.11f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.09f, 0.07f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.12f, 0.16f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.22f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.0f, 0.90f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.0f, 0.90f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.2f, 1.00f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.07f, 0.10f, 0.07f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.18f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.90f, 0.20f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.90f, 0.20f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.90f, 0.20f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.90f, 0.20f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.98f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.60f, 0.65f, 0.60f, 1.0f));

        // Hide scrollbars completely (touch drag handles all scrolling)
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0, 0, 0, 0));

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ui_opacity);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * ui_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.4f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * ui_scale, 10.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * ui_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * ui_scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);

        ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoScrollbar;

        if (ImGui::Begin("##MAIN_OVERLAY_WINDOW", nullptr, winFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 curWinPos = ImGui::GetWindowPos();
            ImVec2 curWinSize = ImGui::GetWindowSize();

            // ----------------------------------------------------------------
            // 1. TITLE BAR (CRACK MASTER in Neon Green + [ - ])
            // ----------------------------------------------------------------
            const char* brandTitle = "CRACK MASTER";

            // Centered Header Title (Large & Prominent)
            ImFont* font = ImGui::GetFont();
            float titleFontSize = 23.0f * ui_scale;
            ImVec2 titleSz = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, brandTitle);
            float titleX = curWinPos.x + (curWinSize.x - titleSz.x) * 0.5f;
            float titleY = curWinPos.y + (headerH - titleSz.y) * 0.5f;
            dl->AddText(font, titleFontSize, ImVec2(titleX, titleY), theme_overlay::ColTitleGreen, brandTitle);

            // Drag area across header (except right buttons)
            const float btnSize = 32.0f * ui_scale;
            const float rightButtonsWidth = btnSize + 14.0f * ui_scale;
            ImGui::SetCursorPos(ImVec2(0, 0));
            ImGui::InvisibleButton("##HeaderDragZone", ImVec2(curWinSize.x - rightButtonsWidth - 10.0f, headerH));
            if (ImGui::IsItemActive()) {
                s_menu_pos.x += io.MouseDelta.x;
                s_menu_pos.y += io.MouseDelta.y;
                s_menu_pos.x = std::clamp(s_menu_pos.x, minPosX, maxPosX);
                s_menu_pos.y = std::clamp(s_menu_pos.y, minPosY, maxPosY);
            }

            // Top-right button: [ - ] (Enlarged)
            float btnY = curWinPos.y + (headerH - btnSize) * 0.5f;
            float btnMinusX = curWinPos.x + curWinSize.x - btnSize - 14.0f * ui_scale;

            // [ - ] Button (Minimize to floating ball)
            if (DrawHeaderSquareBtn("##TopBtnMinus", "-", ImVec2(btnMinusX, btnY), btnSize, ui_scale)) {
                menu_open = false;
                s_circle_pos = ImVec2(std::clamp(s_menu_pos.x + logoRadius + 8.0f, margin + logoRadius, realW - logoRadius - margin),
                                      std::clamp(s_menu_pos.y + logoRadius + 8.0f, margin + logoRadius, realH - logoRadius - margin));
            }

            // ----------------------------------------------------------------
            // 2. HORIZONTAL NAVIGATION TABS: [ Info ] [ Visual ] [ Aimbot ] [ Motion ] [ Settings ]
            // ----------------------------------------------------------------
            const float tabPadX = 14.0f * ui_scale;
            const float tabGap  = 8.0f * ui_scale;
            const float tabW    = (curWinSize.x - tabPadX * 2.0f - tabGap * 4.0f) / 5.0f;
            const float tabH    = 44.0f * ui_scale;

            const char* tabNames[] = { "Info", "Visual", "Aimbot", "Motion", "Settings" };

            ImGui::SetCursorPos(ImVec2(tabPadX, headerH));
            for (int i = 0; i < 5; ++i) {
                if (i > 0) ImGui::SameLine(0, tabGap);
                if (DrawTopTabButton(tabNames[i], (activeTab == i), ImVec2(tabW, tabH), ui_scale)) {
                    activeTab = i;
                }
            }

            // ----------------------------------------------------------------
            // 3. MAIN CONTENT BODY AREA
            // ----------------------------------------------------------------
            const float contentPadX = tabPadX;
            const float contentY = headerH + tabH + 14.0f * ui_scale;
            const float contentW = curWinSize.x - contentPadX * 2.0f;
            const float contentH = curWinSize.y - contentY - 8.0f * ui_scale;

            ImGui::SetCursorPos(ImVec2(contentPadX, contentY));

            if (ImGui::BeginChild("##MainContent", ImVec2(contentW, contentH), false, ImGuiWindowFlags_NoScrollbar)) {
                ImDrawList* cdl = ImGui::GetWindowDrawList();

                // Style scaling for sliders, combo dropdowns and option controls
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * ui_scale, 8.0f * ui_scale));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * ui_scale, 8.0f * ui_scale));
                ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 22.0f * ui_scale);

                // ============================================================
                // TAB 0: INFO & KPM LIVE TELEMETRY
                // ============================================================
                if (activeTab == 0) {
                    DrawSectionHeader(cdl, "System Controls", ui_scale);

                    if (DrawToggleSwitch("Master Switch", &cfg::master_enabled, 48.0f * ui_scale, ui_scale)) {
                        if (!cfg::master_enabled) {
                            func::aim::motion::reset();
                        }
                    }

                    ImGui::Dummy(ImVec2(0, 8.0f * ui_scale));
                    DrawSectionHeader(cdl, "Driver Engine & Mode Selection", ui_scale);

                    ImGui::Text("Backend Engine:");
                    const char* modeLabels[] = { "AUTO", "TB KPM", "NEO KPM", "SYSCALL" };
                    float btnW = (curWinSize.x - contentPadX * 2.0f - 18.0f * ui_scale) / 4.0f;
                    for (int m = 0; m < 4; m++) {
                        if (m > 0) ImGui::SameLine(0, 6.0f * ui_scale);
                        bool is_sel = (kpm.diag.selected_engine == m);
                        if (is_sel) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.8f, 0.2f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.12f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                        }
                        if (ImGui::Button(modeLabels[m], ImVec2(btnW, 36.0f * ui_scale))) {
                            kpm.diag.selected_engine = m;
                            kpm.test_connection(proc::pid);
                        }
                        ImGui::PopStyleColor(2);
                    }

                    ImGui::Dummy(ImVec2(0, 4.0f * ui_scale));
                    DrawToggleSwitch("Auto Fallback to Syscall", &kpm.diag.auto_fallback, 48.0f * ui_scale, ui_scale);

                    ImGui::Dummy(ImVec2(0, 8.0f * ui_scale));
                    DrawSectionHeader(cdl, "Live Kernel & Driver Telemetry", ui_scale);

                    if (kpm.diag.is_connected) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "Driver State: CONNECTED");
                    } else if (kpm.diag.auto_fallback && kpm.diag.active_engine == KPM_ENGINE_SYSCALL) {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "Driver State: SYSCALL FALLBACK (ACTIVE)");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Driver State: DISCONNECTED (KPM NOT RESPONDING)");
                    }

                    ImGui::Text("Active Engine: %s", kpm.get_active_engine_name());

                    // Detailed TB KPM Diagnostics
                    if (kpm.diag.tb_ret >= 0 || kpm.diag.tb_works) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "TB KPM (0x63687501): OK (ret=%d, errno=%d)", kpm.diag.tb_ret, kpm.diag.tb_errno);
                    } else if (kpm.diag.tb_errno == 22) {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "TB KPM (0x63687501): ret=%d, errno=22 (EINVAL - Opcode Not Hooked)", kpm.diag.tb_ret);
                    } else {
                        ImGui::Text("TB KPM (0x63687501): ret=%d, errno=%d", kpm.diag.tb_ret, kpm.diag.tb_errno);
                    }

                    // Detailed Neo KPM Diagnostics
                    if (kpm.diag.neo_ret == 0 && (kpm.diag.neo_result == 0 || kpm.diag.neo_works)) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "Neo KPM (0x4E41): OK (ret=%d, res=%d)", kpm.diag.neo_ret, kpm.diag.neo_result);
                    } else if (kpm.diag.neo_errno == 22) {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Neo KPM (0x4E41): ret=%d, errno=22 (EINVAL - Opcode Not Hooked)", kpm.diag.neo_ret);
                    } else {
                        ImGui::Text("Neo KPM (0x4E41): ret=%d, res=%d, errno=%d", kpm.diag.neo_ret, kpm.diag.neo_result, kpm.diag.neo_errno);
                    }

                    ImGui::Text("Syscall 270: ret=%d [%s]",
                                kpm.diag.syscall_ret, kpm.diag.syscall_works ? "OK" : "Idle");

                    ImGui::Dummy(ImVec2(0, 8.0f * ui_scale));
                    DrawSectionHeader(cdl, "Target Process & Memory", ui_scale);

                    if (proc::pid > 0) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "Game PID: %d (Active Target)", proc::pid);
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Game PID: Searching (Free Fire)...");
                    }

                    if (proc::lib > 0) {
                        uint32_t magic = kpm.read<uint32_t>(proc::lib);
                        if (magic == 0x464C457F) {
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "libil2cpp: 0x%lx (ELF OK: 0x464C457F)", (unsigned long)proc::lib);
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "libil2cpp: 0x%lx (Magic: 0x%08X FAIL)", (unsigned long)proc::lib, magic);
                        }
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "libil2cpp: Not Resolved");
                    }

                    size_t pCount = 0;
                    {
                        std::lock_guard<std::mutex> lk(data::players_mutex);
                        pCount = data::players.size();
                    }
                    ImGui::Text("Entity Scraper: %zu players in cache", pCount);
                    ImGui::Text("Reads: %llu total / %llu failed",
                                (unsigned long long)kpm.diag.total_reads,
                                (unsigned long long)kpm.diag.failed_reads);

                    ImGui::Dummy(ImVec2(0, 8.0f * ui_scale));
                    if (ImGui::Button("Test / Re-detect KPM Connection", ImVec2(curWinSize.x - contentPadX * 2.0f, 40.0f * ui_scale))) {
                        kpm.test_connection(proc::pid);
                        if (proc::pid > 0) proc::lib = get_lib();
                    }
                }
                // ============================================================
                // TAB 1: VISUAL (EXACT REPLICA OF USER SCREENSHOT)
                // ============================================================
                else if (activeTab == 1) {
                    // 1. First Section Header: | Visual
                    DrawSectionHeader(cdl, "Visual", ui_scale);

                    // Master Switch — Global Toggle (matching Trace Line switch)
                    if (DrawToggleSwitch("Master Switch", &cfg::master_enabled, 48.0f * ui_scale, ui_scale)) {
                        if (!cfg::master_enabled) {
                            func::aim::motion::reset();
                        }
                    }

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));

                    // 2. Sub-Header: | ESP Display Elements
                    DrawSectionHeader(cdl, "ESP Display Elements", ui_scale);

                    // 3. Toggle Switches with Capsule shape (Green ON, Dark Slate OFF)
                    DrawToggleSwitch("Esp Box", &cfg::esp::box, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Box Filled", &cfg::esp::box_filled, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Line", &cfg::esp::tracer, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Name", &cfg::esp::name, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Health", &cfg::esp::health, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Distance", &cfg::esp::distance, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Armor", &cfg::esp::armor, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Weapon", &cfg::esp::weapon, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Esp Bot Tag", &cfg::esp::bot_tag, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Firing Indicator", &cfg::esp::firing_ind, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Crosshair", &cfg::esp::crosshair, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Loot Esp", &cfg::esp::loot, 48.0f * ui_scale, ui_scale);

                    ImGui::Dummy(ImVec2(0, 10.0f * ui_scale));
                    DrawSectionHeader(cdl, "ESP Configuration", ui_scale);

                    const char* boxTypes[] = { "Corner", "Full Chaturbhuj", "Glow" };
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##BoxStyle", &cfg::esp::box_type, boxTypes, 3);
                    ImGui::TextDisabled("Box Style Mode");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    const char* linePositions[] = { "Screen Top (Original)", "Screen Center (Crosshair Dot)", "Screen Bottom" };
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##LinePos", &cfg::esp::tracer_pos, linePositions, 3);
                    ImGui::TextDisabled("Tracer Line Origin");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##MaxRange", &cfg::esp::max_range, 50.0f, 500.0f, "%.0f m");
                    ImGui::TextDisabled("Max Render Distance");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##LaserThickness", &cfg::esp::tracer_thickness, 0.5f, 4.0f, "%.1f px");
                    ImGui::TextDisabled("Laser Line Width");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##BoxThickness", &cfg::esp::box_thickness, 1.0f, 5.0f, "%.1f px");
                    ImGui::TextDisabled("Box Border Width");
                }
                // ============================================================
                // TAB 2: AIMBOT
                // ============================================================
                else if (activeTab == 2) {
                    DrawSectionHeader(cdl, "Aimbot", ui_scale);

                    // Master Switch — Global Toggle
                    if (DrawToggleSwitch("Master Switch", &cfg::master_enabled, 48.0f * ui_scale, ui_scale)) {
                        if (!cfg::master_enabled) {
                            func::aim::motion::reset();
                        }
                    }

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));

                    DrawSectionHeader(cdl, "Aim Assist Controls", ui_scale);

                    // Single aim path: Drag Headshot. "Aim Silent" was removed.
                    DrawToggleSwitch("Drag Headshot (100% Head)", &cfg::aim::enable, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Draw FOV Circle", &cfg::aim::show_fov, 48.0f * ui_scale, ui_scale);
                    DrawToggleSwitch("Ignore AI Bots", &cfg::aim::ignore_bots, 48.0f * ui_scale, ui_scale);

                    ImGui::Dummy(ImVec2(0, 10.0f * ui_scale));
                    DrawSectionHeader(cdl, "FOV & Target Settings", ui_scale);

                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##AimFov", &cfg::aim::aim_fov, 0.0f, 180.0f, "%.0f deg");
                    ImGui::TextDisabled("AimFov (0 - 180)");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##AimMaxDist", &cfg::aim::max_distance, 0.0f, 500.0f, "%.0f m");
                    ImGui::TextDisabled("Max Target Distance (0 = Unlimited)");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    const char* triggerModes[] = { "When Firing", "Always In FOV" };
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##AimTrigger", &cfg::aim::trigger_mode, triggerModes, 2);
                    ImGui::TextDisabled("Aim Trigger Condition");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    const char* priorityModes[] = { "Crosshair Distance", "Lowest HP", "3D Distance" };
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##AimPriority", &cfg::aim::priority_mode, priorityModes, 3);
                    ImGui::TextDisabled("Target Priority Mode");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Head Lock: 100%% (always)");
                    ImGui::TextDisabled("Every bullet is dragged to the head bone.");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##HeadBias", &cfg::aim::head_bias, -0.20f, 0.30f, "%.2f m");
                    ImGui::TextDisabled("Head Center Alignment (Bias)");

                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##AimSmooth", &cfg::aim::smooth, 0.0f, 25.0f, "%.1f");
                    ImGui::TextDisabled("Aim Smoothness");
                }
                // ============================================================
                // TAB 3: MOTION (ANTI-BAN PROTECTED)
                // ============================================================
                else if (activeTab == 3) {
                    DrawSectionHeader(cdl, "Motion", ui_scale);
                    DrawSectionHeader(cdl, "Movement Integrity Guard", ui_scale);

                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Anti-Ban Protection: Active");
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Server-Authoritative Movement Locked");
                    ImGui::Dummy(ImVec2(0, 6.0f * ui_scale));

                    ImGui::TextDisabled("Air Flight and Phantom Mode are permanently disabled");
                    ImGui::TextDisabled("to protect your game account from instant server-side");
                    ImGui::TextDisabled("physics validation and position desync auto-bans.");
                }
                // ============================================================
                // TAB 4: SETTINGS
                // ============================================================
                else if (activeTab == 4) {
                    DrawSectionHeader(cdl, "Settings", ui_scale);

                    DrawSectionHeader(cdl, "Master Control", ui_scale);
                    if (DrawToggleSwitch("Master Switch", &cfg::master_enabled, 48.0f * ui_scale, ui_scale)) {
                        if (!cfg::master_enabled) {
                            func::aim::motion::reset();
                        }
                    }

                    ImGui::Dummy(ImVec2(0, 8.0f * ui_scale));
                    DrawSectionHeader(cdl, "Menu Preferences", ui_scale);

                    ImGui::SliderFloat("##Opacity", &ui_opacity, 0.2f, 1.0f, "Opacity: %.2f");
                    ImGui::TextDisabled("Menu Window Opacity");

                    ImGui::Dummy(ImVec2(0, 20.0f * ui_scale));
                    if (ImGui::Button("SAFE EXIT APPLICATION", ImVec2(ImGui::GetContentRegionAvail().x, 48.0f * ui_scale))) {
                        g_should_exit = true;
                    }
                }

                // Single authoritative touch scroll handler for ##MainContent (swipe anywhere inside menu)
                handleTouchScroll();

                ImGui::PopStyleVar(3);
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::PopStyleVar(7);
        ImGui::PopStyleColor(20);

        // Deferred safe exit outside of ImGui frame
        if (g_should_exit) {
            main_thread_flag = false;
            Touch_Close();
            aimbot::thread_stop();
            data::thread_stop();
            shutdown();
            settings_guard::restore();
            exit(0);
        }
    }
}
