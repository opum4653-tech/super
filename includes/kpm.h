#pragma once
// ============================================================================
// MEMORY ENGINE & LIVE TELEMETRY (DIRECT SYSCALL 270)
// ============================================================================
// Direct process_vm_readv / process_vm_writev memory engine with live
// diagnostics and verified write/rollback counters.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifndef __NR_process_vm_readv
#if defined(__aarch64__)
#define __NR_process_vm_readv 270
#define __NR_process_vm_writev 271
#elif defined(__x86_64__)
#define __NR_process_vm_readv 310
#define __NR_process_vm_writev 311
#endif
#endif

enum EKpmEngine {
    KPM_ENGINE_SYSCALL = 0
};

struct KpmDiagnostics {
    std::atomic<int>  selected_engine{KPM_ENGINE_SYSCALL};
    std::atomic<int>  active_engine{KPM_ENGINE_SYSCALL};
    std::atomic<bool> is_connected{false};

    // Syscall Test
    std::atomic<int>  syscall_ret{-999};
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

    bool syscall_read(pid_t target_pid, uintptr_t addr, void *buffer, size_t size) {
        if (target_pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct iovec loc = { buffer, size };
        struct iovec rem = { reinterpret_cast<void*>(addr), size };
        ssize_t r = syscall(__NR_process_vm_readv, target_pid, &loc, 1, &rem, 1, 0);
        diag.syscall_ret = static_cast<int>(r);
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

    void test_connection(pid_t target_pid = -1) {
        if (target_pid <= 0) target_pid = this->pid;
        if (target_pid <= 0) target_pid = getpid();
        if (target_pid > 0 && target_pid != getpid() && this->pid != target_pid) {
            initialize(target_pid);
        }

        if (target_pid > 0 && target_pid != getpid()) {
            uint32_t dummy = 0;
            struct iovec loc = { &dummy, sizeof(dummy) };
            struct iovec rem = { reinterpret_cast<void*>(0x10000), sizeof(dummy) };
            ssize_t sr = syscall(__NR_process_vm_readv, target_pid, &loc, 1, &rem, 1, 0);
            diag.syscall_ret = static_cast<int>(sr);
            diag.syscall_works = (sr > 0 || (sr < 0 && errno == EFAULT));
        } else {
            volatile uint32_t test_val = 0x5A5A1234;
            uint32_t read_buf = 0;
            struct iovec loc = { &read_buf, sizeof(read_buf) };
            struct iovec rem = { const_cast<uint32_t*>(&test_val), sizeof(test_val) };
            ssize_t sr = syscall(__NR_process_vm_readv, getpid(), &loc, 1, &rem, 1, 0);
            diag.syscall_ret = static_cast<int>(sr);
            diag.syscall_works = (sr == static_cast<ssize_t>(sizeof(test_val)) && read_buf == test_val);
        }

        diag.active_engine = KPM_ENGINE_SYSCALL;
        diag.is_connected = diag.syscall_works.load();
        if (diag.is_connected) {
            snprintf(diag.status_str, sizeof(diag.status_str), "Syscall 270 Active");
        } else {
            snprintf(diag.status_str, sizeof(diag.status_str), "Syscall 270 Idle (ret=%d)", diag.syscall_ret.load());
        }
    }

    bool init(bool verbose = true) {
        test_connection(this->pid);
        if (verbose) {
            printf("[MEM] %s | Active Engine: %s\n", diag.status_str, get_active_engine_name());
            fflush(stdout);
        }
        return diag.is_connected;
    }

    void initialize(pid_t target_pid) {
        if (this->pid == target_pid) return;
        this->pid = target_pid;
        diag.active_engine = KPM_ENGINE_SYSCALL;
        diag.is_connected  = false;
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
        return "Syscall 270 (process_vm_readv)";
    }

    bool read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_reads++;
        if (syscall_read(this->pid, addr, buffer, size)) {
            diag.active_engine = KPM_ENGINE_SYSCALL;
            diag.is_connected = true;
            return true;
        }
        diag.failed_reads++;
        return false;
    }

    bool write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        diag.total_writes++;
        if (syscall_write(this->pid, addr, buffer, size)) {
            diag.active_engine = KPM_ENGINE_SYSCALL;
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
