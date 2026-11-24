#pragma once
// ============================================================================
// IL2CPP MONO DICTIONARY PARSER & CHUNK MEMORY READER
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Parses Unity Il2Cpp's internal System.Collections.Generic.Dictionary<TKey, TValue>
// structure directly from external memory:
//
// IL2CPP 64-BIT DICTIONARY MEMORY LAYOUT:
//   +0x18 -> _entries (pointer to managed array of entries)
//   +0x20 -> _count (number of active elements)
//   _entries + 0x20 -> start of Entry array data (skipping 32-byte Il2Cpp array header)
//   Entry size: 0x18 bytes
//     +0x00: int32_t hashCode
//     +0x04: int32_t next
//     +0x08: uint64_t key
//     +0x10: uint64_t value (pointer to entity object, e.g. ReplicationEntity*)
//
// STEALTH & PERFORMANCE OPTIMIZATION:
// Instead of issuing 3 individual syscalls per slot, read_block() reads runs of
// 64 slots (0x600 bytes) in ONE contiguous memory read, completely eliminating
// high syscall overhead during entity enumeration.
// ============================================================================

#include "memory.hpp"
#include <cstdint>

class MonoDictionary {

public:
    uint64_t address;

    // stealth: 1 syscall lê os dois campos que importam.
    uint64_t entries_head;
    int      count;
    bool     valid;

    explicit MonoDictionary(uint64_t addr) : address(addr), entries_head(0), count(0), valid(false) {
        if (!addr_valid(addr)) return;

        // 1. STORM_MODS / Free Fire Max proven layout:
        //    Dictionary (+0x28) -> values (monoArray<V>*)
        //    values (+0x18) -> length (count)
        //    values (+0x20) -> vector elements start (stride = 0x18)
        uint64_t valuesArray = rpm<uint64_t>(addr + 0x28);
        if (addr_valid(valuesArray)) {
            int len = rpm<int>(valuesArray + 0x18);
            if (len > 0 && len <= 200) {
                entries_head = valuesArray + 0x20;
                count = len;
                valid = true;
                return;
            }
        }

        // 2. Alternative layout (.NET Core / IL2CPP Entry[] array):
        //    Dictionary (+0x18) -> entries (Entry[] array)
        //    Dictionary (+0x20) or entries (+0x18) -> count
        uint64_t entriesArray = rpm<uint64_t>(addr + 0x18);
        if (addr_valid(entriesArray)) {
            int len = rpm<int>(addr + 0x20);
            if (len <= 0 || len > 200) {
                len = rpm<int>(entriesArray + 0x18);
            }
            if (len > 0 && len <= 200) {
                entries_head = entriesArray + 0x20;
                count = len;
                valid = true;
                return;
            }
        }
    }

    inline uint64_t getValues()   const { return entries_head; }
    inline int      getNumValues() const { return count; }

    // ReplicationEntity* de uma entry — matches STORM_MODS entry resolution
    inline uint64_t entry(int i) const {
        if (!valid || i < 0 || i >= count) return 0;
        uint64_t val = rpm<uint64_t>(entries_head + i * 0x18 + 0x10);
        return (addr_valid(val)) ? val : 0;
    }

    // The entries array is contiguous, so a run of them is one read instead of
    // three per slot. 64 entries is 0x600 bytes — one syscall, small enough to
    // never straddle an unmapped page of the backing array.
    static constexpr int kBlockSize = 64;

    struct EntryBlock {
        uint64_t value[kBlockSize];   // 0 when the slot is empty or invalid
        int      count;               // entries actually filled from this block
    };

    inline bool read_block(int start, EntryBlock& out) const {
        out.count = 0;
        if (!valid || start < 0 || start >= count) return false;

        int n = count - start;
        if (n > kBlockSize) n = kBlockSize;

        // Slot layout: +0x00 hashCode(int32), +0x08 key, +0x10 value
        struct Raw {
            int32_t  hash;
            int32_t  pad;
            uint64_t key;
            uint64_t value;
        };
        static_assert(sizeof(Raw) == 0x18, "dictionary entry must be 0x18 bytes");

        Raw raw[kBlockSize];
        if (!mem_read(entries_head + static_cast<uint64_t>(start) * 0x18,
                      raw, static_cast<size_t>(n) * 0x18)) {
            return false;
        }

        for (int i = 0; i < n; i++) {
            // Validate value pointer (must be valid 64-bit userland heap address)
            out.value[i] = (addr_valid(raw[i].value)) ? raw[i].value : 0;
        }
        out.count = n;
        return true;
    }
};
