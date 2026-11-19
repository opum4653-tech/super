#pragma once
// ============================================================================
// ZERO-FORK SAFE DISPLAY & SCREEN ORIENTATION MANAGER
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Queries physical screen resolution (width, height) and device orientation safely.
// Designed to avoid spawning shell processes (wm size, dumpsys input) which can be
// flagged by runtime security checks.
//
// MULTI-TIER PROBING ARCHITECTURE:
// - Tier 1: Zero-fork direct Linux sysfs (/sys/class/graphics/fb0/virtual_size)
// - Tier 2: Linux Framebuffer IOCTL (/dev/graphics/fb0 FBIOGET_VSCREENINFO)
// - Tier 3: In-memory Android system properties (ro.surface_flinger.max_graphics_width)
// - Tier 4: Single-shot bounded fallback via wm size (only if tiers 1-3 fail)
// - RAM Caching: Probed once and stored in RAM for zero-latency per-frame access.
// ============================================================================

#include <cstdint>

struct SafeDisplayInfo {
    int32_t width = 1080;
    int32_t height = 2400;
    uint32_t orientation = 0; // 0=0deg, 1=90deg, 2=180deg, 3=270deg
};

namespace display_safe {
    // Queries screen dimensions safely using zero-fork sysfs and system properties first,
    // with guarded fallbacks. Completely isolated from volatile libgui.so binder calls.
    // Results are cached in RAM so calling from the 60fps render loop is instant.
    SafeDisplayInfo get_info(bool force_refresh = false);

    // Explicitly update orientation if device rotates. Fed by the render loop,
    // which reads it off the display binder — never by forking a shell.
    void update_orientation(uint32_t rot);
}

