#pragma once
#include "common.hpp"

namespace aimbot {

    // Evaluates headshot rate roll (0% - 100%) and configures head collider overrides
    inline bool evaluate_headshot(const PlayerData& target, bool isScoping) {
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

        // AIM COLLIDER OVERRIDE (Reference-aligned hit registration)
        // Maps target's LockedAimingCollider (0x80) to HeadCollider (0x738 / 0x740)
        // Ensures the game engine's internal raycast registers bullet impacts as Headshots.
        if (lockToHead && is_valid_ptr(target.addr)) {
            uint64_t headCol = rpm<uint64_t>(target.addr + Offsets::Player::HeadCollider);
            if (!is_valid_ptr(headCol)) {
                headCol = rpm<uint64_t>(target.addr + 0x740);
            }
            if (is_valid_ptr(headCol)) {
                wpm<uint64_t>(target.addr + Offsets::Player::LockedAimingCollider, headCol);
            }
        }

        return lockToHead;
    }

} // namespace aimbot
