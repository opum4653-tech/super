#pragma once
// ============================================================================
// ZERO-MEMORY-TOUCH GUEST ACCOUNT RESET (MSDK SHARED_PREFS CLEANUP)
// ============================================================================
// Project: crack_master
//
// Force-stops the target game and removes only the verified MSDK guest token
// XML files & guest100067.dat, preserving com.dts.freefiremax.v2.playerprefs.xml
// (HUD layout and game preferences stay intact). Zero game memory writes.
// ============================================================================

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "memory.hpp"

namespace guest_reset {

    inline std::atomic<int> g_state{0}; // 0=idle, 1=running, 2=done, 3=failed

    inline void trigger() {
        int expected = 0;
        if (!g_state.compare_exchange_strong(expected, 1)) {
            if (g_state.load() == 1) return;
            g_state.store(1);
        }

        std::thread([]() {
            const char* script =
                "am force-stop com.dts.freefiremax 2>/dev/null\n"
                "am force-stop com.dts.freefireth 2>/dev/null\n"
                "rm -f /data/data/com.dts.freefiremax/shared_prefs/com.garena.msdk.token_cache.xml "
                "/data/data/com.dts.freefiremax/shared_prefs/com.garena.msdk.persist.fallback.xml "
                "/data/data/com.dts.freefiremax/shared_prefs/com.garena.msdk_crypto_v2.xml "
                "/data/data/com.dts.freefiremax/shared_prefs/com.garena.msdk.SERVICE_PREF_crypto_v2.xml "
                "/data/data/com.dts.freefiremax/shared_prefs/com.beetalk.sdk.account.MigrateGuestActivity.xml "
                "/data/data/com.dts.freefireth/shared_prefs/com.garena.msdk.token_cache.xml "
                "/data/data/com.dts.freefireth/shared_prefs/com.garena.msdk.persist.fallback.xml "
                "/data/data/com.dts.freefireth/shared_prefs/com.garena.msdk_crypto_v2.xml "
                "/data/data/com.dts.freefireth/shared_prefs/com.garena.msdk.SERVICE_PREF_crypto_v2.xml "
                "/data/data/com.dts.freefireth/shared_prefs/com.beetalk.sdk.account.MigrateGuestActivity.xml "
                "/sdcard/com.garena.msdk/guest100067.dat "
                "/storage/emulated/0/com.garena.msdk/guest100067.dat 2>/dev/null\n"
                "exit 0\n";

            FILE* pipe = ::popen("/system/bin/sh", "w");
            bool ok = false;
            if (pipe) {
                std::fwrite(script, 1, std::strlen(script), pipe);
                int rc = ::pclose(pipe);
                ok = (rc != -1);
            }
            if (ok) {
                proc::pid = -1;
                proc::pid_cache = 0;
                proc::lib = 0;
                g_state.store(2);
            } else {
                g_state.store(3);
            }
        }).detach();
    }
}
