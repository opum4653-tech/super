#pragma once
// ============================================================================
// AIMBOT ASSIST & TRAJECTORY STEERING WORKER THREAD
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Coordinates the full trajectory steering and motion modifier cycle:
// - Thread Management: Started & stopped by attach_worker in src/main.cpp.
// - Dynamic Thread Cadence:
//     * 2ms sleep when actively shooting (for ultra-crisp bullet alignment).
//     * 10ms sleep when idling (reduces CPU cycles).
// - Sub-Module Orchestration:
//     * motion::update()      -> Motion enhancements (air flight & phantom)
//     * acquire_target()      -> Target scoring, FOV filtering, and hysteresis
//     * evaluate_headshot()   -> RNG headshot roll & raycast collider redirection
//     * apply_trajectory()    -> 3D unit trajectory steering into AimingInfo
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "headshot.hpp"
#include "silent.hpp"
#include "fov.hpp"
#include "motion.hpp"

namespace aimbot {


    inline std::atomic<bool>& aim_thread_running() {
        static std::atomic<bool> s_run{false};
        return s_run;
    }

    inline void update() {
        if (!cfg::master_enabled) {
            motion::reset();
            return;
        }

        // Run motion enhancement updates
        motion::update();

        g_currentTarget.valid = false;
        if (!cfg::aim::enable && !cfg::aim::silent) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            s_smoothDir = Vector3::Zero();
            return;
        }

        if (!data::inMatch.load(std::memory_order_relaxed)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            return;
        }

        uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
        if (!is_valid_ptr(localPlayer)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            return;
        }

        TargetAcquisitionResult acq = acquire_target(localPlayer);
        if (!acq.valid) {
            return;
        }

        // Target screen position tracking
        g_currentTarget.valid = true;
        g_currentTarget.screenPos = acq.bestScreenPos;

        // Headshot evaluation & collider mapping
        bool lockToHead = evaluate_headshot(acq.target, acq.isScoping);

        // 3D Silent bullet trajectory steering
        apply_trajectory(localPlayer, acq.target, lockToHead, acq.isFiring);
    }

    inline void loop() {
        while (aim_thread_running().load(std::memory_order_relaxed)) {
            if (cfg::master_enabled && (cfg::aim::enable || cfg::aim::silent || cfg::motion::air_flight || cfg::motion::phantom_mode)) {
                update();
            }

            uint64_t lp = data::localPlayerAddr.load(std::memory_order_relaxed);
            bool isFiring = false;
            if (lp) {
                isFiring = rpm<bool>(lp + Offsets::Player::IsFiring) || rpm<bool>(lp + 0x850);
            }

            if (isFiring) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    inline void thread_start() {
        if (aim_thread_running().exchange(true)) return;
        std::thread(loop).detach();
    }

    inline void thread_stop() {
        aim_thread_running().store(false);
        motion::reset();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
