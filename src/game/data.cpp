// ============================================================================
// GAME ENTITY SCRAPING & MATRIX SYNCHRONIZATION ENGINE
// ============================================================================
// Project: crack_master (Pure External Ring-3 Read Engine)
//
// ARCHITECTURAL OVERVIEW:
// 1. Base Chain Resolution:
//    Traverses GameFacade (0xAD68E18) -> StaticFields (0xB8) -> BaseGame (0x8) -> Match (0x90).
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
#include <algorithm>
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
    std::mutex               g_viewMutex;

    std::atomic<uint64_t>    selectedMatchAddr{0};
    std::atomic<uint64_t>    selectedDictAddr{0};
    std::atomic<int>         selectedEnemyCount{0};
    std::atomic<int>         selectedDictCount{0};

    std::atomic<uint64_t>    diagDirectCell{0};
    std::atomic<uint64_t>    diagStaticFields{0};
    std::atomic<uint64_t>    diagBaseGame{0};
    std::atomic<uint64_t>    diagRawMatch{0};

    matrix get_view_matrix() {
        std::lock_guard<std::mutex> lk(g_viewMutex);
        return viewMatrix;
    }

    static void clear_resolution_telemetry() {
        selectedMatchAddr.store(0);
        selectedDictAddr.store(0);
        selectedEnemyCount.store(0);
        selectedDictCount.store(0);
        diagDirectCell.store(0);
        diagStaticFields.store(0);
        diagBaseGame.store(0);
        diagRawMatch.store(0);
    }

    // State-transition signal only: not an entity cache or fallback value.
    static std::atomic<bool> s_scrapeReady{false};
    static void set_scrape_ready(bool ready) {
        const bool changed =
            (s_scrapeReady.exchange(ready, std::memory_order_relaxed) != ready);
        // Every fresh entity snapshot must wake the aim worker. The old
        // edge-only signal let aim write once per enable and then sleep while
        // the target kept moving. Menu-off only notifies on state change.
        if (changed || ready) cfg::notify_state_change();
    }

    // Internal background thread state
    static std::atomic<bool> g_run{false};
    static std::thread       g_thread;

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
    static bool readHealthAndArmor(uint64_t pool, int& curHP, int& maxHP, int& curArmor,
                                   bool wantArmor = true) {
        curHP = 0;
        maxHP = 0;
        curArmor = 0;
        if (!pool) return false;

        uint64_t datas = 0;
        if (!mem_read(pool + Offsets::HP_List, &datas, sizeof(datas)) || datas <= 0x10000) {
            return false;
        }

        // 1. Live Current Health (Proven Index 0 at Offsets::HP_Entry = 0x20).
        // Use status-returning reads here: a failed KPM read used to leave curHP
        // at 0, which then looked like a dead player and blanked the target list.
        uint64_t elem0 = 0;
        const bool hpPtrOk = mem_read(datas + Offsets::HP_Entry, &elem0, sizeof(elem0)) &&
                             elem0 > 0x10000;
        bool hpValueOk = false;
        if (hpPtrOk) {
            hpValueOk = mem_read(elem0 + Offsets::HP_Value, &curHP, sizeof(curHP));
        }

        // 2. Live Maximum Health (Index 1 at 0x28 if registered).
        bool maxHPRead = false;
        uint64_t elem1 = 0;
        if (mem_read(datas + 0x28, &elem1, sizeof(elem1)) && elem1 > 0x10000) {
            int m = 0;
            if (mem_read(elem1 + Offsets::HP_Value, &m, sizeof(m)) && m > 0 && m <= 1000) {
                maxHP = m;
                maxHPRead = true;
            }
        }
        if (maxHP < curHP) maxHP = curHP;
        if (maxHP <= 0) maxHP = (curHP > 0 ? curHP : 200);

        // 3. Live Armor Points (Index 4 at 0x40) — skip entirely when the
        //    armor bar is off; this is 2 syscalls per enemy per tick.
        if (wantArmor) {
            uint64_t elem4 = 0;
            if (mem_read(datas + 0x40, &elem4, sizeof(elem4)) && elem4 > 0x10000) {
                int armor = 0;
                if (mem_read(elem4 + Offsets::HP_Value, &armor, sizeof(armor)) &&
                    armor >= 0 && armor <= 1000) {
                    curArmor = armor;
                }
            }
        }

        // A real dead player has 0 HP but a readable chain. A broken chain has
        // no pointer, an out-of-range value, or no HP/maxHP signal at all.
        const bool saneHP = hpValueOk && curHP >= 0 && curHP <= 10000;
        return hpPtrOk && saneHP && (curHP > 0 || maxHPRead);
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
    static Vector3 boneScreen(uint64_t node, const matrix& vm) {
        if (!node) return Vector3::Zero();
        Vector3 w = GetNodePosition(node);
        if (w == Vector3::Zero()) return Vector3::Zero();
        return WorldToScreenPoint(vm, w);
    }

    // ------------------------------------------------------------------------
    // GROUND LOOT SCANNER
    // ------------------------------------------------------------------------
    // Traverses MetaManager.DropObjects dictionary to locate weapons, armor, ammo
    static void scanLoot(uint64_t BaseGame, const Vector3& localPos, const matrix& vm) {
        const cfg::RuntimeConfig rc = cfg::runtime();
        std::vector<LootItem> tmp;

        uint64_t match = rpm<uint64_t>(BaseGame + Offsets::BaseGame_Match);
        if (!match) return;

        uint64_t metaMgr = rpm<uint64_t>(match + Offsets::Match_MetaManager);
        if (!metaMgr) return;

        uint64_t dictPtr = rpm<uint64_t>(metaMgr + Offsets::MetaManager_DropObjects);
        if (!dictPtr) return;

        MonoDictionary dict(dictPtr);
        if (!dict.valid) return;

        MonoDictionary::EntryBlock blk{};
        bool blockOk = false;
        for (int i = 0; i < dict.count; i++) {
            if (i % MonoDictionary::kBlockSize == 0) {
                blockOk = dict.read_block(i, blk);
            }
            uint64_t dropObj = blockOk ? blk.value[i % MonoDictionary::kBlockSize] : dict.entry(i);
            if (!dropObj || dropObj < 0x100000) continue;

            bool landed = rpm<bool>(dropObj + Offsets::DropObject_Landed);
            if (!landed) continue;

            Vector3 w = rpm<Vector3>(dropObj + Offsets::DropObject_Position);
            if (w == Vector3::Zero()) continue;

            float dist = calculate_distance(localPos, w);
            if (rc.espLootRange > 0.0f && dist > rc.espLootRange) continue;

            Vector3 sc = WorldToScreenPoint(vm, w);
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

        // Single immutable config snapshot for this scrape tick.
        const cfg::RuntimeConfig rc = cfg::runtime();

        // Menu-off means game-off: no chain reads, no camera reads, no entity
        // reads, no cached/fallback local player. Official game state untouched.
        if (!rc.anyFeature) {
            inMatch.store(false);
            localPlayerAddr.store(0);
            set_scrape_ready(false);
            clear_resolution_telemetry();
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

        // 1. Resolve Base Chain (Free Fire Max: maxff.h primary 0xAD68E18, STORM_MODS alt 0xAD6DC18)
        // Do not take the first non-null Match. Training and a live BR match can
        // both leave old Match objects mapped; choosing wrong here makes every
        // later read point at a dead/local-only entity set.
        struct ChainCandidate {
            uint64_t base = 0;
            uint64_t match = 0;
            uint64_t local = 0;
            int score = -1;
        };
        ChainCandidate best;

        uint64_t lastCell = 0, lastSf = 0, lastBg = 0, lastRawMatch = 0;

        auto score_chain = [&](uint64_t bg, uint64_t match) {
            if (!bg || bg < 0x10000 || !match || match < 0x10000) return;
            lastRawMatch = match;
            uint64_t lp = rpm<uint64_t>(match + Offsets::Match_LocalPlayer);
            int score = is_valid_ptr(lp) ? 1000 : 0;

            // A live match dictionary with real enemy pointers outranks a stale
            // lobby/training dictionary that merely has count > 0.
            const uint64_t dicts[] = {
                rpm<uint64_t>(bg + Offsets::Match_PlayerDict),
                rpm<uint64_t>(bg + 0xC8),
                rpm<uint64_t>(match + Offsets::Match_PlayerDict),
                rpm<uint64_t>(match + 0xC8)
            };
            int bestDictCount = 0;
            for (uint64_t d : dicts) {
                MonoDictionary td(d);
                if (td.valid) bestDictCount = std::max(bestDictCount, td.getNumValues());
            }
            score += std::min(bestDictCount, 100) * 10;

            int ms = rpm<int>(match + Offsets::MatchIsRunning);
            if (ms == 1 || (ms != 0 && ms != 4)) score += 50;

            if (score > best.score) best = { bg, match, lp, score };
        };

        const uintptr_t gfOffsets[] = {
            Offsets::Base::GameFacade,
            Offsets::Base::GameFacade_Alt
        };

        for (uintptr_t gfOff : gfOffsets) {
            uint64_t directCell = rpm<uint64_t>(proc::lib + gfOff);
            if (!directCell || directCell < 0x10000) continue;
            if (!lastCell) lastCell = directCell;

            uint64_t classCandidates[] = { directCell, rpm<uint64_t>(directCell) };

            for (uint64_t gfClass : classCandidates) {
                if (!gfClass || gfClass < 0x10000) continue;

                uint64_t staticFields = rpm<uint64_t>(gfClass + Offsets::GameFacade_P2);
                if (!staticFields || staticFields < 0x10000) continue;
                if (!lastSf) lastSf = staticFields;

                uint64_t bgCandidates[] = {
                    rpm<uint64_t>(staticFields),        // +0x0 (STORM_MODS pattern)
                    rpm<uint64_t>(staticFields + 0x8)   // +0x8 (legacy/alt pattern)
                };

                for (uint64_t bg : bgCandidates) {
                    if (bg >= 0x10000 && !lastBg) lastBg = bg;
                    score_chain(bg, rpm<uint64_t>(bg + Offsets::BaseGame_Match));
                }
            }
        }

        diagDirectCell.store(lastCell);
        diagStaticFields.store(lastSf);
        diagBaseGame.store(lastBg);
        diagRawMatch.store(lastRawMatch);

        uint64_t BaseGame = best.base;
        uint64_t m_Match  = best.match;
        uint64_t localPlayer = best.local;

        if (!BaseGame || !m_Match) {
            inMatch.store(false);
            localPlayerAddr.store(0);
            set_scrape_ready(false);
            clear_resolution_telemetry();
            return;
        }

        int matchStateMatch = rpm<int>(m_Match + Offsets::MatchIsRunning);
        int matchStateBase  = rpm<int>(BaseGame + Offsets::MatchIsRunning);
        bool matchActive = (localPlayer != 0) ||
                           (matchStateMatch == 1 || (matchStateMatch != 4 && matchStateMatch != 0)) ||
                           (matchStateBase == 1);
        inMatch.store(matchActive);

        // Clear states if match is over or in lobby
        if (!matchActive) {
            localPlayerAddr.store(0);
            set_scrape_ready(false);
            clear_resolution_telemetry();
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

        // Store active local player pointer
        localPlayerAddr.store(localPlayer ? localPlayer : 0);

        if (!localPlayer) {
            set_scrape_ready(false);
            clear_resolution_telemetry();
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
                {
                    std::lock_guard<std::mutex> lk(g_viewMutex);
                    viewMatrix = vm;
                }
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

        // No stale matrix tolerance. A camera read that is not valid right now
        // drops this tick; the game keeps its own state until a fresh read
        // succeeds. No failure-count cache.
        if (!matrix_resolved) {
            set_scrape_ready(false);
            selectedEnemyCount.store(0);
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }
        const matrix vmNow = get_view_matrix();
        if (!is_valid_matrix(vmNow)) {
            set_scrape_ready(false);
            selectedEnemyCount.store(0);
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // 3. Local Player 3D Position
        Vector3 localPos = Vector3::Zero();
        uint64_t headNode = rpm<uint64_t>(localPlayer + Offsets::Bone_Head);
        if (headNode) localPos = GetNodePosition(headNode);
        if (localPos == Vector3::Zero() || !std::isfinite(localPos.x)) {
            localPos = GetCameraPosition(vmNow);
        }

        // 4. Ground Loot Scan
        if (rc.espLoot) {
            scanLoot(BaseGame, localPos, vmNow);
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

        // Pick the dictionary with the most usable enemy pointers, not simply
        // the first valid one. A stale 0xC0 dictionary can hold only the local
        // player while the live BR entity list sits at 0xC8 / m_Match.
        MonoDictionary dict(0);
        int bestDictScore = -1;
        int bestSampleCount = 0;
        int bestDictTotal = 0;
        uint64_t bestDictPointer = 0;
        for (uint64_t candAddr : dictCandidates) {
            if (!candAddr || candAddr < 0x10000) continue;
            MonoDictionary testDict(candAddr);
            if (!testDict.valid) continue;

            // Sample up to 8 real entities. Raw pointer count alone can let a
            // stale dictionary full of valid-looking heap addresses win.
            int usable = 0;
            int checked = 0;
            int attempts = 0;
            MonoDictionary::EntryBlock scoreBlk{};
            for (int i = 0; i < testDict.getNumValues() && checked < 8 && attempts < 24;
                 i += MonoDictionary::kBlockSize) {
                if (!testDict.read_block(i, scoreBlk)) continue;
                for (int j = 0; j < scoreBlk.count && checked < 8 && attempts < 24; j++) {
                    uint64_t e = scoreBlk.value[j];
                    if (e < 0x100000000 || e == localPlayer) continue;
                    attempts++;
                    PlayerSpan es(e);
                    if (es.at<bool>(Offsets::Player_IsDead)) continue;
                    if (!es.at<uint64_t>(Offsets::Bone_Head)) continue;
                    usable++;
                    checked++;
                }
            }

            int score = usable * 100 + std::min(testDict.getNumValues(), 50);
            if (score > bestDictScore) {
                bestDictScore = score;
                bestSampleCount = usable;
                bestDictTotal = testDict.getNumValues();
                bestDictPointer = candAddr;
                dict = testDict;
            }
        }

        selectedDictAddr.store(bestDictPointer);
        selectedDictCount.store(bestSampleCount > 0 ? bestDictTotal : 0);
        selectedMatchAddr.store(m_Match);

        if (!dict.valid) {
            set_scrape_ready(false);
            selectedEnemyCount.store(0);
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
            return;
        }

        // Resolve which per-entity queries are actually needed this tick.
        // Every one of these used to run for every enemy on every frame even
        // when the corresponding overlay element was switched off.
        const bool wantName    = rc.espName;
        const bool wantWeapon  = rc.espWeapon;
        const bool wantArmor   = rc.espArmor;
        const bool wantToe     = rc.espBox || rc.espHealth || rc.espArmor ||
                                 rc.espName || rc.espDistance || rc.espWeapon ||
                                 rc.espTracer || rc.espFiringInd || rc.espBotTag;
        // Aim penalises knocked targets in target.hpp, so the flag must be read
        // whenever aim is enabled, not only when the ESP box is drawn.
        const bool wantKnocked = rc.espBox || rc.aimEnable;

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
            // hpValid separates a real 0 HP from a broken HP pointer chain.
            const bool hpValid = readHealthAndArmor(span.at<uint64_t>(Offsets::Player_HP),
                                                    curHP, maxHP, curArmor, wantArmor);
            if (maxHP <= 0) maxHP = 200;

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
            if (rc.espMaxRange > 0.0f && dist > rc.espMaxRange) continue;

            // 2D Screen Projections
            Vector3 headScreen = WorldToScreenPoint(vmNow, headWorld);
            if (headScreen.z < 0.001f) continue;

            // Toe is only used to size ESP boxes. The drag shot is head-only,
            // so in pure-aim mode this entire transform walk is skipped.
            Vector3 toeScreen = Vector3::Zero();
            if (wantToe) {
                uint64_t toeNode = span.at<uint64_t>(Offsets::Bone_LeftToe);
                toeScreen = toeNode ? boneScreen(toeNode, vmNow) : Vector3::Zero();
                if (toeScreen.z < 0.001f || toeScreen == Vector3::Zero()) {
                    Vector3 toeWorld = headWorld;
                    toeWorld.y -= 1.8f;
                    toeScreen = WorldToScreenPoint(vmNow, toeWorld);
                    if (toeScreen.z < 0.001f) {
                        // Last resort: W2S maps world dy to -cy * m22 * dy / w.
                        float w = headScreen.z;
                        if (w < 0.1f) w = 0.1f;
                        float heightPx = (rc.screenH * 0.5f) * fabsf(vmNow.m22) * 1.8f / w;
                        if (!std::isfinite(heightPx) || heightPx <= 0.0f) heightPx = 4.0f;
                        if (heightPx > rc.screenH) heightPx = rc.screenH;
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
            pd.hpValid    = hpValid;
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
            selectedEnemyCount.store(static_cast<int>(players.size()));
        }
        set_scrape_ready(true); // Wake aim on a fresh real-time snapshot only.
    }

    // ------------------------------------------------------------------------
    // REAL-TIME BACKGROUND READER THREAD
    // ------------------------------------------------------------------------
    static void loop() {
        while (g_run.load(std::memory_order_relaxed)) {
            update();
            if (proc::pid <= 0 || proc::lib == 0 ||
                !cfg::runtime().anyFeature ||
                !inMatch.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                sched_yield();
            }
        }
    }

    void thread_start() {
        if (g_run.exchange(true)) return;
        g_thread = std::thread(loop);
    }

    void thread_stop() {
        if (!g_run.exchange(false)) return;
        if (g_thread.joinable()) g_thread.join();
        set_scrape_ready(false);
        localPlayerAddr.store(0);
        inMatch.store(false);
        clear_resolution_telemetry();
        {
            std::lock_guard<std::mutex> lk(players_mutex);
            players.clear();
        }
        {
            std::lock_guard<std::mutex> lk(loots_mutex);
            loots.clear();
        }
    }

} // namespace data
