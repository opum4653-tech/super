#pragma once
// ============================================================================
// AIMBOT — SINGLE DRAG HEADSHOT ENGINE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// One and only aim path. "Aim Silent" has been removed completely: there is no
// second toggle, no separate writer thread and no RNG body fallback. When the
// Aimbot trigger fires, the muzzle-to-head unit vector is written straight into
// our own weapon's AimInfo.Direction (0x40). Offsets are verified, so every
// bullet is dragged onto the head collider (100% head connection).
//
// ANTI-BAN INVARIANT:
// We never mutate enemy entity memory and never write camera rotation. Only our
// own weapon's AimInfo direction is touched, and writes stop the instant the
// trigger goes idle or the target is lost.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "motion.hpp"

namespace aimbot {

    // Camera rotation write stays permanently neutralized for anti-ban safety.
    inline void apply_camera_aim(uint64_t localPlayer, const PlayerData& target, bool triggerActive) {
        (void)localPlayer;
        (void)target;
        (void)triggerActive;
    }

    inline std::atomic<bool>& aim_thread_running() {
        static std::atomic<bool> s_run{false};
        return s_run;
    }

    // ------------------------------------------------------------------------
    // DRAG HEADSHOT — the only trajectory writer in the project.
    // ------------------------------------------------------------------------
    // Normalized direction from muzzle (AimInfo.StartPosition, 0x4C) to the
    // enemy head bone. There is deliberately no headshot roll and no chest
    // fallback: target bone 0 is always the head. cfg::aim::smooth only lerps
    // the vector between frames; the result is re-normalized so Unity always
    // receives a unit vector.
    inline void apply_drag_shot(uint64_t localPlayer, const PlayerData& target, bool triggerActive) {
        if (!is_valid_ptr(localPlayer) || !triggerActive) {
            s_smoothDir = Vector3::Zero();
            return;
        }
        if (target.curHP <= 0) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        // Resolve our own weapon AimingInfo (primary / alt alias)
        uint64_t aimInfo = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo);
        if (!is_valid_ptr(aimInfo)) {
            aimInfo = rpm<uint64_t>(localPlayer + Offsets::Player_AimInfo_Alt);
        }
        if (!is_valid_ptr(aimInfo)) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        // Muzzle origin; fall back to our own head bone only if invalid
        Vector3 origin = rpm<Vector3>(aimInfo + Offsets::AimInfo_StartPosition);
        if (origin == Vector3::Zero() || !std::isfinite(origin.x)) {
            uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
            if (localHead) origin = GetNodePosition(localHead);
        }
        if (origin == Vector3::Zero() || !std::isfinite(origin.x)) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        // ALWAYS HEAD. Offset-verified head bone with user head_bias alignment.
        Vector3 head = target.getHeadAimPoint(cfg::aim::head_bias);
        if (head == Vector3::Zero() || !std::isfinite(head.x)) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        Vector3 dir = head - origin;
        if (dir.sqrMagnitude() <= 1e-4f) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        Vector3 unit = SafeNormalize(dir);
        if (!std::isfinite(unit.x) || unit == Vector3::Zero()) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        Vector3 writeDir = unit;
        float sm = cfg::aim::smooth;
        if (sm > 0.0f && s_smoothDir != Vector3::Zero() &&
            std::isfinite(s_smoothDir.x) && s_smoothDir.sqrMagnitude() > 0.5f) {
            float k = 1.0f / (sm + 1.0f);
            if (k < 0.02f) k = 0.02f;
            if (k > 1.0f) k = 1.0f;
            writeDir = Vector3(
                s_smoothDir.x + (unit.x - s_smoothDir.x) * k,
                s_smoothDir.y + (unit.y - s_smoothDir.y) * k,
                s_smoothDir.z + (unit.z - s_smoothDir.z) * k);
            float lenSq = writeDir.sqrMagnitude();
            if (lenSq > 1e-6f) {
                float invLen = 1.0f / sqrtf(lenSq);
                writeDir.x *= invLen;
                writeDir.y *= invLen;
                writeDir.z *= invLen;
            } else {
                writeDir = unit;
            }
        }

        s_smoothDir = writeDir;
        wpm<Vector3>(aimInfo + Offsets::AimInfo_Direction, writeDir); // 0x40 RayDir
    }

    // ------------------------------------------------------------------------
    // AIM TICK
    // ------------------------------------------------------------------------
    inline void clear_aim_state() {
        s_lockedTargetAddr = 0;
        s_lockTimeoutTicks = 0;
        s_smoothDir = Vector3::Zero();
    }

    inline void update() {
        if (!cfg::master_enabled) {
            motion::reset();
            clear_aim_state();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        motion::update();

        if (!cfg::aim::enable) {
            clear_aim_state();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        if (!data::inMatch.load(std::memory_order_relaxed)) {
            clear_aim_state();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
        if (!is_valid_ptr(localPlayer)) {
            clear_aim_state();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        TargetAcquisitionResult acq = acquire_target(localPlayer);
        if (!acq.valid || acq.target.curHP <= 0) {
            s_smoothDir = Vector3::Zero();
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = false;
            return;
        }

        // triggerActive encodes When Firing / Always In FOV (scope modes removed).
        if (!acq.triggerActive) {
            s_smoothDir = Vector3::Zero();
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_targetMutex);
            g_currentTarget.valid = true;
            g_currentTarget.screenPos = acq.bestScreenPos;
        }

        // Single head-lock trajectory write.
        apply_drag_shot(localPlayer, acq.target, acq.triggerActive);

        // No-op by design; kept as the explicit anti-ban boundary.
        apply_camera_aim(localPlayer, acq.target, acq.triggerActive);
    }

    inline void loop() {
        while (aim_thread_running().load(std::memory_order_relaxed)) {
            if (cfg::master_enabled && cfg::aim::enable) {
                update();
            } else {
                s_smoothDir = Vector3::Zero();
            }

            // Natural game frame cadence, no forced microsecond pacing.
            int interval = cfg::aim::read_interval_ms;
            int sleepMs = (interval >= 8 && interval <= 33) ? interval : 16;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }

    inline void thread_start() {
        if (aim_thread_running().exchange(true)) return;
        std::thread(loop).detach();
    }

    inline void thread_stop() {
        aim_thread_running().store(false);
        motion::reset();
        s_smoothDir = Vector3::Zero();
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
