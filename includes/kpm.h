#pragma once
// ============================================================================
// NINMODS KPM ENGINE & LIVE TELEMETRY (0x4E41)
// ============================================================================
// Works exclusively with NinMods.kpm (NA_PRCTL_MAGIC = 0x4E41).
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

// ==================== NINMODS KPM DEFINITIONS ====================
#define NA_PRCTL_MAGIC      0x4E41
#define NA_PRCTL_AUTH       0x4E494E4D4F445331ULL
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

struct KpmDiagnostics {
    std::atomic<bool> is_connected{false};

    // NinMods KPM Probe
    std::atomic<int>  neo_ret{-999};
    std::atomic<int>  neo_errno{0};
    std::atomic<int>  neo_result{-999};
    std::atomic<bool> neo_works{false};

    // Counters
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> failed_reads{0};
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> failed_writes{0};
    std::atomic<uint32_t> last_magic{0};
    char status_str[128] = "Initializing...";
};

class c_kpm {
private:
    pid_t pid = -1;

public:
    KpmDiagnostics diag;

    c_kpm() : pid(-1) {}

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
            long r = syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_READ_MEM, (unsigned long)&cmd, NA_PRCTL_AUTH, 0);
            diag.neo_ret = (int)r;
            diag.neo_errno = errno;
            diag.neo_result = cmd.result;
            if (r == 0 && cmd.result == 0) {
                memcpy(ptr, cmd.data, chunk);
                memset(&cmd, 0, sizeof(cmd));
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
                diag.neo_works = true;
                continue;
            }
            memset(&cmd, 0, sizeof(cmd));
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

            long r = syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_WRITE_MEM, (unsigned long)&cmd, NA_PRCTL_AUTH, 0);
            int32_t res = cmd.result;
            memset(&cmd, 0, sizeof(cmd));
            if (r == 0 && res == 0) {
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
                continue;
            }
            return false;
        }
        return true;
    }

    void test_connection(pid_t target_pid = -1) {
        if (target_pid <= 0) target_pid = this->pid;
        if (target_pid <= 0) target_pid = getpid();
        if (target_pid > 0 && target_pid != getpid() && this->pid != target_pid) {
            initialize(target_pid);
        }

        // Test NinMods.kpm via CMD_READ_MEM self-canary
        volatile uint32_t test_val = 0x5A5A1234;
        uint32_t neo_canary = 0;
        na_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op   = NA_CMD_READ_MEM;
        cmd.pid  = (uint32_t)getpid();
        cmd.addr = reinterpret_cast<uint64_t>(&test_val);
        cmd.size = sizeof(test_val);
        errno = 0;
        long r = syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_READ_MEM, (unsigned long)&cmd, NA_PRCTL_AUTH, 0);
        if (r == 0 && cmd.result == 0) {
            memcpy(&neo_canary, cmd.data, sizeof(neo_canary));
        }
        diag.neo_ret = (int)r;
        diag.neo_errno = errno;
        diag.neo_result = cmd.result;
        diag.neo_works = (r == 0 && cmd.result == 0 && neo_canary == test_val);

        if (diag.neo_works) {
            snprintf(diag.status_str, sizeof(diag.status_str), "NinMods KPM Active (PhysWalk: %s)",
                     cmd.screen_w ? "ON" : "OFF");
            diag.is_connected = true;
        } else {
            snprintf(diag.status_str, sizeof(diag.status_str), "NinMods KPM Disconnected");
            diag.is_connected = false;
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
        diag.is_connected = false;
        diag.neo_works    = false;
        diag.last_magic   = 0;
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
        return diag.is_connected.load() ? "NinMods KPM (0x4E41)" : "None / Disconnected";
    }

    bool read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_reads++;
        if (neo_kpm_read(this->pid, addr, buffer, size)) {
            diag.is_connected = true;
            return true;
        }
        diag.failed_reads++;
        return false;
    }

    bool write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_writes++;
        if (neo_kpm_write(this->pid, addr, buffer, size)) {
            diag.is_connected = true;
            return true;
        }
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
        if (syscall(__NR_prctl, NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, NA_PRCTL_AUTH, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
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

        return first_base;
    }

    uintptr_t getModuleBase(const char* name) {
        return parse_maps_module_base(name);
    }
};

inline c_kpm kpm;
