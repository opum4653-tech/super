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

    // Real Camera Aim Assist (Safe Anti-Ban Zero-Op)
    // Direct memory mutation of Player::Rotation (0x61C) creates client-side touch desync
    // and server-side angle snap anomalies. Targeting is strictly and safely handled by
    // normalized silent trajectory steering (apply_trajectory) without mutating player camera in memory.
    inline void apply_camera_aim(uint64_t localPlayer, const PlayerData& target, bool lockToHead, bool triggerActive) {
        (void)localPlayer;
        (void)target;
        (void)lockToHead;
        (void)triggerActive;
    }

    inline std::atomic<bool>& aim_thread_running() {
        static std::atomic<bool> s_run{false};
        return s_run;
    }

    inline void update() {
        if (!cfg::master_enabled) {
            motion::reset();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        // Run motion enhancement updates
        motion::update();

        if (!cfg::aim::enable && !cfg::aim::silent) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            s_smoothDir = Vector3::Zero();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            silent::clear_silent_data();
            return;
        }

        if (!data::inMatch.load(std::memory_order_relaxed)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            silent::clear_silent_data();
            return;
        }

        uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
        if (!is_valid_ptr(localPlayer)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            silent::clear_silent_data();
            return;
        }

        TargetAcquisitionResult acq = acquire_target(localPlayer);
        // Instant Death / Target Lost Reversion:
        // The microsecond the target dies (curHP <= 0) or leaves FOV, immediately
        // clear write buffers so game memory instantly returns to 100% natural state.
        if (!acq.valid || acq.target.curHP <= 0) {
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            silent::clear_silent_data();
            return;
        }

        // Firing Invariant: If not actively firing, keep memory 100% pristine and unwritten
        if (!acq.isFiring) {
            silent::clear_silent_data();
            return;
        }

        // Target screen position tracking (thread-safe snapshot for FOV ring)
        {
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = true;
            g_currentTarget.screenPos = acq.bestScreenPos;
        }

        // Headshot evaluation & collider mapping (strictly fire-gated)
        bool lockToHead = evaluate_headshot(acq.target, acq.isScoping, acq.isFiring);

        // 3D Silent bullet trajectory steering (Aim Silent & Aimbot Trajectory)
        if (cfg::aim::silent || cfg::aim::enable) {
            apply_trajectory(localPlayer, acq.target, lockToHead, acq.isFiring);
        } else {
            silent::clear_silent_data();
        }

        // Camera rotation write is permanently neutralized for anti-ban safety
        if (cfg::aim::enable) {
            apply_camera_aim(localPlayer, acq.target, lockToHead, acq.isFiring || acq.triggerActive);
        }
    }

    inline void loop() {
        while (aim_thread_running().load(std::memory_order_relaxed)) {
            if (cfg::master_enabled && (cfg::aim::enable || cfg::aim::silent)) {
                update();
            } else {
                silent::clear_silent_data();
            }

            // Natural game frame cadence (~60 FPS steady update, zero artificial millisecond forcing)
            int interval = cfg::aim::read_interval_ms;
            int sleepMs = (interval >= 8 && interval <= 33) ? interval : 16;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }

    inline void thread_start() {
        if (aim_thread_running().exchange(true)) return;
        silent::thread_start();
        std::thread(loop).detach();
    }

    inline void thread_stop() {
        aim_thread_running().store(false);
        silent::thread_stop();
        motion::reset();
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
