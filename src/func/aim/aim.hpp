#pragma once
#include "common.hpp"
#include "target.hpp"
#include "headshot.hpp"
#include "silent.hpp"
#include "fov.hpp"
#include "motion.hpp"

// TRAJECTORY ASSIST & MOTION ENHANCEMENT ENGINE (NATURAL GAMEPLAY — ZERO CAMERA JUMPING)
// Features:
// 1. Natural Gameplay: Camera rotation is 100% untouched. Finger aiming is free.
// 2. Strict Target Stickiness & Hysteresis: ZERO screen shaking when multiple enemies enter FOV.
// 3. Grace-Period Burst Lock: Prevents lock resets between rapid bullet intervals.
// 4. Trajectory Write: only the client bullet direction is steered. Recoil is
//    never read, never scaled, never written — the game's own recoil runs as shipped.
// 5. Strict FOV Circle: Strictly respects FOV limit; never locks outside.
// 6. Wall Filter: a target behind cover is never acquired and never held.
// 7. Motion Modifiers: Air Flight, Sprint Boost, and Phantom Mode.

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

        // Record target diagnostic telemetry
        g_currentTarget.valid = true;
        snprintf(g_currentTarget.name, sizeof(g_currentTarget.name), "%s",
                 acq.target.name[0] ? acq.target.name : (acq.target.isBot ? "BOT" : "Player"));
        g_currentTarget.distance = acq.target.distance;
        g_currentTarget.hp = static_cast<int>(acq.target.curHP);
        g_currentTarget.screenPos = acq.bestScreenPos;
        g_currentTarget.headWorld = acq.target.headWorld;

        // Headshot evaluation & collider mapping
        bool lockToHead = evaluate_headshot(acq.target, acq.isScoping);

        // 3D Silent bullet trajectory steering
        apply_trajectory(localPlayer, acq.target, lockToHead);
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
