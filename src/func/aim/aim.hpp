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

    // Real Camera Aim Rotation Assist (AimBot / AimHard from STORM_MODS)
    // Directly aligns character and crosshair rotation with target bone
    inline void apply_camera_aim(uint64_t localPlayer, const PlayerData& target, bool lockToHead, bool triggerActive) {
        if (!is_valid_ptr(localPlayer) || !triggerActive) return;

        // Origin point for aim direction (from Camera matrix or local player head)
        Vector3 camPos = GetCameraPosition(data::viewMatrix);
        if (camPos == Vector3::Zero()) {
            uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
            if (localHead) camPos = GetNodePosition(localHead);
        }
        if (camPos == Vector3::Zero() || !std::isfinite(camPos.x)) return;

        // Target bone coordinate
        Vector3 target3D = target.headWorld;
        if (target3D == Vector3::Zero() || !std::isfinite(target3D.x)) return;

        Vector3 hipPos = target.hipWorld;
        bool hasHip = (hipPos != Vector3::Zero() && std::isfinite(hipPos.x));
        Vector3 chestPos = hasHip ? (target3D + (hipPos - target3D) * 0.35f) : (target3D - Vector3(0.0f, 0.28f, 0.0f));
        Vector3 pelvisPos = hasHip ? hipPos : (target3D - Vector3(0.0f, 0.55f, 0.0f));

        if (cfg::aim::target_bone == 1) {
            target3D = chestPos;
        } else if (cfg::aim::target_bone == 2) {
            target3D = pelvisPos;
        } else {
            if (!lockToHead) {
                target3D = chestPos;
            }
        }

        Vector3 dir = target3D - camPos;
        if (dir.sqrMagnitude() <= 1e-4f) return;

        Vector3 normDir = SafeNormalize(dir);
        if (!std::isfinite(normDir.x) || normDir == Vector3::Zero()) return;

        // Calculate target orientation Quaternion
        Quaternion targetRot = Quaternion::LookRotation(normDir, Vector3(0.0f, 1.0f, 0.0f));

        // Smooth rotation interpolation
        float smoothVal = cfg::aim::smooth;
        if (smoothVal <= 1.0f) {
            wpm<Quaternion>(localPlayer + Offsets::Player::Rotation, targetRot);
        } else {
            Quaternion currentRot = rpm<Quaternion>(localPlayer + Offsets::Player::Rotation);
            float t = std::clamp(1.0f / (smoothVal + 0.1f), 0.05f, 1.0f);
            Quaternion blended = Quaternion::Slerp(currentRot, targetRot, t);
            wpm<Quaternion>(localPlayer + Offsets::Player::Rotation, blended);
        }
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
            return;
        }

        if (!data::inMatch.load(std::memory_order_relaxed)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
        if (!is_valid_ptr(localPlayer)) {
            s_lockedTargetAddr = 0;
            s_lockTimeoutTicks = 0;
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        TargetAcquisitionResult acq = acquire_target(localPlayer);
        if (!acq.valid) {
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
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

        // 3D Silent bullet trajectory steering (Aim Silent from STORM_MODS)
        if (cfg::aim::silent) {
            apply_trajectory(localPlayer, acq.target, lockToHead, acq.isFiring);
        }

        // Real Camera Aim Rotation Assist (AimBot / AimHard from STORM_MODS)
        if (cfg::aim::enable) {
            apply_camera_aim(localPlayer, acq.target, lockToHead, acq.isFiring || acq.triggerActive);
        }
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
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
