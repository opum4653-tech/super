#pragma once

// The overlay relaxes block_untrusted_touches at startup so the injected touch
// stream is accepted. That write is a persistent forensic artifact unless the
// original value is put back.
//
// capture_and_relax() records the pre-existing value of each namespace (or that
// the key was absent) and then relaxes the flag. restore() reverses it exactly:
// a key that was not present before is deleted rather than overwritten, so the
// settings database ends byte-identical to how it was found.
//
// Both sides cost one fork each, run once per session. restore() is safe to call
// from any teardown path, repeatedly, and from the signal handler.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../protect/oxorany.hpp"

namespace settings_guard {

namespace detail {

// One shell invocation reads both namespaces; one writes them back.
// Binary and key names go through oxorany so they do not sit in .rodata.
inline bool read_pair(std::string& global_out, std::string& system_out) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "%s get %s %s 2>/dev/null; %s get %s %s 2>/dev/null",
             oxorany("settings"), oxorany("global"), oxorany("block_untrusted_touches"),
             oxorany("settings"), oxorany("system"),  oxorany("block_untrusted_touches"));

    FILE* fp = popen(cmd, "r");
    if (!fp) return false;

    char line[64] = {0};
    std::string a, b;
    if (fgets(line, sizeof(line), fp)) { a = line; }
    memset(line, 0, sizeof(line));
    if (fgets(line, sizeof(line), fp)) { b = line; }
    pclose(fp);

    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) {
            s.pop_back();
        }
    };
    trim(a);
    trim(b);

    // "null" is what `settings get` prints for a key that does not exist.
    if (a == "null") a.clear();
    if (b == "null") b.clear();

    global_out = a;
    system_out = b;
    return true;
}

// Restore one namespace: delete the row when it did not exist, otherwise write
// the recorded value back. Returns false if the command buffer would overflow.
inline bool restore_ns(const char* ns, const std::string& saved, bool absent) {
    char cmd[256];
    if (absent) {
        snprintf(cmd, sizeof(cmd), "%s delete %s %s 2>/dev/null",
                 oxorany("settings"), ns, oxorany("block_untrusted_touches"));
    } else {
        snprintf(cmd, sizeof(cmd), "%s put %s %s %s 2>/dev/null",
                 oxorany("settings"), ns, oxorany("block_untrusted_touches"), saved.c_str());
    }
    return system(cmd) == 0;
}

} // namespace detail

inline std::atomic<bool> g_captured{false};
inline std::atomic<bool> g_in_restore{false};

// Saved state. Empty string means "key was absent", which restores as a delete.
inline std::string g_saved_global;
inline std::string g_saved_system;

// Capture the current values, then relax the flag. Idempotent.
inline void capture_and_relax() {
    if (g_captured.exchange(true)) return;

    std::string g, s;
    if (!detail::read_pair(g, s)) {
        // Nothing was read, so nothing can be restored — do not write either.
        g_captured.store(false);
        return;
    }

    g_saved_global = g;
    g_saved_system = s;

    char cmd[384];
    snprintf(cmd, sizeof(cmd),
             "%s put %s %s 0 2>/dev/null; %s put %s %s 0 2>/dev/null",
             oxorany("settings"), oxorany("global"), oxorany("block_untrusted_touches"),
             oxorany("settings"), oxorany("system"),  oxorany("block_untrusted_touches"));
    system(cmd);
}

// Put the settings database back exactly as it was. Safe to call repeatedly,
// from the exit button, from the normal return path, and from the signal handler.
inline void restore() {
    if (!g_captured.exchange(false)) return;
    if (g_in_restore.exchange(true)) return;   // re-entry guard for signal paths

    const bool global_absent = g_saved_global.empty();
    const bool system_absent = g_saved_system.empty();

    detail::restore_ns(oxorany("global"), g_saved_global, global_absent);
    detail::restore_ns(oxorany("system"),  g_saved_system, system_absent);

    g_saved_global.clear();
    g_saved_system.clear();
    g_in_restore.store(false);
}

} // namespace settings_guard
