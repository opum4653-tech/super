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
    constexpr int            kAimTouchSlot = 7;
    inline float             s_fingerX = 0.0f;
    inline float             s_fingerY = 0.0f;

    inline void release_aim_finger() {
        if (IsFingerDown(kAimTouchSlot)) Touch_Up_Slot(kAimTouchSlot);
        s_fingerX = 0.0f;
        s_fingerY = 0.0f;
    }

    inline Quaternion quat_mul(const Quaternion& a, const Quaternion& b) {
        return Quaternion(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        );
    }

    inline Quaternion quat_conj(const Quaternion& q) {
        return Quaternion(-q.x, -q.y, -q.z, q.w);
    }

    // Lock-free head guidance: pairs real uinput touch drag with rate-limited
    // Player_Rotation (0x61C) Slerp so rotation never moves without touch input.
    inline void write_head_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const TargetAcquisitionResult& acq,
                               bool triggerActive) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f) {
            release_aim_finger();
            return;
        }
        if (!is_valid_ptr(localPlayer)) {
            release_aim_finger();
            return;
        }

        // 1. Emit real touch drag in the right-side camera look zone
        const float cx = rc.screenW * 0.5f;
        const float cy = rc.screenH * 0.5f;
        const float dx = acq.bestScreenPos.x - cx;
        const float dy = acq.bestScreenPos.y - cy;
        const float err = sqrtf(dx * dx + dy * dy);
        if (std::isfinite(err) && err >= 1.0f) {
            const float anchorX = rc.screenW * 0.72f;
            const float anchorY = rc.screenH * 0.45f;
            const float minX = rc.screenW * 0.55f, maxX = rc.screenW * 0.92f;
            const float minY = rc.screenH * 0.12f, maxY = rc.screenH * 0.88f;

            if (!IsFingerDown(kAimTouchSlot) || s_fingerX <= 0.0f) {
                s_fingerX = anchorX;
                s_fingerY = anchorY;
                Touch_Down_Slot(kAimTouchSlot, s_fingerX, s_fingerY);
            }
            float dragStep = (rc.aimPullStepPx > 0.0f) ? rc.aimPullStepPx : 12.0f;
            if (dragStep > err) dragStep = err;
            float nx = s_fingerX + (dx / err) * dragStep;
            float ny = s_fingerY + (dy / err) * dragStep;
            if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
                Touch_Up_Slot(kAimTouchSlot);
                s_fingerX = anchorX;
                s_fingerY = anchorY;
                Touch_Down_Slot(kAimTouchSlot, s_fingerX, s_fingerY);
                nx = s_fingerX + (dx / err) * dragStep;
                ny = s_fingerY + (dy / err) * dragStep;
            }
            s_fingerX = nx;
            s_fingerY = ny;
            Touch_Move_Slot(kAimTouchSlot, s_fingerX, s_fingerY);
        } else {
            release_aim_finger();
        }

        // 2. Gentle, rate-limited Player_Rotation (0x61C) head alignment
        const matrix vm = data::get_view_matrix();
        const Vector3 camPos = GetCameraPosition(vm);
        Vector3 camFwd = GetCameraForward(vm);
        if (camPos == Vector3::Zero() || !std::isfinite(camPos.x) ||
            !std::isfinite(camPos.y) || !std::isfinite(camPos.z) ||
            camFwd.sqrMagnitude() < 1e-4f) {
            return;
        }

        const Vector3 headPos = acq.target.getHeadAimPoint(rc.aimHeadBias);
        Vector3 dir = headPos - camPos;
        if (dir.sqrMagnitude() < 1e-4f || !std::isfinite(dir.x) ||
            !std::isfinite(dir.y) || !std::isfinite(dir.z)) {
            return;
        }

        const Quaternion qCam  = Quaternion::LookRotation(camFwd);
        const Quaternion qHead = Quaternion::LookRotation(dir.normalized());
        const Quaternion qDelta = quat_mul(qHead, quat_conj(qCam));

        const uint64_t rotAddr = localPlayer + Offsets::Player_Rotation;

        Quaternion currentRot{};
        if (!mem_read(rotAddr, &currentRot, sizeof(currentRot))) return;
        const float qNorm = Quaternion::Norm(currentRot);
        if (!std::isfinite(qNorm) || qNorm < 0.5f || qNorm > 1.5f) return;

        const Quaternion targetRot = Quaternion::Normalize(quat_mul(qDelta, currentRot));

        // Deadzone (~0.25 deg) to prevent micro-jitter writes
        float dot = fabsf(currentRot.x * targetRot.x + currentRot.y * targetRot.y +
                          currentRot.z * targetRot.z + currentRot.w * targetRot.w);
        if (dot > 1.0f) dot = 1.0f;
        if (dot > 0.999990f) return;

        // Safe 16ms (~60Hz) cadence and 4.5 deg max step per write
        static auto s_lastWrite = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastWrite.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastWrite).count() < 16) {
            return;
        }

        const float totalAngleRad = 2.0f * acosf(dot);
        float t = (rc.aimPullStepPx / 40.0f) * 0.35f;
        if (!std::isfinite(t) || t < 0.04f) t = 0.04f;
        if (t > 0.35f) t = 0.35f;

        constexpr float kMaxStepRad = 4.5f * (3.14159265f / 180.0f);
        if (totalAngleRad > 1e-4f && (totalAngleRad * t) > kMaxStepRad) {
            t = kMaxStepRad / totalAngleRad;
        }

        const Quaternion outRot = Quaternion::Slerp(currentRot, targetRot, t);

        mem_guard::Scope guard(rotAddr, sizeof(outRot));
        if (mem_write(rotAddr, &outRot, sizeof(outRot))) {
            s_lastWrite = now;
        }
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

            write_head_aim(localPlayer, rc, acq, triggerActive);

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
