#pragma once
// ============================================================================
// MEMORY ENGINE & PROCESS RESOLVER (SYSCALL 270)
// ============================================================================
// Project: crack_master
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include "kpm.h"
#include "../offsets/offsets.hpp"
#include "../protect/oxorany.hpp"

namespace proc {
    inline int      pid = -1;       // Target game process PID
    inline uint64_t lib = 0;        // Base virtual address of libil2cpp.so
    // Last target pid that was confirmed by name. get_pid() re-checks this one
    // entry instead of re-walking all of /proc on every poll.
    inline int      pid_cache = 0;
}

#ifdef SYS_openat
#undef SYS_openat
#endif
#ifdef SYS_close
#undef SYS_close
#endif
#ifdef SYS_read
#undef SYS_read
#endif
#ifdef SYS_getdents64
#undef SYS_getdents64
#endif

namespace sys {

    #if defined(__aarch64__)
    static constexpr long SYS_openat     = 56;
    static constexpr long SYS_close      = 57;
    static constexpr long SYS_read       = 63;
    static constexpr long SYS_getdents64 = 61;
    #elif defined(__x86_64__)
    static constexpr long SYS_openat     = 257;
    static constexpr long SYS_close      = 3;
    static constexpr long SYS_read       = 0;
    static constexpr long SYS_getdents64 = 217;
    #endif

    #ifndef AT_FDCWD
    #define AT_FDCWD -100
    #endif

    static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
        long r;
        #if defined(__aarch64__)
        register long x8 __asm__("x8") = n;
        register long x0 __asm__("x0") = a1;
        register long x1 __asm__("x1") = a2;
        register long x2 __asm__("x2") = a3;
        register long x3 __asm__("x3") = a4;
        register long x4 __asm__("x4") = a5;
        register long x5 __asm__("x5") = a6;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
        r = x0;
        #elif defined(__x86_64__)
        register long rax asm("rax") = n;
        register long rdi asm("rdi") = a1;
        register long rsi asm("rsi") = a2;
        register long rdx asm("rdx") = a3;
        register long r10 asm("r10") = a4;
        register long r8  asm("r8")  = a5;
        register long r9  asm("r9")  = a6;
        asm volatile("syscall" : "=a"(r) : "a"(rax), "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
        #else
        r = syscall(n, a1, a2, a3, a4, a5, a6);
        #endif
        return r;
    }

    static inline int  openat(int fd, const char* p, int f)  { return static_cast<int>(sc6(SYS_openat, fd, reinterpret_cast<long>(p), f, 0, 0, 0)); }
    static inline int  close (int fd)                        { return static_cast<int>(sc6(SYS_close, fd, 0, 0, 0, 0, 0)); }
    static inline long read  (int fd, void* b, size_t c)     { return sc6(SYS_read, fd, reinterpret_cast<long>(b), c, 0, 0, 0); }
    static inline long getdents64(int fd, void* d, size_t c) { return sc6(SYS_getdents64, fd, reinterpret_cast<long>(d), c, 0, 0, 0); }
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char d_name[];
};

// Wipe stack buffers after use
static inline void wipe(void* p, size_t n) {
    volatile uint8_t* v = reinterpret_cast<volatile uint8_t*>(p);
    for (size_t i = 0; i < n; i++) v[i] = 0;
}

inline bool is_valid_ptr(uint64_t a) {
    return (a >= 0x10000000ULL && a <= 0x7FFFFFFFFFFFULL);
}

inline bool addr_valid(uint64_t a) { return (a >= 0x10000ULL); }

// ============================================================================
// PROCESS MEMORY READ
// ============================================================================

inline bool mem_read(uint64_t a, void* b, size_t l) {
    if (proc::pid <= 0 || a < 0x10000 || l == 0 || b == nullptr) return false;
    // Clear first so a failed or partial read never leaves stale bytes.
    memset(b, 0, l);
    kpm.initialize(proc::pid);
    return kpm.read(static_cast<uintptr_t>(a), b, l);
}

template<typename T> inline T rpm(uint64_t a) {
    T v{};
    mem_read(a, &v, sizeof(T));
    return v;
}

// Confirm one cached pid still belongs to the target.
inline bool pid_alive(int pid) {
    if (pid <= 0) return false;

    char cp[64];
    int ps = 0;
    const char* ps_ = oxorany("/proc/");
    while (*ps_) cp[ps++] = *ps_++;

    char pds[16];
    int pl = 0;
    int tm = pid;
    while (tm > 0) { pds[pl++] = '0' + (tm % 10); tm /= 10; }
    if (pl == 0) return false;
    for (int i = pl - 1; i >= 0; i--) cp[ps++] = pds[i];

    const char* cs = oxorany("/cmdline");
    while (*cs) cp[ps++] = *cs++;
    cp[ps] = '\0';

    int cfd = sys::openat(AT_FDCWD, cp, O_RDONLY);
    wipe(cp, sizeof(cp));
    wipe(pds, sizeof(pds));
    if (cfd < 0) return false;

    char cmd[256] = {0};
    long b = sys::read(cfd, cmd, sizeof(cmd) - 1);
    sys::close(cfd);

    bool ok = false;
    if (b > 0) {
        cmd[b] = '\0';
        ok = (strstr(cmd, oxorany("com.dts.freefiremax")) != nullptr);
        wipe(cmd, sizeof(cmd));
    }
    return ok;
}

// ============================================================================
// TARGET GAME PROCESS DISCOVERY
// ============================================================================

inline int get_pid() {
    // Fast path: cached PID is still alive and matches game package
    if (proc::pid_cache > 0 && pid_alive(proc::pid_cache)) {
        kpm.initialize(proc::pid_cache);
        return proc::pid_cache;
    }
    proc::pid_cache = 0;

    int pfd = sys::openat(AT_FDCWD, oxorany("/proc"), O_RDONLY | O_DIRECTORY);
    if (pfd < 0) return -1;

    char buf[4096];
    int found = -1;

    while (true) {
        long nr = sys::getdents64(pfd, buf, sizeof(buf));
        if (nr <= 0) break;

        char* p = buf;
        while (p < buf + nr) {
            auto* e = reinterpret_cast<linux_dirent64*>(p);

            if (e->d_type == 4) {
                char* n = e->d_name;
                bool in = true;
                for (char* c = n; *c; c++) {
                    if (*c < '0' || *c > '9') { in = false; break; }
                }

                if (in && *n != '\0') {
                    int pid = 0;
                    for (char* c = n; *c; c++) pid = pid * 10 + (*c - '0');

                    char cp[256];
                    int ps = 0;
                    const char* ps_ = oxorany("/proc/");
                    while (*ps_) cp[ps++] = *ps_++;
                    for (char* c = n; *c; c++) cp[ps++] = *c;
                    const char* cs = oxorany("/cmdline");
                    while (*cs) cp[ps++] = *cs++;
                    cp[ps] = '\0';

                    int cfd = sys::openat(AT_FDCWD, cp, O_RDONLY);
                    wipe(cp, sizeof(cp));
                    if (cfd >= 0) {
                        char cmd[256] = {0};
                        long b = sys::read(cfd, cmd, sizeof(cmd) - 1);
                        sys::close(cfd);

                        if (b > 0) {
                            cmd[b] = '\0';
                            if (strstr(cmd, oxorany("com.dts.freefiremax"))) {
                                found = pid;
                            }
                            wipe(cmd, sizeof(cmd));
                        }
                    }
                    if (found > 0) break;
                }
            }

            p += e->d_reclen;
        }
        if (found > 0) break;
    }

    sys::close(pfd);
    wipe(buf, sizeof(buf));

    if (found > 0) {
        proc::pid_cache = found;
        kpm.initialize(found);
    }
    return found;
}

// ============================================================================
// MODULE BASE RESOLVER (KPM VERIFIED ELF & GAMEFACADE SCANNER)
// ============================================================================

inline uint64_t get_lib() {
    if (proc::pid <= 0) return 0;
    kpm.initialize(proc::pid);

    char mp[64];
    int ps = 0;
    const char* ps_ = oxorany("/proc/");
    while (*ps_) mp[ps++] = *ps_++;

    int pid = proc::pid;
    char pds[16];
    int pl = 0;
    int tm = pid;
    while (tm > 0) { pds[pl++] = '0' + (tm % 10); tm /= 10; }
    for (int i = pl - 1; i >= 0; i--) mp[ps++] = pds[i];

    const char* ms = oxorany("/maps");
    while (*ms) mp[ps++] = *ms++;
    mp[ps] = '\0';

    int mfd = sys::openat(AT_FDCWD, mp, O_RDONLY);
    wipe(mp,  sizeof(mp));
    wipe(pds, sizeof(pds));

    // Fallback: if openat fails, query module base through KPM maps resolver
    if (mfd < 0) {
        uintptr_t base = kpm.parse_maps_module_base(oxorany("libil2cpp.so"));
        if (base > 0) return static_cast<uint64_t>(base);
        return 0;
    }

    char buf[8192];
    long tr = 0;
    uint64_t candidates[8] = {0};
    int candCount = 0;
    uint64_t firstMatch = 0;

    while (true) {
        long nr = sys::read(mfd, buf + tr, sizeof(buf) - tr - 1);
        if (nr <= 0) break;
        tr += nr;
        buf[tr] = '\0';

        char* ls = buf;
        while (true) {
            char* le = ls;
            while (*le && *le != '\n') le++;
            if (*le == '\0') break;

            *le = '\0';

            uint64_t st = 0, en = 0;
            char* pt = ls;

            while (*pt && ((*pt >= '0' && *pt <= '9') || (*pt >= 'a' && *pt <= 'f') || (*pt >= 'A' && *pt <= 'F'))) {
                uint64_t v = (*pt <= '9') ? (*pt - '0') : ((*pt >= 'a') ? (*pt - 'a' + 10) : (*pt - 'A' + 10));
                st = st * 16 + v;
                pt++;
            }
            if (*pt != '-') goto nl;
            pt++;
            while (*pt && ((*pt >= '0' && *pt <= '9') || (*pt >= 'a' && *pt <= 'f') || (*pt >= 'A' && *pt <= 'F'))) {
                uint64_t v = (*pt <= '9') ? (*pt - '0') : ((*pt >= 'a') ? (*pt - 'a' + 10) : (*pt - 'A' + 10));
                en = en * 16 + v;
                pt++;
            }
            while (*pt == ' ') pt++;

            if (pt[0] == 'r' && pt[3] == 'p') {
                while (*pt == ' ') pt++;
                while (*pt && *pt != '/') pt++;

                if (*pt == '/') {
                    const char* lu = oxorany("libil2cpp.so");
                    int ll = 0;
                    while (lu[ll]) ll++;

                    char* ph = pt;
                    while (*ph) {
                        bool mt = true;
                        for (int i = 0; i < ll && ph[i]; i++) {
                            if (ph[i] != lu[i]) { mt = false; break; }
                        }
                        if (mt) {
                            if (firstMatch == 0) firstMatch = st;

                            // Verify ELF magic header via KPM: 0x7F 'E' 'L' 'F' (0x464C457F)
                            uint32_t magic = rpm<uint32_t>(st);
                            if (magic == 0x464C457F) {
                                if (candCount < 8) {
                                    candidates[candCount++] = st;
                                }
                            }
                            break;
                        }
                        ph++;
                    }
                }
            }

        nl:
            ls = le + 1;
        }

        if (ls < buf + tr) {
            int rm = (buf + tr) - ls;
            for (int i = 0; i < rm; i++) buf[i] = ls[i];
            tr = rm;
        } else {
            tr = 0;
        }
    }

    sys::close(mfd);
    wipe(buf, sizeof(buf));

    // Candidate validation via KPM:
    // Test if candidate base resolves GameFacade static pointer (> 0x10000000)
    for (int i = 0; i < candCount; i++) {
        uint64_t cand = candidates[i];
        if (cand == 0) continue;
        uint64_t gf1 = rpm<uint64_t>(cand + Offsets::Base::GameFacade);
        if (gf1 > 0x10000000) {
            return cand;
        }
        uint64_t gf2 = rpm<uint64_t>(cand + Offsets::Base::GameFacade_Alt);
        if (gf2 > 0x10000000) {
            return cand;
        }
    }

    // Fallback 1: Return first verified ELF segment
    if (candCount > 0) {
        return candidates[0];
    }

    // Fallback 2: Return first readable libil2cpp segment
    if (firstMatch > 0) {
        return firstMatch;
    }

    // Fallback 3: Return base from KPM maps resolver
    return static_cast<uint64_t>(kpm.parse_maps_module_base(oxorany("libil2cpp.so")));
}
