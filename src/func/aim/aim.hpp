#pragma once
// ============================================================================
// AIMBOT — SAFE STATELESS PLAYER_ROTATION SLERP (FREE FIRE MAX)
// ============================================================================
// Always reads live currentRot from Player_Rotation (0x61C) and applies a
// gentle, frame-paced (16ms / <= 20 deg) Slerp toward headWorld while firing.
// Never overwrites 0x61C with stale cached state; stops writing within ~1 deg.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
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

    inline void write_head_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const TargetAcquisitionResult& acq,
                               bool triggerActive) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f || !is_valid_ptr(localPlayer)) {
            return;
        }

        const matrix vm = data::get_view_matrix();
        const Vector3 camPos = GetCameraPosition(vm);
        if (camPos == Vector3::Zero() || !std::isfinite(camPos.x) ||
            !std::isfinite(camPos.y) || !std::isfinite(camPos.z)) {
            return;
        }

        const Vector3 headPos = acq.target.headWorld;
        Vector3 dir = headPos - camPos;
        if (dir.sqrMagnitude() < 1e-4f || !std::isfinite(dir.x) ||
            !std::isfinite(dir.y) || !std::isfinite(dir.z)) {
            return;
        }

        const Quaternion targetRot = Quaternion::Normalize(Quaternion::LookRotation(dir.normalized()));
        const uint64_t rotAddr = localPlayer + Offsets::Player_Rotation;

        // 16ms (~60Hz) frame-paced writes matching Unity's frame rate
        static auto s_lastWrite = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastWrite.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastWrite).count() < 16) {
            return;
        }

        // Always read live currentRot from 0x61C and require a valid unit quaternion
        Quaternion currentRot{};
        if (!mem_read(rotAddr, &currentRot, sizeof(currentRot))) return;
        const float qNorm = Quaternion::Norm(currentRot);
        if (!std::isfinite(qNorm) || qNorm < 0.5f || qNorm > 1.5f) return;

        // Deadzone (~1.0 deg): stop writing once aligned with head
        float dot = fabsf(currentRot.x * targetRot.x + currentRot.y * targetRot.y +
                          currentRot.z * targetRot.z + currentRot.w * targetRot.w);
        if (dot > 1.0f) dot = 1.0f;
        if (dot > 0.99985f) return;

        float t = (rc.aimPullStepPx > 0.0f) ? (rc.aimPullStepPx / 35.0f) : 0.63f;
        if (!std::isfinite(t) || t < 0.20f) t = 0.20f;
        if (t > 0.85f) t = 0.85f;

        const float totalAngleRad = 2.0f * acosf(dot);
        constexpr float kMaxStepRad = 20.0f * (3.14159265f / 180.0f);
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

        bool activeTrigger = false;
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (activeTrigger) {
                cfg::g_state_cv.wait_for(waitLock, std::chrono::milliseconds(16), [&]() {
                    return !g_aim_running.load(std::memory_order_relaxed) ||
                           cfg::g_state_generation != lastState;
                });
            } else {
                cfg::g_state_cv.wait(waitLock, [&]() {
                    return !g_aim_running.load(std::memory_order_relaxed) ||
                           cfg::g_state_generation != lastState;
                });
            }
            if (!g_aim_running.load(std::memory_order_relaxed)) break;
            lastState = cfg::g_state_generation;
            waitLock.unlock();

            const cfg::RuntimeConfig rc = cfg::runtime();
            const bool active = rc.master && rc.aimEnable &&
                                data::inMatch.load(std::memory_order_relaxed);
            const uint64_t localPlayer = active
                ? data::localPlayerAddr.load(std::memory_order_relaxed) : 0;

            if (!active || !is_valid_ptr(localPlayer)) {
                activeTrigger = false;
                clear_target_snapshot();
                waitLock.lock();
                continue;
            }

            TargetAcquisitionResult acq = acquire_target(localPlayer);
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = acq.valid;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            const bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            const bool triggerActive = (rc.aimTriggerMode == 1) || isFiring;
            activeTrigger = triggerActive && acq.valid;

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
