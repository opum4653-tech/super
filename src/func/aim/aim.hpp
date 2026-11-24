#pragma once
// ============================================================================
// AIMBOT — REAL-TIME DUAL-THREAD HEADSHOT TRAJECTORY ENGINE (FREE FIRE MAX)
// ============================================================================
// Project: crack_master
// Target Platform: com.dts.freefiremax (Offsets strictly from maxff.h)
//
// PROVEN PEWPEW ARCHITECTURE:
// 1. Writer Thread (writer_loop):
//    Floods g_targetDir into local weapon AimInfo.Direction (0x40 raycast in maxff.h)
//    during active firing. This ensures bullets snap directly to the target head
//    before Unity updates weapon spread/rotation.
// 2. Producer Thread (producer_loop):
//    Tracks closest target in FOV, checks isFiring (0x850 in maxff.h), resolves
//    AimInfo (0xE58 in maxff.h) & StartPos (0x4C in maxff.h), and computes the 3D
//    head trajectory vector (targetHead - ammoBase).
//
// ZERO FAKE DAMAGE INVARIANT:
// - NEVER mutates enemy Colider_Field (0x80), which causes server-side desync.
// - Only the local player's weapon raycast (0x40) is redirected.
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

    inline void clear_silent_data() {
        if (g_hasData.load(std::memory_order_relaxed) || g_hitObj != 0) {
            std::lock_guard<std::mutex> lock(g_silent_lock);
            g_hasData.store(false, std::memory_order_relaxed);
            g_hitObj = 0;
        }
    }

    inline void clear_aim_state() {
        s_lockedTargetAddr = 0;
        s_smoothDir = Vector3::Zero();
    }

    // ------------------------------------------------------------------------
    // HIGH-SPEED WEAPON RAYCAST WRITER THREAD (Matching pewpew writer_loop)
    // ------------------------------------------------------------------------
    inline void writer_loop() {
        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!g_hasData.load(std::memory_order_relaxed) || g_hitObj == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Zero-delay continuous memory write while shooting
            {
                std::lock_guard<std::mutex> lock(g_silent_lock);
                if (g_hasData.load(std::memory_order_relaxed) && g_hitObj != 0) {
                    // Write to local weapon AimInfo.raycast (0x40 in maxff.h)
                    wpm<Vector3>(g_hitObj + Offsets::AimInfo_Direction, g_targetDir);
                }
            }
            sched_yield();
        }
    }

    // ------------------------------------------------------------------------
    // TARGET SELECTOR & TRAJECTORY PRODUCER THREAD (Matching pewpew)
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
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
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
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }

            motion::update();

            TargetAcquisitionResult acq = acquire_target(localPlayer);
            if (!acq.valid || acq.target.curHP <= 0) {
                clear_silent_data();
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

            // Resolve local weapon AimInfo (maxff.h: aimInfo == 0xE58)
            uint64_t hitObjAddress = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo);
            if (!is_valid_ptr(hitObjAddress)) {
                hitObjAddress = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo_Alt);
            }

            // Check firing state strictly as 1-byte bool (maxff.h: isFiring == 0x850, matching STORM_MODS)
            bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            bool triggerActive = (cfg::aim::trigger_mode == 1) ? true : isFiring;

            if (triggerActive && is_valid_ptr(hitObjAddress) && acq.target.headWorld != Vector3::Zero()) {
                // Resolve muzzle startPos (maxff.h: startPos == 0x4C)
                Vector3 ammoBase = rpm<Vector3>(hitObjAddress + Offsets::AimInfo_StartPosition);
                if (ammoBase == Vector3::Zero() || !std::isfinite(ammoBase.x)) {
                    uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
                    if (localHead) ammoBase = GetNodePosition(localHead);
                }

                if (ammoBase != Vector3::Zero() && std::isfinite(ammoBase.x)) {
                    Vector3 targetHead = acq.target.getHeadAimPoint(cfg::aim::head_bias);
                    Vector3 dir;
                    dir.x = targetHead.x - ammoBase.x;
                    dir.y = targetHead.y - ammoBase.y;
                    dir.z = targetHead.z - ammoBase.z;

                    if (dir.sqrMagnitude() > 1e-4f && std::isfinite(dir.x)) {
                        std::lock_guard<std::mutex> lock(g_silent_lock);
                        g_hitObj = hitObjAddress;
                        g_targetDir = dir;
                        g_hasData.store(true, std::memory_order_relaxed);
                    }
                }
            } else {
                clear_silent_data();
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
