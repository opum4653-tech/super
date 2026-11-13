#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include "../protect/oxorany.hpp"

namespace proc {
    inline int      pid = -1;
    inline uint64_t lib = 0;
    // Last target pid that was confirmed by name. get_pid() re-checks this one
    // entry instead of re-walking all of /proc on every poll.
    inline int      pid_cache = 0;
    // proc::pm removido — pagemap FD ficava aberto e nunca era lido,
    // deixando um handle pro /proc/<pid>/pagemap detectável em /proc/self/fd.
    inline int      pm  = -1;   // mantido só pra código legado, sempre = -1
}

namespace sys {
    #if defined(__aarch64__)
    static constexpr long SYS_openat     = 56;
    static constexpr long SYS_close      = 57;
    static constexpr long SYS_read       = 63;
    static constexpr long SYS_getdents64 = 61;
    static constexpr long SYS_vm_read    = 270;
    static constexpr long SYS_vm_write   = 271;
    #elif defined(__x86_64__)
    static constexpr long SYS_openat     = 257;
    static constexpr long SYS_close      = 3;
    static constexpr long SYS_read       = 0;
    static constexpr long SYS_getdents64 = 217;
    static constexpr long SYS_vm_read    = 310;
    static constexpr long SYS_vm_write   = 311;
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
    static inline long vm_read (int pid, struct iovec* l, unsigned long lc, struct iovec* r, unsigned long rc) {
        return sc6(SYS_vm_read, pid, reinterpret_cast<long>(l), lc, reinterpret_cast<long>(r), rc, 0);
    }
    static inline long vm_write(int pid, struct iovec* l, unsigned long lc, struct iovec* r, unsigned long rc) {
        return sc6(SYS_vm_write, pid, reinterpret_cast<long>(l), lc, reinterpret_cast<long>(r), rc, 0);
    }
}

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char d_name[];
};

// Wipe stack buffers depois de usar — nada de path linger em snapshots de memória.
static inline void wipe(void* p, size_t n) {
    volatile uint8_t* v = reinterpret_cast<volatile uint8_t*>(p);
    for (size_t i = 0; i < n; i++) v[i] = 0;
}

inline bool is_valid_ptr(uint64_t a) {
    return (a >= 0x10000000ULL && a <= 0x7FFFFFFFFFFFULL);
}

inline bool addr_valid(uint64_t a) { return is_valid_ptr(a); }

inline bool mem_read(uint64_t a, void* b, size_t l) {
    if (proc::pid <= 0 || a < 0x10000 || l == 0 || b == nullptr) return false;
    struct iovec loc[1], rem[1];
    loc[0].iov_base = b;
    loc[0].iov_len  = l;
    rem[0].iov_base = reinterpret_cast<void*>(a);
    rem[0].iov_len  = l;
    ssize_t r = sys::vm_read(proc::pid, loc, 1, rem, 1);
    if (r != static_cast<long>(l)) {
        memset(b, 0, l);
        return false;
    }
    return true;
}

inline bool mem_write(uint64_t a, const void* b, size_t l) {
    if (proc::pid <= 0 || !is_valid_ptr(a) || l == 0 || b == nullptr) return false;
    struct iovec loc[1], rem[1];
    loc[0].iov_base = const_cast<void*>(b);
    loc[0].iov_len  = l;
    rem[0].iov_base = reinterpret_cast<void*>(a);
    rem[0].iov_len  = l;
    return sys::vm_write(proc::pid, loc, 1, rem, 1) == static_cast<long>(l);
}

template<typename T> inline T rpm(uint64_t a) {
    T v{};
    mem_read(a, &v, sizeof(T));
    return v;
}

template<typename T> inline bool wpm(uint64_t a, const T& d) {
    return mem_write(a, &d, sizeof(T));
}

// Confirm one cached pid still belongs to the target. One openat + one read,
// versus a getdents64 sweep of every entry in /proc.
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
    // Fast path — the previously confirmed target, still alive and still ours.
    // PID reuse is caught because the name has to match again, not just the number.
    if (proc::pid_cache > 0 && pid_alive(proc::pid_cache)) {
        return proc::pid_cache;
    }
    proc::pid_cache = 0;

    // Cooldown after a failed sweep. Reading every /proc/<pid>/cmdline on a timer
    // is a signature of its own, and the target is not going to materialise within
    // a couple of seconds of the last time we looked. The interval drifts so the
    // scan does not land on a fixed period.
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
                    wipe(cp, sizeof(cp));                       // scrub path
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
                            wipe(cmd, sizeof(cmd));             // scrub cmdline
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

inline uint64_t get_lib() {
    if (proc::pid <= 0) return 0;

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
    if (mfd < 0) return 0;

    char buf[8192];
    long tr = 0;
    uint64_t bs = 0;

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
                        if (mt && (en - st) < 0x5100000) {
                            bs = st;
                            sys::close(mfd);
                            wipe(buf, sizeof(buf));
                            return bs;
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
    return bs;
}

