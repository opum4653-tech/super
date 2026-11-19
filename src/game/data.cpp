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
//    Reads contiguous span (0x70 to 0x860) in ONE process_vm_readv call instead of 15 individual syscalls.
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

    // Visibility debouncer: eliminates 1-frame flickering when enemies pass behind thin foliage
    static std::unordered_map<uint64_t, int>  g_visCounter;
    static std::unordered_map<uint64_t, bool> g_visStable;

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
    // process_vm_readv replaces roughly a dozen syscalls per player per tick
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
    static void readHealthAndArmor(uint64_t pool, int& curHP, int& maxHP, int& curArmor) {
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

        // 3. Live Armor Points (Index 4 at 0x40 if registered)
        auto elem4 = rpm<uint64_t>(datas + 0x40);
        if (elem4 && elem4 > 0x10000) {
            curArmor = rpm<int>(elem4 + Offsets::HP_Value);
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

        uint64_t entries = rpm<uint64_t>(dictPtr + 0x18);
        int count = rpm<int>(dictPtr + 0x20);
        if (!entries || count <= 0 || count > 500) return;

        uint64_t base = entries + 0x20;
        for (int i = 0; i < count; i++) {
            uint64_t e = base + i * 0x18;
            uint64_t dropObj = rpm<uint64_t>(e + 0x10);
            if (!dropObj || dropObj < 0x100000) continue;

            bool landed = rpm<bool>(dropObj + Offsets::DropObject_Landed);
            if (!landed) continue;

            Vector3 w = rpm<Vector3>(dropObj + Offsets::DropObject_Position);
            if (w == Vector3::Zero()) continue;

            float dist = calculate_distance(localPos, w);
            if (dist > cfg::esp::loot_range) continue;

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

        int matchStateMatch = rpm<int>(m_Match + Offsets::MatchIsRunning);
        int matchStateBase  = rpm<int>(BaseGame + Offsets::MatchIsRunning);
        bool matchActive = (localPlayer != 0) ||
                           (matchStateMatch == 1 || (matchStateMatch != 4 && matchStateMatch != 0)) ||
                           (matchStateBase == 1);
        inMatch.store(matchActive);

        // Clear states if match is over or in lobby
        if (!matchActive) {
            localPlayerAddr.store(0);
            {
                std::lock_guard<std::mutex> lk(players_mutex);
                if (!players.empty()) players.clear();
            }
            {
                std::lock_guard<std::mutex> lk(loots_mutex);
                if (!loots.empty()) loots.clear();
            }
            if (!g_visStable.empty()) g_visStable.clear();
            if (!g_visCounter.empty()) g_visCounter.clear();
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
            g_visStable.clear();
            g_visCounter.clear();
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

        if (!matrix_resolved && !is_valid_matrix(viewMatrix)) {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // 3. Local Player 3D Position
        Vector3 localPos = Vector3::Zero();
        uint64_t headNode = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
        if (headNode) localPos = GetNodePosition(headNode);

        // 4. Ground Loot Scan
        if (cfg::esp::loot) {
            scanLoot(BaseGame, localPos);
        } else {
            std::lock_guard<std::mutex> lk(loots_mutex);
            loots.clear();
        }

        // 5. Enemy Players Dictionary Traversal
        uint64_t dictAddr = rpm<uint64_t>(BaseGame + Offsets::Match_PlayerDict);
        if (!dictAddr) {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        MonoDictionary dict(dictAddr);
        if (!dict.valid) {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        int nv = dict.getNumValues();
        MonoDictionary::EntryBlock blk{};
        for (int i = 0; i < nv; i++) {
            // Dictionary slots come out of the array 64 at a time (block read optimization)
            if (i % MonoDictionary::kBlockSize == 0 && !dict.read_block(i, blk)) break;
            uint64_t enemy = blk.value[i % MonoDictionary::kBlockSize];
            if (!enemy || enemy < 0x100000000 || enemy == localPlayer) continue;

            // One syscall for everything that lives on the player object itself
            PlayerSpan span(enemy);
            if (!span.ok) continue;

            if (span.at<bool>(Offsets::Player_IsDead)) continue;
            bool isBot = span.at<bool>(Offsets::Player_IsBot);

            int curHP = 0, maxHP = 0, curArmor = 0;
            readHealthAndArmor(span.at<uint64_t>(Offsets::Player_HP), curHP, maxHP, curArmor);
            if (curHP <= 0) {
                curHP = 200;
                maxHP = 200;
            }

            auto avatarMgr = span.at<uint64_t>(Offsets::Player_Avatar);
            if (!avatarMgr) continue;
            auto uma = rpm<uint64_t>(avatarMgr + Offsets::Avatar_Uma);
            if (!uma) continue;

            bool rawVisible = rpm<bool>(uma + Offsets::Uma_Visible);

            // Debounce visibility state across frames
            bool visible;
            {
                auto it = g_visStable.find(enemy);
                if (it == g_visStable.end()) {
                    g_visStable[enemy] = rawVisible;
                    g_visCounter[enemy] = 0;
                    visible = rawVisible;
                } else if (it->second == rawVisible) {
                    g_visCounter[enemy] = 0;
                    visible = rawVisible;
                } else {
                    g_visCounter[enemy]++;
                    if (g_visCounter[enemy] >= 2) {
                        g_visStable[enemy] = rawVisible;
                        g_visCounter[enemy] = 0;
                        visible = rawVisible;
                    } else {
                        visible = it->second;
                    }
                }
            }

            // Zero-overhead filtering: drop teammates & local user
            auto umaData = rpm<uint64_t>(uma + Offsets::Uma_Data);
            if (umaData) {
                if (rpm<bool>(umaData + Offsets::Avatar::UmaData_Teammate)) continue;
                if (rpm<bool>(umaData + Offsets::Avatar::UmaData_LocalPlayer)) continue;
            }

            // Head 3D World Position
            uint64_t hNode = span.at<uint64_t>(Offsets::Bone_Head);
            if (!hNode) continue;
            Vector3 headWorld = GetNodePosition(hNode);
            if (headWorld == Vector3::Zero() || headWorld.sqrMagnitude() < 1.0f) continue;
            if (!std::isfinite(headWorld.x) || !std::isfinite(headWorld.y) || !std::isfinite(headWorld.z)) continue;
            if (headWorld.y < -150.0f || headWorld.y > 600.0f) continue;

            // Hip 3D World Position (used for STORM_MODS chest & body targeting math)
            uint64_t hipNode = span.at<uint64_t>(Offsets::Bone_Hip);
            Vector3 hipWorld = Vector3::Zero();
            if (hipNode) hipWorld = GetNodePosition(hipNode);

            // Spine 3D World Position & Interpolated Neck World Position (pewpew pattern)
            uint64_t spineNode = span.at<uint64_t>(Offsets::Bone::Breast);
            Vector3 spineWorld = Vector3::Zero();
            if (spineNode) spineWorld = GetNodePosition(spineNode);
            if (spineWorld == Vector3::Zero() || !std::isfinite(spineWorld.x)) {
                spineWorld = (hipWorld != Vector3::Zero()) ? hipWorld : headWorld;
            }
            Vector3 neckWorld = (spineWorld != headWorld)
                ? (headWorld + (spineWorld - headWorld) * 0.35f)
                : (headWorld - Vector3(0.0f, 0.20f, 0.0f));

            float dist = calculate_distance(localPos, headWorld);
            if (dist < 0.8f || dist > cfg::esp::max_range) continue;

            // 2D Screen Projections
            Vector3 headScreen = WorldToScreenPoint(viewMatrix, headWorld);
            if (headScreen.z < 0.001f) continue;

            uint64_t toeNode = span.at<uint64_t>(Offsets::Bone_LeftToe);
            Vector3 toeScreen = toeNode ? boneScreen(toeNode) : Vector3::Zero();
            if (toeScreen.z < 0.001f || toeScreen == Vector3::Zero()) {
                Vector3 toeWorld = headWorld;
                toeWorld.y -= 1.8f;
                toeScreen = WorldToScreenPoint(viewMatrix, toeWorld);
                if (toeScreen.z < 0.001f) {
                    float estimatedHeight = (g_sh * 1.8f) / std::max(1.0f, dist * 1.2f);
                    toeScreen = Vector3(headScreen.x, headScreen.y + estimatedHeight, headScreen.z);
                }
            }

            // Knocked / Downed state detection (PhyXData 0x1D60 -> MaybeDead 0x20 -> IsKnocked 0x10 == 8)
            bool isKnocked = false;
            uint64_t phyx = rpm<uint64_t>(enemy + Offsets::Player::PhyXData);
            if (phyx) {
                uint64_t md = rpm<uint64_t>(phyx + Offsets::Player::PhyX_MaybeDead);
                if (md && rpm<int>(md + Offsets::Player::PhyX_IsKnocked) == 8) {
                    isKnocked = true;
                }
            }

            // Build synchronized PlayerData entity snapshot
            PlayerData pd;
            readName(span, pd.name, sizeof(pd.name));
            pd.weaponType = readWeapon(span);
            if (!pd.name[0] && isBot) snprintf(pd.name, sizeof(pd.name), "BOT");

            pd.addr       = enemy;
            pd.Head       = headScreen;
            pd.Toe        = toeScreen;
            pd.headWorld  = headWorld;
            pd.hipWorld   = hipWorld;
            pd.spineWorld = spineWorld;
            pd.neckWorld  = neckWorld;
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

        // Clean up debounce cache if map exceeds 200 stale entries
        if (g_visStable.size() > 200) {
            g_visStable.clear();
            g_visCounter.clear();
        }

        // Atomic double-buffer swap into shared state container
        {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.swap(tmp);
        }
    }

    // ------------------------------------------------------------------------
    // BACKGROUND READER THREAD CADENCE
    // ------------------------------------------------------------------------
    static void loop() {
        while (g_run.load(std::memory_order_relaxed)) {
            update();
            int iv = cfg::aim::read_interval_ms;
            if (iv < 8) iv = 8;
            if (iv > 100) iv = 100;

            // In lobby or between matches: throttle politely to 100ms (10 checks/sec)
            if (!inMatch.load(std::memory_order_relaxed)) {
                iv = 100;
            } else {
                // Anti-detection timing jitter: +/- 2ms non-periodic cadence to foil frequency analyzers
                static uint32_t s_jitterRng = 0x12345678;
                s_jitterRng = s_jitterRng * 1103515245 + 12345;
                iv += static_cast<int>((s_jitterRng % 5) - 2);
                if (iv < 5) iv = 5;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(iv));
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
