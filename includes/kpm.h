#pragma once
// ============================================================================
// KPM (KERNEL PATCH MODULE) ENGINE & LIVE TELEMETRY
// ============================================================================
// Sourced from: pewpew (C:\Users\Sajala\Documents\mods\refer\pewpew\includes\driver.h)
//
// Dual-engine KPM driver supporting TB KPM (0x63687501) and Neo KPM (0x4E41)
// with live diagnostics, manual engine selection, and safe syscall fallback.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifndef __NR_prctl
#if defined(__aarch64__)
#define __NR_prctl 167
#elif defined(__arm__)
#define __NR_prctl 172
#elif defined(__x86_64__)
#define __NR_prctl 157
#elif defined(__i386__)
#define __NR_prctl 172
#endif
#endif

#ifndef __NR_process_vm_readv
#if defined(__aarch64__)
#define __NR_process_vm_readv 270
#define __NR_process_vm_writev 271
#elif defined(__x86_64__)
#define __NR_process_vm_readv 310
#define __NR_process_vm_writev 311
#endif
#endif

// ==================== TB KPM DEFINITIONS ====================
#define PRCTL_MEM_READ  0x63687501
#define PRCTL_MEM_WRITE 0x63687502

struct mem_operation {
    pid_t    target_pid;
    uint64_t addr;
    void    *buffer;
    uint64_t size;
};

// ==================== NEO KPM DEFINITIONS ====================
#define NA_PRCTL_MAGIC      0x4E41
#define NA_CMD_READ_MEM     1
#define NA_CMD_WRITE_MEM    2
#define NA_CMD_GET_PID      5
#define NA_MAX_RW_SIZE      4096
#define NA_MAX_PKG_NAME     256

struct na_cmd {
    uint32_t op;
    uint32_t pid;
    uint64_t addr;
    uint32_t size;
    int32_t  result;
    int32_t  screen_w;
    int32_t  screen_h;
    char     pkg[NA_MAX_PKG_NAME];
    uint8_t  data[NA_MAX_RW_SIZE];
};

enum EKpmEngine {
    KPM_ENGINE_AUTO = 0,
    KPM_ENGINE_TB,      // TB KPM (0x63687501)
    KPM_ENGINE_NEO,     // Neo KPM (0x4E41)
    KPM_ENGINE_SYSCALL  // Direct Syscall 270 fallback
};

struct KpmDiagnostics {
    int  selected_engine = KPM_ENGINE_AUTO;
    int  active_engine   = KPM_ENGINE_AUTO;
    bool is_connected    = false;
    bool auto_fallback   = true;  // Safe fallback if KPM is not loaded

    // TB KPM Probe & Test
    int  tb_ret          = -999;
    int  tb_errno        = 0;
    bool tb_works        = false;

    // Neo KPM Probe & Test
    int  neo_ret         = -999;
    int  neo_errno       = 0;
    int  neo_result      = -999;
    bool neo_works       = false;

    // Syscall Test
    int  syscall_ret     = -999;
    bool syscall_works   = false;

    // Counters
    uint64_t total_reads  = 0;
    uint64_t failed_reads = 0;
    uint32_t last_magic   = 0;
    char status_str[128]  = "Initializing...";
};

class c_kpm {
private:
    pid_t pid = -1;

public:
    KpmDiagnostics diag;

    c_kpm() : pid(-1) {}

    // ------------------------------------------------------------------------
    // TB KPM READ & WRITE (Direct Pointer Kernel Copy)
    // ------------------------------------------------------------------------
    bool tb_kpm_read(pid_t target_pid, uintptr_t addr, void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { target_pid, (uint64_t)addr, buffer, (uint64_t)size };
        errno = 0;
        long r = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        diag.tb_ret = (int)r;
        diag.tb_errno = errno;
        if (r >= 0) {
            diag.tb_works = true;
            return true;
        }
        // Fallback try standard libc prctl wrapper
        errno = 0;
        r = prctl(PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        diag.tb_ret = (int)r;
        diag.tb_errno = errno;
        if (r >= 0) {
            diag.tb_works = true;
            return true;
        }
        return false;
    }

    bool tb_kpm_write(pid_t target_pid, uintptr_t addr, const void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { target_pid, (uint64_t)addr, const_cast<void*>(buffer), (uint64_t)size };
        long r = syscall(__NR_prctl, PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0);
        if (r >= 0) return true;
        return (prctl(PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0) >= 0);
    }

    // ------------------------------------------------------------------------
    // NEO KPM READ & WRITE (Chunked Kernel Copy with Dual Calling Variants)
    // ------------------------------------------------------------------------
    bool neo_kpm_read(pid_t target_pid, uintptr_t addr, void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        uint8_t *ptr = reinterpret_cast<uint8_t*>(buffer);
        size_t remaining = size;
        uintptr_t curr_addr = addr;
        while (remaining > 0) {
            size_t chunk = (remaining > NA_MAX_RW_SIZE) ? NA_MAX_RW_SIZE : remaining;
            na_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.op   = NA_CMD_READ_MEM;
            cmd.pid  = (uint32_t)target_pid;
            cmd.addr = (uint64_t)curr_addr;
            cmd.size = (uint32_t)chunk;

            // Variant 1: pewpew style (NA_CMD_READ_MEM in arg2, &cmd in arg3)
            errno = 0;
            long r = syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_READ_MEM, (unsigned long)&cmd, 0, 0);
            diag.neo_ret = (int)r;
            diag.neo_errno = errno;
            diag.neo_result = cmd.result;
            if (r == 0 && cmd.result == 0) {
                memcpy(ptr, cmd.data, chunk);
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
                diag.neo_works = true;
                continue;
            }

            // Variant 2: Standard Neo KPM style (&cmd in arg2)
            errno = 0;
            r = syscall(__NR_prctl, NA_PRCTL_MAGIC, (unsigned long)&cmd, 0, 0, 0);
            diag.neo_ret = (int)r;
            diag.neo_errno = errno;
            diag.neo_result = cmd.result;
            if (r == 0 && cmd.result == 0) {
                memcpy(ptr, cmd.data, chunk);
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
                diag.neo_works = true;
                continue;
            }
            return false;
        }
        return true;
    }

    bool neo_kpm_write(pid_t target_pid, uintptr_t addr, const void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        const uint8_t *ptr = reinterpret_cast<const uint8_t*>(buffer);
        size_t remaining = size;
        uintptr_t curr_addr = addr;
        while (remaining > 0) {
            size_t chunk = (remaining > NA_MAX_RW_SIZE) ? NA_MAX_RW_SIZE : remaining;
            na_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.op   = NA_CMD_WRITE_MEM;
            cmd.pid  = (uint32_t)target_pid;
            cmd.addr = (uint64_t)curr_addr;
            cmd.size = (uint32_t)chunk;
            memcpy(cmd.data, ptr, chunk);

            long r = syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_WRITE_MEM, (unsigned long)&cmd, 0, 0);
            if (r == 0 && cmd.result == 0) {
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
                continue;
            }

            r = syscall(__NR_prctl, NA_PRCTL_MAGIC, (unsigned long)&cmd, 0, 0, 0);
            if (r == 0 && cmd.result == 0) {
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
                continue;
            }
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // DIRECT SYSCALL FALLBACK (Zero-Crash Safety Engine)
    // ------------------------------------------------------------------------
    bool syscall_read(pid_t target_pid, uintptr_t addr, void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct iovec loc = { buffer, size };
        struct iovec rem = { reinterpret_cast<void*>(addr), size };
        ssize_t r = syscall(__NR_process_vm_readv, target_pid, &loc, 1, &rem, 1, 0);
        diag.syscall_ret = (int)r;
        if (r == static_cast<ssize_t>(size)) {
            diag.syscall_works = true;
            return true;
        }
        return false;
    }

    bool syscall_write(pid_t target_pid, uintptr_t addr, const void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct iovec loc = { const_cast<void*>(buffer), size };
        struct iovec rem = { reinterpret_cast<void*>(addr), size };
        return syscall(__NR_process_vm_writev, target_pid, &loc, 1, &rem, 1, 0) == static_cast<ssize_t>(size);
    }

    // ------------------------------------------------------------------------
    // CONNECTION PROBE & LIVE DIAGNOSTICS TEST
    // ------------------------------------------------------------------------
    void test_connection(pid_t target_pid = -1) {
        if (target_pid <= 0) target_pid = this->pid;
        if (target_pid <= 0) target_pid = getpid();

        // 1. Test TB KPM with self
        volatile uint32_t test_val = 0x5A5A1234;
        uint32_t read_buf = 0;
        struct mem_operation op = { getpid(), reinterpret_cast<uint64_t>(&test_val), &read_buf, sizeof(test_val) };
        errno = 0;
        long r = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        diag.tb_ret = (int)r;
        diag.tb_errno = errno;
        diag.tb_works = (r >= 0 && read_buf == test_val);

        // If target pid is available, also test real target memory
        if (!diag.tb_works && target_pid > 0 && target_pid != getpid()) {
            uint32_t dummy = 0;
            struct mem_operation op2 = { target_pid, 0x10000, &dummy, sizeof(dummy) };
            errno = 0;
            long r2 = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op2, 0, 0, 0);
            diag.tb_ret = (int)r2;
            diag.tb_errno = errno;
            if (r2 >= 0) diag.tb_works = true;
        }

        // 2. Test Neo KPM
        na_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = NA_CMD_GET_PID;
        strncpy(cmd.pkg, "com.dts.freefiremax", sizeof(cmd.pkg) - 1);
        errno = 0;
        r = syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0);
        diag.neo_ret = (int)r;
        diag.neo_errno = errno;
        diag.neo_result = cmd.result;
        diag.neo_works = (r == 0 && (cmd.result == 0 || errno != EINVAL));

        // 3. Test Syscall
        if (target_pid > 0 && target_pid != getpid()) {
            uint32_t dummy = 0;
            struct iovec loc = { &dummy, sizeof(dummy) };
            struct iovec rem = { reinterpret_cast<void*>(0x10000), sizeof(dummy) };
            ssize_t sr = syscall(__NR_process_vm_readv, target_pid, &loc, 1, &rem, 1, 0);
            diag.syscall_ret = (int)sr;
            diag.syscall_works = (sr > 0 || (sr < 0 && errno == EFAULT));
        }

        // 4. Update status summary string
        if (diag.tb_works) {
            snprintf(diag.status_str, sizeof(diag.status_str), "TB KPM Active (prctl 0x63687501)");
            diag.is_connected = true;
            if (diag.selected_engine == KPM_ENGINE_AUTO) diag.active_engine = KPM_ENGINE_TB;
        } else if (diag.neo_works) {
            snprintf(diag.status_str, sizeof(diag.status_str), "Neo KPM Active (prctl 0x4E41)");
            diag.is_connected = true;
            if (diag.selected_engine == KPM_ENGINE_AUTO) diag.active_engine = KPM_ENGINE_NEO;
        } else {
            snprintf(diag.status_str, sizeof(diag.status_str), "KPM Not Responding (TB:%d, Neo:%d)", diag.tb_errno, diag.neo_errno);
            diag.is_connected = false;
            if (diag.auto_fallback) diag.active_engine = KPM_ENGINE_SYSCALL;
        }
    }

    bool init(bool verbose = true) {
        test_connection(this->pid);
        if (verbose) {
            printf("[KPM] %s | Active Engine: %s\n", diag.status_str, get_active_engine_name());
            fflush(stdout);
        }
        return diag.is_connected;
    }

    void initialize(pid_t target_pid) {
        this->pid = target_pid;
    }

    void set_pid(pid_t target_pid) {
        this->pid = target_pid;
    }

    pid_t get_pid() const {
        return this->pid;
    }

    bool is_active() const {
        return (diag.is_connected || (diag.auto_fallback && diag.active_engine == KPM_ENGINE_SYSCALL));
    }

    const char* get_active_engine_name() const {
        switch (diag.active_engine) {
            case KPM_ENGINE_TB:      return "TB KPM (0x63687501)";
            case KPM_ENGINE_NEO:     return "Neo KPM (0x4E41)";
            case KPM_ENGINE_SYSCALL: return "Syscall 270 (Fallback)";
            default:                 return "None / Disconnected";
        }
    }

    const char* get_selected_mode_name() const {
        switch (diag.selected_engine) {
            case KPM_ENGINE_AUTO:    return "AUTO";
            case KPM_ENGINE_TB:      return "TB KPM";
            case KPM_ENGINE_NEO:     return "Neo KPM";
            case KPM_ENGINE_SYSCALL: return "Syscall Only";
            default:                 return "AUTO";
        }
    }

    // ------------------------------------------------------------------------
    // UNIFIED SMART MEMORY READ
    // ------------------------------------------------------------------------
    bool read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_reads++;

        int mode = diag.selected_engine;
        if (mode == KPM_ENGINE_TB) {
            if (tb_kpm_read(this->pid, addr, buffer, size)) {
                diag.active_engine = KPM_ENGINE_TB;
                diag.is_connected = true;
                return true;
            }
        } else if (mode == KPM_ENGINE_NEO) {
            if (neo_kpm_read(this->pid, addr, buffer, size)) {
                diag.active_engine = KPM_ENGINE_NEO;
                diag.is_connected = true;
                return true;
            }
        } else if (mode == KPM_ENGINE_SYSCALL) {
            if (syscall_read(this->pid, addr, buffer, size)) {
                diag.active_engine = KPM_ENGINE_SYSCALL;
                diag.is_connected = true;
                return true;
            }
        } else {
            // AUTO MODE:
            // 1. Try TB KPM first
            if (tb_kpm_read(this->pid, addr, buffer, size)) {
                diag.active_engine = KPM_ENGINE_TB;
                diag.is_connected = true;
                return true;
            }
            // 2. Try Neo KPM second
            if (neo_kpm_read(this->pid, addr, buffer, size)) {
                diag.active_engine = KPM_ENGINE_NEO;
                diag.is_connected = true;
                return true;
            }
            // 3. Fallback to Syscall if auto_fallback is enabled
            if (diag.auto_fallback) {
                if (syscall_read(this->pid, addr, buffer, size)) {
                    diag.active_engine = KPM_ENGINE_SYSCALL;
                    diag.is_connected = true;
                    return true;
                }
            }
        }

        diag.failed_reads++;
        return false;
    }

    // ------------------------------------------------------------------------
    // UNIFIED SMART MEMORY WRITE
    // ------------------------------------------------------------------------
    bool write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        int mode = diag.selected_engine;
        if (mode == KPM_ENGINE_AUTO) mode = diag.active_engine;

        if (mode == KPM_ENGINE_TB) {
            return tb_kpm_write(this->pid, addr, buffer, size);
        } else if (mode == KPM_ENGINE_NEO) {
            return neo_kpm_write(this->pid, addr, buffer, size);
        } else if (mode == KPM_ENGINE_SYSCALL || diag.auto_fallback) {
            return syscall_write(this->pid, addr, buffer, size);
        }
        return false;
    }

    template <typename T>
    T read(uintptr_t addr) {
        T res{};
        this->read(addr, &res, sizeof(T));
        return res;
    }

    template <typename T>
    bool write(uintptr_t addr, const T& value) {
        return this->write(addr, &value, sizeof(T));
    }

    // Direct Neo KPM PID Query
    pid_t get_pid_neo(const char* pkg_name) {
        if (!pkg_name) return -1;
        na_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = NA_CMD_GET_PID;
        strncpy(cmd.pkg, pkg_name, sizeof(cmd.pkg) - 1);
        if (syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
            return static_cast<pid_t>(cmd.pid);
        }
        if (syscall(__NR_prctl, NA_PRCTL_MAGIC, (unsigned long)&cmd, 0, 0, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
            return static_cast<pid_t>(cmd.pid);
        }
        return -1;
    }

    // Module base resolver via /proc/maps
    uintptr_t parse_maps_module_base(const char *name) {
        if (this->pid <= 0) return 0;
        uintptr_t first_base = 0;
        uintptr_t valid_elf_base = 0;

        char maps_path[64];
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", this->pid);
        FILE *f = fopen(maps_path, "r");
        if (f != nullptr) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, name)) {
                    uint64_t start = 0, end = 0;
                    if (sscanf(line, "%lx-%lx", &start, &end) == 2 && start > 0) {
                        if (first_base == 0) first_base = static_cast<uintptr_t>(start);
                        uint32_t magic = 0;
                        if (this->read(static_cast<uintptr_t>(start), &magic, sizeof(magic)) && magic == 0x464C457F) {
                            valid_elf_base = static_cast<uintptr_t>(start);
                            diag.last_magic = magic;
                            break;
                        }
                    }
                }
            }
            fclose(f);
        }

        if (valid_elf_base != 0) {
            return valid_elf_base;
        }

        // Fallback: su -c cat in case SELinux restricted direct fopen
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "su -c 'cat /proc/%d/maps | grep %s'", this->pid, name);
        FILE *pf = popen(cmd, "r");
        if (pf != nullptr) {
            char line[1024];
            while (fgets(line, sizeof(line), pf)) {
                if (strstr(line, name)) {
                    uint64_t start = 0, end = 0;
                    if (sscanf(line, "%lx-%lx", &start, &end) == 2 && start > 0) {
                        if (first_base == 0) first_base = static_cast<uintptr_t>(start);
                        uint32_t magic = 0;
                        if (this->read(static_cast<uintptr_t>(start), &magic, sizeof(magic)) && magic == 0x464C457F) {
                            valid_elf_base = static_cast<uintptr_t>(start);
                            diag.last_magic = magic;
                            break;
                        }
                    }
                }
            }
            pclose(pf);
        }

        if (valid_elf_base != 0) {
            return valid_elf_base;
        }

        return first_base;
    }

    uintptr_t getModuleBase(const char* name) {
        return parse_maps_module_base(name);
    }
};

inline c_kpm kpm;
