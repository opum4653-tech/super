#pragma once
#include "common.hpp"

namespace aimbot {

    // 3D Silent trajectory calculation and steering
    // Steers the client-side bullet trajectory towards target without touching camera rotation
    inline void apply_trajectory(uint64_t localPlayer, const PlayerData& target, bool lockToHead) {
        if (!is_valid_ptr(localPlayer)) return;

        uint64_t aimInfo = rpm<uint64_t>(localPlayer + Offsets::Player::AimInfo);
        if (!is_valid_ptr(aimInfo)) {
            aimInfo = rpm<uint64_t>(localPlayer + 0xE58);
        }

        if (!is_valid_ptr(aimInfo)) return;

        uint64_t dirAddr = aimInfo + Offsets::Aim::Direction;
        Vector3 bulletOrigin = rpm<Vector3>(aimInfo + Offsets::Aim::StartPosition);
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
                target3D.y -= 0.30f; // Body / Chest on non-headshot rolls
            }
            // Direct head targeting (0.00m offset) when lockToHead is true
        }

        if (!std::isfinite(bulletOrigin.x) ||
            !std::isfinite(bulletOrigin.y) ||
            !std::isfinite(bulletOrigin.z) ||
            bulletOrigin.sqrMagnitude() <= 1e-4f) {
            return;
        }

        Vector3 diff = target3D - bulletOrigin;
        float targetDist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

        if (targetDist <= 1e-4f || !std::isfinite(targetDist)) return;

        Vector3 shotDir(diff.x / targetDist, diff.y / targetDist, diff.z / targetDist);

        // Instant trajectory write on Aim Silent (lerp = 1.0); smooth lerp on standard aim assist
        float lerpT = cfg::aim::silent ? 1.0f : (1.0f / std::max(1.0f, cfg::aim::smooth));

        if (s_smoothDir.sqrMagnitude() < 1e-6f || cfg::aim::silent) {
            s_smoothDir = shotDir;
        } else {
            s_smoothDir.x += (shotDir.x - s_smoothDir.x) * lerpT;
            s_smoothDir.y += (shotDir.y - s_smoothDir.y) * lerpT;
            s_smoothDir.z += (shotDir.z - s_smoothDir.z) * lerpT;
            float smLen = sqrtf(s_smoothDir.x * s_smoothDir.x + s_smoothDir.y * s_smoothDir.y + s_smoothDir.z * s_smoothDir.z);
            if (smLen > 1e-4f) {
                s_smoothDir.x /= smLen;
                s_smoothDir.y /= smLen;
                s_smoothDir.z /= smLen;
            }
        }

        // Distance-scaled ray direction: ray terminates directly at target contact position
        Vector3 finalRayDir(s_smoothDir.x * targetDist,
                           s_smoothDir.y * targetDist,
                           s_smoothDir.z * targetDist);

        wpm<Vector3>(dirAddr, finalRayDir);
        wpm<bool>(aimInfo + 0x71, false);
    }

} // namespace aimbot
