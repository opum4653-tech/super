#pragma once
// ============================================================================
// AIMBOT — ONE-SHOT 3MS HEAD DELIVERY (FREE FIRE MAX)
// ============================================================================
// Writes targetRot to Player_Rotation (0x61C) only for a brief ~3ms pulse
// when firing starts, then stops writing completely for the rest of the burst.
// Zero continuous head lock, zero collider/AimInfo tampering.
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
    inline bool              s_pulseActive = false;
    inline bool              s_pulseDone   = false;
    inline uint64_t          s_pulseTarget = 0;
    inline std::chrono::steady_clock::time_point s_pulseStart{};

    inline void reset_pulse_state() {
        s_pulseActive = false;
        s_pulseDone   = false;
        s_pulseTarget = 0;
    }

    inline void write_head_aim(uint64_t localPlayer,
                               const cfg::RuntimeConfig& rc,
                               const TargetAcquisitionResult& acq,
                               bool triggerActive) {
        if (!triggerActive || !acq.valid || acq.bestScreenPos.z < 0.001f || !is_valid_ptr(localPlayer)) {
            reset_pulse_state();
            return;
        }

        if (rc.aimTriggerMode == 1 && acq.target.addr != s_pulseTarget) {
            s_pulseActive = false;
            s_pulseDone   = false;
        }
        if (s_pulseDone) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!s_pulseActive) {
            s_pulseActive = true;
            s_pulseTarget = acq.target.addr;
            s_pulseStart  = now;
        } else {
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - s_pulseStart).count();
            if (elapsedMs >= 3) {
                s_pulseDone   = true;
                s_pulseActive = false;
                return;
            }
        }

        const matrix vm = data::get_view_matrix();
        const Vector3 camPos = GetCameraPosition(vm);
        if (camPos == Vector3::Zero() || !std::isfinite(camPos.x) ||
            !std::isfinite(camPos.y) || !std::isfinite(camPos.z)) {
            return;
        }

        const Vector3 headPos = acq.target.getHeadAimPoint(rc.aimHeadBias);
        Vector3 dir = headPos - camPos;
        if (dir.sqrMagnitude() < 1e-4f || !std::isfinite(dir.x) ||
            !std::isfinite(dir.y) || !std::isfinite(dir.z)) {
            return;
        }

        const Quaternion targetRot = Quaternion::Normalize(Quaternion::LookRotation(dir.normalized()));
        const uint64_t rotAddr = localPlayer + Offsets::Player_Rotation;

        mem_guard::Scope guard(rotAddr, sizeof(targetRot));
        mem_write(rotAddr, &targetRot, sizeof(targetRot));
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
            if (s_pulseActive && !s_pulseDone) {
                cfg::g_state_cv.wait_for(waitLock, std::chrono::milliseconds(1), [&]() {
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
                reset_pulse_state();
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

            write_head_aim(localPlayer, rc, acq, triggerActive);

            waitLock.lock();
        }

        waitLock.unlock();
        reset_pulse_state();
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
        reset_pulse_state();
        clear_target_snapshot();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
