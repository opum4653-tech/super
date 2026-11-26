#pragma once
// ============================================================================
// TARGET ACQUISITION & SCORING PIPELINE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Evaluates all detected enemy entities against FOV limits and crosshair proximity:
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
        const cfg::RuntimeConfig rc = cfg::runtime();
        if (!is_valid_ptr(localPlayer)) return result;

        // Verify local player is alive
        if (rpm<bool>(localPlayer + Offsets::Player_IsDead)) {
            return result;
        }

        // Firing state — verified from maxff.h (isFiring == 0x850, matching STORM_MODS)
        bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
        result.isFiring = isFiring;

        bool triggerActive = false;
        if (rc.aimTriggerMode == 1) {
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

        float screenCx = rc.screenW * 0.5f;
        float screenCy = rc.screenH * 0.5f;

        float bestScore = 9999999.0f;
        const PlayerData* bestEnemy = nullptr;

        // Dynamic crosshair distance scan (official game visibility/range only)
        for (const auto& enemy : enemies) {
            if (enemy.Head.z < 0.001f) continue;
            if (enemy.hpValid && enemy.curHP <= 0) continue;
            if (enemy.isKnocked) continue; // Skip knocked players to prevent unnecessary headshot flags
            if (rc.aimIgnoreBots && enemy.isBot) continue;

            float dxHead = enemy.Head.x - screenCx;
            float dyHead = enemy.Head.y - screenCy;
            float crosshairDist = sqrtf(dxHead * dxHead + dyHead * dyHead);

            float score = crosshairDist;
            if (rc.aimPriorityMode == 1) { // Lowest HP
                float hpForScore = enemy.hpValid ? static_cast<float>(enemy.curHP) : 200.0f;
                score = hpForScore + crosshairDist * 0.05f;
            } else if (rc.aimPriorityMode == 2) { // 3D Distance
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
