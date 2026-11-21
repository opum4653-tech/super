#pragma once
// ============================================================================
// HIT REGISTRATION & HEADSHOT PROBABILITY EVALUATOR (100% STEALTH PROTOCOL)
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Controls server-acknowledged hit registration and headshot rates:
// 1. Headshot Percentage Roll:
//    Evaluates user-configured headshot_rate (0% to 100%) via high-speed,
//    non-blocking Xorshift RNG.
// 2. Burst-Locked Headshot Decision:
//    The RNG roll is locked per target-acquisition burst, NOT per frame.
//    This prevents the "drag" effect where each bullet in a burst gets an
//    independent roll, causing half the mag to hit head and half to hit neck.
//    When a target is acquired, one roll decides headshot for the entire
//    burst until the target dies, leaves FOV, or the firing stops for >200ms.
// 3. Anti-Ban Zero-Mutation Stealth:
//    NEVER writes to enemy memory (0x80 LockedAimingCollider is 100% read-only).
//    Mutating another player's entity in client RAM is detected by Free Fire's
//    integrity watchdog, causing account bans.
//    Instead, clean trajectory steering directs bullets into the head collider
//    via physical raycast intersection without touching enemy heap objects.
// ============================================================================

#include "common.hpp"

namespace aimbot {

    // Burst-locked headshot state: one roll per firing burst, not per frame.
    // This eliminates the "drag headshot" where individual bullets in a burst
    // independently roll head vs body, producing inconsistent hit registration.
    inline bool    s_headshotLock     = false;   // Current burst's headshot decision
    inline uint64_t s_lockTargetForRng = 0;      // Target address this roll belongs to
    inline int64_t  s_lastFiringTimeMs = 0;      // Last time isFiring was true (ms)
    inline bool    s_lastFiringState   = false;  // Previous frame's firing state
    inline int64_t  s_burstStartMs     = 0;      // When the current burst's roll was drawn

    // Hard cap on how long a single roll may govern a target.
    // Without this, "Always In FOV" (trigger_mode == 1) keeps the trigger active
    // on every tick, so the >200 ms gap never elapses and the first roll for an
    // enemy is reused forever: that enemy is then either always-headshot or
    // always-body-shot for the whole match. The user-visible symptom is a
    // headshot rate that reads far below the configured slider value.
    static constexpr int64_t kBurstMaxMs = 1200;

    // Returns current monotonic time in milliseconds (steady clock, no wall-clock jumps)
    // Plain inline (not static): the aim headers are compiled into more than one
    // TU, and an inline external function must not capture TU-local linkage here.
    inline int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Evaluates headshot rate roll (0% - 100%) with burst-locked decision
    inline bool evaluate_headshot(const PlayerData& target, bool isScoping, bool isFiring) {
        float rate = cfg::aim::headshot_rate;
        bool isSniperShot = (cfg::aim::sniper_aim && isScoping);

        // Sniper scoped or 100% rate: always headshot, no roll needed
        if (isSniperShot || rate >= 100.0f) {
            s_headshotLock = true;
            s_lockTargetForRng = target.addr;
            s_lastFiringTimeMs = now_ms();
            s_burstStartMs = s_lastFiringTimeMs;
            s_lastFiringState = isFiring;
            return true;
        }

        // 0% rate: never headshot
        if (rate <= 0.0f) {
            s_headshotLock = false;
            s_lockTargetForRng = target.addr;
            s_lastFiringTimeMs = now_ms();
            s_burstStartMs = s_lastFiringTimeMs;
            s_lastFiringState = isFiring;
            return false;
        }

        // Burst lock: if we're still firing at the same target and the burst
        // hasn't timed out (200ms gap = new burst), reuse the previous decision.
        int64_t curMs = now_ms();

        // Burst break: a >200ms gap since the last trigger, a firing->idle
        // transition, a different target, or the kBurstMaxMs ceiling expiring.
        // evaluate_headshot is only reached while the trigger is active
        // (including "Always In FOV" mode), so the timestamp gap is the reliable
        // signal that the player released fire. The hard ceiling is required
        // because in "Always In FOV" mode the gap never elapses on its own.
        bool burstBroken = false;
        if (curMs - s_lastFiringTimeMs > 200) burstBroken = true;
        if (s_lastFiringState && !isFiring) burstBroken = true;
        if (s_lockTargetForRng != target.addr) burstBroken = true;
        if (curMs - s_burstStartMs > kBurstMaxMs) burstBroken = true;

        s_lastFiringTimeMs = curMs;
        s_lastFiringState = isFiring;

        // Burst still active and same target: reuse the locked decision.
        // No isFiring condition here — "Always In FOV" trigger mode must also
        // keep the roll stable across frames or the drag effect returns.
        if (!burstBroken && s_lockTargetForRng == target.addr) {
            return s_headshotLock;
        }

        // New burst or new target: roll the RNG once for this entire burst
        s_lockTargetForRng = target.addr;
        s_burstStartMs = curMs;

        // Independent xorshift RNG roll against user's headshot_rate
        static uint32_t s_rollRng =
            0x9E3779B9u ^ static_cast<uint32_t>(time(nullptr)) ^
            (static_cast<uint32_t>(getpid()) * 2654435761u);
        s_rollRng ^= s_rollRng << 13;
        s_rollRng ^= s_rollRng >> 17;
        s_rollRng ^= s_rollRng << 5;

        s_headshotLock = ((s_rollRng % 100u) < static_cast<uint32_t>(rate));
        return s_headshotLock;
    }

} // namespace aimbot
