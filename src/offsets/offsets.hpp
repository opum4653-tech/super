#pragma once
// ============================================================================
// CENTRALIZED GAME MEMORY OFFSETS & CLASS LAYOUT DICTIONARY
// ============================================================================
// Project: crack_master
// Target Game: Free Fire Max (ARM64-v8a / 64-bit pointers)
// Source Reference: maxff.h & STORM_MODS (FFMax / FFMaxInd)
//
// ARCHITECTURAL MANDATE:
// This file is the single source of truth for all memory offsets in Free Fire Max.
// Target Package: com.dts.freefiremax
//
// MEMORY LAYOUTS & CHAIN TOPOLOGY (FREE FIRE MAX):
// 1. Base Chain:
//    libil2cpp.so + Base::GameFacade (0xAD68E18) -> StaticFields (0xB8) -> BaseGame (0x8)
//    BaseGame + BaseGame_Match (0x90) -> MatchInstance
// 2. Player Dictionary:
//    BaseGame + Match_PlayerDict (0xC0) -> MonoDictionary<Byte, Player>
// 3. Camera Projection Matrix:
//    Player + FollowCamera (0x698) -> Camera (0x20 / 0x30) -> IntPtr (0x10) -> Matrix (0x100)
// 4. Hit Registration Raycast & Aiming:
//    Player + HeadCollider (0x740) -> AttackableEntity + LockedAimingCollider (0x80)
//    Player + AimInfo (0xE58) -> StartPos (0x4C) / RayDir (0x40)
//    Player + AimRotation (0x61C)
// ============================================================================

#include <cstdint>

namespace Offsets {

// Base Chain (GameFacade → BaseGame → Match)
    namespace Base {
        constexpr uintptr_t GameFacade           = 0xAD68E18;   // GameFacade TypeInfo / BSS slot (maxff.h)
        constexpr uintptr_t GameFacade_Alt       = 0xAD6DC18;   // Secondary GameFacade candidate (STORM_MODS)
        constexpr uintptr_t GameFacade_P2        = 0xB8;        // StaticClass / StaticGameFacade
        constexpr uintptr_t BaseGame_Match       = 0x90;        // CurrentMatch / m_Match
        constexpr uintptr_t BaseGame_Timer       = 0x20;
        constexpr uintptr_t Timer_Delta          = 0x2C;
        constexpr uintptr_t MatchIsRunning       = 0xCC;        // MatchIsRunning flag
        constexpr uintptr_t Match_PlayerDict     = 0xC0;        // Dictionary of Entities / Players
        constexpr uintptr_t Match_LocalPlayer    = 0xD8;        // LocalPlayer on Match
        constexpr uintptr_t Match_VehicleDict    = 0x188;
        constexpr uintptr_t Match_MetaManager    = 0xB8;
    }

// Il2Cpp Mono Dictionary Traversal
    namespace Dict {
        constexpr uintptr_t Entries              = 0x18;
        constexpr uintptr_t Count                = 0x20;
        constexpr uintptr_t ArrayHeader          = 0x20;
        constexpr uintptr_t EntryStride          = 0x18;
        constexpr uintptr_t EntryValue           = 0x10;
    }

// Player Entity Fields (Free Fire Max ARM64)
    namespace Player {
        constexpr uintptr_t Rotation             = 0x61C;   // Quaternion AimRotation (maxff.h / STORM_MODS)
        constexpr uintptr_t HeadTF               = 0x6A8;   // Head ITransformNode (maxff.h Head)
        constexpr uintptr_t FootTF               = 0x6F0;   // LeftToe ITransformNode (maxff.h lToe)
        constexpr uintptr_t Camera               = 0x698;   // FollowCamera on Player (maxff.h / STORM_MODS)
        constexpr uintptr_t IsFiring             = 0x850;   // isFiring Boolean (maxff.h / STORM_MODS)
        constexpr uintptr_t IsFiring_Alt         = 0x850;   // Secondary
        constexpr uintptr_t HP                   = 0x70;    // GetCurHP / PlayerData pointer (maxff.h)
        constexpr uintptr_t Armor                = 0x74;    // Int32
        constexpr uintptr_t IsDead               = 0x7C;    // Player_IsDead Boolean (maxff.h)
        constexpr uintptr_t Avatar               = 0x778;   // AvatarManager pointer (maxff.h / STORM_MODS)
        constexpr uintptr_t HeadCollider         = 0x740;   // HedColider / ColliderHead (maxff.h / STORM_MODS)
        constexpr uintptr_t HeadCollider_Alt     = 0x740;
        constexpr uintptr_t LockedAimingCollider = 0x80;    // Colider_Field / ColliderBody on AttackableEntity (maxff.h / STORM_MODS)
        constexpr uintptr_t IsBot                = 0x4A8;   // IsClientBot Boolean (maxff.h / STORM_MODS)
        constexpr uintptr_t Name                 = 0x498;   // NamePtr MonoString* (maxff.h / STORM_MODS)
        constexpr uintptr_t Speed                = 0x560;   // Single (Default 1.0f)
        constexpr uintptr_t AimInfo              = 0xE58;   // LastAimingInfoFromWeapon (maxff.h / STORM_MODS)
        constexpr uintptr_t AimInfo_Alt          = 0xE58;
        constexpr uintptr_t AimTarget            = 0xE70;   // AimTarget on Player (STORM_MODS)
        constexpr uintptr_t Velocity             = 0xF0;    // Vector3
        constexpr uintptr_t IsGrounded           = 0x11C;   // Boolean
        constexpr uintptr_t FOVOffset            = 0x84;    // Single
        constexpr uintptr_t Weapon               = 0x608;   // Weapon / ActiveUISightingWeapon (maxff.h / STORM_MODS)
        constexpr uintptr_t DeathInfo            = 0x24A8;  // DeathInfo
        constexpr uintptr_t Attributes           = 0x770;   // PlayerAttributes (maxff.h / STORM_MODS)
        constexpr uintptr_t Attributes_Max       = 0x770;
        constexpr uintptr_t PhantomState         = 0x814;   // GhostHack flag (maxff.h / STORM_MODS)
        constexpr uintptr_t FlightFlag           = 0x2098;  // Elevation flight state flag on Player
        constexpr uintptr_t PhyXData             = 0x1D60;  // Physics / Collision & Downed state (maxff.h)
        constexpr uintptr_t PhyX_MaybeDead       = 0x20;    // maybeDead pointer (maxff.h)
        constexpr uintptr_t PhyX_IsKnocked       = 0x10;    // isKnocked == 8 when downed (maxff.h)
    }

// Transform Hierarchy Traversal (Unity Matrix Resolution)
    namespace Transform {
        constexpr uintptr_t Node                 = 0x10;    // Transform -> Component / Node (maxff.h transformValue)
        constexpr uintptr_t Obj                  = 0x10;    // Node -> TransformObj (maxff.h transformObjValue)
        constexpr uintptr_t MatrixPtr            = 0x38;    // TransformObj -> MatrixPtr (maxff.h matrixValue)
        constexpr uintptr_t Position             = 0x90;    // MatrixPtr -> Vector3 Position (STORM_MODS)
        constexpr uintptr_t Rotation             = 0xA0;    // MatrixPtr -> Rotation (STORM_MODS)
        constexpr uintptr_t Scale                = 0xAC;    // MatrixPtr -> Scale (STORM_MODS)
    }

// Weapon Data Fields (Free Fire Max)
    namespace Weapon {
        constexpr uintptr_t Data                 = 0x98;    // Weapon_Data (maxff.h)
        constexpr uintptr_t Component            = 0x80;    // WeaponComponent (maxff.h)
        constexpr uintptr_t Item                 = 0xA0;    // Item (maxff.h)
        constexpr uintptr_t ItemData             = 0x28;    // ItemData (maxff.h)
        constexpr uintptr_t WeaponId             = 0x40;    // ItemData_WeaponId (maxff.h)
        constexpr uintptr_t NoRecoil             = 0x18;    // NoRecoil (maxff.h)
        constexpr uintptr_t AmmoLeft             = 0x610;   // AmmoLeft (STORM_MODS)
        constexpr uintptr_t IsSight              = 0x8A0;   // isSight (STORM_MODS)
    }

// Avatar Visibility (Free Fire Max)
    namespace Avatar {
        constexpr uintptr_t Uma                  = 0x138;   // UmaAvatarSimple (maxff.h)
        constexpr uintptr_t Uma_Visible          = 0x101;   // IsVisible (maxff.h)
        constexpr uintptr_t Uma_Data             = 0x28;    // UmaData (maxff.h)
        constexpr uintptr_t UmaData_LocalPlayer  = 0x80;    // Boolean: UMAData.isLocalPlayer
        constexpr uintptr_t UmaData_Teammate     = 0x81;    // Boolean: UmaData_IsTeam (maxff.h)
        constexpr uintptr_t UmaData_Skip         = 0x81;    // Alias for teammate filter
    }

// Camera Chain (Free Fire Max)
    namespace Camera {
        constexpr uintptr_t Follow               = 0x20;    // Camera on FollowCamera (maxff.h: Camera = 0x20)
        constexpr uintptr_t Follow_Alt           = 0x30;    // Camera on FollowCamera (STORM_MODS: Camera = 0x30)
        constexpr uintptr_t IntPtr               = 0x10;    // IntPtrCam on Camera (maxff.h / STORM_MODS: 0x10)
        constexpr uintptr_t Matrix               = 0x100;   // ViewMatrix on IntPtrCam (maxff.h / STORM_MODS: 0x100)
    }

// Aim Information (Free Fire Max)
    namespace Aim {
        constexpr uintptr_t Direction            = 0x40;    // Vector3 raycast / RayDir (maxff.h / STORM_MODS)
        constexpr uintptr_t StartPosition        = 0x4C;    // Vector3 startPos / StartPos (maxff.h / STORM_MODS)
        constexpr uintptr_t Target               = 0xE70;   // AimTarget (STORM_MODS)
    }

// Skeleton Bones (ITransformNode offsets inside Player for Free Fire Max)
// Shifted by +0x8 relative to legacy Free Fire
    namespace Bone {
        constexpr uintptr_t Head                 = 0x6A8;   // Head (maxff.h bone::Head)
        constexpr uintptr_t Hip                  = 0x6B0;   // Hip (maxff.h bone::Hip)
        constexpr uintptr_t Breast               = 0x6B8;   // Spine / Breast (maxff.h bone::Spine)
        constexpr uintptr_t Root                 = 0x6D0;   // Root (maxff.h bone::Root)
        constexpr uintptr_t LeftShoulder         = 0x710;   // lShoulder (maxff.h bone::lShoulder)
        constexpr uintptr_t RightShoulder        = 0x718;   // rShoulder (maxff.h bone::rShoulder)
        constexpr uintptr_t LeftAnkle            = 0x6E0;   // lAnkle (maxff.h bone::lAnkle)
        constexpr uintptr_t RightAnkle           = 0x6E8;   // rAnkle (maxff.h bone::rAnkle)
        constexpr uintptr_t LeftToe              = 0x6F0;   // lToe (maxff.h bone::lToe)
        constexpr uintptr_t RightToe             = 0x6F8;   // rToe (maxff.h bone::rToe)
        constexpr uintptr_t RightForeArm         = 0x730;   // rElbow / ForeArm (maxff.h bone::rElbow)
        constexpr uintptr_t LeftForeArm          = 0x738;   // lElbow / ForeArm (maxff.h bone::lElbow)
        constexpr uintptr_t RightHand            = 0x720;   // rHand (maxff.h bone::rHand)
        constexpr uintptr_t LeftHand             = 0x728;   // lHand (maxff.h bone::lHand)
    }

// Loot & Drop Items
    namespace Loot {
        constexpr uintptr_t DropObjects          = 0x18;
        constexpr uintptr_t MetaObjects          = 0x10;
        constexpr uintptr_t SpeedObjects         = 0x20;
        constexpr uintptr_t DropObject_Guid      = 0x20;
        constexpr uintptr_t DropObject_Model     = 0x28;
        constexpr uintptr_t DropObject_Shadow    = 0x30;
        constexpr uintptr_t DropObject_ResID     = 0x64;
        constexpr uintptr_t DropObject_Position  = 0x68;
        constexpr uintptr_t DropObject_Rotation  = 0x74;
        constexpr uintptr_t DropObject_Scale     = 0x84;
        constexpr uintptr_t DropObject_Landed    = 0x9C;
    }

// Backwards-Compatible Flat Aliases
    constexpr uintptr_t GameFacade               = Base::GameFacade;
    constexpr uintptr_t GameFacade_P2            = Base::GameFacade_P2;
    constexpr uintptr_t BaseGame_Match           = Base::BaseGame_Match;
    constexpr uintptr_t BaseGame_Timer           = Base::BaseGame_Timer;
    constexpr uintptr_t Timer_Delta              = Base::Timer_Delta;
    constexpr uintptr_t MatchIsRunning           = Base::MatchIsRunning;
    constexpr uintptr_t Match_PlayerDict         = Base::Match_PlayerDict;
    constexpr uintptr_t Match_LocalPlayer        = Base::Match_LocalPlayer;
    constexpr uintptr_t Match_MetaManager        = Base::Match_MetaManager;

    constexpr uintptr_t Dict_Entries             = Dict::Entries;
    constexpr uintptr_t Dict_Count               = Dict::Count;
    constexpr uintptr_t Dict_ArrayHeader         = Dict::ArrayHeader;
    constexpr uintptr_t Dict_EntryStride         = Dict::EntryStride;
    constexpr uintptr_t Dict_EntryValue          = Dict::EntryValue;

    constexpr uintptr_t Player_Rotation          = Player::Rotation;
    constexpr uintptr_t Player_HeadTF            = Player::HeadTF;
    constexpr uintptr_t Player_FootTF            = Player::FootTF;
    constexpr uintptr_t Player_Camera            = Player::Camera;
    constexpr uintptr_t Player_IsFiring          = Player::IsFiring;
    constexpr uintptr_t Player_HP                = Player::HP;
    constexpr uintptr_t Player_Armor             = Player::Armor;
    constexpr uintptr_t Player_IsDead            = Player::IsDead;
    constexpr uintptr_t Player_Avatar            = Player::Avatar;
    constexpr uintptr_t Player_HeadCollider         = Player::HeadCollider;
    constexpr uintptr_t Player_HeadCollider_Alt     = Player::HeadCollider_Alt;
    constexpr uintptr_t Player_LockedAimingCollider = Player::LockedAimingCollider;
    constexpr uintptr_t Player_IsBot             = Player::IsBot;
    constexpr uintptr_t Player_Name              = Player::Name;
    constexpr uintptr_t Player_Speed             = Player::Speed;
    constexpr uintptr_t Player_AimInfo           = Player::AimInfo;
    constexpr uintptr_t Player_AimInfo_Alt       = Player::AimInfo_Alt;
    constexpr uintptr_t Player_AimTarget         = Player::AimTarget;
    constexpr uintptr_t Player_Velocity          = Player::Velocity;
    constexpr uintptr_t Player_IsGrounded        = Player::IsGrounded;
    constexpr uintptr_t Player_FOVOffset         = Player::FOVOffset;
    constexpr uintptr_t Player_Weapon            = Player::Weapon;
    constexpr uintptr_t Player_DeathInfo         = Player::DeathInfo;
    constexpr uintptr_t Player_Attributes        = Player::Attributes;
    constexpr uintptr_t Player_Attributes_Max    = Player::Attributes_Max;
    constexpr uintptr_t Player_PhantomState      = Player::PhantomState;
    constexpr uintptr_t Player_FlightFlag        = Player::FlightFlag;

    constexpr uintptr_t Transform_Node           = Transform::Node;
    constexpr uintptr_t Transform_Obj            = Transform::Obj;
    constexpr uintptr_t Transform_MatrixPtr      = Transform::MatrixPtr;
    constexpr uintptr_t Transform_Position       = Transform::Position;

    constexpr uintptr_t Avatar_Uma               = Avatar::Uma;
    constexpr uintptr_t Uma_Visible              = Avatar::Uma_Visible;
    constexpr uintptr_t Uma_Data                 = Avatar::Uma_Data;
    constexpr uintptr_t UmaData_Skip             = Avatar::UmaData_Skip;

    constexpr uintptr_t Camera_Follow            = Camera::Follow;
    constexpr uintptr_t Camera_Follow_Alt        = Camera::Follow_Alt;
    constexpr uintptr_t Camera_IntPtr            = Camera::IntPtr;
    constexpr uintptr_t Camera_Matrix            = Camera::Matrix;

    constexpr uintptr_t HP_List                  = 0x10;
    constexpr uintptr_t HP_Entry                 = 0x20;
    constexpr uintptr_t HP_Value                 = 0x18;

    constexpr uintptr_t Weapon_Data              = Weapon::Data;

    constexpr uintptr_t AimInfo_Direction        = Aim::Direction;
    constexpr uintptr_t AimInfo_StartPosition    = Aim::StartPosition;

    constexpr uintptr_t Bone_Head                = Bone::Head;
    constexpr uintptr_t Bone_Hip                 = Bone::Hip;
    constexpr uintptr_t Bone_Breast              = Bone::Breast;
    constexpr uintptr_t Bone_Root                = Bone::Root;
    constexpr uintptr_t Bone_LeftShoulder        = Bone::LeftShoulder;
    constexpr uintptr_t Bone_RightShoulder       = Bone::RightShoulder;
    constexpr uintptr_t Bone_LeftAnkle           = Bone::LeftAnkle;
    constexpr uintptr_t Bone_RightAnkle          = Bone::RightAnkle;
    constexpr uintptr_t Bone_LeftToe             = Bone::LeftToe;
    constexpr uintptr_t Bone_RightToe            = Bone::RightToe;
    constexpr uintptr_t Bone_RightForeArm        = Bone::RightForeArm;
    constexpr uintptr_t Bone_LeftForeArm         = Bone::LeftForeArm;
    constexpr uintptr_t Bone_RightHand           = Bone::RightHand;
    constexpr uintptr_t Bone_LeftHand            = Bone::LeftHand;

    constexpr uintptr_t MetaManager_DropObjects  = Loot::DropObjects;
    constexpr uintptr_t MetaManager_MetaObjects  = Loot::MetaObjects;
    constexpr uintptr_t MetaManager_SpeedObjects = Loot::SpeedObjects;
    constexpr uintptr_t DropObject_Guid          = Loot::DropObject_Guid;
    constexpr uintptr_t DropObject_Model         = Loot::DropObject_Model;
    constexpr uintptr_t DropObject_Shadow        = Loot::DropObject_Shadow;
    constexpr uintptr_t DropObject_ResID         = Loot::DropObject_ResID;
    constexpr uintptr_t DropObject_Position      = Loot::DropObject_Position;
    constexpr uintptr_t DropObject_Rotation      = Loot::DropObject_Rotation;
    constexpr uintptr_t DropObject_Scale         = Loot::DropObject_Scale;
    constexpr uintptr_t DropObject_Landed        = Loot::DropObject_Landed;

    constexpr uintptr_t Head                     = Bone_Head;
    constexpr uintptr_t Hip                      = Bone_Hip;
    constexpr uintptr_t Root                     = Bone_Root;

} // namespace Offsets
