#pragma once
// ============================================================================
// AIMBOT — SMOOTH FRAME-PACED PLAYER_ROTATION HEAD CARRY (FREE FIRE MAX)
// ============================================================================
// Tracks moving/walking enemy heads smoothly while firing (or Always In FOV)
// using 12ms-paced Slerp on Player_Rotation (0x61C) with a 25 deg/tick cap.
// Releases immediately when trigger stops. Zero collider/AimInfo tampering.
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

        // 12ms (~80Hz) pacing: fast enough to track walking enemies every game frame
        // without spamming kpm.write at 333Hz.
        static auto s_lastWrite = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (s_lastWrite.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastWrite).count() < 12) {
            return;
        }

        Quaternion currentRot{};
        bool hasRot = mem_read(rotAddr, &currentRot, sizeof(currentRot));
        const float qNorm = hasRot ? Quaternion::Norm(currentRot) : 0.0f;
        if (!hasRot || !std::isfinite(qNorm) || qNorm < 0.5f || qNorm > 1.5f) {
            Vector3 camFwd = GetCameraForward(vm);
            currentRot = (camFwd.sqrMagnitude() > 1e-4f)
                ? Quaternion::Normalize(Quaternion::LookRotation(camFwd))
                : targetRot;
        }

        // Deadzone (~0.8 deg): keeps head tracking tight while skipping idle writes
        float dot = fabsf(currentRot.x * targetRot.x + currentRot.y * targetRot.y +
                          currentRot.z * targetRot.z + currentRot.w * targetRot.w);
        if (dot > 1.0f) dot = 1.0f;
        if (dot > 0.99990f) return;

        float t = (rc.aimPullStepPx > 0.0f) ? (rc.aimPullStepPx / 35.0f) : 0.63f;
        if (!std::isfinite(t) || t < 0.25f) t = 0.25f;
        if (t > 0.90f) t = 0.90f;

        // Cap per-tick delta to <= 25 deg so moving targets stay tracked smoothly
        const float totalAngleRad = 2.0f * acosf(dot);
        constexpr float kMaxStepRad = 25.0f * (3.14159265f / 180.0f);
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
                cfg::g_state_cv.wait_for(waitLock, std::chrono::milliseconds(12), [&]() {
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
