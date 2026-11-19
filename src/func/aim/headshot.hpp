#pragma once
// ============================================================================
// HIT REGISTRATION & HEADSHOT PROBABILITY EVALUATOR (100% STEALTH PROTOCOL)
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Controls server-acknowledged hit registration and headshot rates:
// 1. Headshot Percentage Roll:
//    Evaluates user-configured headshot_rate (0% to 100%) via high-speed,
//    non-blocking Xorshift RNG.
// 2. Anti-Ban Zero-Mutation Stealth:
//    NEVER writes to enemy memory (0x80 LockedAimingCollider is 100% read-only).
//    Mutating another player's entity in client RAM is detected by Free Fire's
//    integrity watchdog, causing account bans.
//    Instead, clean trajectory steering directs bullets into the head collider
//    via physical raycast intersection without touching enemy heap objects.
// ============================================================================

#include "common.hpp"

namespace aimbot {

    // Evaluates headshot rate roll (0% - 100%)
    inline bool evaluate_headshot(const PlayerData& target, bool isScoping, bool isFiring) {
        float rate = cfg::aim::headshot_rate;
        bool isSniperShot = (cfg::aim::sniper_aim && isScoping);

        if (isSniperShot || rate >= 100.0f) {
            return true;
        }
        if (rate <= 0.0f) {
            return false;
        }

        // Independent xorshift RNG roll against user's headshot_rate
        static uint32_t s_rollRng =
            0x9E3779B9u ^ static_cast<uint32_t>(time(nullptr)) ^
            (static_cast<uint32_t>(getpid()) * 2654435761u);
        s_rollRng ^= s_rollRng << 13;
        s_rollRng ^= s_rollRng >> 17;
        s_rollRng ^= s_rollRng << 5;
        return ((s_rollRng % 100u) < static_cast<uint32_t>(rate));
    }

} // namespace aimbot
