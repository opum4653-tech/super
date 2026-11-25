#pragma once
// ============================================================================
// TEMPORARY DOMAIN LOGGER -> /storage/emulated/0/Download/firewall.txt
// ============================================================================
// Captures all outbound DNS (UDP:53), HTTP Host (TCP:80), and TLS SNI (TCP:443)
// domains while the game is running, and saves unique domains to:
//   /storage/emulated/0/Download/firewall.txt
//
// See FIREWALL_LOGGER_GUIDE.md for exact remove instructions.
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

    inline FILE* open_log_file(const char* mode) {
        const char* paths[] = {
            "/storage/emulated/0/Download/firewall.txt",
            "/data/media/0/Download/firewall.txt",
            "/sdcard/Download/firewall.txt"
        };
        for (const char* p : paths) {
            FILE* f = fopen(p, mode);
            if (f) return f;
        }
        return nullptr;
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
        while (len > 0 && (raw[len - 1] == '.' || raw[len - 1] == ' ')) --len;
        std::string out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i]))));
        }
        return is_valid_domain(out) ? out : std::string();
    }

    // Parses UDP:53 DNS query QNAME
    inline std::string parse_dns_qname(const uint8_t* data, size_t len) {
        if (len < 13) return {};
        // QR bit must be 0 (query), QDCOUNT >= 1
        if ((data[2] & 0x80) != 0) return {};
        uint16_t qdcount = (static_cast<uint16_t>(data[4]) << 8) | data[5];
        if (qdcount == 0) return {};

        size_t pos = 12;
        std::string name;
        while (pos < len) {
            uint8_t lab_len = data[pos++];
            if (lab_len == 0) break;
            if ((lab_len & 0xC0) != 0 || pos + lab_len > len) return {};
            if (!name.empty()) name.push_back('.');
            name.append(reinterpret_cast<const char*>(data + pos), lab_len);
            pos += lab_len;
            if (name.size() > 253) return {};
        }
        return normalize_domain(name.c_str(), name.size());
    }

    // Parses TCP:80 HTTP Host: header
    inline std::string parse_http_host(const uint8_t* data, size_t len) {
        if (len < 16) return {};
        const char* s = reinterpret_cast<const char*>(data);
        for (size_t i = 0; i + 6 < len; ++i) {
            if ((i == 0 || s[i - 1] == '\n') &&
                (s[i] == 'H' || s[i] == 'h') &&
                (s[i + 1] == 'O' || s[i + 1] == 'o') &&
                (s[i + 2] == 'S' || s[i + 2] == 's') &&
                (s[i + 3] == 'T' || s[i + 3] == 't') &&
                s[i + 4] == ':') {
                size_t start = i + 5;
                while (start < len && (s[start] == ' ' || s[start] == '\t')) ++start;
                size_t end = start;
                while (end < len && s[end] != '\r' && s[end] != '\n' && s[end] != ':') ++end;
                if (end > start) {
                    return normalize_domain(s + start, end - start);
                }
                break;
            }
        }
        return {};
    }

    // Parses TLS ClientHello SNI extension (RFC 6066) from TCP payload
    inline std::string parse_tls_sni(const uint8_t* data, size_t len) {
        if (len < 43) return {};
        if (data[0] != 0x16 || data[1] != 0x03) return {};
        if (data[5] != 0x01) return {};

        size_t pos = 5 + 4 + 2 + 32;
        if (pos >= len) return {};

        uint8_t sess_len = data[pos];
        pos += 1 + sess_len;
        if (pos + 2 > len) return {};

        uint16_t cipher_len = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2 + cipher_len;
        if (pos + 1 > len) return {};

        uint8_t comp_len = data[pos];
        pos += 1 + comp_len;
        if (pos + 2 > len) return {};

        uint16_t ext_total = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        size_t ext_end = pos + ext_total;
        if (ext_end > len) ext_end = len;

        while (pos + 4 <= ext_end) {
            uint16_t ext_type = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
            uint16_t ext_len  = (static_cast<uint16_t>(data[pos + 2]) << 8) | data[pos + 3];
            pos += 4;
            if (pos + ext_len > ext_end) break;

            if (ext_type == 0x0000 && ext_len >= 5) {
                size_t p = pos + 2;
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
        FILE* f = open_log_file("r");
        if (f) {
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
        } else {
            FILE* wf = open_log_file("a");
            if (wf) {
                fprintf(wf, "# firewall.txt domain logger started\n");
                fclose(wf);
            }
        }
    }

    inline void record_domain(const std::string& domain) {
        if (domain.empty()) return;
        std::lock_guard<std::mutex> lk(g_file_mutex);
        if (!g_seen.insert(domain).second) return;

        FILE* f = open_log_file("a");
        if (f) {
            fprintf(f, "%s\n", domain.c_str());
            fclose(f);
        }
    }

    inline void logger_loop() {
        prctl(PR_SET_NAME, "netd_diag", 0, 0, 0);
        load_existing_domains();

        int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (sock < 0) {
            std::lock_guard<std::mutex> lk(g_file_mutex);
            FILE* f = open_log_file("a");
            if (f) {
                fprintf(f, "# socket(AF_PACKET) failed\n");
                fclose(f);
            }
            return;
        }

        struct timeval tv{0, 200000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::vector<uint8_t> buf(4096);

        while (g_running.load(std::memory_order_relaxed)) {
            if (proc::pid <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            struct sockaddr_ll sll{};
            socklen_t sll_len = sizeof(sll);
            ssize_t n = recvfrom(sock, buf.data(), buf.size(), 0,
                                 reinterpret_cast<struct sockaddr*>(&sll), &sll_len);
            if (n <= 0) continue;

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

            if (proto == IPPROTO_UDP) {
                if (ip_len <= l4_off + 8) continue;
                const uint8_t* udp = ip + l4_off;
                uint16_t dport = (static_cast<uint16_t>(udp[2]) << 8) | udp[3];
                if (dport == 53) {
                    record_domain(parse_dns_qname(udp + 8, ip_len - l4_off - 8));
                }
            } else if (proto == IPPROTO_TCP) {
                if (ip_len < l4_off + 20) continue;
                const uint8_t* tcp = ip + l4_off;
                size_t tcp_rem = ip_len - l4_off;
                uint16_t dport = (static_cast<uint16_t>(tcp[2]) << 8) | tcp[3];
                uint8_t tcp_hdr_len = ((tcp[12] >> 4) & 0x0F) * 4;
                if (tcp_hdr_len < 20 || tcp_rem <= tcp_hdr_len) continue;

                const uint8_t* payload = tcp + tcp_hdr_len;
                size_t payload_len = tcp_rem - tcp_hdr_len;

                if (dport == 443) {
                    record_domain(parse_tls_sni(payload, payload_len));
                } else if (dport == 80) {
                    record_domain(parse_http_host(payload, payload_len));
                }
            }
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
