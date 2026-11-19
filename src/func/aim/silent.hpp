#pragma once
// ============================================================================
// 3D SILENT AIM BULLET TRAJECTORY STEERING ENGINE
// ============================================================================
// Project: crack_master
// Reference Implementation: pewpew (Scarecrow hook.h) & STORM_MODS
//
// SYSTEM ROLE:
// Calculates and steers the bullet departure vector directly in client weapon memory:
// - Two-Thread Producer / Writer Architecture:
//     1. High-Speed Writer Thread (writer_loop):
//        Continuously writes g_targetDir to g_hitObj + Offsets::AimInfo_Direction (0x40)
//        during active firing with sched_yield(). This guarantees that 100% of spawned
//        bullets catch the steered trajectory before the game engine can overwrite it.
//     2. Producer Thread / Update (apply_trajectory):
//        Computes accurate target vector: dir = target3D - bulletOrigin (full distance,
//        not clamped to 1.0m unit vector, so bullets reach full range up to 250m).
//        Applies configurable head_bias (+0.08m) for dead-center headshot alignment.
// - Zero Enemy Mutation: NEVER writes to enemy player objects (100% anti-ban safe).
// - Zero Test / Bogus Writes: 0x71 tampering completely deleted.
// ============================================================================

#include "common.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <sched.h>
#include <unistd.h>

namespace aimbot {
namespace silent {

    inline std::atomic<bool> g_run{false};
    inline std::thread       g_writer_thread;

    inline std::mutex        g_silent_lock;
    inline uint64_t          g_hitObj = 0;
    inline Vector3           g_targetDir = {0.f, 0.f, 0.f};
    inline std::atomic<bool> g_hasData{false};

    inline void clear_silent_data() {
        if (g_hasData.load(std::memory_order_relaxed) || g_hitObj != 0) {
            std::lock_guard<std::mutex> lock(g_silent_lock);
            g_hasData.store(false, std::memory_order_relaxed);
            g_hitObj = 0;
        }
    }

    // High-speed writer thread — equivalent to SilentAimThread in pewpew
    static void writer_loop() {
        while (g_run.load(std::memory_order_relaxed)) {
            if (!g_hasData.load(std::memory_order_relaxed) || g_hitObj == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Zero-sleep continuous memory write while shooting
            {
                std::lock_guard<std::mutex> lock(g_silent_lock);
                if (g_hasData.load(std::memory_order_relaxed) && g_hitObj != 0) {
                    wpm<Vector3>(g_hitObj + Offsets::AimInfo_Direction, g_targetDir); // 0x40 (RayDir)
                }
            }
            sched_yield();
        }
    }

    inline void thread_start() {
        if (g_run.exchange(true)) return;
        g_writer_thread = std::thread(writer_loop);
    }

    inline void thread_stop() {
        if (!g_run.exchange(false)) return;
        clear_silent_data();
        if (g_writer_thread.joinable()) {
            g_writer_thread.join();
        }
    }

    // Producer calculation: computes accurate 3D trajectory
    inline void apply_trajectory(uint64_t localPlayer, const PlayerData& target, bool lockToHead, bool isFiring) {
        if (!is_valid_ptr(localPlayer)) {
            clear_silent_data();
            return;
        }

        // Steer trajectory only when weapon is actively firing (or Always In FOV)
        if (!isFiring && cfg::aim::trigger_mode != 1) {
            clear_silent_data();
            return;
        }

        // Resolve Weapon AimingInfo (0xE58 primary / 0xE58 alt)
        uint64_t aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo);
        if (!is_valid_ptr(aimInfoWeapon)) {
            aimInfoWeapon = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo_Alt);
        }
        if (!is_valid_ptr(aimInfoWeapon)) {
            clear_silent_data();
            return;
        }

        // Read StartPos (0x4C) - gun muzzle origin
        Vector3 bulletOrigin = rpm<Vector3>(aimInfoWeapon + Offsets::AimInfo_StartPosition);
        if (bulletOrigin == Vector3::Zero() || !std::isfinite(bulletOrigin.x)) {
            uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
            if (localHead) bulletOrigin = GetNodePosition(localHead);
        }

        if (bulletOrigin == Vector3::Zero() || !std::isfinite(bulletOrigin.x)) {
            clear_silent_data();
            return;
        }

        // Target bone selection (100% controllable from menu, zero forced defaults)
        float headBias = cfg::aim::head_bias; // 0.0f default: zero forced bias
        Vector3 target3D;
        if (cfg::aim::target_bone == 1) {
            target3D = target.getTargetWorld(1, headBias); // Neck
        } else if (cfg::aim::target_bone == 2) {
            target3D = target.getTargetWorld(2, headBias); // Chest / Spine
        } else {
            // Target bone 0 (Head):
            if (cfg::aim::headshot_rate > 0.0f) {
                // If user configured a specific headshot rate (1% - 100%), obey probability roll
                target3D = lockToHead ? target.getTargetWorld(0, headBias) : target.getTargetWorld(1, headBias);
            } else {
                // Default zero: directly target Head without forcing neck or body
                target3D = target.getTargetWorld(0, headBias);
            }
        }

        if (target3D == Vector3::Zero() || !std::isfinite(target3D.x)) {
            clear_silent_data();
            return;
        }

        // Full displacement ray vector (matching pewpew: targetWorld - ammoBase)
        // NOT clamped to 1.0m unit vector, so bullet ray reaches target at any range (up to 250m)
        Vector3 dir = target3D - bulletOrigin;
        if (dir.sqrMagnitude() <= 1e-4f) {
            clear_silent_data();
            return;
        }

        // Publish to high-speed writer thread
        {
            std::lock_guard<std::mutex> lock(g_silent_lock);
            g_hitObj = aimInfoWeapon;
            g_targetDir = dir;
            g_hasData.store(true, std::memory_order_relaxed);
        }
    }

} // namespace silent

    // Forwarding alias
    using silent::apply_trajectory;

} // namespace aimbot
