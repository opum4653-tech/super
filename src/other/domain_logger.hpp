#pragma once
// ============================================================================
// TEMPORARY GAME DOMAIN LOGGER -> /sdcard/Download/firewall.txt
// ============================================================================
// Captures real outbound TLS SNI (TCP:443) domains owned by the target game UID
// (checked against /proc/net/tcp & /proc/net/tcp6) and writes unique domains
// to /sdcard/Download/firewall.txt.
//
// Remove this file and its 3 lines in src/main.cpp when done collecting domains.
// See FIREWALL_LOGGER_GUIDE.md for exact add/remove instructions.
// ============================================================================

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "memory.hpp"

namespace domain_logger {

    inline std::atomic<bool> g_running{false};
    inline std::thread       g_thread;
    inline std::mutex        g_file_mutex;
    inline std::unordered_set<std::string> g_seen;

    inline int get_uid_for_pid(int pid) {
        if (pid <= 0) return -1;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE* f = fopen(path, "r");
        if (!f) return -1;
        char line[256];
        int uid = -1;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                int ruid = -1;
                if (sscanf(line + 4, "%d", &ruid) == 1 && ruid >= 0) {
                    uid = ruid;
                }
                break;
            }
        }
        fclose(f);
        return uid;
    }

    inline bool port_belongs_to_uid_file(const char* proc_net_path, uint16_t local_port, int target_uid) {
        FILE* f = fopen(proc_net_path, "r");
        if (!f) return false;
        char line[512];
        // Skip header line
        if (!fgets(line, sizeof(line), f)) {
            fclose(f);
            return false;
        }
        bool matched = false;
        while (fgets(line, sizeof(line), f)) {
            unsigned int sl = 0;
            char laddr[128] = {0}, raddr[128] = {0};
            unsigned int lport = 0, rport = 0, st = 0;
            unsigned long txq = 0, rxq = 0, tr = 0, tm = 0, retr = 0;
            int uid = -1;
            // Format: sl local_address:port rem_address:port st tx:rx tr:tm retr uid
            if (sscanf(line, " %u: %64[^:]:%x %64[^:]:%x %x %lx:%lx %lx:%lx %lx %d",
                       &sl, laddr, &lport, raddr, &rport, &st,
                       &txq, &rxq, &tr, &tm, &retr, &uid) == 12) {
                if (uid == target_uid && static_cast<uint16_t>(lport) == local_port) {
                    matched = true;
                    break;
                }
            }
        }
        fclose(f);
        return matched;
    }

    inline bool is_game_tcp_socket(uint16_t local_port, int game_uid) {
        if (game_uid < 0 || local_port == 0) return false;
        if (port_belongs_to_uid_file("/proc/net/tcp6", local_port, game_uid)) return true;
        if (port_belongs_to_uid_file("/proc/net/tcp", local_port, game_uid)) return true;
        return false;
    }

    inline bool is_valid_domain(const std::string& d) {
        if (d.size() < 4 || d.size() > 253) return false;
        bool has_dot = false;
        for (char c : d) {
            if (c == '.') {
                has_dot = true;
                continue;
            }
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
                continue;
            }
            return false;
        }
        return has_dot;
    }

    inline std::string normalize_domain(const char* raw, size_t len) {
        std::string out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i]))));
        }
        return is_valid_domain(out) ? out : std::string();
    }

    // Parses TLS ClientHello SNI extension (RFC 6066) from TCP payload
    inline std::string parse_tls_sni(const uint8_t* data, size_t len) {
        if (len < 43) return {};
        // TLS Record: ContentType=0x16 (Handshake), Version=0x03xx
        if (data[0] != 0x16 || data[1] != 0x03) return {};
        // HandshakeType=0x01 (ClientHello)
        if (data[5] != 0x01) return {};

        size_t pos = 5 + 4 + 2 + 32; // Record(5) + HandshakeHdr(4) + Version(2) + Random(32)
        if (pos >= len) return {};

        // Session ID
        uint8_t sess_len = data[pos];
        pos += 1 + sess_len;
        if (pos + 2 > len) return {};

        // Cipher Suites
        uint16_t cipher_len = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2 + cipher_len;
        if (pos + 1 > len) return {};

        // Compression Methods
        uint8_t comp_len = data[pos];
        pos += 1 + comp_len;
        if (pos + 2 > len) return {};

        // Extensions total length
        uint16_t ext_total = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        size_t ext_end = pos + ext_total;
        if (ext_end > len) ext_end = len;

        while (pos + 4 <= ext_end) {
            uint16_t ext_type = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
            uint16_t ext_len  = (static_cast<uint16_t>(data[pos + 2]) << 8) | data[pos + 3];
            pos += 4;
            if (pos + ext_len > ext_end) break;

            if (ext_type == 0x0000 && ext_len >= 5) { // server_name
                size_t p = pos + 2; // skip server_name_list_length
                while (p + 3 <= pos + ext_len) {
                    uint8_t name_type = data[p];
                    uint16_t name_len = (static_cast<uint16_t>(data[p + 1]) << 8) | data[p + 2];
                    p += 3;
                    if (p + name_len > pos + ext_len) break;
                    if (name_type == 0x00 && name_len > 0) {
                        return normalize_domain(reinterpret_cast<const char*>(data + p), name_len);
                    }
                    p += name_len;
                }
            }
            pos += ext_len;
        }
        return {};
    }

    inline void load_existing_domains() {
        std::lock_guard<std::mutex> lk(g_file_mutex);
        const char* paths[] = { "/sdcard/Download/firewall.txt", "/storage/emulated/0/Download/firewall.txt" };
        for (const char* p : paths) {
            FILE* f = fopen(p, "r");
            if (!f) continue;
            char line[300];
            while (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
                    line[--n] = '\0';
                }
                if (n > 0 && line[0] != '#') {
                    g_seen.insert(std::string(line, n));
                }
            }
            fclose(f);
            break;
        }
    }

    inline void record_domain(const std::string& domain) {
        if (domain.empty()) return;
        std::lock_guard<std::mutex> lk(g_file_mutex);
        if (!g_seen.insert(domain).second) return;

        const char* paths[] = { "/sdcard/Download/firewall.txt", "/storage/emulated/0/Download/firewall.txt" };
        for (const char* p : paths) {
            FILE* f = fopen(p, "a");
            if (f) {
                fprintf(f, "%s\n", domain.c_str());
                fclose(f);
                return;
            }
        }
    }

    inline void logger_loop() {
        prctl(PR_SET_NAME, "netd_diag", 0, 0, 0);
        load_existing_domains();

        int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (sock < 0) return;

        struct timeval tv{0, 200000}; // 200ms timeout for clean shutdown
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::vector<uint8_t> buf(4096);
        int cached_pid = -1;
        int cached_uid = -1;

        while (g_running.load(std::memory_order_relaxed)) {
            int cur_pid = proc::pid;
            if (cur_pid <= 0) {
                cached_pid = -1;
                cached_uid = -1;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
            if (cur_pid != cached_pid) {
                cached_pid = cur_pid;
                cached_uid = get_uid_for_pid(cur_pid);
            }

            struct sockaddr_ll sll{};
            socklen_t sll_len = sizeof(sll);
            ssize_t n = recvfrom(sock, buf.data(), buf.size(), 0,
                                 reinterpret_cast<struct sockaddr*>(&sll), &sll_len);
            if (n <= 0) continue;

            // Only inspect outgoing packets
            if (sll.sll_pkttype != PACKET_OUTGOING) continue;

            size_t ip_off = 0;
            if (sll.sll_hatype == ARPHRD_ETHER) {
                if (static_cast<size_t>(n) < sizeof(struct ethhdr)) continue;
                ip_off = sizeof(struct ethhdr);
            }

            if (static_cast<size_t>(n) < ip_off + 20) continue;
            const uint8_t* ip = buf.data() + ip_off;
            size_t ip_len = static_cast<size_t>(n) - ip_off;
            uint8_t version = (ip[0] >> 4) & 0x0F;

            size_t l4_off = 0;
            uint8_t proto = 0;
            if (version == 4) {
                uint8_t ihl = (ip[0] & 0x0F) * 4;
                if (ihl < 20 || ip_len < ihl) continue;
                proto = ip[9];
                l4_off = ihl;
            } else if (version == 6) {
                if (ip_len < 40) continue;
                proto = ip[6];
                l4_off = 40;
            } else {
                continue;
            }

            if (proto != IPPROTO_TCP) continue;
            if (ip_len < l4_off + 20) continue;

            const uint8_t* tcp = ip + l4_off;
            size_t tcp_rem = ip_len - l4_off;
            uint16_t sport = (static_cast<uint16_t>(tcp[0]) << 8) | tcp[1];
            uint16_t dport = (static_cast<uint16_t>(tcp[2]) << 8) | tcp[3];
            uint8_t tcp_hdr_len = ((tcp[12] >> 4) & 0x0F) * 4;
            if (tcp_hdr_len < 20 || tcp_rem <= tcp_hdr_len) continue;

            if (dport != 443) continue;

            const uint8_t* payload = tcp + tcp_hdr_len;
            size_t payload_len = tcp_rem - tcp_hdr_len;
            std::string sni = parse_tls_sni(payload, payload_len);
            if (sni.empty()) continue;

            if (cached_uid >= 0 && !is_game_tcp_socket(sport, cached_uid)) {
                continue;
            }

            record_domain(sni);
        }

        close(sock);
    }

    inline void start() {
        if (g_running.exchange(true)) return;
        g_thread = std::thread(logger_loop);
    }

    inline void stop() {
        if (!g_running.exchange(false)) return;
        if (g_thread.joinable()) g_thread.join();
    }

} // namespace domain_logger
