#include "license.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <jni.h>
#include <curl/curl.h>
#include "../protect/oxorany.hpp"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif


static const char kCaBundlePem[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIF9DCCA9ygAwIBAgIRAPJLbRf52a18scn+p4eCaZ8wDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMjYwNTEzMDAwMDAw
WhcNMzIwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsGA1UEChMESVNSRzEQ
MA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIB
ANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9sGNiB0BD1fcOxbSUQCJI
M1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGgYbSQ4OpzI+DG8SGuTlcE
873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBNJAY+OKfX/FUvYKuhjT+n
o49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBznZqvbNPLMXMLFxCb3WTfr
JBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904M+faKx8hnLCpJ15ZqaEg
cNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawTvSZuVvlbRrAlLxIB6pwM
BjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEatnMdmDT5BqnKC92bd0Eh
M1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lzYal+9zTg7C5DALyVOeG/
CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU6H1qGg3DgTOuskf8eahT
MiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9IWhH4YZKh3WnJEIt+oQv
lYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/BAgMBAAGjgeswgegwDgYD
VR0PAQH/BAQDAgEGMBMGA1UdJQQMMAoGCCsGAQUFBwMBMA8GA1UdEwEB/wQFMAMB
Af8wHQYDVR0OBBYEFN7nW2DQIm1AKH0/DQH+pLVStFGUMB8GA1UdIwQYMBaAFHm0
WeZ7tuXkAXOACIjIGlj26ZtuMDIGCCsGAQUFBwEBBCYwJDAiBggrBgEFBQcwAoYW
aHR0cDovL3gxLmkubGVuY3Iub3JnLzATBgNVHSAEDDAKMAgGBmeBDAECATAnBgNV
HR8EIDAeMBygGqAYhhZodHRwOi8veDEuYy5sZW5jci5vcmcvMA0GCSqGSIb3DQEB
CwUAA4ICAQA8spSI95KKfn2W6GMmDpHBJSPaLbsS3W93cijJCRCYAc1fsJgL1FIL
7C0C9ecPOdcwB2fi0Dk2p94j9iTJCxmt5CFSKLRWwnXT2MMSXexVxqoVB79BdWPx
VXETkVme/qYSAuKVHh5Ps+5BixgmwS1JkjSAc+MfrUbNssVEEnH0aEiAh+rotXAV
JSP/Ye7LJPEwD9DWG72vVWbhAcuOf5OLjz57Ctk7MgQHynZ7+PlHJtajroCaIbtC
r6tcZZaAwUQm+jQyeWdV+2hv9deOYFmKeQyjjcSrN5Nadrw+L9DZJLbA1HqeNvLh
BgqpP0fvJq2N6EtD574N6eMI7uMsJTnji2UDz9el5XLSv9fqJMuDQtYVb2oTNoKp
oUqhxPVC0aq4eG5MESaIdn8b5ZGSSeAJLMHXljEdlNza+ncfkviXk1POLnnFdvx8
/gk6M374WbLWFXw8N141B/Rl/tINGfl1TxOIiqtiMYkL02RSGb1kq34BL9NPP27z
RGMuHGnzS3hFIrRTfKxrzUZ9RzQWzEG3K6fJ3r2nqSltkeytis9DIBoFY9VmVyjL
M71DMi+y1+TRSJVClEMwvA4yL++7q9XZx5r5wBRWB4kQTKH5qyoZnDw7iiuh1lID
yDFx8r7i9vIJU5HS3moZLkYWAOilMaV9N56A9Bgb6dNcHkvg3NoaYA==
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

static int make_ca_memfd() {
#ifdef __NR_memfd_create
    int fd = static_cast<int>(syscall(__NR_memfd_create, "lic-ca", MFD_CLOEXEC));
    if (fd < 0) return -1;

    const char* p = kCaBundlePem;
    size_t left = sizeof(kCaBundlePem) - 1;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
#else
    return -1;
#endif
}

static constexpr unsigned char K_TELEGRAM_URL_BYTES[20] = {
    0x0A, 0x16, 0x16, 0x12, 0x11, 0x58, 0x4D, 0x4D, 0x16, 0x4C,
    0x0F, 0x07, 0x4D, 0x0C, 0x0B, 0x0C, 0x0F, 0x0D, 0x06, 0x11
};
static constexpr unsigned char K_TELEGRAM_URL_SEED = 0x62;

static std::string decode_string(const unsigned char* bytes, size_t len, unsigned char seed) {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(bytes[i] ^ seed));
    }
    return s;
}

static std::string normalize(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

static std::mutex s_lic_mutex;
static std::string s_stored_key = "";
static time_t s_online_expiry_ts = 0;
static bool s_online_verified = false;
static bool s_is_valid = false;
static bool s_initialized = false;

static JavaVM* s_jvm = nullptr;
static jobject s_activity = nullptr;

static void evaluate_state(time_t now) {
    if (s_stored_key.empty()) {
        s_is_valid = false;
        return;
    }

    if (s_online_verified) {
        if (s_online_expiry_ts > 0 && now >= s_online_expiry_ts) {
            s_is_valid = false;
            return;
        }
        s_is_valid = true;
        return;
    }

    s_is_valid = false;
}

namespace lic {

    time_t expiry_date() {
        if (s_online_verified && s_online_expiry_ts > 0) {
            return s_online_expiry_ts;
        }
        return 0;
    }

    void init() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        if (s_initialized) return;

        // RAM-only gate. Nothing is read from or written to storage: the key
        // arrives from argv/env/stdin, the verdict comes back over the wire,
        // and both die with this process. A restart starts from "no key".
        s_stored_key.clear();
        s_online_expiry_ts = 0;
        s_online_verified  = false;
        s_is_valid         = false;
        s_initialized      = true;
    }

    bool is_valid() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        if (!s_initialized) return false;
        time_t now = time(nullptr);
        evaluate_state(now);
        return s_is_valid;
    }

    static std::string get_device_hwid() {
        const char* env_hwid = getenv("NIN_DEVICE_HWID");
        if (env_hwid && strlen(env_hwid) > 0) {
            std::string h(env_hwid);
            while (!h.empty() && (h.back() == '\n' || h.back() == '\r' || h.back() == ' ' || h.back() == '\t')) {
                h.pop_back();
            }
            if (h.size() >= 4) return h;
        }

        // Match launcher's Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID)
        FILE* fp = popen("settings get secure android_id 2>/dev/null", "r");
        if (fp) {
            char aid[128] = {0};
            if (fgets(aid, sizeof(aid), fp)) {
                size_t l = strlen(aid);
                while (l > 0 && (aid[l-1] == '\n' || aid[l-1] == '\r' || aid[l-1] == ' ' || aid[l-1] == '\t')) {
                    aid[--l] = '\0';
                }
                if (l >= 8 && strcmp(aid, "null") != 0) {
                    pclose(fp);
                    return std::string(aid);
                }
            }
            pclose(fp);
        }

        char buf[PROP_VALUE_MAX] = {0};
        __system_property_get(oxorany("ro.serialno"), buf);
        if (buf[0] != '\0' && strcmp(buf, "unknown") != 0) {
            return std::string(buf);
        }

        memset(buf, 0, sizeof(buf));
        __system_property_get(oxorany("ro.build.id"), buf);
        return buf[0] != '\0' ? std::string(buf) : "NIN_DEVICE_HWID";
    }

    static size_t curl_write_string_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        auto* s = reinterpret_cast<std::string*>(userp);
        if (s) s->append(reinterpret_cast<const char*>(contents), total);
        return total;
    }

    static std::string url_encode(CURL* curl, const std::string& val) {
        if (!curl) return val;
        char* output = curl_easy_escape(curl, val.c_str(), static_cast<int>(val.length()));
        if (output) {
            std::string res(output);
            curl_free(output);
            return res;
        }
        return val;
    }

    static bool verify_license_online(const std::string& key) {
        std::string hwid = get_device_hwid();
        std::string server_url = "https://crackmaster.shop";
        const char* env_url = getenv("CRACKMASTER_SERVER_URL");
        if (!env_url) env_url = getenv("AUTH_SERVER_URL");
        if (env_url && strlen(env_url) > 0) {
            server_url = std::string(env_url);
            while (!server_url.empty() && server_url.back() == '/') {
                server_url.pop_back();
            }
        }

        char response[4096] = {0};
        bool executed = false;

        auto perform_request = [&](const std::string& base_url, bool insecure_ssl) -> bool {
            CURL* curl = curl_easy_init();
            if (!curl) return false;

            std::string curl_resp;
            std::string endpoint = base_url + "/api/verify.php";

            std::string enc_key = url_encode(curl, key);
            std::string enc_hwid = url_encode(curl, hwid);
            std::string post_fields = "user_key=" + enc_key + "&serial=" + enc_hwid + "&game=FreeFire";

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            headers = curl_slist_append(headers, "Accept: application/json");
            headers = curl_slist_append(headers, "User-Agent: CrackMaster-Core/2.0");

            curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &curl_resp);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

            int ca_fd = -1;
            if (insecure_ssl) {
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            } else {
                ca_fd = make_ca_memfd();
                if (ca_fd >= 0) {
                    char ca_path[64];
                    snprintf(ca_path, sizeof(ca_path), "/proc/self/fd/%d", ca_fd);
                    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path);
                } else {
                    curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
                }
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            }

            CURLcode res = curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            if (ca_fd >= 0) close(ca_fd);

            if (res == CURLE_OK && !curl_resp.empty()) {
                strncpy(response, curl_resp.c_str(), sizeof(response) - 1);
                response[sizeof(response) - 1] = '\0';
                return true;
            }
            return false;
        };

        // 1. Standard HTTPS with CA bundle
        executed = perform_request(server_url, false);
        // 2. Fallback HTTPS without strict CA check
        if (!executed) {
            executed = perform_request(server_url, true);
        }
        // 3. Fallback HTTP if HTTPS port is blocked
        if (!executed && server_url.rfind("https://", 0) == 0) {
            std::string http_url = "http://" + server_url.substr(8);
            executed = perform_request(http_url, true);
        }

        if (!executed || strlen(response) == 0) {
            printf("[-] Authentication failed: Network error - unable to connect to license server.\n");
            fflush(stdout);
            return false;
        }

        // Match status: true (api_config.json §license.response_success)
        bool status_ok = (strstr(response, "\"status\":true") != nullptr)
                      || (strstr(response, "\"status\": true") != nullptr)
                      || (strstr(response, "\"status\":\"true\"") != nullptr)
                      || (strstr(response, "\"status\": \"true\"") != nullptr)
                      || (strstr(response, "\"status\":\"success\"") != nullptr);

        if (status_ok) {
            time_t now = time(nullptr);
            time_t expiry_ts = 0;

            // Flexible parse of expired_date ("Y-m-d H:i:s" UTC+6)
            const char* expPtr = strstr(response, "\"expired_date\"");
            if (expPtr) {
                const char* colon = strchr(expPtr + 14, ':');
                if (colon) {
                    const char* q1 = strchr(colon, '"');
                    if (q1) {
                        int yr = 0, mo = 0, dy = 0, hr = 0, mi = 0, sc = 0;
                        if (sscanf(q1 + 1, "%d-%d-%d %d:%d:%d", &yr, &mo, &dy, &hr, &mi, &sc) == 6
                            && yr >= 2024 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {

                            static const int kDays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
                            long days = 0;
                            for (int y = 1970; y < yr; ++y) {
                                bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
                                days += leap ? 366 : 365;
                            }
                            bool is_leap = (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0));
                            for (int m = 1; m < mo; ++m) {
                                days += kDays[m];
                                if (m == 2 && is_leap) days += 1;
                            }
                            days += dy - 1;

                            // Convert UTC+6 -> UTC (-21600 seconds)
                            expiry_ts = (time_t)(days * 86400LL + hr * 3600LL + mi * 60LL + sc - 21600LL);
                        }
                    }
                }
            }

            // Fallback: remaining_seconds field
            if (expiry_ts == 0) {
                const char* remPtr = strstr(response, "\"remaining_seconds\"");
                if (remPtr) {
                    const char* colon = strchr(remPtr + 19, ':');
                    if (colon) {
                        long parsed = atol(colon + 1);
                        if (parsed > 0) expiry_ts = now + parsed;
                    }
                }
            }

            // Per api_config.json rule 42: "App must treat status=true as logged in"
            // If date parsing was omitted by server, default to safe active timestamp
            if (expiry_ts == 0) {
                expiry_ts = now + 86400 * 30; // 30 days active
            }

            s_stored_key       = key;
            s_online_verified  = true;
            s_online_expiry_ts = expiry_ts;
            s_is_valid         = true;
            return true;
        }

        // Error handling: api_config.json §license.response_error uses "reason"
        char errMsg[256] = {0};
        const char* errPtr = strstr(response, "\"reason\"");
        if (!errPtr) errPtr = strstr(response, "\"message\"");
        if (!errPtr) errPtr = strstr(response, "\"error\"");

        if (errPtr) {
            const char* colon = strchr(errPtr, ':');
            if (colon) {
                const char* q1 = strchr(colon, '"');
                if (q1) {
                    const char* q2 = strchr(q1 + 1, '"');
                    if (q2) {
                        size_t elen = static_cast<size_t>(q2 - (q1 + 1));
                        if (elen > sizeof(errMsg) - 1) elen = sizeof(errMsg) - 1;
                        strncpy(errMsg, q1 + 1, elen);
                        errMsg[elen] = '\0';
                    }
                }
            }
        }

        if (errMsg[0] != '\0') {
            printf("[-] Authentication failed: %s\n", errMsg);
        } else {
            printf("[-] Authentication failed: Invalid or expired API token.\n");
        }
        fflush(stdout);

        s_is_valid = false;
        return false;
    }


    bool set_key(const std::string& key) {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        if (key.empty()) return false;

        if (verify_license_online(key)) {
            return true;
        }

        return false;
    }

    static void open_url_jni(const char* url) {
        if (!url || !*url) return;

        if (s_jvm && s_activity) {
            JNIEnv* env = nullptr;
            jint res = s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
            bool attached = false;
            if (res == JNI_EDETACHED) {
                if (s_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
            }
            if (env) {
                jclass uriClass = env->FindClass("android/net/Uri");
                if (uriClass) {
                    jmethodID parseMethod = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
                    if (parseMethod) {
                        jstring urlStr = env->NewStringUTF(url);
                        if (urlStr) {
                            jobject uriObj = env->CallStaticObjectMethod(uriClass, parseMethod, urlStr);
                            if (uriObj) {
                                jclass intentClass = env->FindClass("android/content/Intent");
                                if (intentClass) {
                                    jmethodID intentInit = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
                                    if (intentInit) {
                                        jstring actionView = env->NewStringUTF("android.intent.action.VIEW");
                                        if (actionView) {
                                            jobject intentObj = env->NewObject(intentClass, intentInit, actionView, uriObj);
                                            if (intentObj) {
                                                jmethodID addFlagsMethod = env->GetMethodID(intentClass, "addFlags", "(I)Landroid/content/Intent;");
                                                if (addFlagsMethod) {
                                                    env->CallObjectMethod(intentObj, addFlagsMethod, 0x10000000);
                                                }
                                                jclass activityClass = env->GetObjectClass(s_activity);
                                                if (activityClass) {
                                                    jmethodID startActivityMethod = env->GetMethodID(activityClass, "startActivity", "(Landroid/content/Intent;)V");
                                                    if (startActivityMethod) {
                                                        env->CallVoidMethod(s_activity, startActivityMethod, intentObj);
                                                    }
                                                    env->DeleteLocalRef(activityClass);
                                                }
                                                env->DeleteLocalRef(intentObj);
                                            }
                                            env->DeleteLocalRef(actionView);
                                        }
                                    }
                                    env->DeleteLocalRef(intentClass);
                                }
                                env->DeleteLocalRef(uriObj);
                            }
                            env->DeleteLocalRef(urlStr);
                        }
                    }
                    env->DeleteLocalRef(uriClass);
                }
                if (attached) s_jvm->DetachCurrentThread();
                return;
            }
        }

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "am start -a android.intent.action.VIEW -d '%s' >/dev/null 2>&1", url);
        system(cmd);
    }

    void open_update_page() {
        std::string url = decode_string(K_TELEGRAM_URL_BYTES, sizeof(K_TELEGRAM_URL_BYTES), K_TELEGRAM_URL_SEED);
        open_url_jni(url.c_str());
    }

}

