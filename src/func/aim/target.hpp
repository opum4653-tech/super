#pragma once
// ============================================================================
// TARGET ACQUISITION & SCORING PIPELINE (MATCHING STORM_MODS EXACT ENGINE)
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Evaluates all detected enemy entities against FOV limits and crosshair proximity.
// Matches STORM_MODS Server.cpp lines 1123-1170:
// - Dynamic crosshair distance scoring on every frame (zero sticky lock).
// - When the crosshair is moved away or the enemy leaves FOV, lock immediately releases.
// ============================================================================

#include "common.hpp"

namespace aimbot {

    // Output structure returned by acquire_target()
    struct TargetAcquisitionResult {
        PlayerData target;                      // Selected enemy entity snapshot
        Vector3 bestScreenPos = Vector3::Zero();// 2D screen coordinate of lock point
        bool isFiring = false;                  // True if local player is shooting weapon
        bool triggerActive = false;             // True if aim assist should be actively engaged
        bool valid = false;                     // True if a qualified target was successfully acquired
    };

    // ------------------------------------------------------------------------
    // TARGET ACQUISITION & SCORING PIPELINE (STORM_MODS PATTERN)
    // ------------------------------------------------------------------------
    inline TargetAcquisitionResult acquire_target(uint64_t localPlayer) {
        TargetAcquisitionResult result;
        if (!is_valid_ptr(localPlayer)) return result;

        // Verify local player is alive
        if (rpm<bool>(localPlayer + Offsets::Player_IsDead)) {
            return result;
        }

        // Firing state — verified from maxff.h (isFiring == 0x850, matching STORM_MODS)
        bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
        result.isFiring = isFiring;

        bool triggerActive = false;
        if (cfg::aim::trigger_mode == 1) {
            triggerActive = true; // Always In FOV
        } else {
            triggerActive = isFiring; // When Firing (Default & Recommended)
        }
        result.triggerActive = triggerActive;

        std::vector<PlayerData> enemies;
        {
            std::lock_guard<std::mutex> lk(data::players_mutex);
            enemies = data::players;
        }

        if (enemies.empty()) {
            return result;
        }

        float screenCx = g_sw * 0.5f;
        float screenCy = g_sh * 0.5f;
        float fovLimit = cfg::aim::get_fov_radius();

        float bestScore = 9999999.0f;
        const PlayerData* bestEnemy = nullptr;

        // Dynamic crosshair distance scan (matches STORM_MODS lines 1123-1170)
        for (const auto& enemy : enemies) {
            // Must be alive and in front of camera
            if (enemy.Head.z < 0.001f || enemy.curHP <= 0) continue;
            if (cfg::aim::ignore_bots && enemy.isBot) continue;
            if (cfg::aim::max_distance > 0.0f && enemy.distance > cfg::aim::max_distance) continue;

            // Screen boundary check (matches STORM_MODS line 1137)
            if (enemy.Head.x < 0 || enemy.Head.x > g_sw ||
                enemy.Head.y < 0 || enemy.Head.y > g_sh) continue;

            float dxHead = enemy.Head.x - screenCx;
            float dyHead = enemy.Head.y - screenCy;
            float crosshairDist = sqrtf(dxHead * dxHead + dyHead * dyHead);

            // FOV circle filter (matches STORM_MODS isWithinFOV)
            if (fovLimit > 0.0f && crosshairDist > fovLimit) continue;

            float score = crosshairDist;
            if (cfg::aim::priority_mode == 1) { // Lowest HP
                float fovNorm = (fovLimit > 0.0f) ? (crosshairDist / fovLimit) : 0.0f;
                if (fovNorm > 1.0f) fovNorm = 1.0f;
                score = (float)enemy.curHP + fovNorm * 50.0f;
            } else if (cfg::aim::priority_mode == 2) { // 3D Distance
                score = enemy.distance;
            }

            // Standing enemies take precedence over crawling/knocked enemies
            if (enemy.isKnocked) {
                score += 5000.0f;
            }

            if (score < bestScore) {
                bestScore = score;
                bestEnemy = &enemy;
            }
        }

        if (bestEnemy) {
            result.target = *bestEnemy;
            result.bestScreenPos = bestEnemy->Head;
            result.valid = true;
        }

        return result;
    }

} // namespace aimbot
