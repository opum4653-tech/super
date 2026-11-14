#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "../game/player.hpp"
#include "../game/data.hpp"
#include "../offsets/offsets.hpp"
#include "../ui/cfg.hpp"
#include "../other/memory.hpp"
#include "../other/vector3.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <unistd.h>

extern float g_sw;
extern float g_sh;

// TRAJECTORY ASSIST ENGINE (NATURAL GAMEPLAY — ZERO CAMERA JUMPING)
// Features:
// 1. Natural Gameplay: Camera rotation is 100% untouched. Finger aiming is free.
// 2. Strict Target Stickiness & Hysteresis: ZERO screen shaking when multiple enemies enter FOV.
// 3. Grace-Period Burst Lock: Prevents lock resets between rapid bullet intervals.
// 4. Trajectory Write: only the client bullet direction is steered. Recoil is
//    never read, never scaled, never written — the game's own recoil runs as shipped.
// 5. Strict FOV Circle: Strictly respects FOV limit; never locks outside.
// 6. Wall Filter: a target behind cover is never acquired and never held. The
//    gate is the game's own AvatarManager visible flag, debounced in data.cpp.

namespace aimbot {

    struct TargetInfo {
        bool valid = false;
        char name[36] = {0};
        float distance = 0.0f;
        int hp = 0;
        Vector3 screenPos = Vector3::Zero();
        Vector3 headWorld = Vector3::Zero();
    };

    inline TargetInfo g_currentTarget;

    // Safe 3D vector normalization with division-by-zero & NaN protection
    inline Vector3 SafeNormalize(const Vector3& v) {
        float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lenSq < 1e-6f || !std::isfinite(lenSq)) {
            return Vector3(0.0f, 0.0f, 1.0f); // Default forward vector
        }
        float invLen = 1.0f / sqrtf(lenSq);
        return Vector3(v.x * invLen, v.y * invLen, v.z * invLen);
    }

    // Target stickiness with hysteresis & grace period to completely eliminate screen shaking
    inline uint64_t s_lockedTargetAddr = 0;
    inline int      s_lockTimeoutTicks = 0;

    // ── SMOOTH DIRECTION LERP STATE ──────────────────────────────────────────
    // Tracks the last written shot direction so lerp interpolates from it each frame.
    inline Vector3 s_smoothDir = Vector3::Zero();

    inline void update() {
        g_currentTarget.valid = false;
        if (!cfg::aim::enable) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            
            
            s_smoothDir = Vector3::Zero();
            return;
        }
        if (!data::inMatch.load(std::memory_order_relaxed)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            
            
            return;
        }

        uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
        if (!is_valid_ptr(localPlayer)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            
            
            return;
        }

        // Verify local player is alive
        if (rpm<bool>(localPlayer + Offsets::Player::IsDead)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            
            
            return;
        }

        // FIRING & SCOPING STATE
        bool isFiring = rpm<bool>(localPlayer + Offsets::Player::IsFiring);
        if (!isFiring) {
            isFiring = rpm<bool>(localPlayer + 0x850);
        }

        // FollowCamera is at localPlayer + 0x690. Inside FollowCamera, FOVOffset is at 0x84!
        // When scoping (especially sniper rifles like AWM, Kar98, M82B), FOVOffset shifts negative.
        uint64_t followCam = rpm<uint64_t>(localPlayer + Offsets::Player::Camera);
        float camFovOffset = 0.0f;
        if (is_valid_ptr(followCam)) {
            camFovOffset = rpm<float>(followCam + Offsets::Player::FOVOffset);
        }
        bool isScoping = (camFovOffset < -0.5f || camFovOffset > 0.5f);

        bool triggerActive = false;
        if (cfg::aim::trigger_mode == 1) {
            triggerActive = true; // Always In FOV
        } else if ((cfg::aim::scope_aim || cfg::aim::sniper_aim) && isScoping) {
            triggerActive = true; // Instant activation when scoping (Sniper rifles & Scoped guns)
        } else {
            triggerActive = isFiring;
        }

        if (!triggerActive) {
            // Grace period: keep aim trajectory active across rapid-fire bullet intervals
            if (s_lockTimeoutTicks > 0) {
                s_lockTimeoutTicks--;
            } else {
                s_lockedTargetAddr = 0;
                s_smoothDir = Vector3::Zero();
                return;
            }
        } else {
            // Active trigger -> refresh lock lease (approx 15-20 frames)
            s_lockTimeoutTicks = 16;
        }

        std::vector<PlayerData> enemies;
        {
            std::lock_guard<std::mutex> lk(data::players_mutex);
            enemies = data::players;
        }

        if (enemies.empty()) {
            s_lockedTargetAddr = 0;
            
            
            return;
        }

        float screenCx = g_sw * 0.5f;
        float screenCy = g_sh * 0.5f;
        float fovLimit = cfg::aim::fov_radius;
        const PlayerData* bestEnemy = nullptr;
        Vector3 bestScreenPos = Vector3::Zero();

        // 1. STRICT TARGET HYSTERESIS (STICK TO CURRENT TARGET INSIDE FOV CIRCLE)
        // If Target is locked, NEVER switch to another enemy as long as Target remains in FOV circle!
        if (s_lockedTargetAddr != 0) {
            for (const auto& enemy : enemies) {
                if (enemy.addr == s_lockedTargetAddr) {
                    // Wall filter on the sticky path too: a target that stepped
                    // behind cover drops the lock on the same pass the game
                    // clears its visible flag (debounced 2 frames in data.cpp,
                    // ~32 ms at the 16 ms read cadence). No direction write
                    // happens on a frame without a live visible target.
                    if (enemy.curHP > 0 && enemy.Head.z >= 0.05f && enemy.isVisible) {
                        float dxHead = enemy.Head.x - screenCx;
                        float dyHead = enemy.Head.y - screenCy;
                        float distHead = sqrtf(dxHead * dxHead + dyHead * dyHead);

                        float bodyX = (enemy.Head.x + enemy.Toe.x) * 0.5f;
                        float bodyY = (enemy.Head.y + enemy.Toe.y) * 0.5f;
                        float dxBody = bodyX - screenCx;
                        float dyBody = bodyY - screenCy;
                        float distBody = sqrtf(dxBody * dxBody + dyBody * dyBody);

                        float dist = std::min(distHead, distBody);
                        // Hysteresis boundary (1.35x FOV): never detach on micro-movements
                        if (dist <= fovLimit * 1.35f) {
                            bestEnemy = &enemy;
                            bestScreenPos = enemy.Head;
                        }
                    }
                    break;
                }
            }

            // If locked enemy died or left FOV circle, clear lock to allow new target acquisition
            if (!bestEnemy) {
                s_lockedTargetAddr = 0;
                
                
            }
        }

        // 2. NEW TARGET ACQUISITION (BASED ON SCREEN CENTER FOV CIRCLE)
        // Find closest enemy whose head or body enters the FOV circle (matching refer/internal logic)
        if (!bestEnemy) {
            float bestScore = 9999999.0f;
            for (const auto& enemy : enemies) {
                if (enemy.Head.z < 0.05f || enemy.curHP <= 0) continue;
                if (cfg::aim::ignore_bots && enemy.isBot) continue;

                // Unconditional wall filter. The AvatarManager visible flag is
                // the only occlusion signal a read-only external process has;
                // acquiring through cover is exactly the behaviour that gets an
                // account flagged, so there is no toggle for it.
                if (!enemy.isVisible) continue;
                if (enemy.distance > cfg::aim::max_distance) continue;

                // Test both Head and Body center against screen FOV circle
                float dxHead = enemy.Head.x - screenCx;
                float dyHead = enemy.Head.y - screenCy;
                float distHead = sqrtf(dxHead * dxHead + dyHead * dyHead);

                float bodyX = (enemy.Head.x + enemy.Toe.x) * 0.5f;
                float bodyY = (enemy.Head.y + enemy.Toe.y) * 0.5f;
                float dxBody = bodyX - screenCx;
                float dyBody = bodyY - screenCy;
                float distBody = sqrtf(dxBody * dxBody + dyBody * dyBody);

                float crosshairDist = std::min(distHead, distBody);

                // STRICT FOV CIRCLE: Only lock if inside circle
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
                    bestScreenPos = enemy.Head;
                }
            }

            if (bestEnemy) {
                s_lockedTargetAddr = bestEnemy->addr;
                s_lockTimeoutTicks = 30;
                
            }
        }

        if (!bestEnemy) {
            s_lockedTargetAddr = 0;
            
            
            return;
        }

        

        // Record target diagnostic telemetry
        g_currentTarget.valid = true;
        snprintf(g_currentTarget.name, sizeof(g_currentTarget.name), "%s",
                 bestEnemy->name[0] ? bestEnemy->name : (bestEnemy->isBot ? "BOT" : "Player"));
        g_currentTarget.distance = bestEnemy->distance;
        g_currentTarget.hp = static_cast<int>(bestEnemy->curHP);
        g_currentTarget.screenPos = bestScreenPos;
        g_currentTarget.headWorld = bestEnemy->headWorld;

        // 3. HEADSHOT PERCENTAGE RATE ENGINE (0% - 100% controlled from menu)
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

        // 4. AIM COLLIDER OVERRIDE (Reference-aligned hit detection)
        // Maps target's LockedAimingCollider (0x80) to HeadCollider (0x738 / 0x740)
        // Ensures the game engine's internal raycast registers bullet impacts as Headshots.
        if (lockToHead && is_valid_ptr(bestEnemy->addr)) {
            uint64_t headCol = rpm<uint64_t>(bestEnemy->addr + Offsets::Player::HeadCollider);
            if (!is_valid_ptr(headCol)) {
                headCol = rpm<uint64_t>(bestEnemy->addr + 0x740);
            }
            if (is_valid_ptr(headCol)) {
                wpm<uint64_t>(bestEnemy->addr + Offsets::Player::LockedAimingCollider, headCol);
            }
        }

        // 5. 3D TRAJECTORY STEERING (AIMING INFO RAY DIRECTION)
        uint64_t aimInfo = rpm<uint64_t>(localPlayer + Offsets::Player::AimInfo);
        if (!is_valid_ptr(aimInfo)) {
            aimInfo = rpm<uint64_t>(localPlayer + 0xE58);
        }

        if (is_valid_ptr(aimInfo)) {
            uint64_t dirAddr = aimInfo + Offsets::Aim::Direction;
            Vector3 bulletOrigin = rpm<Vector3>(aimInfo + Offsets::Aim::StartPosition);
            if (bulletOrigin == Vector3::Zero() || bulletOrigin.sqrMagnitude() < 0.01f) {
                uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
                if (localHead) bulletOrigin = GetNodePosition(localHead);
            }

            Vector3 target3D = bestEnemy->headWorld;
            if (target3D != Vector3::Zero()) {
                if (cfg::aim::target_bone == 1) {
                    target3D.y -= 0.15f; // Neck / Upper Chest
                } else if (cfg::aim::target_bone == 2) {
                    target3D.y -= 0.40f; // Body / Pelvis
                } else {
                    if (!lockToHead) {
                        target3D.y -= 0.30f; // Body / Chest on non-headshot rolls
                    }
                    // When lockToHead is true: direct exact Head bone target (0.00m offset)!
                }

                if (std::isfinite(bulletOrigin.x) &&
                    std::isfinite(bulletOrigin.y) &&
                    std::isfinite(bulletOrigin.z) &&
                    bulletOrigin.sqrMagnitude() > 1e-4f) {

                    Vector3 diff = target3D - bulletOrigin;
                    float targetDist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

                    if (targetDist > 1e-4f && std::isfinite(targetDist)) {
                        Vector3 shotDir(diff.x / targetDist, diff.y / targetDist, diff.z / targetDist);

                        float lerpT = 1.0f / std::max(1.0f, cfg::aim::smooth);

                        if (s_smoothDir.sqrMagnitude() < 1e-6f) {
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

                        // Scaled ray direction vector matching reference implementation:
                        // RayDir has exact magnitude of distance to target, terminating directly on head.
                        Vector3 finalRayDir(s_smoothDir.x * targetDist,
                                           s_smoothDir.y * targetDist,
                                           s_smoothDir.z * targetDist);

                        wpm<Vector3>(dirAddr, finalRayDir);
                        wpm<bool>(aimInfo + 0x71, false);
                    }
                }
            }
        }

        // NOTE: ZERO camera Rotation write!
        // Camera stays 100% untouched by code. User has complete, natural thumb control.
        // Screen will never snap, jump, or pull between multiple enemies!
    }

    // FOV CIRCLE & ACTIVE LOCK VISUALIZER
    // Scaled dynamically by cfg::aim::fov_radius
    inline void render_fov(ImDrawList* dl) {
        if (!cfg::aim::enable || !cfg::aim::show_fov) return;
        if (!data::inMatch.load(std::memory_order_relaxed)) return;

        float cx = g_sw * 0.5f;
        float cy = g_sh * 0.5f;
        float r  = cfg::aim::fov_radius;

        // Smooth Orchid / White FOV circle (shows exact trigger zone)
        dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 255, 255, 45), 64, 1.0f);
        dl->AddCircle(ImVec2(cx, cy), r + 1.0f, IM_COL32(225, 0, 153, 160), 64, 1.5f);

        // If target is actively locked inside FOV, draw target lock ring
        if (g_currentTarget.valid && g_currentTarget.screenPos.z >= 0.001f) {
            dl->AddCircle(ImVec2(g_currentTarget.screenPos.x, g_currentTarget.screenPos.y),
                          10.0f, IM_COL32(255, 30, 80, 240), 16, 2.0f);
            dl->AddCircle(ImVec2(g_currentTarget.screenPos.x, g_currentTarget.screenPos.y),
                          3.5f, IM_COL32(255, 255, 255, 255), 12, 1.5f);
        }
    }

} // namespace aimbot
