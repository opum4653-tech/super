#pragma once
#include "common.hpp"

namespace aimbot {

    // 3D Silent bullet trajectory calculation and steering
    // Directly steers client-side bullet trajectory without touching camera rotation or weapon recoil.
    inline void apply_trajectory(uint64_t localPlayer, const PlayerData& target, bool lockToHead, bool isFiring) {
        if (!is_valid_ptr(localPlayer)) return;

        // Trajectory write should only execute when weapon is actively firing (or when trigger mode is Always In FOV)
        if (!isFiring && cfg::aim::trigger_mode != 1) return;

        // Resolve Weapon AimingInfo (0xE58)
        uint64_t aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player::AimInfo);
        if (!is_valid_ptr(aimInfoWeapon)) {
            // Fallback to auxiliary only if primary weapon AimInfo is uninitialized
            aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player::AimInfo_Alt);
        }
        if (!is_valid_ptr(aimInfoWeapon)) return;

        // Read start position (0x4C). Must be a valid world coordinate.
        Vector3 bulletOrigin = rpm<Vector3>(aimInfoWeapon + Offsets::Aim::StartPosition);
        if (bulletOrigin == Vector3::Zero() || bulletOrigin.sqrMagnitude() < 0.01f || !std::isfinite(bulletOrigin.x)) {
            uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
            if (localHead) bulletOrigin = GetNodePosition(localHead);
        }

        if (bulletOrigin == Vector3::Zero() || bulletOrigin.sqrMagnitude() < 0.01f ||
            !std::isfinite(bulletOrigin.x) || !std::isfinite(bulletOrigin.y) || !std::isfinite(bulletOrigin.z)) {
            return; // Never compute trajectory from invalid/zero origin
        }

        Vector3 target3D = target.headWorld;
        if (target3D == Vector3::Zero() || !std::isfinite(target3D.x) || !std::isfinite(target3D.y) || !std::isfinite(target3D.z)) {
            return;
        }

        // Apply target bone height adjustments
        if (cfg::aim::target_bone == 1) {
            target3D.y -= 0.15f; // Neck / Upper Chest
        } else if (cfg::aim::target_bone == 2) {
            target3D.y -= 0.40f; // Body / Pelvis
        } else {
            // Target bone 0 (Head):
            if (!lockToHead) {
                target3D.y -= 0.28f; // Chest on non-headshot rolls
            }
            // When lockToHead is true, target3D stays strictly at target.headWorld
        }

        // Compute delta vector
        Vector3 diff = target3D - bulletOrigin;
        if (diff.sqrMagnitude() <= 1e-4f) return;

        // CRITICAL: The Direction vector at AimingInfo+0x40 MUST BE A NORMALIZED UNIT VECTOR (|v| == 1.0).
        // Passing an unnormalized distance vector (magnitude 20m..100m) into Unity's LookRotation causes
        // camera rotation matrices to produce NaN / extreme values, breaking character and camera angles.
        Vector3 shotDir = SafeNormalize(diff);
        if (!std::isfinite(shotDir.x) || !std::isfinite(shotDir.y) || !std::isfinite(shotDir.z) || shotDir == Vector3::Zero()) {
            return;
        }

        // Write unit direction vector to LastAimingInfoFromWeapon (0xE58 + 0x40)
        wpm<Vector3>(aimInfoWeapon + Offsets::Aim::Direction, shotDir);

        // Clear adjusted / interpolation flag (0x71) to ensure instant bullet alignment without camera warping
        wpm<bool>(aimInfoWeapon + 0x71, false);
    }

} // namespace aimbot
