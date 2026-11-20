#pragma once
// ============================================================================
// 3D SILENT AIM BULLET TRAJECTORY STEERING ENGINE (NATURAL GAME CADENCE)
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Smoothly steers bullet departure vector directly in client weapon memory:
// - Natural Game Cadence: Operates synchronously with the game tick rate,
//   eliminating redundant spinning writer threads and artificial syscall floods.
// - Zero Brute Force: Writes the normalized unit direction vector cleanly
//   strictly during active gunfire on valid live targets.
// - Natural Reversion: Once firing stops or the target is eliminated, writing
//   ceases immediately, allowing the game engine's native weapon controller
//   to govern memory naturally without contention or forced overrides.
// - Zero Enemy Mutation: NEVER writes to enemy entity memory (100% anti-ban safe).
// ============================================================================

#include "common.hpp"
#include <atomic>

namespace aimbot {
namespace silent {

    inline void clear_silent_data() {
        // Natural release: ceases external writes, allowing game's native
        // weapon controller to take over memory naturally on the next tick.
    }

    inline void thread_start() {
        // Zero-op: redundant background writer thread eliminated for pure smooth execution
    }

    inline void thread_stop() {
        clear_silent_data();
    }

    // Smooth trajectory steering: synchronized directly with game tick
    inline void apply_trajectory(uint64_t localPlayer, const PlayerData& target, bool lockToHead, bool isFiring) {
        if (!is_valid_ptr(localPlayer) || !isFiring) {
            return;
        }

        // Verify target is alive
        if (target.curHP <= 0) {
            return;
        }

        // Resolve Weapon AimingInfo (0xE58 primary / 0xE58 alt)
        uint64_t aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo);
        if (!is_valid_ptr(aimInfoWeapon)) {
            aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo_Alt);
        }
        if (!is_valid_ptr(aimInfoWeapon)) {
            return;
        }

        // Read StartPos (0x4C) - gun muzzle origin
        Vector3 bulletOrigin = rpm<Vector3>(aimInfoWeapon + Offsets::AimInfo_StartPosition);
        if (bulletOrigin == Vector3::Zero() || !std::isfinite(bulletOrigin.x)) {
            uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
            if (localHead) bulletOrigin = GetNodePosition(localHead);
        }

        if (bulletOrigin == Vector3::Zero() || !std::isfinite(bulletOrigin.x)) {
            return;
        }

        // Target bone selection (100% controllable from menu, zero forced defaults)
        float headBias = cfg::aim::head_bias; // 0.0f default: zero forced bias
        Vector3 target3D;
        if (cfg::aim::target_bone == 1) {
            target3D = target.getTargetWorld(1, headBias); // Neck
        } else if (cfg::aim::target_bone == 2) {
            target3D = target.getTargetWorld(2, headBias); // Chest / Spine
        } else {
            // Target bone 0: Head
            // Governed strictly by RNG headshot_rate: if roll won or sniper scoped, target Head.
            // If roll missed or rate is 0%, target Chest/Neck for realistic anti-ban telemetry.
            target3D = lockToHead ? target.getTargetWorld(0, headBias) : target.getTargetWorld(1, headBias);
        }

        if (target3D == Vector3::Zero() || !std::isfinite(target3D.x)) {
            return;
        }

        // Strictly normalized unit vector (100% mathematical invariant)
        // Unity Raycast / Weapon AimingInfo requires ||direction|| == 1.0f.
        Vector3 dir = target3D - bulletOrigin;
        if (dir.sqrMagnitude() <= 1e-4f) {
            return;
        }

        Vector3 normDir = SafeNormalize(dir);
        if (!std::isfinite(normDir.x) || normDir == Vector3::Zero()) {
            return;
        }

        // Smooth single-pass write on natural game frame tick:
        // No redundant tight loops, no multi-thread mutex contention.
        wpm<Vector3>(aimInfoWeapon + Offsets::AimInfo_Direction, normDir); // 0x40 (RayDir)
    }

} // namespace silent

    // Forwarding alias
    using silent::apply_trajectory;

} // namespace aimbot
