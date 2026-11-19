#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <unistd.h>
#include <cctype>
#include <ctime>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/utsname.h>
#include <sys/system_properties.h>
#include <sys/prctl.h>
#include <cstdint>
#include <cerrno>
#include <sys/syscall.h>

inline int select_driver_option = 0;

// ==================== TB KPM (TongBao / TianBao) ====================
struct mem_operation {
    pid_t target_pid;
    uint64_t addr;
    void *buffer;
    uint64_t size;
};

#define PRCTL_MEM_READ  0x63687501
#define PRCTL_MEM_WRITE 0x63687502

// ==================== NEO KPM (prctl 0x4E41) ====================
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

class c_driver {
private:
    int fd = -1;
    pid_t pid = -1;
    bool is_initialized = false;

    uint32_t active_read_op = 0x801;
    uint32_t active_write_op = 0x802;
    uint32_t active_module_op = 0x803;

    typedef struct _COPY_MEMORY {
        pid_t pid;
        uintptr_t addr;
        void* buffer;
        size_t size;
    } COPY_MEMORY, *PCOPY_MEMORY;

    typedef struct _MODULE_BASE {
        pid_t pid;
        char* name;
        uintptr_t base;
    } MODULE_BASE, *PMODULE_BASE;

    // Fast direct /dev scanning for known RT DEV nodes
    char *driver_path() {
        const char *dev_path = "/dev";
        DIR *dir = opendir(dev_path);
        if (dir == nullptr) return nullptr;

        const char *files[] = { "wanbai", "CheckMe", "Ckanri", "lanran", "video188", "facking", "YBBBYN" };
        struct dirent *entry;
        char *file_path = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            size_t path_length = strlen(dev_path) + strlen(entry->d_name) + 2;
            file_path = (char *)malloc(path_length);
            snprintf(file_path, path_length, "%s/%s", dev_path, entry->d_name);
            for (int i = 0; i < 7; i++) {
                if (strcmp(entry->d_name, files[i]) == 0) {
                    closedir(dir);
                    return file_path;
                }
            }

            struct stat file_info;
            if (stat(file_path, &file_info) < 0) { free(file_path); file_path = nullptr; continue; }
            if (strstr(entry->d_name, "gpiochip") != nullptr) { free(file_path); file_path = nullptr; continue; }

            if ((S_ISCHR(file_info.st_mode) || S_ISBLK(file_info.st_mode))
                && strchr(entry->d_name, '_') == nullptr 
                && strchr(entry->d_name, '-') == nullptr 
                && strchr(entry->d_name, ':') == nullptr) {
                if (strcmp(entry->d_name, "stdin") == 0 || strcmp(entry->d_name, "stdout") == 0
                    || strcmp(entry->d_name, "stderr") == 0) {
                    free(file_path); file_path = nullptr; continue;
                }

                size_t file_name_length = strlen(entry->d_name);
                time_t current_time; time(&current_time);
                int file_year = localtime(&file_info.st_ctime)->tm_year + 1900;
                if (file_year <= 1980) { free(file_path); file_path = nullptr; continue; }

                time_t atime = file_info.st_atime; time_t ctime = file_info.st_ctime;
                if (atime == ctime) {
                    if ((file_info.st_mode & S_IFMT) == 8192 && file_info.st_size == 0
                        && file_info.st_gid == 0 && file_info.st_uid == 0 && file_name_length <= 9) {
                        closedir(dir);
                        return file_path;
                    }
                }
            }
            free(file_path); file_path = nullptr;
        }
        closedir(dir);
        return nullptr;
    }

    // Fast heuristic /dev character device scanner (gtqwq)
    char *gtqwq() {
        const char *dev_path = "/dev";
        DIR *dir = opendir(dev_path);
        if (dir == nullptr) return nullptr;

        struct dirent *entry;
        char file_path[256];
        while ((entry = readdir(dir)) != nullptr) {
            if (strstr(entry->d_name, "std") != nullptr || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strstr(entry->d_name, "gpiochip") != nullptr) continue;
            if (strchr(entry->d_name, '_') != nullptr || strchr(entry->d_name, '-') != nullptr || strchr(entry->d_name, ':') != nullptr) continue;

            snprintf(file_path, sizeof(file_path), "%s/%s", dev_path, entry->d_name);
            struct stat file_info;
            if (stat(file_path, &file_info) < 0) continue;
            if ((localtime(&file_info.st_ctime)->tm_year + 1900) <= 1980) continue;
            if (strlen(entry->d_name) > 7 || strlen(entry->d_name) < 5) continue;
            if (file_info.st_gid != 0 || file_info.st_uid != 0) continue;

            if (S_ISCHR(file_info.st_mode) || S_ISBLK(file_info.st_mode)) {
                if (file_info.st_gid == 0 && file_info.st_uid == 0) {
                    char *devpath = (char *)malloc(256);
                    if (devpath) strcpy(devpath, file_path);
                    closedir(dir);
                    return devpath;
                }
            }
        }
        closedir(dir);
        return nullptr;
    }

    // Key unlocking sequence for RT DEV drivers
    void send_unlock_keys() {
        if (fd < 0) return;
        const char* keys[] = { "123456", "wanbai", "CheckMe", "lanran", "Ckanri", "" };
        const uint32_t init_ops[] = { 0x800, 0xC00, 600 };
        for (uint32_t op : init_ops) {
            for (const char* k : keys) {
                char buf[0x100] = {0};
                strncpy(buf, k, sizeof(buf) - 1);
                ioctl(fd, op, buf);
            }
        }
    }

public:
    // Probe TB KPM (TongBao / TianBao: prctl 0x63687501)
    bool probe_tb_kpm() {
        mem_operation op = { 0, 0, nullptr, 0 };
        errno = 0;
        long r = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        if (r == 0 || (r == -1 && (errno == EFAULT || errno == ESRCH || errno == EPERM))) {
            return true;
        }
        return false;
    }

    // Probe Neo KPM (prctl 0x4E41)
    bool probe_neo_kpm() {
        na_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        errno = 0;
        return (prctl(NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0) == 0);
    }

    bool tb_kpm_read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { this->pid, (uint64_t)addr, buffer, (uint64_t)size };
        long r = syscall(__NR_prctl, PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0);
        return r == 0;
    }

    bool tb_kpm_write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;
        struct mem_operation op = { this->pid, (uint64_t)addr, const_cast<void*>(buffer), (uint64_t)size };
        long r = syscall(__NR_prctl, PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0);
        return r == 0;
    }

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

    int get_pid_from_kpm(const char* pkg) {
        if (select_driver_option == 2) {
            na_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.op = NA_CMD_GET_PID;
            if (pkg) strncpy(cmd.pkg, pkg, sizeof(cmd.pkg) - 1);
            if (prctl(NA_PRCTL_MAGIC, NA_CMD_GET_PID, (unsigned long)&cmd, 0, 0) == 0 && cmd.result == 0 && cmd.pid > 0) {
                return (int)cmd.pid;
            }
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

        // Try su -c cat fallback
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

        if (first_base != 0) {
            return first_base;
        }

        return 0;
    }

    void init_selection() {
        const char* env_opt = getenv("NIN_DRIVER_OPTION");
        if (env_opt && strlen(env_opt) > 0) {
            select_driver_option = atoi(env_opt);
            return;
        }

        // Auto-probe KPM priority
        if (probe_tb_kpm()) {
            select_driver_option = 3; // TB KPM
            return;
        }
        if (probe_neo_kpm()) {
            select_driver_option = 2; // Neo KPM
            return;
        }

        char *dev_node = driver_path();
        if (!dev_node) dev_node = gtqwq();
        if (dev_node != nullptr) {
            free(dev_node);
            select_driver_option = 1; // RT DEV
            return;
        }

        // Default to TB KPM for seamless daemon/app bridge
        select_driver_option = 3;
    }

    void connect() {
        if (select_driver_option == 1) {
            char *dev_node = driver_path();
            if (!dev_node) dev_node = gtqwq();

            if (dev_node != nullptr) {
                fd = open(dev_node, O_RDWR);
                unlink(dev_node); // Stealth: remove from VFS tree
                free(dev_node);
            }

            if (fd == -1) {
                fd = 0; // RT Hook fallback
            }

            send_unlock_keys();
            is_initialized = true;
        } else if (select_driver_option == 2) {
            is_initialized = true;
        } else if (select_driver_option == 3) {
            is_initialized = true;
        }
    }

    c_driver() {
        init_selection();
        connect();
    }

    ~c_driver() {
        if (fd > 0) close(fd);
    }

    void initialize(pid_t target_pid) {
        this->pid = target_pid;
    }

    bool is_loaded() const {
        return is_initialized;
    }

    const char* get_driver_name() const {
        if (select_driver_option == 1) return "RT DEV";
        if (select_driver_option == 2) return "Neo KPM (0x4E41)";
        if (select_driver_option == 3) return "TB KPM (0x63687501)";
        return "KPM (Dual-Engine)";
    }

    bool read(uintptr_t addr, void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;

        if (select_driver_option == 1) { // RT DEV
            COPY_MEMORY cm;
            cm.pid = this->pid;
            cm.addr = addr;
            cm.buffer = buffer;
            cm.size = size;

            if (ioctl(fd, active_read_op, &cm) == 0) return true;

            static const uint32_t candidate_ops[] = { 0x801, 601, 0xC01 };
            for (uint32_t op : candidate_ops) {
                if (op == active_read_op) continue;
                if (ioctl(fd, op, &cm) == 0) {
                    active_read_op = op;
                    return true;
                }
            }
            return false;
        } else if (select_driver_option == 2) { // Neo KPM
            return neo_kpm_read(addr, buffer, size);
        } else if (select_driver_option == 3) { // TB KPM
            return tb_kpm_read(addr, buffer, size);
        }
        return false;
    }

    bool write(uintptr_t addr, const void *buffer, size_t size) {
        if (this->pid <= 0 || addr == 0 || buffer == nullptr || size == 0) return false;

        if (select_driver_option == 1) { // RT DEV
            COPY_MEMORY cm;
            cm.pid = this->pid;
            cm.addr = addr;
            cm.buffer = const_cast<void*>(buffer);
            cm.size = size;

            if (ioctl(fd, active_write_op, &cm) == 0) return true;

            static const uint32_t candidate_ops[] = { 0x802, 602, 0xC02 };
            for (uint32_t op : candidate_ops) {
                if (op == active_write_op) continue;
                if (ioctl(fd, op, &cm) == 0) {
                    active_write_op = op;
                    return true;
                }
            }
            return false;
        } else if (select_driver_option == 2) { // Neo KPM
            return neo_kpm_write(addr, buffer, size);
        } else if (select_driver_option == 3) { // TB KPM
            return tb_kpm_write(addr, buffer, size);
        }
        return false;
    }

    template <typename T>
    T read(uintptr_t addr) {
        T res{};
        if (this->read(addr, &res, sizeof(T))) {
            return res;
        }
        return res;
    }

    template <typename T>
    bool write(uintptr_t addr, T value) {
        return this->write(addr, &value, sizeof(T));
    }

    uintptr_t getModuleBase(const char* name) {
        if (this->pid <= 0) return 0;

        if (select_driver_option == 1) { // RT DEV
            static const uint32_t mod_ops[] = { 0x803, 603, 0xC03 };
            for (uint32_t op : mod_ops) {
                MODULE_BASE mb;
                memset(&mb, 0, sizeof(mb));
                char buf[0x100] = {0};
                strncpy(buf, name, sizeof(buf) - 1);
                mb.pid = this->pid;
                mb.name = buf;
                mb.base = 0;
                if (ioctl(fd, op, &mb) == 0 && mb.base > 0) {
                    uint32_t magic = 0;
                    if (this->read(mb.base, &magic, sizeof(magic)) && magic == 0x464C457F) {
                        active_module_op = op;
                        return mb.base;
                    }
                }
            }
            return parse_maps_module_base(name);
        } else if (select_driver_option == 2 || select_driver_option == 3) {
            return parse_maps_module_base(name);
        }
        return 0;
    }

    void hide_process() {
        if (select_driver_option == 1 && fd >= 0) {
            ioctl(fd, active_module_op == 603 ? 605 : 0x804);
        }
    }
};

inline c_driver *driver = new c_driver();
