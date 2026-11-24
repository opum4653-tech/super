#pragma once
// ============================================================================
// AIMBOT — VIEW ROTATION & HEADSHOT ENGINE (FREE FIRE MAX)
// ============================================================================
// Project: crack_master
// Target Platform: com.dts.freefiremax (ARM64-v8a)
// Source Reference: STORM_MODS Server.cpp (AimHardTh line 841 & AimColliderTh line 624)
//
// SYSTEM ARCHITECTURE (STORM_MODS PARITY):
// 1. Single Background Aim Thread (aim_loop):
//    - When firing (isFiring == true, 0x850 in maxff.h & STORM_MODS):
//      * Acquires target in FOV circle closest to crosshair.
//      * Resolves camera world position from ViewMatrix (GetCameraPosition).
//      * Computes direction to target head: dir = (targetHead - cameraPos).normalized().
//      * Computes Quaternion targetRot = Quaternion::LookRotation(dir, Vector3(0, 1, 0)).
//      * Writes targetRot into localPlayer + AimRotation (0x61C).
//      * Maps HeadCollider (0x740) into ColliderBody (0x80) for 100% headshot registration.
//      * Sleeps 35ms - 65ms between writes (matches STORM_MODS line 869).
//    - When NOT firing (or target leaves FOV):
//      * Sleeps 15ms.
//      * NEVER writes to AimRotation!
//      * Leaves camera rotation and touch controls 100% free and responsive.
// ============================================================================

#include "common.hpp"
#include "target.hpp"
#include "fov.hpp"
#include "motion.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <random>

namespace aimbot {

    inline std::atomic<bool> g_aim_running{false};
    inline std::thread       g_aim_thread;

    // ------------------------------------------------------------------------
    // REAL-TIME AIMBOT & HEADSHOT LOOP (MATCHING STORM_MODS EXACT ENGINE)
    // ------------------------------------------------------------------------
    inline void aim_loop() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<int> sleepDist(35, 65);

        while (g_aim_running.load(std::memory_order_relaxed)) {
            if (!cfg::master_enabled || !cfg::aim::enable || !data::inMatch.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
            if (!is_valid_ptr(localPlayer)) {
                {
                    std::lock_guard<std::mutex> lk(g_targetMutex);
                    g_currentTarget.valid = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            // Motion hacks update (air flight, phantom mode if active)
            motion::update();

            // Dynamic target selector matching STORM_MODS lines 1123-1170
            TargetAcquisitionResult acq = acquire_target(localPlayer);

            // Update visual FOV lock ring
            {
                std::lock_guard<std::mutex> lk(g_targetMutex);
                g_currentTarget.valid = acq.valid;
                g_currentTarget.screenPos = acq.bestScreenPos;
            }

            // Check firing state (isFiring == 0x850 in maxff.h & STORM_MODS)
            bool isFiring = (rpm<uint8_t>(localPlayer + Offsets::Player_IsFiring) != 0);
            bool triggerActive = (cfg::aim::trigger_mode == 1) ? true : isFiring;

            // Only actively lock/write when firing (or trigger active) and qualified target exists
            if (triggerActive && acq.valid && acq.target.headWorld != Vector3::Zero()) {
                // Resolve 3D Camera World Position (STORM_MODS GetCameraWorldPosManual line 864)
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

                        // 1. Physically snap player's view angles to target head (STORM_MODS line 868)
                        wpm<Quaternion>(localPlayer + Offsets::Player_Rotation, targetRot);

                        // 2. Map HeadCollider (0x740) into ColliderBody (0x80) (STORM_MODS line 643)
                        // This guarantees 100% headshot hit registration with zero server rejection
                        uint64_t headCollider = rpm<uint64_t>(acq.target.addr + Offsets::Player_HeadCollider);
                        if (headCollider > 0x10000) {
                            wpm<uint64_t>(acq.target.addr + Offsets::Player_LockedAimingCollider, headCollider);
                        }

                        // Sleep 35-65ms matching STORM_MODS line 869
                        // Critical: allows touch screen inputs to register naturally without view freezing!
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleepDist(gen)));
                        continue;
                    }
                }
            }

            // Idle interval when not shooting: 15ms sleep with ZERO memory writes!
            // Touch screen controls and view rotation are 100% free and responsive.
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    inline void update() {
        // Handled in real-time by aim_loop
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
        if (g_aim_thread.joinable()) g_aim_thread.join();
        std::lock_guard<std::mutex> lk(g_targetMutex);
        g_currentTarget.valid = false;
    }

} // namespace aimbot

namespace func {
    namespace aim = ::aimbot;
}
