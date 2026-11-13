#pragma once

namespace crash_guard {
    // Installs a global signal handler for fatal faults (SIGSEGV, SIGBUS, SIGFPE,
    // SIGILL, SIGABRT) and for termination signals (SIGTERM, SIGINT, SIGHUP).
    // Termination signals run the registered cleanup so the process can put back
    // whatever it changed before exiting.
    void install();

    // Cleanup invoked before the process exits from a signal. Must be safe to call
    // with no arguments and to call more than once. Optional.
    void set_cleanup(void (*fn)());
}
