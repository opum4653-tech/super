#pragma once
// ============================================================================
// GAME DATA SYNCHRONIZATION ENGINE INTERFACE
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Declares public APIs and synchronized state variables for entity scraping.
// Implemented in: src/game/data.cpp.
// Started/Stopped by: src/main.cpp (attach_worker thread).
// Consumed by: src/func/esp/*.hpp and src/func/aim/*.hpp.
// ============================================================================

#include <vector>
#include <mutex>
#include <atomic>
#include "player.hpp"
#include "math.hpp"

namespace data {

    // Lifecycle Controls
    void update();        // Single-pass memory scrape iteration
    void thread_start();  // Starts background real-time memory reader thread
    void thread_stop();   // Gracefully terminates reader thread and clears memory containers

    // Synchronized Entity Containers (Guarded by dedicated mutexes)
    extern std::vector<PlayerData> players;        // Active enemy player snapshot
    extern std::mutex              players_mutex;  // Mutex protecting 'players' vector access

    extern std::vector<LootItem>   loots;          // Active ground loot items snapshot
    extern std::mutex              loots_mutex;    // Mutex protecting 'loots' vector access

    // Shared Match & Camera States
    extern std::atomic<uint64_t>   localPlayerAddr;// Pointer to local user player entity
    extern std::atomic<bool>       inMatch;        // True when local player is in an active match
    extern matrix                  viewMatrix;     // Validated 4x4 camera view matrix for W2S projections
    matrix                         get_view_matrix(); // Mutex-protected snapshot for worker threads

    // Runtime resolution telemetry (read by the Info tab)
    extern std::atomic<uint64_t>   selectedMatchAddr;
    extern std::atomic<uint64_t>   selectedDictAddr;
    extern std::atomic<int>        selectedEnemyCount;
    extern std::atomic<int>        selectedDictCount;

} // namespace data
