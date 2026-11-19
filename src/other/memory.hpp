#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include "../protect/oxorany.hpp"
#include "driver.h"

namespace proc {
    inline int      pid = -1;
    inline uint64_t lib = 0;
    // Last target pid that was confirmed by name. get_pid() re-checks this one
    // entry instead of re-walking all of /proc on every poll.
    inline int      pid_cache = 0;
    inline int      pm  = -1;   // Legacy compatibility
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

inline bool addr_valid(uint64_t a) { return a >= 0x1000; }

// ==================== PURE KPM KERNEL MEMORY BRIDGE ====================
inline bool mem_read(uint64_t a, void* b, size_t l) {
    if (proc::pid <= 0 || a < 0x1000 || l == 0 || b == nullptr) return false;
    if (!driver) return false;
    driver->initialize(proc::pid);
    return driver->read(a, b, l);
}

inline bool mem_write(uint64_t a, const void* b, size_t l) {
    if (proc::pid <= 0 || a == 0 || l == 0 || b == nullptr) return false;
    if (!driver) return false;
    driver->initialize(proc::pid);
    return driver->write(a, const_cast<void*>(b), l);
}

template<typename T> inline T rpm(uint64_t a) {
    T v{};
    mem_read(a, &v, sizeof(T));
    return v;
}

template<typename T> inline bool wpm(uint64_t a, const T& d) {
    return mem_write(a, &d, sizeof(T));
}

// Single-pass chunk reader for high-throughput batch reading
struct MemChunk {
    uint64_t base;
    size_t   size;
    uint8_t  buf[0x2400];
    bool     ok;

    inline bool load(uint64_t addr, size_t len) {
        if (len > sizeof(buf)) len = sizeof(buf);
        base = addr;
        size = len;
        ok   = mem_read(addr, buf, len);
        return ok;
    }

    template<typename T>
    inline T at(size_t off) const {
        T v{};
        if (!ok || off + sizeof(T) > size) return v;
        memcpy(&v, buf + off, sizeof(T));
        return v;
    }
};

// Confirm cached PID is still alive and matches the target package
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
        ok = (strstr(cmd, oxorany("com.dts.freefireth")) != nullptr)
          || (strstr(cmd, oxorany("com.dts.freefiremax")) != nullptr);
        wipe(cmd, sizeof(cmd));
    }
    return ok;
}

inline int get_pid() {
    // 1. Neo KPM Kernel-level PID extraction (zero /proc footprint)
    if (driver) {
        int kpm_pid = driver->get_pid_from_kpm("com.dts.freefiremax");
        if (kpm_pid <= 0) {
            kpm_pid = driver->get_pid_from_kpm("com.dts.freefireth");
        }
        if (kpm_pid > 0) {
            proc::pid_cache = kpm_pid;
            return kpm_pid;
        }
    }

    // 2. Fast path: cached PID validation
    if (proc::pid_cache > 0 && pid_alive(proc::pid_cache)) {
        return proc::pid_cache;
    }
    proc::pid_cache = 0;

    // 3. Cooldown after failed sweep to prevent timer fingerprinting
    static long long s_next_scan_ms = 0;
    static unsigned  s_scan_seq     = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (now_ms < s_next_scan_ms) return -1;

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
                            if (strstr(cmd, oxorany("com.dts.freefireth")) ||
                                strstr(cmd, oxorany("com.dts.freefiremax"))) {
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
    } else {
        s_next_scan_ms = now_ms + 1800 + (long long)((s_scan_seq++ * 137u) % 900u);
    }
    return found;
}

// Module base address resolution via KPM Kernel driver
inline uint64_t get_lib() {
    if (proc::pid <= 0) return 0;
    if (!driver) return 0;
    driver->initialize(proc::pid);
    return driver->getModuleBase("libil2cpp.so");
}

inline void init_mem() { /* Legacy compatibility */ }

namespace game {
    inline int  tick        = 0;
    inline int  jitter_next = 47;

    inline bool valid() {
        if (proc::pid <= 0) {
            proc::pid = get_pid();
            if (proc::pid <= 0) return false;
        }

        if (proc::lib == 0) {
            proc::lib = get_lib();
            return false;
        }

        tick++;
        if (tick >= jitter_next) {
            int p = get_pid();
            if (p != proc::pid) {
                proc::pid = p;
                if (proc::pid > 0 && driver) driver->initialize(proc::pid);
                proc::lib = 0;
                tick = 0;
                jitter_next = 47;
                return false;
            }
            tick = 0;
            static uint32_t s = 0xC5A1B4E7u;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            jitter_next = 40 + (int)(s % 31u);
        }
        return true;
    }

    inline void init() {
        proc::pid = get_pid();
        if (proc::pid > 0) {
            if (driver) driver->initialize(proc::pid);
            proc::lib = get_lib();
        }
    }
}
