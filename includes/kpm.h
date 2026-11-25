#pragma once
// ============================================================================
// KPM (KERNEL PATCH MODULE) ENGINE & LIVE TELEMETRY (KPM-ONLY)
// ============================================================================
// Supports TB KPM (0x63687501) and Neo KPM (0x4E41) via prctl.
// Direct Syscall 270 (process_vm_readv / process_vm_writev) is disabled.
// ============================================================================

#include <atomic>
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
// #include <sys/uio.h> // Syscall 270 disabled

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
    KPM_ENGINE_SYSCALL  // Disabled
};

struct KpmDiagnostics {
    std::atomic<int>  selected_engine{KPM_ENGINE_AUTO};
    std::atomic<int>  active_engine{KPM_ENGINE_AUTO};
    std::atomic<bool> is_connected{false};

    // TB KPM Probe
    std::atomic<int>  tb_ret{-999};
    std::atomic<int>  tb_errno{0};
    std::atomic<bool> tb_works{false};

    // Neo KPM Probe
    std::atomic<int>  neo_ret{-999};
    std::atomic<int>  neo_errno{0};
    std::atomic<int>  neo_result{-999};
    std::atomic<bool> neo_works{false};

    // Syscall 270 (Disabled)
    std::atomic<int>  syscall_ret{-1};
    std::atomic<bool> syscall_works{false};

    // Counters
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> failed_reads{0};
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> failed_writes{0};
    std::atomic<uint64_t> verified_writes{0};
    std::atomic<uint64_t> failed_verified_writes{0};
    std::atomic<uint64_t> rollback_ok{0};
    std::atomic<uint64_t> rollback_failed{0};
    std::atomic<uint32_t> last_magic{0};
    char status_str[128] = "Initializing...";
};

class c_kpm {
private:
    pid_t pid = -1;

public:
    KpmDiagnostics diag;

    c_kpm() : pid(-1) {}

    bool tb_kpm_read(pid_t target_pid, uintptr_t addr, void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { target_pid, (uint64_t)addr, buffer, (uint64_t)size };
        errno = 0;
        long r = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        diag.tb_ret = (int)r;
        diag.tb_errno = errno;
        if (r >= 0) return true;
        errno = 0;
        r = prctl(PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        diag.tb_ret = (int)r;
        diag.tb_errno = errno;
        return (r >= 0);
    }

    bool tb_kpm_write(pid_t target_pid, uintptr_t addr, const void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { target_pid, (uint64_t)addr, const_cast<void*>(buffer), (uint64_t)size };
        long r = syscall(__NR_prctl, PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0);
        if (r >= 0) return true;
        return (prctl(PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0) >= 0);
    }

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

            memset(&cmd, 0, sizeof(cmd));
            cmd.op   = NA_CMD_READ_MEM;
            cmd.pid  = (uint32_t)target_pid;
            cmd.addr = (uint64_t)curr_addr;
            cmd.size = (uint32_t)chunk;
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

            memset(&cmd, 0, sizeof(cmd));
            cmd.op   = NA_CMD_WRITE_MEM;
            cmd.pid  = (uint32_t)target_pid;
            cmd.addr = (uint64_t)curr_addr;
            cmd.size = (uint32_t)chunk;
            memcpy(cmd.data, ptr, chunk);
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

    // Syscall 270 (External work) disabled — KPM only
    // bool syscall_read(pid_t target_pid, uintptr_t addr, void *buffer, size_t size) { ... }
    // bool syscall_write(pid_t target_pid, uintptr_t addr, const void *buffer, size_t size) { ... }

    void test_connection(pid_t target_pid = -1) {
        if (target_pid <= 0) target_pid = this->pid;
        if (target_pid <= 0) target_pid = getpid();
        if (target_pid > 0 && target_pid != getpid() && this->pid != target_pid) {
            initialize(target_pid);
        }

        // 1. Test TB KPM
        volatile uint32_t test_val = 0x5A5A1234;
        uint32_t read_buf = 0;
        struct mem_operation op = { getpid(), reinterpret_cast<uint64_t>(&test_val), &read_buf, sizeof(test_val) };
        errno = 0;
        long r = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        diag.tb_ret = (int)r;
        diag.tb_errno = errno;
        diag.tb_works = (r >= 0 && read_buf == test_val);

        if (!diag.tb_works && target_pid > 0 && target_pid != getpid()) {
            uintptr_t base = parse_maps_module_base("libil2cpp.so");
            if (base != 0) {
                uint32_t magic = 0;
                struct mem_operation op2 = { target_pid, (uint64_t)base, &magic, sizeof(magic) };
                errno = 0;
                long r2 = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op2, 0, 0, 0);
                diag.tb_ret = (int)r2;
                diag.tb_errno = errno;
                diag.tb_works = (r2 >= 0 && magic == 0x464C457F);
                if (diag.tb_works) diag.last_magic = magic;
            }
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
        diag.neo_works = (r == 0 && cmd.result == 0 && cmd.pid > 0);

        // Syscall 270 disabled
        diag.syscall_ret = -1;
        diag.syscall_works = false;

        if (diag.tb_works) {
            snprintf(diag.status_str, sizeof(diag.status_str), "TB KPM Active");
            diag.is_connected = true;
            diag.active_engine = KPM_ENGINE_TB;
        } else if (diag.neo_works) {
            snprintf(diag.status_str, sizeof(diag.status_str), "Neo KPM Active");
            diag.is_connected = true;
            diag.active_engine = KPM_ENGINE_NEO;
        } else {
            snprintf(diag.status_str, sizeof(diag.status_str), "KPM Not Connected");
            diag.is_connected = false;
            diag.active_engine = KPM_ENGINE_AUTO;
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
        if (this->pid == target_pid) return;
        this->pid = target_pid;
        diag.active_engine = KPM_ENGINE_AUTO;
        diag.is_connected  = false;
        diag.tb_works      = false;
        diag.neo_works     = false;
        diag.syscall_works = false;
        diag.last_magic    = 0;
    }

    void set_pid(pid_t target_pid) {
        this->pid = target_pid;
    }

    pid_t get_pid() const {
        return this->pid;
    }

    bool is_active() const {
        return diag.is_connected.load();
    }

    const char* get_active_engine_name() const {
        switch (diag.active_engine.load()) {
            case KPM_ENGINE_TB:  return "TB KPM (0x63687501)";
            case KPM_ENGINE_NEO: return "Neo KPM (0x4E41)";
            default:             return "None / Disconnected";
        }
    }

    bool read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_reads++;

        if (tb_kpm_read(this->pid, addr, buffer, size)) {
            diag.active_engine = KPM_ENGINE_TB;
            diag.is_connected = true;
            return true;
        }
        if (neo_kpm_read(this->pid, addr, buffer, size)) {
            diag.active_engine = KPM_ENGINE_NEO;
            diag.is_connected = true;
            return true;
        }

        // Syscall 270 disabled (KPM only)
        // if (syscall_read(this->pid, addr, buffer, size)) return true;

        diag.failed_reads++;
        return false;
    }

    bool write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_writes++;

        if (tb_kpm_write(this->pid, addr, buffer, size)) {
            diag.active_engine = KPM_ENGINE_TB;
            diag.is_connected = true;
            return true;
        }
        if (neo_kpm_write(this->pid, addr, buffer, size)) {
            diag.active_engine = KPM_ENGINE_NEO;
            diag.is_connected = true;
            return true;
        }

        // Syscall 270 disabled (KPM only)
        // if (syscall_write(this->pid, addr, buffer, size)) return true;

        diag.failed_writes++;
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

    pid_t get_pid_neo(const char* pkg_name) {
        if (!pkg_name) return -1;
        na_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = NA_CMD_GET_PID;
        strncpy(cmd.pkg, pkg_name, sizeof(cmd.pkg) - 1);
        if (syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
            return static_cast<pid_t>(cmd.pid);
        }
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = NA_CMD_GET_PID;
        strncpy(cmd.pkg, pkg_name, sizeof(cmd.pkg) - 1);
        if (syscall(__NR_prctl, NA_PRCTL_MAGIC, (unsigned long)&cmd, 0, 0, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
            return static_cast<pid_t>(cmd.pid);
        }
        return -1;
    }

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
