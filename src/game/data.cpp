// ============================================================================
// GAME ENTITY SCRAPING & MATRIX SYNCHRONIZATION ENGINE
// ============================================================================
// Project: crack_master (Pure External Ring-3 Read Engine)
//
// ARCHITECTURAL OVERVIEW:
// 1. Base Chain Resolution:
//    Traverses GameFacade (0xA72E4E0) -> StaticFields (0xB8) -> BaseGame (0x8) -> Match (0x90).
// 2. Camera ViewMatrix Resolution (pewpew-grade multi-candidate fallbacks):
//    - Candidate 1: LocalPlayer -> FollowCamera (0x690) -> Camera (0x28) -> IntPtr (0x10) -> Matrix (0x100)
//    - Candidate 2: LocalPlayer -> FollowCamera (0x690) -> 0x30 -> 0x10 -> Matrix (0x100)
//    - Candidate 3: BaseGame -> CameraManager (0xD8) -> Camera (0x20/0x28) -> 0x10 -> Matrix (0x100)
//    Validated by is_valid_matrix() before committing to avoid W2S screen tearing.
// 3. Syscall Minimization via PlayerSpan:
//    Reads contiguous span (0x70 to 0x860) in ONE Ring-0 KPM prctl read call instead of 15 individual syscalls.
// 4. Entity Filtering:
//    Filters out dead players, teammates (UMAData.isTeammate), and local user.
// 5. Data Delivery:
//    Atomic double-buffered swap into data::players and data::loots vectors protected by mutexes.
// ============================================================================

#include "data.hpp"
#include "Offsets.h"
#include "../other/utils.hpp"
#include "../other/memory.hpp"
#include "../ui/cfg.hpp"
#include "../protect/oxorany.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>

namespace data {

    // Exported thread-synchronized entity buffers
    std::vector<PlayerData>  players;
    std::mutex               players_mutex;
    std::vector<LootItem>    loots;
    std::mutex               loots_mutex;
    std::atomic<uint64_t>    localPlayerAddr{0};
    std::atomic<bool>        inMatch{false};
    matrix                   viewMatrix{};

    // Internal background thread state
    static std::atomic<bool> g_run{false};
    static std::thread       g_thread;

    // Consecutive camera-resolution failures. A single bad read is tolerated to
    // avoid ESP flicker; sustained failure clears projections instead of aiming
    // with last frame's (stale) camera matrix.
    static int s_matrixFailCount = 0;

    // ------------------------------------------------------------------------
    // CAMERA MATRIX INTEGRITY VALIDATION (Cross-referenced from pewpew)
    // ------------------------------------------------------------------------
    // Tests column norms and checks for NaN / Inf to guarantee that the camera
    // matrix is fully initialized and in an active 3D rendering projection state.
    static inline bool is_valid_matrix(const matrix& m) {
        if (std::isnan(m.m11) || std::isnan(m.m22) || std::isnan(m.m33) || std::isnan(m.m44)) return false;
        if (std::isinf(m.m11) || std::isinf(m.m22) || std::isinf(m.m33) || std::isinf(m.m44)) return false;
        float col1 = fabsf(m.m11) + fabsf(m.m21) + fabsf(m.m31);
        float col2 = fabsf(m.m12) + fabsf(m.m22) + fabsf(m.m32);
        float col4 = fabsf(m.m14) + fabsf(m.m24) + fabsf(m.m34) + fabsf(m.m44);
        return (col1 > 0.05f && col2 > 0.05f && col4 > 0.05f);
    }

    // ------------------------------------------------------------------------
    // SINGLE-SYSCALL PLAYER FIELD SPAN (STEALTH OPTIMIZATION)
    // ------------------------------------------------------------------------
    // Every field read directly off the player object sits between 0x70 and
    // 0x860 (Player::HP .. Player::IsFiring). Pulling that span in a single
    // Ring-0 KPM prctl read replaces roughly a dozen memory reads per player per tick
    // with one, which is the point: the read pattern stops looking like a
    // field-by-field scrape of the same object.
    static constexpr size_t kSpanLo = 0x70;    // Offsets::Player::HP
    static constexpr size_t kSpanHi = 0x860;   // Covers up to Player::IsFiring (0x850)

    struct PlayerSpan {
        uint64_t playerAddr{0};
        uint8_t  buf[kSpanHi - kSpanLo];
        bool     ok{false};

        explicit PlayerSpan(uint64_t player)
            : playerAddr(player), ok(mem_read(player + kSpanLo, buf, sizeof(buf))) {}

        template <typename T>
        T at(size_t off) const {
            T v{};
            if (ok && off >= kSpanLo && (off + sizeof(T) <= kSpanHi)) {
                memcpy(&v, buf + (off - kSpanLo), sizeof(T));
                return v;
            }
            if (playerAddr != 0) {
                return rpm<T>(playerAddr + off);
            }
            return v;
        }
    };

    // ------------------------------------------------------------------------
    // HEALTH & ARMOR EXTRACTION (Il2Cpp PRIDataPool)
    // ------------------------------------------------------------------------
    // pool is m_PRIDataPool (0x70) on player.
    // pool + 0x10 is m_Datas (ReplicationData[] array).
    // Array count is at +0x18. Elements start at +0x20.
    // Index 0 (+0x20): CurHP (ReplicationData*)
    // Index 1 (+0x28): MaxHP (ReplicationData*)
    // Index 4 (+0x40): CurAP (ReplicationData*, Armor Points)
    // Value is stored at +0x18 (Offsets::HP_Value) inside each ReplicationData.
    static void readHealthAndArmor(uint64_t pool, int& curHP, int& maxHP, int& curArmor,
                                   bool wantArmor = true) {
        curHP = 0;
        maxHP = 0;
        curArmor = 0;
        if (!pool) return;
        auto datas = rpm<uint64_t>(pool + Offsets::HP_List);
        if (!datas) return;

        // 1. Live Current Health (Proven Index 0 at Offsets::HP_Entry = 0x20)
        auto elem0 = rpm<uint64_t>(datas + Offsets::HP_Entry);
        if (elem0) {
            curHP = rpm<int>(elem0 + Offsets::HP_Value);
        }

        // 2. Live Maximum Health (Index 1 at 0x28 if registered)
        auto elem1 = rpm<uint64_t>(datas + 0x28);
        if (elem1 && elem1 > 0x10000) {
            int m = rpm<int>(elem1 + Offsets::HP_Value);
            if (m > 0 && m <= 1000) maxHP = m;
        }
        if (maxHP < curHP) maxHP = curHP;
        if (maxHP <= 0) maxHP = (curHP > 0 ? curHP : 200);

        // 3. Live Armor Points (Index 4 at 0x40) — skip entirely when the
        //    armor bar is off; this is 2 syscalls per enemy per tick.
        if (wantArmor) {
            auto elem4 = rpm<uint64_t>(datas + 0x40);
            if (elem4 && elem4 > 0x10000) {
                curArmor = rpm<int>(elem4 + Offsets::HP_Value);
            }
        }
    }

    // ------------------------------------------------------------------------
    // PLAYER NAME EXTRACTION (MonoString UTF-16 to ASCII / UTF-8)
    // ------------------------------------------------------------------------
    static int readName(const PlayerSpan& span, char* buf, size_t buflen) {
        buf[0] = 0;
        auto sp = span.at<uint64_t>(Offsets::Player_Name);
        if (!sp || sp < 0x10000) return 0;
        int len = rpm<int>(sp + 0x10);
        if (len <= 0 || len > 64) return 0;
        uint16_t utf16[65];
        if (len > 64) len = 64;
        if (!mem_read(sp + 0x14, utf16, len * 2)) return 0;
        int n = 0;
        for (int i = 0; i < len && n < (int)buflen - 1; i++) {
            uint16_t c = utf16[i];
            if (c < 0x20 || c > 0x7E) continue;
            buf[n++] = (char)c;
        }
        buf[n] = 0;
        return n;
    }

    // ------------------------------------------------------------------------
    // WEAPON CATEGORY EXTRACTION
    // ------------------------------------------------------------------------
    static int readWeapon(const PlayerSpan& span) {
        auto w = span.at<uint64_t>(Offsets::Player_Weapon);
        if (!w) return -1;
        auto wd = rpm<uint64_t>(w + Offsets::Weapon_Data);
        if (!wd) return -1;
        return rpm<int>(wd + 0x64);
    }

    // ------------------------------------------------------------------------
    // BONE PROJECTION HELPER
    // ------------------------------------------------------------------------
    static Vector3 boneScreen(uint64_t node) {
        if (!node) return Vector3::Zero();
        Vector3 w = GetNodePosition(node);
        if (w == Vector3::Zero()) return Vector3::Zero();
        return WorldToScreenPoint(viewMatrix, w);
    }

    // ------------------------------------------------------------------------
    // GROUND LOOT SCANNER
    // ------------------------------------------------------------------------
    // Traverses MetaManager.DropObjects dictionary to locate weapons, armor, ammo
    static void scanLoot(uint64_t BaseGame, const Vector3& localPos) {
        std::vector<LootItem> tmp;

        uint64_t match = rpm<uint64_t>(BaseGame + Offsets::BaseGame_Match);
        if (!match) return;

        uint64_t metaMgr = rpm<uint64_t>(match + Offsets::Match_MetaManager);
        if (!metaMgr) return;

        uint64_t dictPtr = rpm<uint64_t>(metaMgr + Offsets::MetaManager_DropObjects);
        if (!dictPtr) return;

        MonoDictionary dict(dictPtr);
        if (!dict.valid) return;

        for (int i = 0; i < dict.count; i++) {
            uint64_t dropObj = dict.entry(i);
            if (!dropObj || dropObj < 0x100000) continue;

            bool landed = rpm<bool>(dropObj + Offsets::DropObject_Landed);
            if (!landed) continue;

            Vector3 w = rpm<Vector3>(dropObj + Offsets::DropObject_Position);
            if (w == Vector3::Zero()) continue;

            float dist = calculate_distance(localPos, w);
            if (cfg::esp::loot_range > 0.0f && dist > cfg::esp::loot_range) continue;

            Vector3 sc = WorldToScreenPoint(viewMatrix, w);
            if (sc.x < 0) continue;

            LootItem li;
            li.addr      = dropObj;
            li.guid      = rpm<uint32_t>(dropObj + Offsets::DropObject_Guid);
            li.resID     = rpm<uint32_t>(dropObj + Offsets::DropObject_ResID);
            li.worldPos  = w;
            li.screenPos = sc;
            li.distance  = dist;
            tmp.push_back(li);
        }

        std::lock_guard<std::mutex> lk(loots_mutex);
        loots = std::move(tmp);
    }

    // ------------------------------------------------------------------------
    // MAIN MEMORY SCRAPING CYCLE
    // ------------------------------------------------------------------------
    void update() {
        if (proc::pid <= 0 || proc::lib == 0) return;

        std::vector<PlayerData> tmp;
        tmp.reserve(64);

        // 1. Resolve Base Chain (Free Fire Max: maxff.h primary 0xAD68E18, STORM_MODS alt 0xAD6DC18)
        uint64_t BaseGame = 0;
        uint64_t m_Match = 0;
        uint64_t localPlayer = 0;

        const uintptr_t gfOffsets[] = {
            oxorany(Offsets::GameFacade),
            oxorany(Offsets::Base::GameFacade_Alt)
        };

        for (uintptr_t gfOff : gfOffsets) {
            uint64_t directCell = rpm<uint64_t>(proc::lib + gfOff);
            if (!directCell || directCell < 0x10000) continue;

            uint64_t classCandidates[] = { directCell, rpm<uint64_t>(directCell) };

            for (uint64_t gfClass : classCandidates) {
                if (!gfClass || gfClass < 0x10000) continue;

                uint64_t staticFields = rpm<uint64_t>(gfClass + Offsets::GameFacade_P2);
                if (!staticFields || staticFields < 0x10000) continue;

                uint64_t bgCandidates[] = {
                    rpm<uint64_t>(staticFields),        // +0x0 (STORM_MODS pattern)
                    rpm<uint64_t>(staticFields + 0x8)   // +0x8 (legacy/alt pattern)
                };

                for (uint64_t bg : bgCandidates) {
                    if (!bg || bg < 0x10000) continue;

                    uint64_t match = rpm<uint64_t>(bg + Offsets::BaseGame_Match);
                    if (match && match > 0x10000) {
                        BaseGame = bg;
                        m_Match = match;
                        localPlayer = rpm<uint64_t>(m_Match + Offsets::Match_LocalPlayer);
                        break;
                    }
                }
                if (BaseGame) break;
            }
            if (BaseGame) break;
        }

        if (!BaseGame || !m_Match) return;

        // In matches, localPlayer may transiently read 0 during parachute/respawn.
        // Maintain savedLocalPlayer persistence (matches STORM_MODS line 986).
        static uint64_t savedLocalPlayer = 0;
        if (localPlayer != 0) {
            savedLocalPlayer = localPlayer;
        } else if (savedLocalPlayer != 0 && addr_valid(savedLocalPlayer)) {
            localPlayer = savedLocalPlayer;
        }

        int matchStateMatch = rpm<int>(m_Match + Offsets::MatchIsRunning);
        int matchStateBase  = rpm<int>(BaseGame + Offsets::MatchIsRunning);
        bool matchActive = (localPlayer != 0) ||
                           (matchStateMatch == 1 || (matchStateMatch != 4 && matchStateMatch != 0)) ||
                           (matchStateBase == 1);
        inMatch.store(matchActive);

        // Clear states if match is over or in lobby
        if (!matchActive) {
            savedLocalPlayer = 0;
            localPlayerAddr.store(0);
            {
                std::lock_guard<std::mutex> lk(players_mutex);
                if (!players.empty()) players.clear();
            }
            {
                std::lock_guard<std::mutex> lk(loots_mutex);
                if (!loots.empty()) loots.clear();
            }
            return;
        }

        // Feature-muted mode: user turned off all ESP & Aim switches in menu.
        // Return immediately without reading camera, bones, or enemy lists.
        if (!cfg::any_feature_enabled()) {
            {
                std::lock_guard<std::mutex> lk(players_mutex);
                if (!players.empty()) players.clear();
            }
            return;
        }

        if (!m_Match) {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // Store active local player pointer
        localPlayerAddr.store(localPlayer ? localPlayer : 0);

        if (!localPlayer) {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // 2. Camera Chain — Multi-Candidate Resolution with Matrix Validation (pewpew pattern)
        uint64_t Follow = 0, Cam = 0, IntPtr = 0;
        bool matrix_resolved = false;

        auto try_matrix = [&](uint64_t intPtrCandidate) -> bool {
            if (!intPtrCandidate) return false;
            matrix vm = rpm<matrix>(intPtrCandidate + Offsets::Camera_Matrix);
            if (is_valid_matrix(vm)) {
                viewMatrix = vm;
                matrix_resolved = true;
                return true;
            }
            return false;
        };

        // Candidate 1: Standard FollowCamera (0x698) -> Camera (0x20 in maxff.h) -> IntPtr (0x10) -> Matrix
        Follow = rpm<uint64_t>(localPlayer + Offsets::Player_Camera);
        if (Follow) {
            Cam = rpm<uint64_t>(Follow + Offsets::Camera_Follow);
            if (Cam) {
                IntPtr = rpm<uint64_t>(Cam + Offsets::Camera_IntPtr);
                try_matrix(IntPtr);
            }
        }

        // Candidate 2: FollowCamera (0x698) -> Camera (0x30 in STORM_MODS) -> IntPtr (0x10)
        if (!matrix_resolved && Follow) {
            Cam = rpm<uint64_t>(Follow + Offsets::Camera_Follow_Alt);
            if (Cam) {
                IntPtr = rpm<uint64_t>(Cam + Offsets::Camera_IntPtr);
                try_matrix(IntPtr);
            }
        }

        // Candidate 3: FollowCamera (0x698) -> Camera (0x28) -> IntPtr (0x10)
        if (!matrix_resolved && Follow) {
            Cam = rpm<uint64_t>(Follow + 0x28);
            if (Cam) {
                IntPtr = rpm<uint64_t>(Cam + Offsets::Camera_IntPtr);
                try_matrix(IntPtr);
            }
        }

        // Candidate 3: BaseGame + 0xD8 -> 0x20 -> 0x10 (CameraBase fallback)
        if (!matrix_resolved && BaseGame) {
            uint64_t camMgr = rpm<uint64_t>(BaseGame + 0xD8);
            if (camMgr) {
                uint64_t c20 = rpm<uint64_t>(camMgr + 0x20);
                if (c20) {
                    try_matrix(rpm<uint64_t>(c20 + 0x10));
                }
                if (!matrix_resolved) {
                    uint64_t c28 = rpm<uint64_t>(camMgr + 0x28);
                    if (c28) {
                        try_matrix(rpm<uint64_t>(c28 + 0x10));
                    }
                }
            }
        }

        // Projecting with last frame's matrix puts enemies at stale screen
        // positions, which makes the aim steer behind moving targets ("drag").
        // Tolerate up to 3 transient read failures, then drop the snapshot.
        if (!matrix_resolved) {
            if (++s_matrixFailCount > 3) {
                std::lock_guard<std::mutex> lk(players_mutex);
                players.clear();
                return;
            }
        } else {
            s_matrixFailCount = 0;
        }
        if (!is_valid_matrix(viewMatrix)) {
            s_matrixFailCount = 0;
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // 3. Local Player 3D Position
        Vector3 localPos = Vector3::Zero();
        uint64_t headNode = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
        if (headNode) localPos = GetNodePosition(headNode);
        if (localPos == Vector3::Zero() || !std::isfinite(localPos.x)) {
            localPos = GetCameraPosition(viewMatrix);
        }

        // 4. Ground Loot Scan
        if (cfg::esp::loot) {
            scanLoot(BaseGame, localPos);
        } else {
            std::lock_guard<std::mutex> lk(loots_mutex);
            loots.clear();
        }

        // 5. Enemy Players Dictionary Traversal (Candidates: BaseGame + 0xC0/0xC8, m_Match + 0xC0/0xC8)
        const uint64_t dictCandidates[] = {
            rpm<uint64_t>(BaseGame + Offsets::Match_PlayerDict), // 0xC0 (DictionaryEntities)
            rpm<uint64_t>(BaseGame + 0xC8),                      // 0xC8 (LDictionaryEntities for BR match)
            rpm<uint64_t>(m_Match + Offsets::Match_PlayerDict),  // 0xC0 on Match
            rpm<uint64_t>(m_Match + 0xC8)                       // 0xC8 on Match
        };

        MonoDictionary dict(0);
        for (uint64_t candAddr : dictCandidates) {
            if (!candAddr || candAddr < 0x10000) continue;
            MonoDictionary testDict(candAddr);
            if (testDict.valid && testDict.getNumValues() > 0) {
                dict = testDict;
                break;
            }
            if (testDict.valid && !dict.valid) {
                dict = testDict;
            }
        }

        if (!dict.valid) {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // Resolve which per-entity queries are actually needed this tick.
        // Every one of these used to run for every enemy on every frame even
        // when the corresponding overlay element was switched off.
        const bool wantName    = cfg::esp::name;
        const bool wantWeapon  = cfg::esp::weapon;
        const bool wantArmor   = cfg::esp::armor;
        const bool wantToe     = cfg::esp::box || cfg::esp::health || cfg::esp::armor ||
                                 cfg::esp::name || cfg::esp::distance || cfg::esp::weapon ||
                                 cfg::esp::tracer || cfg::esp::firing_ind || cfg::esp::bot_tag;
        const bool wantKnocked = cfg::esp::box; // aim no longer scores knocked state

        int nv = dict.getNumValues();
        MonoDictionary::EntryBlock blk{};
        bool blockOk = false;
        for (int i = 0; i < nv; i++) {
            // Dictionary slots come out of the array 64 at a time (block read optimization)
            if (i % MonoDictionary::kBlockSize == 0) {
                blockOk = dict.read_block(i, blk);
            }
            uint64_t enemy = blockOk ? blk.value[i % MonoDictionary::kBlockSize] : dict.entry(i);
            if (!enemy || enemy < 0x100000000 || enemy == localPlayer) continue;

            // One syscall for everything that lives on the player object itself (falls back to rpm on error)
            PlayerSpan span(enemy);

            if (span.at<bool>(Offsets::Player_IsDead)) continue;
            bool isBot = span.at<bool>(Offsets::Player_IsBot);

            int curHP = 0, maxHP = 0, curArmor = 0;
            // HP is needed for aim validity; armor only when the armor bar is on.
            readHealthAndArmor(span.at<uint64_t>(Offsets::Player_HP), curHP, maxHP,
                               curArmor, wantArmor);
            if (curHP <= 0) {
                curHP = 200;
                maxHP = 200;
            }

            bool visible = true;
            auto avatarMgr = span.at<uint64_t>(Offsets::Player_Avatar);
            if (avatarMgr) {
                auto uma = rpm<uint64_t>(avatarMgr + Offsets::Avatar_Uma);
                if (uma) {
                    visible = rpm<bool>(uma + Offsets::Uma_Visible);

                    // Zero-overhead filtering: drop teammates (matches STORM_MODS line 1076)
                    auto umaData = rpm<uint64_t>(uma + Offsets::Uma_Data);
                    if (umaData) {
                        if (rpm<bool>(umaData + Offsets::Avatar::UmaData_Teammate)) continue;
                    }
                }
            }

            // Head 3D World Position
            uint64_t hNode = span.at<uint64_t>(Offsets::Bone_Head);
            if (!hNode) continue;
            Vector3 headWorld = GetNodePosition(hNode);
            if (headWorld == Vector3::Zero() || headWorld.sqrMagnitude() < 1.0f) continue;
            if (!std::isfinite(headWorld.x) || !std::isfinite(headWorld.y) || !std::isfinite(headWorld.z)) continue;
            if (headWorld.y < -150.0f || headWorld.y > 600.0f) continue;

            // Hip/spine/neck bone walks REMOVED. Each GetNodePosition() walks the
            // Unity transform hierarchy with many syscalls; those bones only fed
            // the old chest/neck target modes and the deleted headshot RNG.
            // The drag shot is head-only, so reading them was pure query waste.

            float dist = calculate_distance(localPos, headWorld);
            // Official Game Range: Zero artificial distance capping.
            // Every active entity spawned in game memory is processed.
            if (dist < 0.8f) continue;
            if (cfg::esp::max_range > 0.0f && dist > cfg::esp::max_range) continue;

            // 2D Screen Projections
            Vector3 headScreen = WorldToScreenPoint(viewMatrix, headWorld);
            if (headScreen.z < 0.001f) continue;

            // Toe is only used to size ESP boxes. The drag shot is head-only,
            // so in pure-aim mode this entire transform walk is skipped.
            Vector3 toeScreen = Vector3::Zero();
            if (wantToe) {
                uint64_t toeNode = span.at<uint64_t>(Offsets::Bone_LeftToe);
                toeScreen = toeNode ? boneScreen(toeNode) : Vector3::Zero();
                if (toeScreen.z < 0.001f || toeScreen == Vector3::Zero()) {
                    Vector3 toeWorld = headWorld;
                    toeWorld.y -= 1.8f;
                    toeScreen = WorldToScreenPoint(viewMatrix, toeWorld);
                    if (toeScreen.z < 0.001f) {
                        // Last resort: W2S maps world dy to -cy * m22 * dy / w.
                        float w = headScreen.z;
                        if (w < 0.1f) w = 0.1f;
                        float heightPx = (g_sh * 0.5f) * fabsf(viewMatrix.m22) * 1.8f / w;
                        if (!std::isfinite(heightPx) || heightPx <= 0.0f) heightPx = 4.0f;
                        if (heightPx > g_sh) heightPx = g_sh;
                        toeScreen = Vector3(headScreen.x, headScreen.y + heightPx, headScreen.z);
                    }
                }
            }

            // Knocked / Downed state (PhyXData -> MaybeDead -> IsKnocked).
            // Two syscalls per enemy; only the ESP box recolors knocked players.
            bool isKnocked = false;
            if (wantKnocked) {
                uint64_t phyx = rpm<uint64_t>(enemy + Offsets::Player::PhyXData);
                if (phyx) {
                    uint64_t md = rpm<uint64_t>(phyx + Offsets::Player::PhyX_MaybeDead);
                    if (md && rpm<int>(md + Offsets::Player::PhyX_IsKnocked) == 8) {
                        isKnocked = true;
                    }
                }
            }

            // Build synchronized PlayerData entity snapshot
            PlayerData pd;
            if (wantName)   readName(span, pd.name, sizeof(pd.name));
            if (wantWeapon) pd.weaponType = readWeapon(span);
            if (!pd.name[0] && isBot) snprintf(pd.name, sizeof(pd.name), "BOT");

            pd.addr       = enemy;
            pd.Head       = headScreen;
            pd.Toe        = toeScreen;
            pd.headWorld  = headWorld;
            pd.isVisible  = visible;
            pd.isBot      = isBot;
            pd.isKnocked  = isKnocked;
            pd.distance   = dist;
            pd.curHP      = curHP;
            pd.maxHP      = maxHP;
            pd.curArmor   = curArmor;
            pd.isFiring   = span.at<bool>(Offsets::Player_IsFiring);
            pd.isGrounded = span.at<bool>(Offsets::Player_IsGrounded);

            Vector3 vel = span.at<Vector3>(Offsets::Player_Velocity);
            pd.speed = sqrtf(vel.x*vel.x + vel.y*vel.y + vel.z*vel.z);

            tmp.push_back(pd);
        }

        // Atomic double-buffer swap into shared state container
        {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.swap(tmp);
        }
    }

    // ------------------------------------------------------------------------
    // REAL-TIME BACKGROUND READER THREAD
    // ------------------------------------------------------------------------
    static void loop() {
        while (g_run.load(std::memory_order_relaxed)) {
            update();
            sched_yield();
        }
    }

    void thread_start() {
        if (g_run.exchange(true)) return;
        g_thread = std::thread(loop);
    }

    void thread_stop() {
        if (!g_run.exchange(false)) return;
        if (g_thread.joinable()) g_thread.join();
        std::lock_guard<std::mutex> lk(players_mutex);
        players.clear();
    }

} // namespace data
