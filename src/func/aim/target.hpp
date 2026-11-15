#pragma once
#include "common.hpp"

namespace aimbot {

    struct TargetAcquisitionResult {
        PlayerData target;
        Vector3 bestScreenPos = Vector3::Zero();
        bool isScoping = false;
        bool isFiring = false;
        bool triggerActive = false;
        bool valid = false;
    };

    inline TargetAcquisitionResult acquire_target(uint64_t localPlayer) {
        TargetAcquisitionResult result;
        if (!is_valid_ptr(localPlayer)) return result;

        // Verify local player is alive
        if (rpm<bool>(localPlayer + Offsets::Player::IsDead)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            return result;
        }

        // FIRING & SCOPING STATE
        bool isFiring = rpm<bool>(localPlayer + Offsets::Player::IsFiring);
        if (!isFiring) {
            isFiring = rpm<bool>(localPlayer + 0x850);
        }
        result.isFiring = isFiring;

        // FollowCamera is at localPlayer + 0x690. Inside FollowCamera, FOVOffset is at 0x84!
        // When scoping (especially sniper rifles like AWM, Kar98, M82B), FOVOffset shifts negative.
        uint64_t followCam = rpm<uint64_t>(localPlayer + Offsets::Player::Camera);
        float camFovOffset = 0.0f;
        if (is_valid_ptr(followCam)) {
            camFovOffset = rpm<float>(followCam + Offsets::Player::FOVOffset);
        }
        result.isScoping = (camFovOffset < -0.5f || camFovOffset > 0.5f);

        bool triggerActive = false;
        if (cfg::aim::trigger_mode == 1) {
            triggerActive = true; // Always In FOV
        } else if ((cfg::aim::scope_aim || cfg::aim::sniper_aim) && result.isScoping) {
            triggerActive = true; // Instant activation when scoping
        } else {
            triggerActive = isFiring;
        }
        result.triggerActive = triggerActive;

        if (triggerActive) {
            s_lockTimeoutTicks = 30; // Active trigger: refresh lock lease
        } else if (s_lockTimeoutTicks > 0) {
            s_lockTimeoutTicks--;    // Grace period across rapid-fire intervals
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
                    if (enemy.curHP > 0 && enemy.Head.z >= 0.05f && (cfg::aim::silent || enemy.isVisible)) {
                        float dxHead = enemy.Head.x - screenCx;
                        float dyHead = enemy.Head.y - screenCy;
                        float distHead = sqrtf(dxHead * dxHead + dyHead * dyHead);

                        float bodyX = (enemy.Head.x + enemy.Toe.x) * 0.5f;
                        float bodyY = (enemy.Head.y + enemy.Toe.y) * 0.5f;
                        float dxBody = bodyX - screenCx;
                        float dyBody = bodyY - screenCy;
                        float distBody = sqrtf(dxBody * dxBody + dyBody * dyBody);

                        float dist = std::min(distHead, distBody);
                        if (dist <= fovLimit * 1.35f) {
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
                if (enemy.Head.z < 0.05f || enemy.curHP <= 0) continue;
                if (cfg::aim::ignore_bots && enemy.isBot) continue;

                // For Silent Aim, acquire enemies within FOV circle even if Uma_Visible is occluded/brush (matching STORM_MODS)
                if (!cfg::aim::silent && !enemy.isVisible) continue;
                if (enemy.distance > cfg::aim::max_distance) continue;

                float dxHead = enemy.Head.x - screenCx;
                float dyHead = enemy.Head.y - screenCy;
                float distHead = sqrtf(dxHead * dxHead + dyHead * dyHead);

                float bodyX = (enemy.Head.x + enemy.Toe.x) * 0.5f;
                float bodyY = (enemy.Head.y + enemy.Toe.y) * 0.5f;
                float dxBody = bodyX - screenCx;
                float dyBody = bodyY - screenCy;
                float distBody = sqrtf(dxBody * dxBody + dyBody * dyBody);

                float crosshairDist = std::min(distHead, distBody);
                if (crosshairDist > fovLimit) continue;

                float score = crosshairDist;
                if (cfg::aim::priority_mode == 1) { // Lowest HP
                    score = (float)enemy.curHP;
                } else if (cfg::aim::priority_mode == 2) { // 3D Distance
                    score = enemy.distance;
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
                s_lockTimeoutTicks = 30;
            }
        }

        if (!result.valid) {
            s_lockedTargetAddr = 0;
            s_smoothDir = Vector3::Zero();
        }

        return result;
    }

} // namespace aimbot
