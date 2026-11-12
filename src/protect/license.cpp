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
#include <sys/stat.h>
#include <sys/types.h>
#include <jni.h>
#include <android/log.h>
#include <curl/curl.h>

#define LIC_LOG_TAG "crackmaster_lic"
#define LIC_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LIC_LOG_TAG, __VA_ARGS__)
#define LIC_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LIC_LOG_TAG, __VA_ARGS__)

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
static time_t s_last_seen_ts = 0;
static time_t s_online_expiry_ts = 0;
static bool s_online_verified = false;
static bool s_is_valid = false;
static bool s_clock_tampered = false;
static bool s_initialized = false;
static float s_toast_timer = 0.0f;

static JavaVM* s_jvm = nullptr;
static jobject s_activity = nullptr;

static std::string get_storage_path() {
    if (s_jvm && s_activity) {
        JNIEnv* env = nullptr;
        jint res = s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        bool attached = false;
        if (res == JNI_EDETACHED) {
            if (s_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        }
        if (env) {
            jclass ctxClass = env->GetObjectClass(s_activity);
            if (ctxClass) {
                jmethodID getFilesDir = env->GetMethodID(ctxClass, "getFilesDir", "()Ljava/io/File;");
                if (getFilesDir) {
                    jobject fileObj = env->CallObjectMethod(s_activity, getFilesDir);
                    if (fileObj) {
                        jclass fileClass = env->GetObjectClass(fileObj);
                        if (fileClass) {
                            jmethodID getAbsPath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
                            if (getAbsPath) {
                                jstring pathStr = reinterpret_cast<jstring>(env->CallObjectMethod(fileObj, getAbsPath));
                                if (pathStr) {
                                    const char* chars = env->GetStringUTFChars(pathStr, nullptr);
                                    std::string dir = chars ? chars : "";
                                    if (chars) env->ReleaseStringUTFChars(pathStr, chars);
                                    env->DeleteLocalRef(pathStr);
                                    if (!dir.empty()) {
                                        env->DeleteLocalRef(fileClass);
                                        env->DeleteLocalRef(fileObj);
                                        env->DeleteLocalRef(ctxClass);
                                        if (attached) s_jvm->DetachCurrentThread();
                                        return dir + "/license.dat";
                                    }
                                }
                            }
                            env->DeleteLocalRef(fileClass);
                        }
                        env->DeleteLocalRef(fileObj);
                    }
                }
                env->DeleteLocalRef(ctxClass);
            }
            if (attached) s_jvm->DetachCurrentThread();
        }
    }

    const char* loader_dirs[] = {
        "/data/data/com.andorid.crackmaster/files",
        "/data/user/0/com.andorid.crackmaster/files",
        "/sdcard/Android/data/com.andorid.crackmaster/files"
    };
    for (const char* ld : loader_dirs) {
        mkdir(ld, 0777);
        if (access(ld, W_OK) == 0) {
            return std::string(ld) + "/.nin_license.dat";
        }
    }

    return "";
}

static void save_state_to_disk() {
    std::string path = get_storage_path();
    if (path.empty()) return;
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return;

    fprintf(fp, "v2\n%s\n%lld\n%lld\n",
        s_stored_key.c_str(),
        static_cast<long long>(s_last_seen_ts),
        static_cast<long long>(s_online_expiry_ts)
    );
    fclose(fp);
}

static void evaluate_state(time_t now) {
    if (s_clock_tampered) {
        s_is_valid = false;
        return;
    }

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

    time_t start_date() {
        return 0;
    }

    time_t expiry_date() {
        if (s_online_verified && s_online_expiry_ts > 0) {
            return s_online_expiry_ts;
        }
        return 0;
    }

    void init() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        if (s_initialized) return;

        s_stored_key.clear();
        s_last_seen_ts = 0;
        s_clock_tampered = false;
        s_is_valid = false;

        std::string path = get_storage_path();
        if (!path.empty()) {
            FILE* fp = fopen(path.c_str(), "r");
            if (fp) {
                char header[16] = {0};
                char keyBuf[128] = {0};
                long long ts = 0;
                long long exp_ts = 0;

                if (fscanf(fp, "%15s\n%127s\n%lld\n%lld", header, keyBuf, &ts, &exp_ts) >= 2) {
                    if (strcmp(header, "v2") == 0) {
                        s_stored_key = keyBuf;
                        s_last_seen_ts = static_cast<time_t>(ts);
                        s_online_expiry_ts = static_cast<time_t>(exp_ts);
                        time_t now_check = time(nullptr);
                        if (s_online_expiry_ts > 0 && now_check < s_online_expiry_ts) {
                            s_online_verified = true;
                        }
                    } else if (strcmp(header, "v1") == 0) {
                        s_stored_key = keyBuf;
                        s_last_seen_ts = static_cast<time_t>(ts);
                    }
                }
                fclose(fp);
            }
        }

        time_t now = time(nullptr);

        if (s_last_seen_ts > 0 && now < (s_last_seen_ts - 86400)) {
            s_clock_tampered = true;
            s_is_valid = false;
        } else {
            s_last_seen_ts = std::max(now, s_last_seen_ts);
            evaluate_state(now);
            if (!s_stored_key.empty()) {
                save_state_to_disk();
            }
        }

        s_initialized = true;
    }

    bool is_valid() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        if (!s_initialized) return false;
        time_t now = time(nullptr);
        evaluate_state(now);
        return s_is_valid;
    }

    std::string current_key() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        return s_stored_key;
    }

    static std::string get_device_hwid() {
        const char* env_hwid = getenv("NIN_DEVICE_HWID");
        if (env_hwid && strlen(env_hwid) > 0) {
            return std::string(env_hwid);
        }

        const char* hwid_candidates[] = {
            "/data/data/com.andorid.crackmaster/files/.nin_hwid",
            "/data/user/0/com.andorid.crackmaster/files/.nin_hwid",
            "/sdcard/Android/data/com.andorid.crackmaster/files/.nin_hwid"
        };
        for (const char* path : hwid_candidates) {
            FILE* fp_file = fopen(path, "r");
            if (fp_file) {
                char buf[128] = {0};
                if (fgets(buf, sizeof(buf), fp_file)) {
                    buf[strcspn(buf, "\r\n")] = 0;
                    if (strlen(buf) > 0) {
                        fclose(fp_file);
                        return std::string(buf);
                    }
                }
                fclose(fp_file);
            }
        }

        char buf[128] = {0};
        FILE* fp = popen("getprop ro.serialno 2>/dev/null", "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                buf[strcspn(buf, "\r\n")] = 0;
            }
            pclose(fp);
        }
        if (strlen(buf) > 0 && strcmp(buf, "unknown") != 0) {
            return std::string(buf);
        }
        fp = popen("getprop ro.build.id 2>/dev/null", "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                buf[strcspn(buf, "\r\n")] = 0;
            }
            pclose(fp);
        }
        return strlen(buf) > 0 ? std::string(buf) : "NIN_DEVICE_HWID";
    }

    static size_t curl_write_string_cb(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        auto* s = reinterpret_cast<std::string*>(userp);
        if (s) s->append(reinterpret_cast<const char*>(contents), total);
        return total;
    }

    static bool verify_license_online(const std::string& key) {
        std::string hwid = get_device_hwid();
        std::string server_url = "https://crackmaster.shop";
        const char* env_url = getenv("CRACKMASTER_SERVER_URL");
        if (env_url && strlen(env_url) > 0) {
            server_url = std::string(env_url);
        }

        char response[2048] = {0};
        bool executed = false;

        CURL* curl = curl_easy_init();
        if (curl) {
            std::string curl_resp;
            std::string endpoint = server_url + "/api/verify.php";
            std::string post_fields = "user_key=" + key + "&serial=" + hwid + "&game=FreeFire";

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            headers = curl_slist_append(headers, "Accept: application/json");

            curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &curl_resp);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);

            CURLcode res = curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK && !curl_resp.empty()) {
                strncpy(response, curl_resp.c_str(), sizeof(response) - 1);
                executed = true;
            }
        }

        if (!executed) {
            const char* clients[] = {
                "curl -k -s -m 12",
                "/system/bin/curl -k -s -m 12",
                "/system/xbin/curl -k -s -m 12",
                "wget -q --no-check-certificate -O - -T 12"
            };

            for (const char* client : clients) {
                char cmd[1536];
                if (strstr(client, "wget")) {
                    snprintf(cmd, sizeof(cmd),
                        "%s --header='Content-Type: application/x-www-form-urlencoded' --post-data='user_key=%s&serial=%s&game=FreeFire' %s/api/verify.php 2>/dev/null",
                        client, key.c_str(), hwid.c_str(), server_url.c_str());
                } else {
                    snprintf(cmd, sizeof(cmd),
                        "%s -X POST -H 'Content-Type: application/x-www-form-urlencoded' -d 'user_key=%s&serial=%s&game=FreeFire' %s/api/verify.php 2>/dev/null",
                        client, key.c_str(), hwid.c_str(), server_url.c_str());
                }

                FILE* fp = popen(cmd, "r");
                if (fp) {
                    size_t bytesRead = fread(response, 1, sizeof(response) - 1, fp);
                    pclose(fp);
                    if (bytesRead > 0) {
                        response[bytesRead] = 0;
                        executed = true;
                        break;
                    }
                }
            }
        }

        if (!executed || strlen(response) == 0) {
            printf("[-] Network error: Unable to reach license verification gateway.\n");
            fflush(stdout);
            return false;
        }

        // Match both "status":true and "status": true (server may add space)
        bool status_ok = (strstr(response, "\"status\":true") != nullptr)
                      || (strstr(response, "\"status\": true") != nullptr);

        if (status_ok) {
            time_t now = time(nullptr);
            time_t expiry_ts = 0;

            // Parse expired_date: "Y-m-d H:i:s" UTC+6 (Asia/Dhaka)
            // api_config.json: expired_date field, format "2026-12-01 18:30:00"
            // We build the UTC epoch manually to avoid mktime() local-timezone dependency.
            const char* expPtr = strstr(response, "\"expired_date\":\"");
            if (expPtr) {
                expPtr += 16; // skip past field key + opening quote
                int yr = 0, mo = 0, dy = 0, hr = 0, mi = 0, sc = 0;
                if (sscanf(expPtr, "%d-%d-%d %d:%d:%d", &yr, &mo, &dy, &hr, &mi, &sc) == 6
                    && yr >= 2024 && mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) {

                    // Days per month (non-leap) — good enough: server expiry is days away
                    static const int kDays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

                    // Days since Unix epoch (1970-01-01 UTC) for given UTC+6 date
                    // Convert UTC+6 → UTC: subtract 6 hours = subtract 21600 seconds
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

                    // Seconds since epoch in UTC+6, then subtract 6h to get UTC
                    expiry_ts = (time_t)(days * 86400LL
                                        + hr * 3600LL
                                        + mi * 60LL
                                        + sc
                                        - 21600LL);  // UTC+6 → UTC
                }
            }

            // Fallback: remaining_seconds field
            if (expiry_ts == 0) {
                const char* remPtr = strstr(response, "\"remaining_seconds\":");
                if (remPtr) {
                    long parsed = atol(remPtr + 20);
                    if (parsed > 0) expiry_ts = now + parsed;
                }
            }

            // Last fallback: 24-hour offline grace
            if (expiry_ts == 0) expiry_ts = now + 86400;

            s_last_seen_ts    = now;
            s_stored_key      = key;
            s_online_verified = true;
            s_online_expiry_ts = expiry_ts;
            s_is_valid        = true;
            s_clock_tampered  = false;
            save_state_to_disk();
            return true;
        }

        // api_config.json error field is "reason", not "error"
        const char* errPtr = strstr(response, "\"reason\":\"");
        if (errPtr) {
            char errMsg[256] = {0};
            sscanf(errPtr + 10, "%255[^\"]", errMsg);
            printf("[-] Authentication failed: %s\n", errMsg);
            fflush(stdout);
        }
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

    const char* status_text() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        time_t now = time(nullptr);
        time_t t_exp = expiry_date();

        if (s_clock_tampered) return "INVALID";
        if (s_stored_key.empty()) return "NO KEY";
        if (t_exp > 0 && now >= t_exp) return "EXPIRED";
        if (s_is_valid) return "VALID";
        return "INVALID";
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

    bool should_show_redirect_toast() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        return s_toast_timer > 0.0f;
    }

    void trigger_redirect_toast() {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        s_toast_timer = 2.0f;
    }

    void update_toast_timer(float dt) {
        std::lock_guard<std::mutex> lock(s_lic_mutex);
        if (s_toast_timer > 0.0f) {
            s_toast_timer -= dt;
            if (s_toast_timer < 0.0f) {
                s_toast_timer = 0.0f;
            }
        }
    }

}
