#pragma once
// ============================================================================
// AIMBOT — EVENT-DRIVEN SMOOTH VIEW LOCK (FREE FIRE MAX)
// ============================================================================
// Project: crack_master
// Target Platform: com.dts.freefiremax (ARM64-v8a)
//
// SYSTEM ARCHITECTURE (NO TIMERS, NO CACHE, NO PRE-WRITE):
// 1. The thread waits on cfg::g_state_cv. It wakes only when the UI changes a
//    menu value or the scraper publishes a fresh entity snapshot.
// 2. Every wake resolves the target from the current snapshot. There is no
//    sticky rotation cache and no fixed 1 ms / 4 ms write cadence.
// 3. When Master or Aimbot is OFF, or firing/trigger is inactive, ZERO memory
//    writes are issued. The game keeps its official view rotation untouched.
// 4. When active, the view moves from the game's current rotation toward the
//    target using Quaternion::Slerp. The menu Smooth slider controls the step:
//    0 = instant, 100 = no movement. It is not a hidden/default behaviour.
// 5. Only localPlayer + Player_Rotation (0x61C) is written. No enemy collider
//    or other game state is modified.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "motion.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_aim_thread;

    inline void clear_target_snapshot() {
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
        g_currentTarget.screenPos = Vector3::Zero();
    }

    // ------------------------------------------------------------------------
    // REAL-TIME AIM LOOP — driven by real menu/scraper state changes only.
    // ------------------------------------------------------------------------
    inline void aim_loop() {
        uint64_t lastState = cfg::g_state_generation;
        std::unique_lock<std::mutex> waitLock(cfg::g_runtime_mutex);

        while (g_aim_running.load(std::memory_order_relaxed)) {
            cfg::g_state_cv.wait(waitLock, [&]() {
                return !g_aim_running.load(std::memory_order_relaxed) ||
                       cfg::g_state_generation != lastState;
            });
            if (!g_aim_running.load(std::memory_order_relaxed)) break;
            lastState = cfg::g_state_generation;
            waitLock.unlock();

            const cfg::RuntimeConfig rc = cfg::runtime();
            const bool active = rc.master && rc.aimEnable &&
                                data::inMatch.load(std::memory_order_relaxed);
            const uint64_t localPlayer = active
                ? data::localPlayerAddr.load(std::memory_order_relaxed) : 0;

            if (!active || !is_valid_ptr(localPlayer)) {
                clear_target_snapshot();
                waitLock.lock();
                continue;
            }

            motion::update(); // Zero-op; motion exploits stay disabled.

            TargetAcquisitionResult acq = acquire_target(localPlayer);
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = acq.valid;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            const bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            const bool triggerActive = (rc.aimTriggerMode == 1) || isFiring;

            if (!triggerActive || !acq.valid || acq.target.headWorld == Vector3::Zero()) {
                clear_target_snapshot();
                waitLock.lock();
                continue;
            }

            Vector3 cameraPos = GetCameraPosition(data::get_view_matrix());
            if (cameraPos == Vector3::Zero() || !std::isfinite(cameraPos.x)) {
                const uint64_t localHead = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
                if (localHead != 0) cameraPos = GetNodePosition(localHead);
            }

            if (cameraPos != Vector3::Zero() && std::isfinite(cameraPos.x)) {
                const Vector3 targetHead = acq.target.getHeadAimPoint(rc.aimHeadBias);
                // Vector3::sqrMagnitude()/normalized() are non-const in this
                // math header, so keep the working direction mutable.
                Vector3 dir = targetHead - cameraPos;

                if (dir.sqrMagnitude() > 1e-4f && std::isfinite(dir.x)) {
                    const Quaternion targetRot =
                        Quaternion::LookRotation(dir.normalized(), Vector3(0, 1, 0));

                    float smooth = rc.aimSmooth;
                    if (!std::isfinite(smooth)) smooth = 0.0f;
                    if (smooth < 0.0f) smooth = 0.0f;
                    if (smooth > 100.0f) smooth = 100.0f;

                    // Smooth = 100 means the user explicitly asked for no
                    // movement. Do not write at all in that state.
                    if (smooth < 100.0f) {
                        Quaternion currentRot =
                            rpm<Quaternion>(localPlayer + Offsets::Player_Rotation);
                        if (!std::isfinite(currentRot.x) || !std::isfinite(currentRot.y) ||
                            !std::isfinite(currentRot.z) || !std::isfinite(currentRot.w) ||
                            Quaternion::Norm(currentRot) < 1e-5f) {
                            currentRot = targetRot;
                        }

                        const float t = 1.0f - (smooth / 100.0f);
                        const Quaternion blended = Quaternion::Slerp(currentRot, targetRot, t);
                        wpm_verified(localPlayer + Offsets::Player_Rotation, blended);
                    }
                }
            }

            waitLock.lock();
        }

        waitLock.unlock();
        clear_target_snapshot();
    }

    inline void update() {
        // Handled by aim_loop on real state-change events.
    }

    // ------------------------------------------------------------------------
    // THREAD CONTROLS
    // ------------------------------------------------------------------------
    inline void thread_start() {
        if (g_aim_running.exchange(true)) return;
        g_aim_thread = std::thread(aim_loop);
    }

    inline void thread_stop() {
        if (!g_aim_running.exchange(false)) return;
        cfg::notify_state_change();
        if (g_aim_thread.joinable()) g_aim_thread.join();
        clear_target_snapshot();
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
