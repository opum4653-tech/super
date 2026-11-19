#pragma once
// ============================================================================
// MATHEMATICAL ENGINE & UNITY TRANSFORM PROJECTION ENGINE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// 1. Unity Transform Hierarchy Resolution (GetPosition):
//    Traverses the internal bone hierarchy tree of Unity Engine to calculate
//    the absolute 3D world space coordinate of player bones (Head, LeftToe, etc.).
// 2. 3D-to-2D Perspective Projection (WorldToScreenPoint):
//    Applies the camera view projection matrix and perspective division (v12 / w)
//    to map 3D world positions onto 2D display pixels for ESP overlay rendering.
// 3. Euclidean Distance Calculation:
//    Calculates physical distance in meters between any two 3D vectors.
// ============================================================================

#include "game.hpp"
#include "Offsets.h"
#include "../other/vector3.h"
#include "../other/memory.hpp"
#include <cmath>

extern float g_sw, g_sh;

// ----------------------------------------------------------------------------
// RECURSIVE UNITY TRANSFORM RECONSTRUCTION
// ----------------------------------------------------------------------------
// Traverses Unity's internal bone hierarchy:
// Transform -> TransformObj (+0x10) -> MatrixPtr (+0x38) -> Matrices (+0x18), Indices (+0x20)
// Recursively applies parent translation, quaternion rotation, and scaling up to 50 levels.
inline Vector3 GetPosition(uint64_t Transform) {
    auto transformObjValue = rpm<uint64_t>(Transform + 0x10);
    if (transformObjValue == 0) return Vector3::Zero();

    struct { uint64_t matrices; uint64_t indices; } ptrs;
    auto matrixValue = rpm<uint64_t>(transformObjValue + 0x38);
    if (!matrixValue) return Vector3::Zero();

    ptrs.matrices = rpm<uint64_t>(matrixValue + 0x18);
    ptrs.indices  = rpm<uint64_t>(matrixValue + 0x20);
    if (!ptrs.matrices || !ptrs.indices) return Vector3::Zero();

    auto indexValue = rpm<uint64_t>(transformObjValue + 0x40);

    auto resultValue = rpm<Vector3>(ptrs.matrices + (indexValue * 0x30));
    auto transformIndexValue = rpm<int>(ptrs.indices + (indexValue * 4));

    // Recursively walk parent hierarchy
    for (int tries = 0; transformIndexValue >= 0 && tries < 50; tries++) {
        uint64_t matAddr = ptrs.matrices + (transformIndexValue * 0x30);
        auto t = rpm<TransformMatrix>(matAddr);

        auto rx = t.rotation.x, ry = t.rotation.y, rz = t.rotation.z, rw = t.rotation.w;
        auto sx = resultValue.x * t.scale.x;
        auto sy = resultValue.y * t.scale.y;
        auto sz = resultValue.z * t.scale.z;

        // Quaternion rotation math: v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        resultValue.x = t.position.x + sx +
                        (sx * ((ry * ry * -2.0) - (rz * rz * 2.0))) +
                        (sy * ((rw * rz * -2.0) - (ry * rx * -2.0))) +
                        (sz * ((rz * rx *  2.0) - (rw * ry * -2.0)));
        resultValue.y = t.position.y + sy +
                        (sx * ((rx * ry *  2.0) - (rw * rz * -2.0))) +
                        (sy * ((rz * rz * -2.0) - (rx * rx *  2.0))) +
                        (sz * ((rw * rx * -2.0) - (rz * ry * -2.0)));
        resultValue.z = t.position.z + sz +
                        (sx * ((rw * ry * -2.0) - (rx * rz * -2.0))) +
                        (sy * ((ry * rz *  2.0) - (rw * rx * -2.0))) +
                        (sz * ((rx * rx * -2.0) - (ry * ry *  2.0)));

        transformIndexValue = rpm<int>(ptrs.indices + (transformIndexValue * 4));
    }

    return resultValue;
}

// Resolves bone transform from an ITransformNode pointer
inline Vector3 GetNodePosition(uint64_t nodeTransform) {
    auto transformValue = rpm<uint64_t>(nodeTransform + 0x10);
    if (transformValue == 0) return Vector3::Zero();
    return GetPosition(transformValue);
}

// ----------------------------------------------------------------------------
// BONE NODE ACCESSORS (FREE FIRE MAX)
// ----------------------------------------------------------------------------
inline uint64_t GetPlayerHeadTF(uint64_t player) {
    return rpm<uint64_t>(player + Offsets::Bone_Head);
}

inline uint64_t GetPlayerHipTF(uint64_t player) {
    return rpm<uint64_t>(player + Offsets::Bone_Hip);
}

inline uint64_t GetPlayerPeTF(uint64_t player) {
    return rpm<uint64_t>(player + Offsets::Bone_Root);
}

// Inverts the 4x4 camera view matrix to obtain the 3D camera world origin (from STORM_MODS)
inline Vector3 GetCameraPosition(const matrix& m) {
    float a11 = m.m11, a12 = m.m21, a13 = m.m31;
    float a21 = m.m12, a22 = m.m22, a23 = m.m32;
    float a31 = m.m14, a32 = m.m24, a33 = m.m34;
    float b1  = -m.m41, b2 = -m.m42, b3 = -m.m44;

    float det = a11 * (a22 * a33 - a23 * a32)
                - a12 * (a21 * a33 - a23 * a31)
                + a13 * (a21 * a32 - a22 * a31);

    if (fabsf(det) < 0.0001f) return Vector3::Zero();
    float inv = 1.0f / det;

    return Vector3(
        (b1 * (a22 * a33 - a23 * a32) - a12 * (b2 * a33 - a23 * b3) + a13 * (b2 * a32 - a22 * b3)) * inv,
        (a11 * (b2 * a33 - a23 * b3) - b1 * (a21 * a33 - a23 * a31) + a13 * (a21 * b3 - b2 * a31)) * inv,
        (a11 * (a22 * b3 - b2 * a32) - a12 * (a21 * b3 - b2 * a31) + b1 * (a21 * a32 - a22 * a31)) * inv
    );
}

// ----------------------------------------------------------------------------
// WORLD-TO-SCREEN (W2S) PERSPECTIVE PROJECTION
// ----------------------------------------------------------------------------
// Multiplies pos by the 4x4 camera view matrix, performs perspective division by w,
// and maps normalized device coordinates (NDC) to screen pixel coordinates.
// Returns Vector3(-1, -1, -1) if point is behind the camera near plane (v12 < 0.001f).
inline Vector3 WorldToScreenPoint(const matrix& vm, const Vector3& pos) {
    float v12 = (pos.x * vm.m14) + (pos.y * vm.m24) + (pos.z * vm.m34) + vm.m44;
    if (v12 >= 0.001f) {
        float inv = 1.0f / v12;
        float cx = g_sw * 0.5f;
        float cy = g_sh * 0.5f;
        float v9  = (pos.x * vm.m11) + (pos.y * vm.m21) + (pos.z * vm.m31) + vm.m41;
        float v10 = (pos.x * vm.m12) + (pos.y * vm.m22) + (pos.z * vm.m32) + vm.m42;
        return Vector3(cx + cx * v9 * inv, cy - cy * v10 * inv, v12);
    }
    return Vector3(-1, -1, -1);
}

// ----------------------------------------------------------------------------
// 3D EUCLIDEAN DISTANCE
// ----------------------------------------------------------------------------
inline float calculate_distance(const Vector3& a, const Vector3& b) noexcept {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
