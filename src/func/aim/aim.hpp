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

    // Lock-free head guidance via Player_Rotation (0x61C).
    // Uses crosshair-relative delta (camForward -> dirToHead) so any scope/shoulder
    // pitch offset or recoil is automatically canceled without locking the camera.
    inline void write_head_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const TargetAcquisitionResult& acq,
                               bool triggerActive) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f) {
            return;
        }
        if (!is_valid_ptr(localPlayer)) return;

        const matrix vm = data::get_view_matrix();
        const Vector3 camPos = GetCameraPosition(vm);
        Vector3 camFwd = GetCameraForward(vm);
        if (camPos == Vector3::Zero() || !std::isfinite(camPos.x) ||
            !std::isfinite(camPos.y) || !std::isfinite(camPos.z) ||
            camFwd.sqrMagnitude() < 1e-4f) {
            return;
        }

        // Bone::Head (0x6A8) sits near the crown of the head; nudge -0.05m to center on face.
        const Vector3 headPos = acq.target.getHeadAimPoint(rc.aimHeadBias - 0.05f);
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

        // Deadzone: skip write when already within ~0.4 deg of head (avoids micro-jitter writes)
        float dot = fabsf(currentRot.x * targetRot.x + currentRot.y * targetRot.y +
                          currentRot.z * targetRot.z + currentRot.w * targetRot.w);
        if (dot > 1.0f) dot = 1.0f;
        if (dot > 0.999975f) return;

        // 2026 Behavioral Stealth: cap angular velocity per write (~8 deg max)
        // and enforce >= 8ms write spacing so rotation never produces unnatural spikes.
        static auto s_lastWrite = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastWrite.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastWrite).count() < 8) {
            return;
        }

        const float totalAngleRad = 2.0f * acosf(dot);
        float t = (rc.aimPullStepPx / 40.0f) * 0.45f;
        if (!std::isfinite(t) || t < 0.04f) t = 0.04f;
        if (t > 0.45f) t = 0.45f;

        constexpr float kMaxStepRad = 8.0f * (3.14159265f / 180.0f);
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
        clear_target_snapshot();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
