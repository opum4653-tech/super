#pragma once
// ============================================================================
// TOUCH AIM — ZOOM-AWARE ZERO-WRITE UINPUT TYPE-B HEAD ASSIST (FREE FIRE MAX)
// ============================================================================
// Independent from any memory-write aimbot (0 game memory writes).
// Uses real game offsets:
// - Head (0x6A8), ViewMatrix, Player_IsFiring (0x850), Player_IsDead (0x7C)
// - Player_Weapon (0x608) -> Weapon::IsSight (0x8A0) for zoom/scope detection
// Injects MT Protocol Type-B uinput drags via Touch.cpp (slot 8) with bounded
// proportional control so crosshair converges smoothly onto head without spin.
// ============================================================================

#include "common.hpp"
#include "Android_touch/TouchHelperA.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <stdarg.h>
#include <stdio.h>
#include <sys/prctl.h>

namespace touch_aim {

    inline std::atomic<bool> g_touch_running{false};
    inline std::thread       g_touch_thread;

    constexpr int            kTouchAimSlot = 8;
    inline float             s_fingerX = 0.0f;
    inline float             s_fingerY = 0.0f;
    inline uint64_t          s_lastGen = 0;
    inline std::chrono::steady_clock::time_point s_lastLogTime{};

    inline void write_diag_log(const char* fmt, ...) {
        const char* paths[] = {
            "/storage/emulated/0/Download/touch_aim_log.txt",
            "/data/media/0/Download/touch_aim_log.txt",
            "/sdcard/Download/touch_aim_log.txt"
        };
        FILE* f = nullptr;
        for (const char* p : paths) {
            f = fopen(p, "a");
            if (f) break;
        }
        if (!f) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fprintf(f, "%s\n", buf);
        fclose(f);
    }

    inline void release_finger() {
        if (IsFingerDown(kTouchAimSlot)) {
            Touch_Up_Slot(kTouchAimSlot);
        }
        s_fingerX = 0.0f;
        s_fingerY = 0.0f;
    }

    inline bool is_player_scoped(uint64_t localPlayer) {
        uint64_t weapon = rpm<uint64_t>(localPlayer + Offsets::Player_Weapon);
        if (!is_valid_ptr(weapon)) return false;
        return rpm<uint64_t>(weapon + Offsets::Weapon::IsSight) != 0;
    }

    struct TouchTargetResult {
        PlayerData target;
        Vector3 bestScreenPos = Vector3::Zero();
        bool valid = false;
    };

    inline TouchTargetResult select_touch_target(uint64_t localPlayer,
                                                   const cfg::RuntimeConfig& rc) {
        TouchTargetResult result;
        if (!is_valid_ptr(localPlayer)) return result;
        if (rpm<bool>(localPlayer + Offsets::Player_IsDead)) return result;

        std::vector<PlayerData> enemies;
        {
            std::lock_guard<std::mutex> lk(data::players_mutex);
            enemies = data::players;
        }
        if (enemies.empty()) return result;

        const matrix vm = data::get_view_matrix();
        const float screenCx = rc.screenW * 0.5f;
        const float screenCy = rc.screenH * 0.5f;

        float bestScore = 9999999.0f;
        const PlayerData* bestEnemy = nullptr;
        Vector3 bestScreen = Vector3::Zero();

        for (const auto& enemy : enemies) {
            if (enemy.Head.z < 0.5f) continue;
            if (enemy.hpValid && enemy.curHP <= 0) continue;
            if (enemy.isKnocked) continue;
            if (rc.touchIgnoreBots && enemy.isBot) continue;

            // Project fresh headWorld against latest ViewMatrix for zoom accuracy
            Vector3 proj = WorldToScreenPoint(vm, enemy.headWorld);
            if (proj.z < 0.5f || !std::isfinite(proj.x) || !std::isfinite(proj.y)) {
                continue;
            }
            // Reject off-screen / near-plane blowups
            if (proj.x < 0.0f || proj.x > rc.screenW || proj.y < 0.0f || proj.y > rc.screenH) {
                continue;
            }

            float dxHead = proj.x - screenCx;
            float dyHead = proj.y - screenCy;
            float crosshairDist = sqrtf(dxHead * dxHead + dyHead * dyHead);

            float score = crosshairDist;
            if (rc.touchPriorityMode == 1) {
                float hpForScore = enemy.hpValid ? static_cast<float>(enemy.curHP) : 200.0f;
                score = hpForScore + crosshairDist * 0.05f;
            } else if (rc.touchPriorityMode == 2) {
                score = enemy.distance;
            }

            if (score < bestScore) {
                bestScore = score;
                bestEnemy = &enemy;
                bestScreen = proj;
            }
        }

        if (bestEnemy) {
            result.target = *bestEnemy;
            result.bestScreenPos = bestScreen;
            result.valid = true;
        }
        return result;
    }

    inline void step_touch_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const TouchTargetResult& acq,
                               bool isFiring,
                               bool triggerActive,
                               uint64_t stateGen) {
        const auto now = std::chrono::steady_clock::now();
        const bool logDue = (s_lastLogTime.time_since_epoch().count() == 0) ||
            (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastLogTime).count() >= 500);

        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.5f ||
            !is_valid_ptr(localPlayer) || !Touch_IsUinputReady()) {
            if (logDue) {
                s_lastLogTime = now;
                write_diag_log("IDLE uinput=%d lp=%d valid=%d firing=%d trig=%d screen=%.0fx%.0f",
                               Touch_IsUinputReady() ? 1 : 0,
                               is_valid_ptr(localPlayer) ? 1 : 0,
                               acq.valid ? 1 : 0,
                               isFiring ? 1 : 0,
                               triggerActive ? 1 : 0,
                               rc.screenW, rc.screenH);
            }
            release_finger();
            return;
        }

        if (stateGen == s_lastGen) return;
        s_lastGen = stateGen;

        const bool scoped = is_player_scoped(localPlayer);
        const float cx = rc.screenW * 0.5f;
        const float cy = rc.screenH * 0.5f;
        const float dx = acq.bestScreenPos.x - cx;
        const float dy = acq.bestScreenPos.y - cy;
        const float err = sqrtf(dx * dx + dy * dy);
        if (!std::isfinite(err)) {
            release_finger();
            return;
        }

        if (err < 4.0f) {
            return; // Already aligned with head; keep finger steady
        }

        const float anchorX = rc.screenW * 0.62f;
        const float anchorY = rc.screenH * 0.28f;
        const float minX = rc.screenW * 0.44f, maxX = rc.screenW * 0.88f;
        const float minY = rc.screenH * 0.10f, maxY = rc.screenH * 0.65f;

        if (!IsFingerDown(kTouchAimSlot) || s_fingerX <= 0.0f) {
            s_fingerX = anchorX;
            s_fingerY = anchorY;
            Touch_Down_Slot(kTouchAimSlot, s_fingerX, s_fingerY);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // Calibrated touch-to-screen gain (measured from device response:
        // ~1px touch moves ~6-10px on screen -> 0.14 hipfire, 0.22 scoped)
        const float userScale = (rc.touchStrength > 0.0f) ? rc.touchStrength : 1.0f;
        const float baseGain = scoped ? 0.22f : 0.14f;
        const float gain = baseGain * userScale;

        float stepX = dx * gain;
        float stepY = dy * gain;
        const float stepLen = sqrtf(stepX * stepX + stepY * stepY);
        const float maxStep = 45.0f * userScale;
        if (stepLen > maxStep && stepLen > 1e-4f) {
            const float s = maxStep / stepLen;
            stepX *= s;
            stepY *= s;
        }

        float nx = s_fingerX + stepX;
        float ny = s_fingerY + stepY;
        if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
            Touch_Up_Slot(kTouchAimSlot);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            s_fingerX = anchorX;
            s_fingerY = anchorY;
            Touch_Down_Slot(kTouchAimSlot, s_fingerX, s_fingerY);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            nx = s_fingerX + stepX;
            ny = s_fingerY + stepY;
        }
        s_fingerX = nx;
        s_fingerY = ny;
        Touch_Move_Slot(kTouchAimSlot, s_fingerX, s_fingerY);

        if (logDue) {
            s_lastLogTime = now;
            write_diag_log("MOVE scoped=%d err=%.1f dx=%.1f dy=%.1f step=(%.1f,%.1f) finger=(%.1f,%.1f) gain=%.2f",
                           scoped ? 1 : 0, err, dx, dy, stepX, stepY, s_fingerX, s_fingerY, gain);
        }
    }

    inline void touch_loop() {
        prctl(PR_SET_NAME, oxorany("Binder:touchaim"), 0, 0, 0);
        uint64_t lastState = cfg::g_state_generation;
        std::unique_lock<std::mutex> waitLock(cfg::g_runtime_mutex);

        while (g_touch_running.load(std::memory_order_relaxed)) {
            cfg::g_state_cv.wait(waitLock, [&]() {
                return !g_touch_running.load(std::memory_order_relaxed) ||
                       cfg::g_state_generation != lastState;
            });
            if (!g_touch_running.load(std::memory_order_relaxed)) break;
            lastState = cfg::g_state_generation;
            waitLock.unlock();

            const cfg::RuntimeConfig rc = cfg::runtime();
            const bool active = rc.master && rc.touchEnable &&
                                data::inMatch.load(std::memory_order_relaxed);
            const uint64_t localPlayer = active
                ? data::localPlayerAddr.load(std::memory_order_relaxed) : 0;

            if (!active || !is_valid_ptr(localPlayer)) {
                release_finger();
                waitLock.lock();
                continue;
            }

            auto acq = select_touch_target(localPlayer, rc);
            const bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            const bool triggerActive = (rc.touchTriggerMode == 1) || isFiring;

            step_touch_aim(localPlayer, rc, acq, isFiring, triggerActive, lastState);

            waitLock.lock();
        }

        waitLock.unlock();
        release_finger();
    }

    inline void thread_start() {
        if (g_touch_running.exchange(true)) return;
        write_diag_log("=== touch_aim started (uinput=%d) ===", Touch_IsUinputReady() ? 1 : 0);
        g_touch_thread = std::thread(touch_loop);
    }

    inline void thread_stop() {
        if (!g_touch_running.exchange(false)) return;
        cfg::notify_state_change();
        if (g_touch_thread.joinable()) g_touch_thread.join();
        release_finger();
    }

} // namespace touch_aim
