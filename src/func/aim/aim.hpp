#pragma once
// ============================================================================
// AIMBOT — FOV-GOVERNED HEAD GUIDANCE (FREE FIRE MAX)
// ============================================================================
// Player_Rotation (0x61C) is never written. The camera is never locked.
// AimFov is the only control: the circle decides which enemies get head
// guidance, and the size of the circle is what the user turns up or down.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "Android_touch/TouchHelperA.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstring>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_aim_thread;
    constexpr int            kAimTouchSlot = 7;

    // Zero memory writes. The head point inside the AimFov circle is dragged
    // under the crosshair with a real touch, so the bullet goes to the head
    // while the camera stays fully under the game's own control.
    inline void touch_head_guidance(const cfg::RuntimeConfig& rc,
                                    const TargetAcquisitionResult& acq,
                                    bool triggerActive) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f) {
            if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
            return;
        }

        const float cx = rc.screenW * 0.5f;
        const float cy = rc.screenH * 0.5f;
        const float dx = acq.bestScreenPos.x - cx;
        const float dy = acq.bestScreenPos.y - cy;
        const float err = sqrtf(dx * dx + dy * dy);

        // Already centered on head — release touch so official aim/camera stays free.
        if (!std::isfinite(err) || err < 0.5f) {
            if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
            return;
        }

        // Full head pull anywhere inside AimFov (no edge attenuation that stalls
        // outside center), bounded by Pull Step so touch drag stays smooth.
        float step = (rc.aimPullStepPx > 0.0f) ? rc.aimPullStepPx : err;
        if (!std::isfinite(step) || step < 0.0f) step = 0.0f;
        if (step > err) step = err;
        if (step < 0.5f) {
            if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
            return;
        }

        float px = cx, py = cy;
        if (IsFingerDown(kAimTouchSlot)) {
            GetFingerScreenPos(kAimTouchSlot, px, py);
        } else {
            Touch_Down_Slot(kAimTouchSlot, cx, cy);
        }

        float nx = px + (dx / err) * step;
        float ny = py + (dy / err) * step;
        if (nx < 0.0f || nx > rc.screenW || ny < 0.0f || ny > rc.screenH) {
            Touch_Up_Slot(kAimTouchSlot);
            Touch_Down_Slot(kAimTouchSlot, cx, cy);
            nx = cx + (dx / err) * step;
            ny = cy + (dy / err) * step;
        }
        Touch_Move_Slot(kAimTouchSlot, nx, ny);
    }

    inline void clear_target_snapshot() {
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
        g_currentTarget.screenPos = Vector3::Zero();
    }

    inline void aim_loop() {
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
                touch_head_guidance(rc, TargetAcquisitionResult{}, false);
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

            touch_head_guidance(rc, acq, triggerActive);

            waitLock.lock();
        }

        waitLock.unlock();
        if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
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
        if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
        clear_target_snapshot();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
