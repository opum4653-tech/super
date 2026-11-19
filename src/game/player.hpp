#pragma once
// ============================================================================
// ENTITY DATA STRUCTURES & MEMORY SNAPSHOT DEFINITIONS
// ============================================================================
// Project: crack_master
//
// ARCHITECTURAL PURPOSE:
// Holds clean, decoupled, frame-synchronous snapshots of in-game players and loot.
// Populated external to game memory by src/game/data.cpp.
// Read concurrently by ESP rendering (src/func/esp/) and aim assistance (src/func/aim/).
//
// ZERO-LOCK CONTENTION ARCHITECTURE:
// Read threads lock the mutex only for a brief vector copy (swap), preventing
// rendering stalls or lock starvation during high-frequency memory reads.
// ============================================================================

#include "../other/vector3.h"
#include <cstdint>

// ----------------------------------------------------------------------------
// PLAYER ENTITY SNAPSHOT
// ----------------------------------------------------------------------------
// Maps directly to Il2Cpp class: Player (Assembly-CSharp.dll)
struct PlayerData {
    uint64_t addr;          // Heap pointer to in-game Player object

    // 2D Screen Space Coordinates (Projected from 3D via WorldToScreenPoint)
    Vector3  Head;          // Projected Head position (x, y = screen pixels, z = W2S depth factor)
    Vector3  Toe;           // Projected LeftToe position (used for bottom-of-box calculation)
    Vector3  headWorld;     // Unprojected 3D Head World Space coordinates (used for 3D Silent Aim trajectory)
    Vector3  hipWorld;      // Unprojected 3D Hip World Space coordinates (used for Chest / Body targeting)
    Vector3  spineWorld;    // Unprojected 3D Spine World Space coordinates (bone::Spine)
    Vector3  neckWorld;     // Unprojected 3D Neck World Space coordinates (interpolated head->spine)

    // Entity State & Gameplay Meta
    bool     isVisible;     // True if player mesh is visible to local camera (Avatar::Uma_Visible)
    bool     isBot;         // True if player is AI bot (Player::IsBot, 0x4A0)
    bool     isKnocked;     // True if player is knocked down (PhyXData + 0x20 + 0x10 == 8)
    float    distance;      // Distance in meters from local player to this enemy
    int      curHP;         // Live Health Points (PRIDataPool -> Datas[0] -> Value)
    int      maxHP;         // Maximum Health Points (PRIDataPool -> Datas[1] -> Value)
    int      curArmor;      // Armor Points (PRIDataPool -> Datas[4] -> Value)
    bool     isFiring;      // True if enemy is currently shooting (Player::IsFiring, 0x850/0x848)
    bool     isGrounded;    // True if entity is touching the terrain (Player::IsGrounded, 0x11C)
    float    speed;         // Current velocity scalar in m/s (sqrt(vx^2 + vy^2 + vz^2))
    char     name[36];      // UTF-8 sanitized player display name (Player::Name, 0x490)
    int      weaponType;    // Active weapon category ID (WeaponOnHand -> WeaponData)

    PlayerData()
        : addr(0),
          Head(Vector3::Zero()), Toe(Vector3::Zero()),
          headWorld(Vector3::Zero()), hipWorld(Vector3::Zero()),
          spineWorld(Vector3::Zero()), neckWorld(Vector3::Zero()),
          isVisible(false), isBot(false), isKnocked(false), distance(0.f),
          curHP(0), maxHP(0), curArmor(0),
          isFiring(false), isGrounded(true), speed(0.f),
          weaponType(-1)
    { name[0] = 0; }

    Vector3 getTargetWorld(int targetPos, float headBias = 0.08f) const {
        switch (targetPos) {
            case 1:  return (neckWorld != Vector3::Zero()) ? neckWorld : (headWorld - Vector3(0.0f, 0.20f, 0.0f));
            case 2:  return (hipWorld != Vector3::Zero()) ? hipWorld : (spineWorld != Vector3::Zero() ? spineWorld : (headWorld - Vector3(0.0f, 0.50f, 0.0f)));
            default: return Vector3(headWorld.x, headWorld.y + headBias, headWorld.z);
        }
    }
};

// ----------------------------------------------------------------------------
// LOOT / WORLD ITEM SNAPSHOT
// ----------------------------------------------------------------------------
// Maps to Il2Cpp DropItem / MetaObject entries in MetaManager (BaseGame_Match + 0xB8)
struct LootItem {
    uint64_t addr;          // Heap pointer to in-game loot item entity
    uint32_t guid;          // Unique entity identifier
    uint32_t resID;         // Resource category ID (determines weapon, ammo, armor, medkit)
    Vector3  worldPos;      // 3D World position of the dropped item
    Vector3  screenPos;     // 2D Screen position projected via WorldToScreenPoint
    float    distance;      // Distance in meters from local player

    LootItem()
        : addr(0), guid(0), resID(0),
          worldPos(Vector3::Zero()), screenPos(Vector3::Zero()),
          distance(0.f) {}
};
