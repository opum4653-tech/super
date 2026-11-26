#pragma once
// ============================================================================
// MODULAR AIMBOT & TRAJECTORY ASSIST SUITE FORWARDER
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Canonical inclusion header for the aimbot subsystem.
// Forwards to src/func/aim/aim.hpp and establishes the namespace alias:
//   namespace func::aim = ::aimbot;
// ============================================================================

#include "aim/touch_aim.hpp"

namespace func {
    namespace aim = ::aimbot;
}

