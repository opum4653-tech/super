#pragma once
#include "common.hpp"

namespace aimbot {

    // 3D Silent bullet trajectory calculation and steering (matching STORM_MODS)
    // Directly steers client-side bullet trajectory without touching camera rotation or weapon recoil.
    inline void apply_trajectory(uint64_t localPlayer, const PlayerData& target, bool lockToHead) {
        if (!is_valid_ptr(localPlayer)) return;

        // Dual resolution: Weapon AimingInfo (0xE58) and Auxiliary AimingInfo (0xE50)
        uint64_t aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player::AimInfo);     // 0xE58
        uint64_t aimInfoAux    = rpm<uint64_t>(localPlayer + Offsets::Player::AimInfo_Alt); // 0xE50

        if (!is_valid_ptr(aimInfoWeapon) && !is_valid_ptr(aimInfoAux)) return;

        uint64_t primaryAimInfo = is_valid_ptr(aimInfoWeapon) ? aimInfoWeapon : aimInfoAux;

        // Read start position (0x4C). Fall back to head position if uninitialized.
        Vector3 bulletOrigin = rpm<Vector3>(primaryAimInfo + Offsets::Aim::StartPosition);
        if (bulletOrigin == Vector3::Zero() || bulletOrigin.sqrMagnitude() < 0.01f) {
            uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
            if (localHead) bulletOrigin = GetNodePosition(localHead);
        }

        Vector3 target3D = target.headWorld;
        if (target3D == Vector3::Zero()) return;

        // Apply target bone height adjustments
        if (cfg::aim::target_bone == 1) {
            target3D.y -= 0.15f; // Neck / Upper Chest
        } else if (cfg::aim::target_bone == 2) {
            target3D.y -= 0.40f; // Body / Pelvis
        } else {
            if (!lockToHead) {
                target3D.y -= 0.28f; // Chest on non-headshot rolls
            }
        }

        if (!std::isfinite(bulletOrigin.x) ||
            !std::isfinite(bulletOrigin.y) ||
            !std::isfinite(bulletOrigin.z) ||
            bulletOrigin.sqrMagnitude() <= 1e-4f) {
            return;
        }

        // Direct trajectory vector (dir = targetPos - startPos, matching reference engine)
        Vector3 dir = target3D - bulletOrigin;
        if (dir.sqrMagnitude() <= 1e-4f) return;

        // Write directly to LastAimingInfoFromWeapon (0xE58 + 0x40)
        if (is_valid_ptr(aimInfoWeapon)) {
            wpm<Vector3>(aimInfoWeapon + Offsets::Aim::Direction, dir);
        }

        // Also write to Auxiliary AimingInfo (0xE50 + 0x40) if registered
        if (is_valid_ptr(aimInfoAux) && aimInfoAux != aimInfoWeapon) {
            wpm<Vector3>(aimInfoAux + Offsets::Aim::Direction, dir);
        }
    }

} // namespace aimbot
