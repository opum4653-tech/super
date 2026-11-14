#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
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
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <unistd.h>

extern float g_sw;
extern float g_sh;

namespace aimbot {

    struct TargetInfo {
        bool valid = false;
        char name[36] = {0};
        float distance = 0.0f;
        int hp = 0;
        Vector3 screenPos = Vector3::Zero();
        Vector3 headWorld = Vector3::Zero();
    };

    inline TargetInfo g_currentTarget;

    // Safe 3D vector normalization with division-by-zero & NaN protection
    inline Vector3 SafeNormalize(const Vector3& v) {
        float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lenSq < 1e-6f || !std::isfinite(lenSq)) {
            return Vector3(0.0f, 0.0f, 1.0f); // Default forward vector
        }
        float invLen = 1.0f / sqrtf(lenSq);
        return Vector3(v.x * invLen, v.y * invLen, v.z * invLen);
    }

    // Target stickiness with hysteresis & grace period to completely eliminate screen shaking
    inline uint64_t s_lockedTargetAddr = 0;
    inline int      s_lockTimeoutTicks = 0;

    // Smooth direction tracking across frame updates
    inline Vector3 s_smoothDir = Vector3::Zero();

} // namespace aimbot
