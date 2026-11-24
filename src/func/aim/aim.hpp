#pragma once
// ============================================================================
// AIMBOT — REAL-TIME TARGET TRACKING & VISUAL LOCK RETICLE ENGINE
// ============================================================================
// Project: crack_master (Pure External Ring-3 Assisted Aim)
//
// ZERO FAKE DAMAGE / 100% REAL DAMAGE ARCHITECTURE:
// - Silent Aim (modifying 0x40 RayDir and 0x80 ColliderBody) has been PERMANENTLY
//   REMOVED because it is server-side patched by Free Fire.
//   Server-side raycast validation rejects modified trajectories, causing
//   client-side "Fake Damage" where enemies take zero actual HP reduction.
// - All memory mutation (wpm) is eliminated in this module.
// - This module runs a high-speed, zero-timer tracking loop that acquires the best
//   target inside the FOV circle and updates g_currentTarget for the visual
//   target lock reticle and FOV overlay (render_fov).
// - Bullets travel naturally along the authentic weapon barrel and camera orientation,
//   ensuring 100% server-validated REAL DAMAGE on all hits.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "motion.hpp"
#include <sched.h>
#include <thread>
#include <atomic>
#include <mutex>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_tracking_thread;

    inline void clear_aim_state() {
        s_lockedTargetAddr = 0;
        s_smoothDir = Vector3::Zero();
    }

    // ------------------------------------------------------------------------
    // REAL-TIME TARGET TRACKING LOOP (Pure Non-Invasive Read-Only)
    // ------------------------------------------------------------------------
    inline void tracking_loop() {
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!cfg::master_enabled || !cfg::aim::enable || !data::inMatch.load(std::memory_order_relaxed)) {
                clear_aim_state();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                sched_yield();
                continue;
            }

            uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
            if (!is_valid_ptr(localPlayer)) {
                clear_aim_state();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                sched_yield();
                continue;
            }

            motion::update();

            TargetAcquisitionResult acq = acquire_target(localPlayer);
            if (!acq.valid || acq.target.curHP <= 0 || !acq.triggerActive) {
                s_smoothDir = Vector3::Zero();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                sched_yield();
                continue;
            }

            // Target actively locked inside FOV: update visual lock reticle
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = true;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            sched_yield();
        }
    }

    // Single-tick update for synchronous callers (menu / UI tests)
    inline void update() {
        // Handled in real-time by tracking_loop
    }

    // ------------------------------------------------------------------------
    // THREAD CONTROLS
    // ------------------------------------------------------------------------
    inline void thread_start() {
        if (g_aim_running.exchange(true)) return;
        g_tracking_thread = std::thread(tracking_loop);
    }

    inline void thread_stop() {
        if (!g_aim_running.exchange(false)) return;
        clear_aim_state();
        if (g_tracking_thread.joinable()) g_tracking_thread.join();
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
