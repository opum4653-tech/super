#pragma once
// ============================================================================
// AIMBOT — STORM_MODS AimHardTh PATTERN (NO AIMFOV)
// ============================================================================
// Direct port of STORM_MODS AimBot (AimHardTh):
// - Target selection via crosshair distance (no AimFov limit).
// - Triggers on isFiring (0x850) or Weapon::IsSight (0x8A0) when enabled.
// - Writes Quaternion::LookRotation((headWorld - camPos).normalized()) to
//   localPlayer + Player_Rotation (0x61C) with STORM_MODS randomized 40-100ms
//   human-like pacing.
// ============================================================================

#include "common.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <sys/prctl.h>
#include <thread>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_aim_thread;

    struct TargetResult {
        PlayerData target;
        bool valid = false;
    };

    inline TargetResult select_target(uint64_t localPlayer, const cfg::RuntimeConfig& rc, uint64_t lockedAddr) {
        TargetResult result;
        if (!is_valid_ptr(localPlayer)) return result;
        if (rpm<bool>(localPlayer + Offsets::Player_IsDead)) return result;

        std::vector<PlayerData> enemies;
        {
            std::lock_guard<std::mutex> lk(data::players_mutex);
            enemies = data::players;
        }
        if (enemies.empty()) return result;

        const float screenCx = rc.screenW * 0.5f;
        const float screenCy = rc.screenH * 0.5f;

        float bestScore = 9999999.0f;
        const PlayerData* bestEnemy = nullptr;

        for (const auto& enemy : enemies) {
            if (enemy.Head.z < 0.001f) continue;
            if (enemy.hpValid && enemy.curHP <= 0) continue;
            if (enemy.isKnocked) continue;
            if (rc.aimIgnoreBots && enemy.isBot) continue;

            float dx = enemy.Head.x - screenCx;
            float dy = enemy.Head.y - screenCy;
            float score = sqrtf(dx * dx + dy * dy);
            if (lockedAddr != 0 && enemy.addr == lockedAddr) {
                score *= 0.72f;
            }

            if (score < bestScore) {
                bestScore = score;
                bestEnemy = &enemy;
            }
        }

        if (bestEnemy) {
            result.target = *bestEnemy;
            result.valid = true;
        }
        return result;
    }

    inline bool is_player_sighted(uint64_t localPlayer) {
        uint64_t weapon = rpm<uint64_t>(localPlayer + Offsets::Player_Weapon);
        if (!is_valid_ptr(weapon)) return false;
        return rpm<uint64_t>(weapon + Offsets::Weapon::IsSight) != 0;
    }

    inline void aim_loop() {
        prctl(PR_SET_NAME, oxorany("InputDispatcher"), 0, 0, 0);

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> trackSleep(28, 52);
        std::uniform_int_distribution<int> holdSleep(45, 85);
        std::uniform_real_distribution<float> stepDist(0.18f, 0.24f);
        uint64_t lockedAddr = 0;

        while (g_aim_running.load(std::memory_order_relaxed)) {
            const cfg::RuntimeConfig rc = cfg::runtime();
            const bool active = rc.master && rc.aimEnable &&
                                data::inMatch.load(std::memory_order_relaxed);
            const uint64_t localPlayer = active
                ? data::localPlayerAddr.load(std::memory_order_relaxed) : 0;

            if (active && is_valid_ptr(localPlayer)) {
                const bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
                const bool isSighted = (rc.aimTriggerMode == 1) ? is_player_sighted(localPlayer) : false;

                if (isFiring || isSighted) {
                    TargetResult acq = select_target(localPlayer, rc, lockedAddr);
                    if (acq.valid && acq.target.headWorld != Vector3::Zero()) {
                        lockedAddr = acq.target.addr;
                        matrix vm = data::get_view_matrix();
                        Vector3 camPos = GetCameraPosition(vm);
                        if (camPos != Vector3::Zero() && std::isfinite(camPos.x) &&
                            std::isfinite(camPos.y) && std::isfinite(camPos.z)) {
                            Vector3 dir = acq.target.headWorld - camPos;
                            if (dir.sqrMagnitude() > 1e-4f && std::isfinite(dir.x) &&
                                std::isfinite(dir.y) && std::isfinite(dir.z)) {
                                Quaternion targetRot = Quaternion::Normalize(
                                    Quaternion::LookRotation(dir.normalized()));
                                const uint64_t rotAddr = localPlayer + Offsets::Player_Rotation;
                                Quaternion curRot = rpm<Quaternion>(rotAddr);
                                const float curSq =
                                    curRot.x * curRot.x + curRot.y * curRot.y +
                                    curRot.z * curRot.z + curRot.w * curRot.w;
                                if (std::isfinite(curSq) && curSq >= 0.5f && curSq <= 1.5f) {
                                    const float dot = std::fabs(
                                        curRot.x * targetRot.x + curRot.y * targetRot.y +
                                        curRot.z * targetRot.z + curRot.w * targetRot.w);
                                    if (dot < 0.9999f && std::isfinite(dot)) {
                                        const float clampedDot = (dot > 1.0f) ? 1.0f : dot;
                                        const float theta = 2.0f * acosf(clampedDot);
                                        const float maxStep = stepDist(gen);
                                        const float t = (theta > maxStep && theta > 1e-4f)
                                            ? (maxStep / theta) : 1.0f;
                                        Quaternion writeRot = (t < 1.0f)
                                            ? Quaternion::Normalize(Quaternion::Slerp(curRot, targetRot, t))
                                            : targetRot;
                                        mem_guard::Scope guard(rotAddr, sizeof(writeRot));
                                        mem_write(rotAddr, &writeRot, sizeof(writeRot));
                                        wipe(&writeRot, sizeof(writeRot));
                                    }
                                    wipe(&curRot, sizeof(curRot));
                                    wipe(&targetRot, sizeof(targetRot));
                                    wipe(&dir, sizeof(dir));
                                    wipe(&camPos, sizeof(camPos));
                                    wipe(&vm, sizeof(vm));
                                    wipe(&acq, sizeof(acq));
                                    std::this_thread::sleep_for(std::chrono::milliseconds(
                                        (dot < 0.98f) ? trackSleep(gen) : holdSleep(gen)));
                                    continue;
                                }
                                wipe(&curRot, sizeof(curRot));
                                wipe(&targetRot, sizeof(targetRot));
                            }
                            wipe(&dir, sizeof(dir));
                        }
                        wipe(&camPos, sizeof(camPos));
                        wipe(&vm, sizeof(vm));
                    } else {
                        lockedAddr = 0;
                    }
                    wipe(&acq, sizeof(acq));
                } else {
                    lockedAddr = 0;
                }
            } else {
                lockedAddr = 0;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    inline void thread_start() {
        if (g_aim_running.exchange(true)) return;
        g_aim_thread = std::thread(aim_loop);
    }

    inline void thread_stop() {
        if (!g_aim_running.exchange(false)) return;
        if (g_aim_thread.joinable()) g_aim_thread.join();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
