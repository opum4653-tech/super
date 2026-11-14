#pragma once
#include "common.hpp"

namespace aimbot::motion {

    inline float s_flightGroundY = 0.0f;
    inline bool  s_flightActive = false;

    inline bool  s_phantomActive = false;

    inline uintptr_t get_root_pos_addr(uint64_t player) {
        if (!is_valid_ptr(player)) return 0;
        uintptr_t hipTF = rpm<uintptr_t>(player + Offsets::Bone::Hip);
        if (!is_valid_ptr(hipTF)) {
            hipTF = rpm<uintptr_t>(player + 0x6B0);
        }
        if (!is_valid_ptr(hipTF)) return 0;

        uintptr_t v1 = rpm<uintptr_t>(hipTF + Offsets::Transform::Node);
        if (!is_valid_ptr(v1)) return 0;

        uintptr_t v2 = rpm<uintptr_t>(v1 + Offsets::Transform::Obj);
        if (!is_valid_ptr(v2)) return 0;

        uintptr_t matrixPtr = rpm<uintptr_t>(v2 + Offsets::Transform::MatrixPtr);
        if (!is_valid_ptr(matrixPtr)) return 0;

        return matrixPtr + Offsets::Transform::Position;
    }

    inline void reset(uint64_t player = 0) {
        if (!player && data::localPlayerAddr.load(std::memory_order_relaxed)) {
            player = data::localPlayerAddr.load(std::memory_order_relaxed);
        }
        if (s_flightActive) {
            s_flightActive = false;
            s_flightGroundY = 0.0f;
            if (is_valid_ptr(player)) {
                wpm<bool>(player + Offsets::Player::FlightFlag, false);
            }
        }
        if (s_phantomActive) {
            s_phantomActive = false;
            if (is_valid_ptr(player)) {
                wpm<bool>(player + Offsets::Player::PhantomState, false);
            }
        }
    }

    inline void update() {
        if (!cfg::master_enabled) {
            reset();
            return;
        }

        if (!data::inMatch.load(std::memory_order_relaxed)) {
            reset(0);
            return;
        }

        uint64_t localPlayer = data::localPlayerAddr.load(std::memory_order_relaxed);
        if (!is_valid_ptr(localPlayer) || rpm<bool>(localPlayer + Offsets::Player::IsDead)) {
            reset(localPlayer);
            return;
        }

        // 1. AIR FLIGHT
        if (cfg::motion::air_flight) {
            uintptr_t rootPosAddr = get_root_pos_addr(localPlayer);
            if (rootPosAddr != 0) {
                Vector3 currentPos = rpm<Vector3>(rootPosAddr);
                if (std::isfinite(currentPos.x) && std::isfinite(currentPos.y) && std::isfinite(currentPos.z)) {
                    if (!s_flightActive) {
                        s_flightGroundY = currentPos.y;
                        s_flightActive = true;
                    }
                    currentPos.y = s_flightGroundY + cfg::motion::flight_height;
                    wpm<Vector3>(rootPosAddr, currentPos);
                    wpm<bool>(localPlayer + Offsets::Player::FlightFlag, true);
                }
            }
        } else if (s_flightActive) {
            s_flightActive = false;
            s_flightGroundY = 0.0f;
            wpm<bool>(localPlayer + Offsets::Player::FlightFlag, false);
        }

        // 2. PHANTOM MODE
        if (cfg::motion::phantom_mode) {
            wpm<bool>(localPlayer + Offsets::Player::PhantomState, true);
            s_phantomActive = true;
        } else if (s_phantomActive) {
            wpm<bool>(localPlayer + Offsets::Player::PhantomState, false);
            s_phantomActive = false;
        }
    }

} // namespace aimbot::motion
