#pragma once
// ============================================================================
// AIMBOT — LOCK-FREE HEAD GUIDANCE (FREE FIRE MAX)
// ============================================================================
// Lock-free: no sticky target ID, no camera freeze. Only nudges Player_Rotation
// smoothly toward headWorld inside AimFov while triggerActive is true.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "Android_touch/TouchHelperA.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <sys/prctl.h>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_aim_thread;
    constexpr int            kAimTouchSlot = 8;
    inline float             s_fingerX = 0.0f;
    inline float             s_fingerY = 0.0f;
    inline uint64_t          s_lastTouchGen = 0;

    // Adaptive sensitivity estimator: learns screen-px per touch-px in real time
    inline float             s_autoSens = 1.0f;
    inline bool              s_hasPrevStep = false;
    inline uint64_t          s_prevTarget = 0;
    inline float             s_prevDx = 0.0f;
    inline float             s_prevDy = 0.0f;
    inline float             s_lastStepX = 0.0f;
    inline float             s_lastStepY = 0.0f;

    inline void release_aim_finger() {
        if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
        s_fingerX = 0.0f;
        s_fingerY = 0.0f;
        s_hasPrevStep = false;
    }

    // 100% Zero-Write Touch Head Guidance (Anti-Ban):
    // Pure memory read (Head + ViewMatrix + IsFiring) + adaptive closed-loop uinput drag.
    // Automatically calibrates to in-game sensitivity so aim reaches head in milliseconds.
    inline void write_head_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const TargetAcquisitionResult& acq,
                               bool triggerActive,
                               uint64_t stateGen) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f || !is_valid_ptr(localPlayer)) {
            release_aim_finger();
            return;
        }

        // Only step when a fresh entity/camera snapshot arrived so we never
        // double-apply the same stale (dx, dy) error across multiple ticks.
        if (stateGen == s_lastTouchGen) {
            return;
        }
        s_lastTouchGen = stateGen;

        const float cx = rc.screenW * 0.5f;
        const float cy = rc.screenH * 0.5f;
        const float dx = acq.bestScreenPos.x - cx;
        const float dy = acq.bestScreenPos.y - cy;
        const float err = sqrtf(dx * dx + dy * dy);
        if (!std::isfinite(err)) {
            release_aim_finger();
            return;
        }

        // Update online sensitivity estimate from the previous swipe response
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
            return; // Keep finger down on target without micro-jitter
        }

        // Empty upper-right camera zone (avoids Fire button & right-thumb HUD)
        const float anchorX = rc.screenW * 0.62f;
        const float anchorY = rc.screenH * 0.28f;
        const float minX = rc.screenW * 0.45f, maxX = rc.screenW * 0.88f;
        const float minY = rc.screenH * 0.10f, maxY = rc.screenH * 0.65f;

        if (!IsFingerDown(kAimTouchSlot) || s_fingerX <= 0.0f) {
            s_fingerX = anchorX;
            s_fingerY = anchorY;
            Touch_Down_Slot(kAimTouchSlot, s_fingerX, s_fingerY);
        }

        // Adaptive step: (0.78 / s_autoSens) carries crosshair onto head in 1-2 frames
        const float userScale = (rc.aimPullStepPx > 0.0f) ? (rc.aimPullStepPx / 22.0f) : 1.0f;
        const float gain = (0.78f / s_autoSens) * userScale;
        float stepX = dx * gain;
        float stepY = dy * gain;
        const float stepLen = sqrtf(stepX * stepX + stepY * stepY);
        const float maxStep = 160.0f * userScale;
        if (stepLen > maxStep && stepLen > 1e-4f) {
            const float s = maxStep / stepLen;
            stepX *= s;
            stepY *= s;
        }

        float nx = s_fingerX + stepX;
        float ny = s_fingerY + stepY;
        if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
            Touch_Up_Slot(kAimTouchSlot);
            s_fingerX = anchorX;
            s_fingerY = anchorY;
            Touch_Down_Slot(kAimTouchSlot, s_fingerX, s_fingerY);
            nx = s_fingerX + stepX;
            ny = s_fingerY + stepY;
        }
        s_fingerX = nx;
        s_fingerY = ny;
        Touch_Move_Slot(kAimTouchSlot, s_fingerX, s_fingerY);

        s_hasPrevStep = true;
        s_prevTarget  = acq.target.addr;
        s_prevDx      = dx;
        s_prevDy      = dy;
        s_lastStepX   = stepX;
        s_lastStepY   = stepY;
    }

    inline void clear_target_snapshot() {
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
        g_currentTarget.screenPos = Vector3::Zero();
    }

    inline void aim_loop() {
        prctl(PR_SET_NAME, oxorany("InputDispatcher"), 0, 0, 0);
        uint64_t lastState = cfg::g_state_generation;
        std::unique_lock<std::mutex> waitLock(cfg::g_runtime_mutex);

        while (g_aim_running.load(std::memory_order_relaxed)) {
            cfg::g_state_cv.wait(waitLock, [&]() {
                return !g_aim_running.load(std::memory_order_relaxed) ||
                       cfg::g_state_generation != lastState;
            });
            if (!g_aim_running.load(std::memory_order_relaxed)) break;
            lastState = cfg::g_state_generation;
            waitLock.unlock();

            const cfg::RuntimeConfig rc = cfg::runtime();
            const bool active = rc.master && rc.aimEnable &&
                                data::inMatch.load(std::memory_order_relaxed);
            const uint64_t localPlayer = active
                ? data::localPlayerAddr.load(std::memory_order_relaxed) : 0;

            if (!active || !is_valid_ptr(localPlayer)) {
                release_aim_finger();
                clear_target_snapshot();
                waitLock.lock();
                continue;
            }

            // acquire_target() already drops every enemy outside the AimFov
            // circle, so the circle itself is the head-carry boundary.
            TargetAcquisitionResult acq = acquire_target(localPlayer);
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = acq.valid;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            const bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            const bool triggerActive = (rc.aimTriggerMode == 1) || isFiring;

            write_head_aim(localPlayer, rc, acq, triggerActive, lastState);

            waitLock.lock();
        }

        waitLock.unlock();
        release_aim_finger();
        clear_target_snapshot();
    }

    inline void thread_start() {
        if (g_aim_running.exchange(true)) return;
        g_aim_thread = std::thread(aim_loop);
    }

    inline void thread_stop() {
        if (!g_aim_running.exchange(false)) return;
        cfg::notify_state_change();
        if (g_aim_thread.joinable()) g_aim_thread.join();
        release_aim_finger();
        clear_target_snapshot();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
