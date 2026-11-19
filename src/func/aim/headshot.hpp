#pragma once
// ============================================================================
// HIT REGISTRATION & HEADSHOT PROBABILITY EVALUATOR
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Controls server-acknowledged hit registration and headshot rates:
// 1. Headshot Percentage Roll:
//    Evaluates user-configured headshot_rate (0% to 100%) via high-speed,
//    non-blocking Xorshift RNG seeded with process PID and time.
// 2. Raycast Collider Redirection (Strictly while firing):
//    Extracts HeadCollider (0x740 / 0x738 fallback) from target player entity
//    and writes it to:
//      - LockedAimingCollider (0x80 on AttackableEntity / ColliderBody)
//
// CRITICAL ARCHITECTURAL SAFETY INVARIANTS:
// 1. FIRE-GATED EXECUTION:
//    Collider mutation is STRICTLY executed only when the weapon is actively
//    firing (isFiring == true). During idle camera panning or passive FOV traversal,
//    zero memory writes are performed on enemy entities.
// 2. FORBIDDEN OFFSET 0x78 (CORRUPTION AUTOPSY):
//    In ARM64, writing an 8-byte pointer to offset 0x78 (LockedAimingCollider_Alt)
//    overwrote bytes 0x78 through 0x7F. Because Offsets::Player::IsDead resides
//    at 0x7C, the upper bytes of the heap pointer clobbered IsDead to non-zero (true).
//    Consequently, data::update() immediately dropped the enemy from data::players,
//    causing ESP boxes, tracer lines, and aim targets to vanish the instant the enemy
//    entered the FOV circle. Offset 0x78 is permanently deleted.
// ============================================================================

#include "common.hpp"

namespace aimbot {

    // Evaluates headshot rate roll (0% - 100%) and configures head collider overrides
    inline bool evaluate_headshot(const PlayerData& target, bool isScoping, bool isFiring) {

        float rate = cfg::aim::headshot_rate;
        bool isSniperShot = (cfg::aim::sniper_aim && isScoping);
        bool lockToHead = false;

        if (isSniperShot || rate >= 100.0f) {
            lockToHead = true;
        } else if (rate <= 0.0f) {
            lockToHead = false;
        } else {
            // Independent xorshift RNG roll against user's headshot_rate
            static uint32_t s_rollRng =
                0x9E3779B9u ^ static_cast<uint32_t>(time(nullptr)) ^
                (static_cast<uint32_t>(getpid()) * 2654435761u);
            s_rollRng ^= s_rollRng << 13;
            s_rollRng ^= s_rollRng >> 17;
            s_rollRng ^= s_rollRng << 5;
            lockToHead = ((s_rollRng % 100u) < static_cast<uint32_t>(rate));
        }

        // AIM COLLIDER OVERRIDE (Reference-aligned hit registration from STORM_MODS ColliderBody)
        // Strictly executed ONLY when actively firing (isFiring == true).
        // Writes HeadCollider (0x740 / 0x738) to LockedAimingCollider (0x80) ONLY.
        // Never mutates enemy memory during passive camera rotation.
        if (lockToHead && isFiring && is_valid_ptr(target.addr)) {
            uint64_t headCol = rpm<uint64_t>(target.addr + Offsets::Player::HeadCollider);     // 0x740
            if (!is_valid_ptr(headCol)) {
                headCol = rpm<uint64_t>(target.addr + Offsets::Player::HeadCollider_Alt); // 0x738
            }
            if (is_valid_ptr(headCol)) {
                wpm<uint64_t>(target.addr + Offsets::Player::LockedAimingCollider, headCol);     // 0x80
            }
        }

        return lockToHead;
    }

} // namespace aimbot
