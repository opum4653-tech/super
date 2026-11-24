#pragma once
// ============================================================================
// TARGET ACQUISITION, SELECTION & HYSTERESIS ENGINE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Evaluates all detected enemy entities against FOV limits, occlusion filters,
// user priority modes (Crosshair, Low HP, 3D Distance), and active trigger states.
//
// KEY ALGORITHMS:
// 1. Scoping Detection:
//    Reads FollowCamera (0x690) -> FOVOffset (0x84). Negative values indicate zoom.
// 2. Target Stickiness / Hysteresis:
//    Maintains active target lock across burst intervals within 1.35x FOV radius,
//    eliminating reticle hopping and erratic direction changes.
// 3. Knocked Player Penalty:
//    Penalizes downed/knocked enemies (+5000 score) so active standing threats
//    are always locked and neutralized first.
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
    // TARGET ACQUISITION & SCORING PIPELINE
    // ------------------------------------------------------------------------
    inline TargetAcquisitionResult acquire_target(uint64_t localPlayer) {
        TargetAcquisitionResult result;
        if (!is_valid_ptr(localPlayer)) return result;

        // Verify local player is alive
        if (rpm<bool>(localPlayer + Offsets::Player::IsDead)) {
            s_lockedTargetAddr = 0;
            return result;
        }

        // FIRING STATE — verified from maxff.h (isFiring == 0x850)
        // Read strictly as 1-byte bool to prevent adjacent struct field contamination
        bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player::IsFiring) != 0);
        result.isFiring = isFiring;

        bool triggerActive = false;
        if (cfg::aim::trigger_mode == 1) {
            triggerActive = true; // Always In FOV
        } else {
            triggerActive = isFiring; // When Firing
        }
        result.triggerActive = triggerActive;

        if (!triggerActive) {
            s_lockedTargetAddr = 0;
        }

        std::vector<PlayerData> enemies;
        {
            std::lock_guard<std::mutex> lk(data::players_mutex);
            enemies = data::players;
        }

        if (enemies.empty()) {
            s_lockedTargetAddr = 0;
            return result;
        }

        float screenCx = g_sw * 0.5f;
        float screenCy = g_sh * 0.5f;
        float fovLimit = cfg::aim::get_fov_radius();
        bool foundLocked = false;

        // 1. STRICT TARGET HYSTERESIS (STICK TO CURRENT TARGET INSIDE FOV CIRCLE)
        if (s_lockedTargetAddr != 0) {
            for (const auto& enemy : enemies) {
                if (enemy.addr == s_lockedTargetAddr) {
                    // The lock must obey the same filters as fresh acquisition.
                    // Matching STORM_MODS: do not gate aim on Uma_Visible flicker.
                    bool lockAllowed = enemy.curHP > 0 && enemy.Head.z >= 0.001f;
                    if (lockAllowed && cfg::aim::ignore_bots && enemy.isBot) lockAllowed = false;
                    if (lockAllowed && cfg::aim::max_distance > 0.0f &&
                        enemy.distance > cfg::aim::max_distance) lockAllowed = false;

                    if (lockAllowed) {
                        // Head-only: the drag shot always steers into the head,
                        // so score strictly on head distance. No Toe dependency.
                        float dxHead = enemy.Head.x - screenCx;
                        float dyHead = enemy.Head.y - screenCy;
                        float dist = sqrtf(dxHead * dxHead + dyHead * dyHead);

                        if (fovLimit <= 0.0f || dist <= fovLimit * 1.35f) {
                            result.target = enemy;
                            result.bestScreenPos = enemy.Head;
                            result.valid = true;
                            foundLocked = true;
                        }
                    }
                    break;
                }
            }

            if (!foundLocked) {
                s_lockedTargetAddr = 0;
            }
        }

        // 2. NEW TARGET ACQUISITION (BASED ON SCREEN CENTER FOV CIRCLE — MATCHING STORM_MODS)
        if (!foundLocked) {
            float bestScore = 9999999.0f;
            const PlayerData* bestEnemy = nullptr;

            for (const auto& enemy : enemies) {
                if (enemy.Head.z < 0.001f || enemy.curHP <= 0) continue;
                if (cfg::aim::ignore_bots && enemy.isBot) continue;

                // Matching STORM_MODS: FOV proximity targeting without frustum culling drops.
                if (cfg::aim::max_distance > 0.0f && enemy.distance > cfg::aim::max_distance) continue;

                // Head-only scoring matches the 100% head drag shot.
                float dxHead = enemy.Head.x - screenCx;
                float dyHead = enemy.Head.y - screenCy;
                float crosshairDist = sqrtf(dxHead * dxHead + dyHead * dyHead);
                if (fovLimit > 0.0f && crosshairDist > fovLimit) continue;

                float score = crosshairDist;
                if (cfg::aim::priority_mode == 1) { // Lowest HP, distance-aware
                    // Pure HP scoring could lock a 1-HP enemy on the far edge of
                    // the FOV while a healthy enemy sat under the crosshair.
                    // Add a bounded screen-distance penalty (max 50 HP-units)
                    // so low HP still wins, but not across the whole FOV.
                    float fovNorm = (fovLimit > 0.0f) ? (crosshairDist / fovLimit) : 0.0f;
                    if (fovNorm > 1.0f) fovNorm = 1.0f;
                    score = (float)enemy.curHP + fovNorm * 50.0f;
                } else if (cfg::aim::priority_mode == 2) { // 3D Distance
                    score = enemy.distance;
                }

                // Prioritize standing threats over crawling/knocked players
                if (enemy.isKnocked) {
                    score += 5000.0f;
                }

                if (score < bestScore) {
                    bestScore = score;
                    bestEnemy = &enemy;
                    result.bestScreenPos = enemy.Head;
                }
            }

            if (bestEnemy) {
                result.target = *bestEnemy;
                result.valid = true;
                s_lockedTargetAddr = result.target.addr;
            }
        }

        if (!result.valid) {
            s_lockedTargetAddr = 0;
            s_smoothDir = Vector3::Zero();
        }

        return result;
    }

} // namespace aimbot
