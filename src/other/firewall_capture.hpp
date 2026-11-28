#pragma once
// ============================================================================
// UID-SCOPED FIREWALL CAPTURE (/proc/net/{tcp,tcp6,udp,udp6} -> nin_firewall.txt)
// ============================================================================
// Project: crack_master
//
// Captures unique outbound IP:Port + reverse DNS hostnames opened by the target
// game UID (com.dts.freefiremax / com.dts.freefireth) without requiring
// tcpdump or kernel xt_LOG modules. Saves directly to /sdcard/Download/nin_firewall.txt
// (fallback /data/local/tmp/nin_firewall.txt).
// ============================================================================

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <netdb.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_set>

#include "log_shield.hpp"
#include "memory.hpp"
#include "../protect/oxorany.hpp"

namespace firewall_capture {

    enum class State : int {
        Off = 0,
        WaitingUid = 1,
        Capturing = 2,
        Error = 3
    };

    struct StatusSnapshot {
        State  state = State::Off;
        int    uid = -1;
        size_t count = 0;
        char   path[128] = "/sdcard/Download/nin_firewall.txt";
    };

    inline std::mutex        g_mutex;
    inline std::thread       g_worker;
    inline std::atomic<bool> g_running{false};
    inline std::atomic<bool> g_enabled{true};
    inline StatusSnapshot    g_status{};

    inline void set_enabled(bool en) {
        g_enabled.store(en, std::memory_order_relaxed);
    }

    inline StatusSnapshot status() {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_status;
    }

    inline bool parse_ipv4_hex(const char* hex8, char* out_ip, size_t out_sz) {
        uint32_t raw = 0;
        if (std::sscanf(hex8, "%8x", &raw) != 1) return false;
        if (raw == 0 || raw == 0x0100007FU) return false; // 0.0.0.0 or 127.0.0.1
        struct in_addr addr{};
        addr.s_addr = raw;
        return ::inet_ntop(AF_INET, &addr, out_ip, static_cast<socklen_t>(out_sz)) != nullptr;
    }

    inline bool parse_ipv6_hex(const char* hex32, char* out_ip, size_t out_sz) {
        uint32_t w[4] = {0};
        if (std::sscanf(hex32, "%8x%8x%8x%8x", &w[0], &w[1], &w[2], &w[3]) != 4) return false;
        if ((w[0] | w[1] | w[2] | w[3]) == 0) return false; // ::
        if (w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0x01000000U) return false; // ::1
        struct in6_addr addr{};
        std::memcpy(&addr, w, sizeof(w));
        return ::inet_ntop(AF_INET6, &addr, out_ip, static_cast<socklen_t>(out_sz)) != nullptr;
    }

    inline std::string resolve_host(const char* ip, int family) {
        struct sockaddr_storage ss{};
        socklen_t slen = 0;
        if (family == AF_INET) {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(&ss);
            sin->sin_family = AF_INET;
            if (::inet_pton(AF_INET, ip, &sin->sin_addr) != 1) return "-";
            slen = sizeof(struct sockaddr_in);
        } else {
            auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(&ss);
            sin6->sin6_family = AF_INET6;
            if (::inet_pton(AF_INET6, ip, &sin6->sin6_addr) != 1) return "-";
            slen = sizeof(struct sockaddr_in6);
        }
        char host[NI_MAXHOST] = {0};
        if (::getnameinfo(reinterpret_cast<struct sockaddr*>(&ss), slen,
                          host, sizeof(host), nullptr, 0, NI_NAMEREQD) == 0 && host[0]) {
            return std::string(host);
        }
        return "-";
    }

    inline void scan_proc_net(const char* path, const char* proto, int family,
                              int target_uid, FILE* out_fp,
                              std::unordered_set<std::string>& seen_ep,
                              std::unordered_set<std::string>& seen_hosts,
                              size_t& total_count) {
        FILE* f = std::fopen(path, "r");
        if (!f) return;

        char line[512];
        bool first = true;
        while (std::fgets(line, sizeof(line), f)) {
            if (first) {
                first = false;
                continue;
            }
            char rem_hex[64] = {0};
            unsigned int rem_port = 0;
            unsigned int state = 0;
            int uid = -1;
            if (std::sscanf(line, " %*d: %*63[0-9A-Fa-f]:%*x %63[0-9A-Fa-f]:%x %x %*x:%*x %*x:%*x %*x %d",
                            rem_hex, &rem_port, &state, &uid) != 4) {
                continue;
            }
            if (uid != target_uid || rem_port == 0) continue;

            char ip[INET6_ADDRSTRLEN] = {0};
            bool ok = (family == AF_INET)
                ? parse_ipv4_hex(rem_hex, ip, sizeof(ip))
                : parse_ipv6_hex(rem_hex, ip, sizeof(ip));
            if (!ok) continue;

            char key[160];
            std::snprintf(key, sizeof(key), "%s|%s|%u", proto, ip, rem_port);
            if (!seen_ep.insert(key).second) continue;

            std::string host = resolve_host(ip, family);
            if (host != "-" && !seen_hosts.insert(host).second) {
                continue;
            }

            std::time_t now = std::time(nullptr);
            struct tm tm_now{};
            localtime_r(&now, &tm_now);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

            std::fprintf(out_fp, "[%s] %-4s %s:%u  host=%s  uid=%d\n",
                         ts, proto, ip, rem_port, host.c_str(), uid);
            std::fflush(out_fp);
            total_count++;
        }
        std::fclose(f);
    }

    inline void seed_existing_capture(const char* path,
                                      std::unordered_set<std::string>& seen_ep,
                                      std::unordered_set<std::string>& seen_hosts) {
        FILE* f = std::fopen(path, "r");
        if (!f) return;
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            char proto[16] = {0};
            char ip_port[128] = {0};
            char host[256] = {0};
            if (std::sscanf(line, "[%*[^]]] %15s %127s host=%255s", proto, ip_port, host) >= 2) {
                char* colon = std::strrchr(ip_port, ':');
                if (colon) {
                    *colon = '\0';
                    char key[160];
                    std::snprintf(key, sizeof(key), "%s|%s|%s", proto, ip_port, colon + 1);
                    seen_ep.insert(key);
                }
                if (host[0] && std::strcmp(host, "-") != 0) {
                    seen_hosts.insert(host);
                }
            }
        }
        std::fclose(f);
    }

    inline FILE* open_capture_file(std::string& used_path) {
        const char* primary = "/sdcard/Download/nin_firewall.txt";
        FILE* fp = std::fopen(primary, "a");
        if (fp) {
            used_path = primary;
            return fp;
        }
        const char* fallback = "/data/local/tmp/nin_firewall.txt";
        fp = std::fopen(fallback, "a");
        if (fp) {
            used_path = fallback;
            return fp;
        }
        return nullptr;
    }

    inline void worker_loop() {
        ::prctl(PR_SET_NAME, oxorany("NetStatsWorker"), 0, 0, 0);

        std::unordered_set<std::string> seen_ep;
        std::unordered_set<std::string> seen_hosts;
        FILE* out_fp = nullptr;
        std::string out_path = "/sdcard/Download/nin_firewall.txt";
        size_t total_count = 0;
        int cached_uid = -1;
        bool seeded = false;

        while (g_running.load(std::memory_order_relaxed)) {
            if (!g_enabled.load(std::memory_order_relaxed)) {
                if (out_fp) {
                    std::fclose(out_fp);
                    out_fp = nullptr;
                }
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_status.state = State::Off;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }

            if (cached_uid < 10000) {
                cached_uid = log_shield::resolve_game_uid();
            }
            if (cached_uid < 10000) {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_status.state = State::WaitingUid;
                g_status.uid = -1;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            if (!out_fp) {
                out_fp = open_capture_file(out_path);
                if (!out_fp) {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_status.state = State::Error;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    continue;
                }
                std::fprintf(out_fp, "\n# === NINMODS FIREWALL CAPTURE START (uid=%d) ===\n", cached_uid);
                std::fflush(out_fp);
                if (!seeded) {
                    seed_existing_capture(out_path.c_str(), seen_ep, seen_hosts);
                    seeded = true;
                }
            }

            const int pid = proc::pid;
            if (pid > 0) {
                char p[64];
                std::snprintf(p, sizeof(p), "/proc/%d/net/tcp", pid);
                scan_proc_net(p, "TCP", AF_INET, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
                std::snprintf(p, sizeof(p), "/proc/%d/net/tcp6", pid);
                scan_proc_net(p, "TCP6", AF_INET6, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
                std::snprintf(p, sizeof(p), "/proc/%d/net/udp", pid);
                scan_proc_net(p, "UDP", AF_INET, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
                std::snprintf(p, sizeof(p), "/proc/%d/net/udp6", pid);
                scan_proc_net(p, "UDP6", AF_INET6, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
            } else {
                scan_proc_net("/proc/net/tcp", "TCP", AF_INET, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
                scan_proc_net("/proc/net/tcp6", "TCP6", AF_INET6, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
                scan_proc_net("/proc/net/udp", "UDP", AF_INET, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
                scan_proc_net("/proc/net/udp6", "UDP6", AF_INET6, cached_uid, out_fp, seen_ep, seen_hosts, total_count);
            }

            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_status.state = State::Capturing;
                g_status.uid = cached_uid;
                g_status.count = total_count;
                std::snprintf(g_status.path, sizeof(g_status.path), "%s", out_path.c_str());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (out_fp) {
            std::fclose(out_fp);
            out_fp = nullptr;
        }
    }

    inline void start() {
        if (g_running.exchange(true)) return;
        g_worker = std::thread(worker_loop);
    }

    inline void stop() {
        if (!g_running.exchange(false)) return;
        g_enabled.store(false, std::memory_order_relaxed);
        if (g_worker.joinable()) g_worker.join();
    }
}
