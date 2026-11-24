#pragma once
// ============================================================================
// AIMBOT COMMON MATHEMATICAL TYPES & LOCK STATE PRIMITIVES
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Shared foundation header for all sub-modules of the aim suite:
// - target.hpp   (Acquisition & scoring)
// - aim.hpp      (Single head-only drag-shot trajectory writer)
// - fov.hpp      (Screen FOV circle & target ring rendering)
// - motion.hpp   (Air flight & phantom mode modifiers)
// ============================================================================

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "../../game/player.hpp"
#include "../../game/data.hpp"
#include "../../game/math.hpp"
#include "../../offsets/offsets.hpp"
#include "../../ui/cfg.hpp"
#include "../../other/memory.hpp"
#include "../../other/vector3.h"
#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <cstdint>
#include <unistd.h>

extern float g_sw;
extern float g_sh;

namespace aimbot {

    // ------------------------------------------------------------------------
    // TARGET LOCK VISUAL STATE
    // ------------------------------------------------------------------------
    // Read by fov.hpp to draw targeting lock reticle ring on active enemy
    struct TargetInfo {
        bool valid = false;
        Vector3 screenPos = Vector3::Zero();
    };

    inline TargetInfo g_currentTarget;
    inline std::mutex g_targetMutex;

    // ------------------------------------------------------------------------
    // SAFE 3D UNIT VECTOR NORMALIZATION
    // ------------------------------------------------------------------------
    // Protects against zero-length vectors, division-by-zero, and IEEE-754 NaN/Inf.
    // Unity's Quaternion::LookRotation crashes or corrupts if fed an unnormalized or NaN vector.
    inline Vector3 SafeNormalize(const Vector3& v) {
        float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lenSq < 1e-6f || !std::isfinite(lenSq)) {
            return Vector3(0.0f, 0.0f, 1.0f); // Fallback: forward unit vector
        }
        float invLen = 1.0f / sqrtf(lenSq);
        return Vector3(v.x * invLen, v.y * invLen, v.z * invLen);
    }

    // ------------------------------------------------------------------------
    // TARGET HYSTERESIS & STICKINESS STATE
    // ------------------------------------------------------------------------
    // Prevents target jumping and screen shaking when 2+ enemies are in the FOV circle.
    // Once locked to an enemy address, the lock is retained until the target dies,
    // leaves the extended FOV radius, or the grace period expires.
    inline uint64_t s_lockedTargetAddr = 0;
    inline int      s_lockTimeoutTicks = 0;

    // Smooth direction tracking across frame updates
    inline Vector3 s_smoothDir = Vector3::Zero();

    // High-speed writer thread state (Two-Thread Architecture from pewpew & STORM_MODS)
    inline std::mutex        g_silent_lock;
    inline uint64_t          g_hitObj = 0;
    inline Vector3           g_targetDir = Vector3::Zero();
    inline uint64_t          g_targetEnemy = 0;
    inline uint64_t          g_targetHeadCollider = 0;
    inline std::atomic<bool> g_hasData{false};

} // namespace aimbot

