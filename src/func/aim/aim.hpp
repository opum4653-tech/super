#pragma once
// ============================================================================
// AIMBOT — REAL-TIME DUAL-THREAD VIEW ROTATION ENGINE (FREE FIRE MAX)
// ============================================================================
// Project: crack_master
// Target Platform: com.dts.freefiremax (Offsets strictly from maxff.h & STORM_MODS)
//
// PURE AIMBOT ARCHITECTURE (ZERO SILENT AIM / ZERO COLLIDER TAMPERING):
// 1. Writer Thread (writer_loop):
//    Continuously writes target view orientation into localPlayer + AimRotation (0x61C)
//    during active firing / trigger state. This snaps the local player's view angles
//    and weapon orientation directly to the target head.
// 2. Producer Thread (producer_loop):
//    Tracks closest target in FOV, checks isFiring (0x850 in maxff.h), calculates
//    3D direction from camera to target head, and constructs the target Quaternion
//    via Quaternion::LookRotation(dir, Vector3(0, 1, 0)) matching STORM_MODS line 868.
//
// ZERO FAKE DAMAGE INVARIANT:
// - ZERO enemy collider tampering (Colider_Field 0x80 is 100% untouched).
// - ZERO Silent Aim / raycast redirection.
// - Pure client-side view rotation lock (AimRotation 0x61C).
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "motion.hpp"
#include <sched.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_writer_thread;
    inline std::thread       g_producer_thread;

    inline void clear_aimbot_data() {
        if (g_hasAimbotData.load(std::memory_order_relaxed) || g_aimbot_player != 0) {
            std::lock_guard<std::mutex> lock(g_aimbot_lock);
            g_hasAimbotData.store(false, std::memory_order_relaxed);
            g_aimbot_player = 0;
            g_aimbot_rotation = Quaternion::Identity();
        }
    }

    inline void clear_aim_state() {
        s_lockedTargetAddr = 0;
        s_smoothDir = Vector3::Zero();
    }

    // ------------------------------------------------------------------------
    // HIGH-SPEED AIM ROTATION WRITER THREAD (True Aimbot View Lock)
    // ------------------------------------------------------------------------
    inline void writer_loop() {
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!g_hasAimbotData.load(std::memory_order_relaxed) || g_aimbot_player == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Zero-delay continuous AimRotation write while shooting
            {
                std::lock_guard<std::mutex> lock(g_aimbot_lock);
                if (g_hasAimbotData.load(std::memory_order_relaxed) && g_aimbot_player != 0) {
                    // Write to local player AimRotation (0x61C in maxff.h / STORM_MODS)
                    wpm<Quaternion>(g_aimbot_player + Offsets::Player_Rotation, g_aimbot_rotation);
                }
            }
            sched_yield();
        }
    }

    // ------------------------------------------------------------------------
    // TARGET SELECTOR & AIM ROTATION PRODUCER THREAD (Matching STORM_MODS)
    // ------------------------------------------------------------------------
    inline void producer_loop() {
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!cfg::master_enabled || !cfg::aim::enable || !data::inMatch.load(std::memory_order_relaxed)) {
                clear_aimbot_data();
                clear_aim_state();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }

            uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
            if (!is_valid_ptr(localPlayer)) {
                clear_aimbot_data();
                clear_aim_state();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }

            motion::update();

            TargetAcquisitionResult acq = acquire_target(localPlayer);
            if (!acq.valid || acq.target.curHP <= 0) {
                clear_aimbot_data();
                s_smoothDir = Vector3::Zero();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // Update visual reticle for FOV overlay
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = true;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            // Check firing state strictly as 1-byte bool (maxff.h: isFiring == 0x850, matching STORM_MODS)
            bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            bool triggerActive = (cfg::aim::trigger_mode == 1) ? true : isFiring;

            if (triggerActive && acq.target.headWorld != Vector3::Zero()) {
                // Resolve 3D Camera World Position (matching STORM_MODS GetCameraWorldPosManual)
                Vector3 cameraPos = GetCameraPosition(data::viewMatrix);
                if (cameraPos == Vector3::Zero() || !std::isfinite(cameraPos.x)) {
                    uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
                    if (localHead) cameraPos = GetNodePosition(localHead);
                }

                if (cameraPos != Vector3::Zero() && std::isfinite(cameraPos.x)) {
                    Vector3 targetHead = acq.target.getHeadAimPoint(cfg::aim::head_bias);
                    Vector3 dir = targetHead - cameraPos;

                    if (dir.sqrMagnitude() > 1e-4f && std::isfinite(dir.x)) {
                        Vector3 normDir = dir.normalized();
                        Quaternion targetRot = Quaternion::LookRotation(normDir, Vector3(0, 1, 0));

                        std::lock_guard<std::mutex> lock(g_aimbot_lock);
                        g_aimbot_player = localPlayer;
                        g_aimbot_rotation = targetRot;
                        g_hasAimbotData.store(true, std::memory_order_relaxed);
                    }
                }
            } else {
                clear_aimbot_data();
            }

            sched_yield();
        }
    }

    inline void update() {
        // Handled in real-time by producer_loop and writer_loop
    }

    // ------------------------------------------------------------------------
    // THREAD CONTROLS
    // ------------------------------------------------------------------------
    inline void thread_start() {
        if (g_aim_running.exchange(true)) return;
        g_writer_thread   = std::thread(writer_loop);
        g_producer_thread = std::thread(producer_loop);
    }

    inline void thread_stop() {
        if (!g_aim_running.exchange(false)) return;
        clear_aimbot_data();
        clear_aim_state();
        if (g_writer_thread.joinable())   g_writer_thread.join();
        if (g_producer_thread.joinable()) g_producer_thread.join();
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
