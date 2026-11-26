#pragma once
// ============================================================================
// UID-SCOPED LOG SHIELD (SECONDARY ANALYTICS / CRASH / TRACKER DROPPER)
// ============================================================================
// Project: crack_master
//
// SYSTEM ROLE:
// Resolves the target game's Linux UID (com.dts.freefiremax / proc::pid) and
// installs a dedicated iptables / ip6tables custom chain (NIN_LOG_SHIELD)
// scoped strictly to that UID. Only the 12 secondary telemetry, crash,
// analytics, and tracker domains are dropped; all core game, login, match,
// and asset domains remain 100% open and active.
//
// LIFECYCLE & SAFETY:
// - All DNS resolution (getaddrinfo) and iptables updates run on a dedicated
//   background worker thread so the 60/120 FPS ImGui render loop never stalls.
// - Symmetrical cleanup: disable() / stop() removes the OUTPUT jump rule and
//   flushes/deletes the custom chain on toggle OFF, Safe Exit, or normal exit.
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "memory.hpp"
#include "../protect/oxorany.hpp"

namespace log_shield {

    enum class State : int {
        Off = 0,
        WaitingUid = 1,
        Active = 2,
        Error = 3
    };

    struct StatusSnapshot {
        State state = State::Off;
        int   uid = -1;
        int   ip4Count = 0;
        int   ip6Count = 0;
    };

    inline constexpr const char* kChainName = "NIN_LOG_SHIELD";

    // Strictly the 12 unimportant secondary log/crash/analytics/tracker domains
    // captured in firewall.txt. Core login/match/asset domains are NEVER touched.
    inline constexpr const char* kTargetDomains[] = {
        "bdnetwork.ggpolarbear.com",
        "bdnetwork.ggblueshark.com",
        "bdevent.ggpolarbear.com",
        "jncgkt.inapps.appsflyersdk.com",
        "jncgkt.launches.appsflyersdk.com",
        "firebase-settings.crashlytics.com",
        "crashlyticsreports-pa.googleapis.com",
        "firebaselogging-pa.googleapis.com",
        "firebaselogging.googleapis.com",
        "app-measurement.com",
        "aaid.umeng.com",
        "ulogs.umeng.com"
    };
    inline constexpr size_t kTargetDomainCount =
        sizeof(kTargetDomains) / sizeof(kTargetDomains[0]);

    inline std::mutex              g_mutex;
    inline std::condition_variable g_cv;
    inline std::thread             g_worker;
    inline std::atomic<bool>       g_running{false};
    inline bool                    g_desired_enabled = false;
    inline bool                    g_rules_installed = false;
    inline int                     g_installed_uid = -1;
    inline StatusSnapshot          g_status{};

    inline int resolve_uid_from_packages_list() {
        FILE* f = std::fopen("/data/system/packages.list", "r");
        if (!f) return -1;

        char line[512];
        int found_uid = -1;
        while (std::fgets(line, sizeof(line), f)) {
            char pkg[256] = {0};
            int uid = -1;
            if (std::sscanf(line, "%255s %d", pkg, &uid) == 2) {
                if (std::strcmp(pkg, "com.dts.freefiremax") == 0 ||
                    std::strcmp(pkg, "com.dts.freefireth") == 0) {
                    if (uid >= 10000) {
                        found_uid = uid;
                        break;
                    }
                }
            }
        }
        std::fclose(f);
        return found_uid;
    }

    inline int resolve_game_uid() {
        int pid = proc::pid;
        if (pid > 0) {
            char proc_path[64];
            std::snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
            struct stat st{};
            if (::stat(proc_path, &st) == 0 && static_cast<int>(st.st_uid) >= 10000) {
                return static_cast<int>(st.st_uid);
            }
        }
        return resolve_uid_from_packages_list();
    }

    inline void resolve_domain_ips(std::vector<std::string>& out_v4,
                                   std::vector<std::string>& out_v6) {
        out_v4.clear();
        out_v6.clear();

        std::unordered_set<std::string> seen_v4;
        std::unordered_set<std::string> seen_v6;

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        for (size_t i = 0; i < kTargetDomainCount; ++i) {
            struct addrinfo* res = nullptr;
            if (::getaddrinfo(kTargetDomains[i], nullptr, &hints, &res) != 0 || !res) {
                continue;
            }
            for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
                char buf[INET6_ADDRSTRLEN] = {0};
                if (p->ai_family == AF_INET) {
                    auto* sin = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
                    if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                        if (seen_v4.insert(buf).second) {
                            out_v4.emplace_back(buf);
                        }
                    }
                } else if (p->ai_family == AF_INET6) {
                    auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
                    if (::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) {
                        if (seen_v6.insert(buf).second) {
                            out_v6.emplace_back(buf);
                        }
                    }
                }
            }
            ::freeaddrinfo(res);
        }
    }

    inline bool run_shell_batch(const std::string& script) {
        FILE* pipe = ::popen("/system/bin/sh", "w");
        if (!pipe) return false;
        std::fwrite(script.data(), 1, script.size(), pipe);
        int rc = ::pclose(pipe);
        if (rc == -1) return false;
        return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
    }

    inline void remove_rules_for_uid(int uid) {
        std::string s;
        s.reserve(512);
        if (uid >= 10000) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "iptables -D OUTPUT -m owner --uid-owner %d -j %s 2>/dev/null\n"
                "ip6tables -D OUTPUT -m owner --uid-owner %d -j %s 2>/dev/null\n",
                uid, kChainName, uid, kChainName);
            s += buf;
        }
        char tail[256];
        std::snprintf(tail, sizeof(tail),
            "iptables -F %s 2>/dev/null\n"
            "iptables -X %s 2>/dev/null\n"
            "ip6tables -F %s 2>/dev/null\n"
            "ip6tables -X %s 2>/dev/null\n"
            "exit 0\n",
            kChainName, kChainName, kChainName, kChainName);
        s += tail;
        run_shell_batch(s);
    }

    inline bool apply_rules_for_uid(int uid,
                                    const std::vector<std::string>& v4,
                                    const std::vector<std::string>& v6) {
        std::string s;
        s.reserve(4096);

        char hdr[512];
        std::snprintf(hdr, sizeof(hdr),
            "iptables -N %s 2>/dev/null || iptables -F %s\n"
            "iptables -C OUTPUT -m owner --uid-owner %d -j %s 2>/dev/null || "
            "iptables -I OUTPUT 1 -m owner --uid-owner %d -j %s\n",
            kChainName, kChainName, uid, kChainName, uid, kChainName);
        s += hdr;

        for (const auto& ip : v4) {
            s += "iptables -A ";
            s += kChainName;
            s += " -d ";
            s += ip;
            s += " -j DROP\n";
        }

        if (!v6.empty()) {
            char hdr6[512];
            std::snprintf(hdr6, sizeof(hdr6),
                "ip6tables -N %s 2>/dev/null || ip6tables -F %s 2>/dev/null\n"
                "ip6tables -C OUTPUT -m owner --uid-owner %d -j %s 2>/dev/null || "
                "ip6tables -I OUTPUT 1 -m owner --uid-owner %d -j %s 2>/dev/null\n",
                kChainName, kChainName, uid, kChainName, uid, kChainName);
            s += hdr6;

            for (const auto& ip : v6) {
                s += "ip6tables -A ";
                s += kChainName;
                s += " -d ";
                s += ip;
                s += " -j DROP 2>/dev/null\n";
            }
        }

        // Verify the jump rule actually exists in OUTPUT
        char verify[192];
        std::snprintf(verify, sizeof(verify),
            "iptables -C OUTPUT -m owner --uid-owner %d -j %s 2>/dev/null\n",
            uid, kChainName);
        s += verify;

        return run_shell_batch(s);
    }

    inline void worker_loop() {
        prctl(PR_SET_NAME, oxorany("Binder:netguard"), 0, 0, 0);

        while (g_running.load(std::memory_order_acquire)) {
            bool want_enabled = false;
            {
                std::unique_lock<std::mutex> lk(g_mutex);
                want_enabled = g_desired_enabled;
            }

            if (!want_enabled) {
                if (g_rules_installed) {
                    remove_rules_for_uid(g_installed_uid);
                    g_rules_installed = false;
                    g_installed_uid = -1;
                }
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    if (!g_desired_enabled) {
                        g_status = StatusSnapshot{State::Off, -1, 0, 0};
                    }
                }
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait(lk, []() {
                    return !g_running.load(std::memory_order_acquire) || g_desired_enabled;
                });
                continue;
            }

            // Shield is enabled: resolve game UID
            int uid = resolve_game_uid();
            if (uid < 10000) {
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_status = StatusSnapshot{State::WaitingUid, -1, 0, 0};
                }
                std::unique_lock<std::mutex> lk(g_mutex);
                g_cv.wait_for(lk, std::chrono::seconds(2), []() {
                    return !g_running.load(std::memory_order_acquire) || !g_desired_enabled;
                });
                continue;
            }

            // If UID changed while running, clean old UID jump rule first
            if (g_rules_installed && g_installed_uid != uid) {
                remove_rules_for_uid(g_installed_uid);
                g_rules_installed = false;
                g_installed_uid = -1;
            }

            std::vector<std::string> v4, v6;
            resolve_domain_ips(v4, v6);

            bool ok = apply_rules_for_uid(uid, v4, v6);
            if (ok) {
                g_rules_installed = true;
                g_installed_uid = uid;
                std::lock_guard<std::mutex> lk(g_mutex);
                if (g_desired_enabled) {
                    g_status = StatusSnapshot{
                        State::Active,
                        uid,
                        static_cast<int>(v4.size()),
                        static_cast<int>(v6.size())
                    };
                }
            } else {
                std::lock_guard<std::mutex> lk(g_mutex);
                if (g_desired_enabled) {
                    g_status = StatusSnapshot{State::Error, uid, 0, 0};
                }
            }

            // Refresh DNS->IP mappings periodically in case CDN IPs rotate,
            // or wake immediately if toggled off / exiting.
            std::unique_lock<std::mutex> lk(g_mutex);
            g_cv.wait_for(lk, std::chrono::seconds(30), []() {
                return !g_running.load(std::memory_order_acquire) || !g_desired_enabled;
            });
        }

        // Guaranteed symmetrical cleanup on thread stop
        if (g_rules_installed) {
            remove_rules_for_uid(g_installed_uid);
            g_rules_installed = false;
            g_installed_uid = -1;
        }
        std::lock_guard<std::mutex> lk(g_mutex);
        g_status = StatusSnapshot{State::Off, -1, 0, 0};
    }

    inline void start() {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        g_worker = std::thread(worker_loop);
    }

    inline void set_enabled(bool enabled) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_desired_enabled == enabled) return;
            g_desired_enabled = enabled;
        }
        g_cv.notify_all();
    }

    inline StatusSnapshot status() {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_status;
    }

    inline void stop() {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_desired_enabled = false;
        }
        g_running.store(false, std::memory_order_release);
        g_cv.notify_all();
        if (g_worker.joinable()) {
            g_worker.join();
        }
    }

} // namespace log_shield
