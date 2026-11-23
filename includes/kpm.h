#pragma once
// ============================================================================
// KPM (KERNEL PATCH MODULE) ENGINE
// ============================================================================
// Sourced from: pewpew (C:\Users\Sajala\Documents\mods\refer\pewpew\includes\driver.h)
//
// Pure KPM prctl method only. No /dev driver nodes, no external syscalls.
// Communicates with APatch / KernelSU KPM module via prctl:
// 1. TB KPM:  PRCTL_MEM_READ (0x63687501) / PRCTL_MEM_WRITE (0x63687502)
// 2. Neo KPM: NA_PRCTL_MAGIC (0x4E41)
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/prctl.h>
#include <sys/types.h>

// ==================== TB KPM STRUCTURES ====================
struct mem_operation {
    pid_t    target_pid;
    uint64_t addr;
    void    *buffer;
    uint64_t size;
};

#define PRCTL_MEM_READ  0x63687501
#define PRCTL_MEM_WRITE 0x63687502

// ==================== NEO KPM STRUCTURES ====================
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

enum EKpmType {
    KPM_NONE = 0,
    KPM_TB,     // TB KPM (prctl 0x63687501)
    KPM_NEO     // Neo KPM (prctl 0x4E41)
};

class c_kpm {
private:
    pid_t    pid   = -1;
    EKpmType type  = KPM_NONE;
    bool     valid = false;

public:
    c_kpm() : pid(-1), type(KPM_NONE), valid(false) {}

    // Probe Neo KPM (from pewpew)
    bool probe_neo_kpm() {
        na_cmd cmd;
        prctl(0xDEAD, 0, 0, 0, 0);
        memset(&cmd, 0, sizeof(cmd));
        errno = 0;
        return (prctl(NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0) == 0);
    }

    // Probe TB KPM (from pewpew)
    bool probe_tb_kpm() {
        volatile uint32_t test_val = 0x5A5A1234;
        uint32_t read_buf = 0;
        mem_operation op = { getpid(), reinterpret_cast<uint64_t>(&test_val), &read_buf, sizeof(test_val) };
        errno = 0;
        long r = prctl(PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        if (r >= 0 && read_buf == test_val) {
            return true;
        }
        mem_operation null_op = { 0, 0, nullptr, 0 };
        errno = 0;
        r = prctl(PRCTL_MEM_READ, (unsigned long)&null_op, 0, 0, 0);
        if (r == 0 || (r == -1 && errno != EINVAL)) {
            return true;
        }
        return false;
    }

    // Auto-probe KPM method (TB KPM first, then Neo KPM)
    bool init(bool verbose = true) {
        if (probe_tb_kpm()) {
            type = KPM_TB;
            valid = true;
            if (verbose) {
                printf("\033[42m\033[37m[+] TB KPM active (prctl 0x63687501)\033[0m\n");
                fflush(stdout);
            }
            return true;
        }

        if (probe_neo_kpm()) {
            type = KPM_NEO;
            valid = true;
            if (verbose) {
                printf("\033[42m\033[37m[+] Neo KPM active (prctl 0x4E41)\033[0m\n");
                fflush(stdout);
            }
            return true;
        }

        valid = false;
        type = KPM_NONE;
        if (verbose) {
            printf("\033[41m\033[37m[!] KPM not available on this device!\033[0m\n");
            fflush(stdout);
        }
        return false;
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
        return valid;
    }

    EKpmType get_type() const {
        return type;
    }

    const char* get_type_name() const {
        switch (type) {
            case KPM_TB:  return "TB KPM";
            case KPM_NEO: return "Neo KPM";
            default:      return "None";
        }
    }

    // Neo KPM read (from pewpew)
    bool neo_kpm_read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        uint8_t *ptr = reinterpret_cast<uint8_t*>(buffer);
        size_t remaining = size;
        uintptr_t curr_addr = addr;
        while (remaining > 0) {
            size_t chunk = (remaining > NA_MAX_RW_SIZE) ? NA_MAX_RW_SIZE : remaining;
            na_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.pid  = (uint32_t)this->pid;
            cmd.addr = (uint64_t)curr_addr;
            cmd.size = (uint32_t)chunk;
            if (prctl(NA_PRCTL_MAGIC, NA_CMD_READ_MEM, (unsigned long)&cmd, 0, 0) == 0 && cmd.result == 0) {
                memcpy(ptr, cmd.data, chunk);
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
            } else {
                return false;
            }
        }
        return true;
    }

    // Neo KPM write (from pewpew)
    bool neo_kpm_write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        const uint8_t *ptr = reinterpret_cast<const uint8_t*>(buffer);
        size_t remaining = size;
        uintptr_t curr_addr = addr;
        while (remaining > 0) {
            size_t chunk = (remaining > NA_MAX_RW_SIZE) ? NA_MAX_RW_SIZE : remaining;
            na_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.pid  = (uint32_t)this->pid;
            cmd.addr = (uint64_t)curr_addr;
            cmd.size = (uint32_t)chunk;
            memcpy(cmd.data, ptr, chunk);
            if (prctl(NA_PRCTL_MAGIC, NA_CMD_WRITE_MEM, (unsigned long)&cmd, 0, 0) == 0 && cmd.result == 0) {
                ptr += chunk;
                curr_addr += chunk;
                remaining -= chunk;
            } else {
                return false;
            }
        }
        return true;
    }

    // TB KPM read (from pewpew)
    bool tb_kpm_read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { this->pid, (uint64_t)addr, buffer, (uint64_t)size };
        long r = prctl(PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        return r >= 0;
    }

    // TB KPM write (from pewpew)
    bool tb_kpm_write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { this->pid, (uint64_t)addr, const_cast<void*>(buffer), (uint64_t)size };
        long r = prctl(PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0);
        return r >= 0;
    }

    // Unified KPM read
    bool read(uintptr_t addr, void *buffer, size_t size) {
        if (!valid || this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        if (type == KPM_TB) {
            return tb_kpm_read(addr, buffer, size);
        } else if (type == KPM_NEO) {
            return neo_kpm_read(addr, buffer, size);
        }
        return false;
    }

    // Unified KPM write
    bool write(uintptr_t addr, const void *buffer, size_t size) {
        if (!valid || this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        if (type == KPM_TB) {
            return tb_kpm_write(addr, buffer, size);
        } else if (type == KPM_NEO) {
            return neo_kpm_write(addr, buffer, size);
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

    // Query PID via Neo KPM directly
    pid_t get_pid_neo(const char* pkg_name) {
        if (!valid || type != KPM_NEO || !pkg_name) return -1;
        na_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = NA_CMD_GET_PID;
        strncpy(cmd.pkg, pkg_name, sizeof(cmd.pkg) - 1);
        if (prctl(NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
            return static_cast<pid_t>(cmd.pid);
        }
        return -1;
    }

    // Parse maps module base (from pewpew)
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

        // Try su -c cat /proc/<pid>/maps fallback (from pewpew)
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "su -c 'cat /proc/%d/maps | grep %s'", this->pid, name);
        f = popen(cmd, "r");
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
                            break;
                        }
                    }
                }
            }
            pclose(f);
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
