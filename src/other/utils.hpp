#pragma once
#include "memory.hpp"
#include <cstdint>

// Dictionary<Byte, Player> layout (IL2CPP 64-bit)
//   +0x18 -> _entries (managed array header)
//   +0x20 -> _count
//   entries[i] = ptr + 0x20 (skip array header) + i * 0x18
//   Entry.value = +0x10 (ReplicationEntity*)
class MonoDictionary {
public:
    uint64_t address;

    // stealth: 1 syscall lê os dois campos que importam.
    uint64_t entries_head;
    int      count;
    bool     valid;

    explicit MonoDictionary(uint64_t addr) : address(addr), entries_head(0), count(0), valid(false) {
        if (!addr_valid(addr)) return;

        struct { uint64_t entries; uint64_t count_and_pad; } header{};
        if (!mem_read(addr + 0x18, &header, sizeof(header))) return;

        if (!addr_valid(header.entries)) return;

        entries_head = header.entries + 0x20;               // skip il2cpp array header
        count        = static_cast<int>(header.count_and_pad & 0xFFFFFFFFu);
        valid        = (count > 0 && count < 0x1000);
    }

    inline uint64_t getValues()   const { return entries_head; }
    inline int      getNumValues() const { return count; }

    // ReplicationEntity* de uma entry — validate hashCode & key to eliminate ghost/dead entries
    inline uint64_t entry(int i) const {
        if (!valid || i < 0 || i >= count) return 0;
        int hashCode = rpm<int>(entries_head + i * 0x18);
        if (hashCode < 0) return 0; // Slot is empty or deleted in il2cpp Dictionary
        uint64_t key = rpm<uint64_t>(entries_head + i * 0x18 + 0x08);
        if (!key) return 0;
        return rpm<uint64_t>(entries_head + i * 0x18 + 0x10);
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
            // Same acceptance rule as entry(): reject empty slots and dead keys.
            out.value[i] = (raw[i].hash < 0 || raw[i].key == 0) ? 0 : raw[i].value;
        }
        out.count = n;
        return true;
    }
};
