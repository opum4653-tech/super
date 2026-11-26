#pragma once
// ============================================================================
// TOUCH AIM — SEPARATE 100% ZERO-WRITE UINPUT HEAD ASSIST (FREE FIRE MAX)
// ============================================================================
// Independent from aim.hpp (Player_Rotation 0x61C).
// Reads real game offsets (Head 0x6A8, ViewMatrix, Player_IsFiring 0x850)
// via data::players / acquire_target() and injects closed-loop uinput touch
// drags on slot 8. Zero game memory writes.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "Android_touch/TouchHelperA.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <sys/prctl.h>

namespace touch_aim {

    inline std::atomic<bool> g_touch_running{false};
    inline std::thread       g_touch_thread;

    constexpr int            kTouchAimSlot = 8;
    inline float             s_fingerX = 0.0f;
    inline float             s_fingerY = 0.0f;
    inline uint64_t          s_lastGen = 0;

    // Online adaptive sensitivity estimator (screen-px per touch-px)
    inline float             s_autoSens = 1.0f;
    inline bool              s_hasPrevStep = false;
    inline uint64_t          s_prevTarget = 0;
    inline float             s_prevDx = 0.0f;
    inline float             s_prevDy = 0.0f;
    inline float             s_lastStepX = 0.0f;
    inline float             s_lastStepY = 0.0f;

    inline void release_finger() {
        if (IsFingerDown(kTouchAimSlot)) Touch_Up_Slot(kTouchAimSlot);
        s_fingerX = 0.0f;
        s_fingerY = 0.0f;
        s_hasPrevStep = false;
    }

    inline aimbot::TargetAcquisitionResult select_touch_target(uint64_t localPlayer,
                                                               const cfg::RuntimeConfig& rc) {
        aimbot::TargetAcquisitionResult result;
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
            if (rc.touchIgnoreBots && enemy.isBot) continue;

            float dxHead = enemy.Head.x - screenCx;
            float dyHead = enemy.Head.y - screenCy;
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
            }
        }

        if (bestEnemy) {
            result.target = *bestEnemy;
            result.bestScreenPos = bestEnemy->Head;
            result.valid = true;
        }
        return result;
    }

    inline void step_touch_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const aimbot::TargetAcquisitionResult& acq,
                               bool triggerActive,
                               uint64_t stateGen) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f || !is_valid_ptr(localPlayer)) {
            release_finger();
            return;
        }

        if (stateGen == s_lastGen) return;
        s_lastGen = stateGen;

        const float cx = rc.screenW * 0.5f;
        const float cy = rc.screenH * 0.5f;
        const float dx = acq.bestScreenPos.x - cx;
        const float dy = acq.bestScreenPos.y - cy;
        const float err = sqrtf(dx * dx + dy * dy);
        if (!std::isfinite(err)) {
            release_finger();
            return;
        }

        if (s_hasPrevStep && acq.target.addr == s_prevTarget) {
            const float stepLen = sqrtf(s_lastStepX * s_lastStepX + s_lastStepY * s_lastStepY);
            if (stepLen >= 4.0f) {
                const float movedX = s_prevDx - dx;
                const float movedY = s_prevDy - dy;
                const float projMove = (movedX * s_lastStepX + movedY * s_lastStepY) / stepLen;
                if (projMove > 0.5f && std::isfinite(projMove)) {
                    float ratio = projMove / stepLen;
                    if (ratio < 0.20f) ratio = 0.20f;
                    if (ratio > 5.00f) ratio = 5.00f;
                    s_autoSens = s_autoSens * 0.65f + ratio * 0.35f;
                }
            }
        }

        if (err < 2.0f) {
            s_hasPrevStep = false;
            return;
        }

        const float anchorX = rc.screenW * 0.62f;
        const float anchorY = rc.screenH * 0.28f;
        const float minX = rc.screenW * 0.45f, maxX = rc.screenW * 0.88f;
        const float minY = rc.screenH * 0.10f, maxY = rc.screenH * 0.65f;

        if (!IsFingerDown(kTouchAimSlot) || s_fingerX <= 0.0f) {
            s_fingerX = anchorX;
            s_fingerY = anchorY;
            Touch_Down_Slot(kTouchAimSlot, s_fingerX, s_fingerY);
        }

        const float userScale = (rc.touchStrength > 0.0f) ? rc.touchStrength : 1.0f;
        const float gain = (0.75f / s_autoSens) * userScale;
        float stepX = dx * gain;
        float stepY = dy * gain;
        const float stepLen = sqrtf(stepX * stepX + stepY * stepY);
        const float maxStep = 150.0f * userScale;
        if (stepLen > maxStep && stepLen > 1e-4f) {
            const float s = maxStep / stepLen;
            stepX *= s;
            stepY *= s;
        }

        float nx = s_fingerX + stepX;
        float ny = s_fingerY + stepY;
        if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
            Touch_Up_Slot(kTouchAimSlot);
            s_fingerX = anchorX;
            s_fingerY = anchorY;
            Touch_Down_Slot(kTouchAimSlot, s_fingerX, s_fingerY);
            nx = s_fingerX + stepX;
            ny = s_fingerY + stepY;
        }
        s_fingerX = nx;
        s_fingerY = ny;
        Touch_Move_Slot(kTouchAimSlot, s_fingerX, s_fingerY);

        s_hasPrevStep = true;
        s_prevTarget  = acq.target.addr;
        s_prevDx      = dx;
        s_prevDy      = dy;
        s_lastStepX   = stepX;
        s_lastStepY   = stepY;
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

            step_touch_aim(localPlayer, rc, acq, triggerActive, lastState);

            waitLock.lock();
        }

        waitLock.unlock();
        release_finger();
    }

    inline void thread_start() {
        if (g_touch_running.exchange(true)) return;
        g_touch_thread = std::thread(touch_loop);
    }

    inline void thread_stop() {
        if (!g_touch_running.exchange(false)) return;
        cfg::notify_state_change();
        if (g_touch_thread.joinable()) g_touch_thread.join();
        release_finger();
    }

} // namespace touch_aim
