#pragma once
// ============================================================================
// NATIVE IMGUI MENU & OVERLAY USER INTERFACE FORWARDER
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Exposes the per-frame render entry point ui::menu::render().
// Called directly inside the 60fps render loop between drawBegin() and drawEnd()
// in src/main.cpp.
// ============================================================================

namespace ui::menu {
    void render();
}


