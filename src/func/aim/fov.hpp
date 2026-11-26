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
        (void)dl; // Official game FOV used — no custom FOV overlay circle
    }

} // namespace aimbot
