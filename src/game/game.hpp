#pragma once
// ============================================================================
// UNITY ENGINE MATRIX & TRANSFORM STRUCT DEFINITIONS
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Defines memory layouts that correspond directly to internal Unity Engine C++
// structs used for bone transforms and 3D camera projection.
//
// STRUCT CORRESPONDENCE:
// - matrix: 4x4 row-major matrix extracted from Unity Camera (Offsets::Camera_Matrix)
//   Used by WorldToScreenPoint in src/game/math.hpp.
// - TransformMatrix: Unity Transform Internal Matrix layout.
//   Read in a chain by GetPosition() in src/game/math.hpp to resolve bone 3D world coordinates.
// ============================================================================

#include "../other/vector3.h"

// 4x4 Row-Major Camera View Projection Matrix
struct matrix {
    float m11, m12, m13, m14;
    float m21, m22, m23, m24;
    float m31, m32, m33, m34;
    float m41, m42, m43, m44;
};

// Unity Engine Transform Hierarchy Matrix Entry (0x30 stride in ptrs.matrices array)
struct TransformMatrix {
    Vector4 position;      // Translation vector (x, y, z, w)
    Quaternion rotation;    // Orientation quaternion (x, y, z, w)
    Vector4 scale;         // Local scale vector (x, y, z, w)
};

