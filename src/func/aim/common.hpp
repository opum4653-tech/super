#pragma once
// ============================================================================
// AIMBOT COMMON MATHEMATICAL TYPES & LOCK STATE PRIMITIVES
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Shared foundation header for all sub-modules of the aim suite:
// - target.hpp   (Acquisition & scoring)
// - aim.hpp      (Touch-guided head assist)
// - fov.hpp      (Screen FOV circle & target ring rendering)
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
#include <atomic>
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
    inline Vector3 SafeNormalize(const Vector3& v) {
        float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lenSq < 1e-6f || !std::isfinite(lenSq)) {
            return Vector3(0.0f, 0.0f, 1.0f); // Fallback: forward unit vector
        }
        float invLen = 1.0f / sqrtf(lenSq);
        return Vector3(v.x * invLen, v.y * invLen, v.z * invLen);
    }

} // namespace aimbot

