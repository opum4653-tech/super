#include "crash_handler.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// Registered teardown. Runs before the process leaves, from both the fatal path
// and the termination path, so anything the process changed is put back.
static void (*s_cleanup)() = nullptr;

static void run_cleanup() {
    if (!s_cleanup) return;
    static volatile sig_atomic_t s_running = 0;
    if (s_running) return;          // signal arrived while already tearing down
    s_running = 1;
    s_cleanup();
    s_running = 0;
}

// Fatal fault: one neutral line on stderr, no logcat, no banner, no description
// of what failed. debuggerd never sees the signal because it is caught here, so
// no tombstone is produced either.
static void fatal_handler(int sig, siginfo_t* info, void* context) {
    (void)info;
    (void)context;

    fprintf(stderr, "[-] native fault (%d)\n", sig);
    fflush(stderr);

    run_cleanup();
    _exit(0);                       // exit 0 keeps the terminal free of signal reports
}

// SIGTERM / SIGINT / SIGHUP: restore state and leave. No output at all — a
// termination signal should not leave a trail behind it.
static void term_handler(int sig) {
    run_cleanup();
    _exit(128 + sig);
}

namespace crash_guard {

    void set_cleanup(void (*fn)()) {
        s_cleanup = fn;
    }

    void install() {
        // Dedicated alternate signal stack so stack overflows are caught too.
        static uint8_t s_alt_stack[SIGSTKSZ * 4];
        stack_t ss{};
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp = s_alt_stack;
        ss.ss_size = sizeof(s_alt_stack);
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sa.sa_sigaction = fatal_handler;

        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS,  &sa, nullptr);
        sigaction(SIGFPE,  &sa, nullptr);
        sigaction(SIGILL,  &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);

        // Termination signals: plain handler, no SA_SIGINFO needed.
        struct sigaction ta;
        memset(&ta, 0, sizeof(ta));
        sigemptyset(&ta.sa_mask);
        ta.sa_flags = SA_RESTART;
        ta.sa_handler = term_handler;

        sigaction(SIGTERM, &ta, nullptr);
        sigaction(SIGINT,  &ta, nullptr);
        sigaction(SIGHUP,  &ta, nullptr);
    }

}
