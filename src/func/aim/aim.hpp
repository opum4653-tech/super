#pragma once
// ============================================================================
// AIMBOT — DUAL-THREAD TRAJECTORY & HEADSHOT INJECTION ENGINE
// ============================================================================
// Project: crack_master (Pure External Ring-3 Assisted Aim)
// Sourced from: pewpew (includes/func/silent.hpp) & STORM_MODS (Server.cpp)
//
// DUAL-THREAD PRODUCER / CONSUMER ARCHITECTURE:
// 1. High-Speed Writer Thread (writer_loop):
//    Runs with sched_yield() during active firing, continuously writing g_targetDir
//    to local weapon's AimInfo.Direction (0x40 RayDir in maxff.h) and setting enemy
//    ColliderBody (0x80) to ColliderHead (0x740).
//    This prevents the game engine from overwriting the raycast vector before
//    the bullet spawns, guaranteeing 100% headshot registration!
// 2. Target Selector Thread (producer_loop):
//    Runs target acquisition, calculates 3D displacement trajectory (targetHead - ammoBase),
//    applies smooth interpolation, and updates the shared lock state.
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

    // Clears active silent aim data when firing ends
    inline void clear_silent_data() {
        if (g_hasData.load(std::memory_order_relaxed) || g_hitObj != 0) {
            std::lock_guard<std::mutex> lock(g_silent_lock);
            g_hasData.store(false, std::memory_order_relaxed);
            g_hitObj = 0;
            g_targetEnemy = 0;
            g_targetHeadCollider = 0;
        }
    }

    inline void clear_aim_state() {
        s_lockedTargetAddr = 0;
        s_lockTimeoutTicks = 0;
        s_smoothDir = Vector3::Zero();
    }

    // ------------------------------------------------------------------------
    // HIGH-SPEED WRITER THREAD (Zero-Latency Raycast Flooding)
    // ------------------------------------------------------------------------
    // Matched directly to pewpew/src/func/silent.hpp & STORM_MODS/Server.cpp.
    // Continuously writes target trajectory to weapon AimInfo + 0x40 and redirects
    // target's body collider (+0x80) to head collider (+0x740).
    inline void writer_loop() {
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!g_hasData.load(std::memory_order_relaxed) || g_hitObj == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Zero-delay continuous memory flooding while firing
            {
                std::lock_guard<std::mutex> lock(g_silent_lock);
                if (g_hasData.load(std::memory_order_relaxed) && g_hitObj != 0) {
                    // 1. Write full 3D trajectory direction (0x40 RayDir in maxff.h)
                    wpm<Vector3>(g_hitObj + Offsets::AimInfo_Direction, g_targetDir);

                    // 2. 100% Headshot Collider Redirection (0x80 -> 0x740 in maxff.h)
                    if (g_targetEnemy != 0 && g_targetHeadCollider != 0) {
                        wpm<uint64_t>(g_targetEnemy + Offsets::Colider_Field, g_targetHeadCollider);
                    }
                }
            }
            sched_yield();
        }
    }

    // ------------------------------------------------------------------------
    // PRODUCER / TARGET ACQUISITION THREAD
    // ------------------------------------------------------------------------
    inline void producer_loop() {
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!cfg::master_enabled || !cfg::aim::enable || !data::inMatch.load(std::memory_order_relaxed)) {
                clear_silent_data();
                clear_aim_state();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
            if (!is_valid_ptr(localPlayer)) {
                clear_silent_data();
                clear_aim_state();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }

            motion::update();

            TargetAcquisitionResult acq = acquire_target(localPlayer);
            if (!acq.valid || acq.target.curHP <= 0 || !acq.triggerActive) {
                clear_silent_data();
                s_smoothDir = Vector3::Zero();
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // Target acquired and trigger is active!
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = true;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            // Resolve our weapon's AimingInfo (maxff.h: aimInfo == 0xE58)
            uint64_t aimInfo = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo);
            if (!is_valid_ptr(aimInfo)) {
                aimInfo = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo_Alt);
            }

            if (!is_valid_ptr(aimInfo)) {
                clear_silent_data();
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }

            // Muzzle origin (maxff.h: startPos == 0x4C)
            Vector3 ammoBase = rpm<Vector3>(aimInfo + Offsets::AimInfo_StartPosition);
            if (ammoBase == Vector3::Zero() || !std::isfinite(ammoBase.x)) {
                uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
                if (localHead) ammoBase = GetNodePosition(localHead);
            }

            if (ammoBase == Vector3::Zero() || !std::isfinite(ammoBase.x)) {
                clear_silent_data();
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }

            // Target head world position with optional head bias
            Vector3 targetHead = acq.target.getHeadAimPoint(cfg::aim::head_bias);
            if (targetHead == Vector3::Zero() || !std::isfinite(targetHead.x)) {
                clear_silent_data();
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }

            // Compute exact 3D displacement vector (matching pewpew & STORM_MODS)
            Vector3 dir;
            dir.x = targetHead.x - ammoBase.x;
            dir.y = targetHead.y - ammoBase.y;
            dir.z = targetHead.z - ammoBase.z;

            if (dir.sqrMagnitude() <= 1e-4f) {
                clear_silent_data();
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }

            // Smooth interpolation if enabled by user
            Vector3 writeDir = dir;
            float sm = cfg::aim::smooth;
            if (sm > 0.0f && s_smoothDir != Vector3::Zero() && std::isfinite(s_smoothDir.x)) {
                float k = 1.0f / (sm + 1.0f);
                if (k < 0.02f) k = 0.02f;
                if (k > 1.0f) k = 1.0f;
                writeDir = Vector3(
                    s_smoothDir.x + (dir.x - s_smoothDir.x) * k,
                    s_smoothDir.y + (dir.y - s_smoothDir.y) * k,
                    s_smoothDir.z + (dir.z - s_smoothDir.z) * k);
            }
            s_smoothDir = writeDir;

            // Resolve target's head collider (maxff.h: HedColider == 0x740)
            uint64_t headCollider = rpm<uint64_t>(acq.target.addr + Offsets::HedColider);

            {
                std::lock_guard<std::mutex> lock(g_silent_lock);
                g_hitObj = aimInfo;
                g_targetDir = writeDir;
                g_targetEnemy = acq.target.addr;
                g_targetHeadCollider = (headCollider > 0x10000) ? headCollider : 0;
                g_hasData.store(true, std::memory_order_relaxed);
            }

            // Yield briefly during active firing so writer thread gets maximum CPU throughput
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Single-tick update for synchronous callers (menu / UI tests)
    inline void update() {
        // Producer thread handles background loop automatically
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
        clear_silent_data();
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
