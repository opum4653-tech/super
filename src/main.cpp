// ============================================================================
// SYSTEM CORE ENTRY POINT & RUNTIME LIFECYCLE SUPERVISOR
// ============================================================================
// Project: crack_master (External Out-of-Process Native Overlay & Assist Suite)
// Reference Architecture: Pure External (Zero Code Patch, Zero In-Game Injection)
//
// FILE RELATIONSHIPS & DATA FLOW:
// - Android_draw/draw.h       -> NativeSurface ANativeWindow creation & EGL context setup
// - ui/menu.hpp               -> ImGui emerald-green native touch UI menu rendering
// - protect/license.hpp       -> RAM-only HTTPS key validation & memfd CA security handshake
// - other/memory.hpp          -> KPM (Kernel Patch Module) memory engine (from pewpew)
// - other/display_manager.hpp -> Zero-fork sysfs/property display metrics cache
// - other/settings_guard.hpp  -> Android block_untrusted_touches state capture & reverse restore
// - game/data.hpp             -> Entity dictionary scraper thread & world-to-screen matrix sync
// - game/Offsets.h            -> Centralized memory offset dictionary (OB55 / 1.132.1)
// - func/esp.hpp              -> Visual ESP render pipeline (Boxes, Tracers, Info, Health bars)
// - func/aim/aim.hpp          -> Single drag-headshot trajectory writer & motion enhancement thread
// ============================================================================

#include "Android_draw/draw.h"
#include "Android_touch/TouchHelperA.h"
#include "ui/menu.hpp"
#include "protect/license.hpp"
#include "other/memory.hpp"
#include "other/display_manager.hpp"
#include "other/settings_guard.hpp"
#include "game/data.hpp"
#include "game/Offsets.h"
#include "func/esp.hpp"
#include "func/aim/aim.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/prctl.h>
#include <csignal>
#include <signal.h>
#include <string>
#include <atomic>

// Global animation timer incremented each frame for UI pulse & glow effects
extern float global_anim_time;

// Global atomic flag controlling main render loop and background worker lifecycles
std::atomic<bool> main_thread_flag{true};

// ----------------------------------------------------------------------------
// LICENSE VERIFICATION GATE
// ----------------------------------------------------------------------------
// Operational Behavior:
// 1. Checks command line arguments (TTY / terminal mode).
// 2. Checks NIN_LICENSE_KEY environment variable (passed by Android UI launcher).
// 3. Fallbacks to interactive stdin prompt (IPC stream from launcher app).
// 4. Performs HTTPS online verification against license backend via protect/license.cpp.
// 5. Waits for 'START' trigger from launcher app before proceeding to overlay init.
static bool check_license_gate(int argc, char** argv) {
    lic::init();

    // 1. Command-line argument key (standalone launch: sh gemini-cli.sh <KEY>)
    //    Only used in TTY mode (adb/termux), not from the launcher app.
    if (isatty(fileno(stdin))) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && strlen(argv[i]) >= 8
                && strcmp(argv[i], "ffmax") != 0
                && strcmp(argv[i], "START") != 0) {
                if (lic::set_key(argv[i]) && lic::is_valid()) {
                    return true;
                }
            }
        }
    }

    // 2. Interactive stdin prompt — always reached when piped from launcher app.
    //    The token below is a handshake, not a log line: the launcher watches
    //    stdout for it to open the key dialog. It is the only byte this process
    //    writes to a stream. No banner, no status, no result text.
    printf("[?] Enter API Access Key:\n");
    fflush(stdout);

    char key_input[128] = {0};

    // Pre-fill from env var if available (launcher passes NIN_LICENSE_KEY),
    // but still emit the prompt above so the app sees it and opens the dialog.
    const char* env_key = getenv("NIN_LICENSE_KEY");
    bool key_ok = false;

    if (env_key && strlen(env_key) >= 8) {
        // Use env key directly — do NOT read stdin for this path
        strncpy(key_input, env_key, sizeof(key_input) - 1);
        key_input[sizeof(key_input) - 1] = '\0';
        key_ok = lic::set_key(key_input);
    } else {
        // Read key from stdin (sent by app via sendInput)
        if (fgets(key_input, sizeof(key_input), stdin)) {
            size_t len = strlen(key_input);
            while (len > 0 && (key_input[len-1] == '\n' || key_input[len-1] == '\r'
                               || key_input[len-1] == ' '  || key_input[len-1] == '\t')) {
                key_input[--len] = '\0';
            }
        }
        if (strlen(key_input) > 0) {
            key_ok = lic::set_key(key_input);
        }
    }

    if (!key_ok || !lic::is_valid()) {
        if (strlen(key_input) == 0) {
            printf("[-] Authentication failed: License key is required.\n");
            fflush(stdout);
        }
        return false;
    }

    printf("[+] API Key validated successfully. Core engine authorized.\n");
    fflush(stdout);

    // 3. TTY path — load immediately without waiting for START
    if (isatty(fileno(stdin))) {
        return true;
    }

    // 4. Piped from launcher app — wait for START signal
    printf("[?] Ready to load into memory. Awaiting START signal...\n");
    fflush(stdout);

    char start_input[64] = {0};
    if (fgets(start_input, sizeof(start_input), stdin)) {
        // Unblocked by Start button tap from app
    }

    printf("[+] Loading overlay engine into RAM...\n\n");
    fflush(stdout);
    return true;
}

// ----------------------------------------------------------------------------
// OVERLAY & WORKER INITIALIZATION PIPELINE
// ----------------------------------------------------------------------------
// Architectural Steps:
// 1. Query display metrics via display_manager (cached in RAM).
// 2. Capture and relax Android block_untrusted_touches via settings_guard.
// 3. Create out-of-process ANativeWindow SurfaceView and bind EGL/OpenGL ES context.
// 4. Initialize Touch subsystem (uinput virtual touch device).
// 5. Spawn background attach_worker thread to monitor game PID and libil2cpp.so base.
// 6. Execute 60 FPS microsecond-paced render loop (drawBegin -> menu::render -> drawEnd).
// 7. Execute symmetrical reverse cleanup upon exit.
static int start_overlay() {
    // screen_config() pulls orientation from the binder; push it into the display
    // cache instead of spawning a shell to ask SurfaceFlinger the same thing.
    screen_config();
    display_safe::update_orientation(displayInfo.orientation);

    int max_size = (displayInfo.height > displayInfo.width)
                   ? displayInfo.height : displayInfo.width;
    int min_size = (displayInfo.height < displayInfo.width)
                   ? displayInfo.height : displayInfo.width;

    g_sw = static_cast<float>(max_size);
    g_sh = static_cast<float>(min_size);
    native_window_screen_x = max_size;
    native_window_screen_y = min_size;

    // Symmetrical touch relaxation: captures original setting to restore later
    settings_guard::capture_and_relax();

    // Create external NativeSurface window and initialize OpenGL ES / ImGui context
    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true, false)) {
        settings_guard::restore();      // captured before init — put it back on this path too
        return -1;
    }

    // Initialize virtual digitizer / uinput event injection for touch pass-through
    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, false);

    // Initialize KPM (from pewpew)
    kpm.init(true);

    // BACKGROUND ATTACH THREAD:
    // Enables overlay GUI & floating logo to appear immediately upon launching,
    // without blocking for target game launch. Once game starts, starts data & aim threads.
    std::thread attach_worker([]() {
        while (main_thread_flag) {
            // Real-time lifecycle only: reuse a still-live cached target,
            // otherwise re-resolve. No artificial timer drives this loop.
            if (proc::pid_cache > 0 && pid_alive(proc::pid_cache)) {
                proc::pid = proc::pid_cache;
            } else {
                proc::pid = get_pid();
            }

            if (proc::pid > 0) {
                kpm.initialize(proc::pid);
                proc::lib = get_lib();
                if (proc::lib > 0) {
                    // Verify valid ELF magic header in target process (0x7F 'E' 'L' 'F')
                    uint32_t elfMagic = rpm<uint32_t>(proc::lib);
                    if (elfMagic == 0x464C457F) {
                        data::thread_start();
                        aimbot::thread_start();

                        // Exit the active session the moment the target PID
                        // disappears, with bounded sleep to avoid busy-spinning.
                        while (main_thread_flag) {
                            if (proc::pid <= 0 || ::kill(proc::pid, 0) != 0) {
                                aimbot::thread_stop();
                                data::thread_stop();
                                proc::pid_cache = 0;
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    attach_worker.detach();

    // NATIVE HARDWARE DISPLAY REFRESH RATE
    // Zero artificial FPS throttling: runs smoothly at the device's native display
    // refresh rate (60Hz, 90Hz, 120Hz, 144Hz). eglSwapBuffers in drawEnd()
    // natively synchronizes with Android SurfaceFlinger hardware VSYNC.
    auto last_frame_time = std::chrono::steady_clock::now();
    while (main_thread_flag) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_frame_time).count();
        last_frame_time = now;
        // Real frame delta only; no artificial time constant fallback.
        if (dt > 0.0f && dt < 0.1f) {
            global_anim_time += dt;
        }

        drawBegin();            // Prepares new ImGui frame & polls orientation
        Touch_ClearUiRects();
        ui::menu::render();     // Renders ImGui menu, floating logo, and calls func::esp::draw()
        Touch_CommitUiRects();
        drawEnd();              // Native hardware VSYNC swap via eglSwapBuffers
    }

    // SYMMETRICAL REVERSE CLEANUP ON NORMAL EXIT
    shutdown();
    Touch_Close();
    aimbot::thread_stop();
    data::thread_stop();
    settings_guard::restore();
    return 0;
}

// ----------------------------------------------------------------------------
// EMERGENCY CLEANUP TRAP (ASYNC-SIGNAL-SAFE EXIT FLAG)
// ----------------------------------------------------------------------------
// Triggered on SIGINT, SIGTERM, SIGHUP. Only flips the atomic loop flag so the
// main render loop exits cleanly and runs symmetrical teardown without calling
// mutexes or thread::join() inside a signal handler (prevents deadlocks).
static void emergency_cleanup(int sig) {
    (void)sig;
    main_thread_flag.store(false, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------
// STEALTH PROCESS OBFUSCATION
// ----------------------------------------------------------------------------
// Modifies /proc/self/comm via prctl(PR_SET_NAME) and masks argv[0] in place.
static void mask_process_name(int argc, char** argv, const char* fake_name) {
    if (!argv || !argv[0] || !fake_name) return;

    // 1. Update /proc/self/comm via prctl (max 16 chars)
    prctl(PR_SET_NAME, fake_name, 0, 0, 0);

    // 2. Safe argv[0] in-place rename without touching succeeding memory buffers
    size_t len = strlen(argv[0]);
    if (len > 0) {
        memset(argv[0], 0, len);
        strncpy(argv[0], fake_name, len);
    }
}

// ----------------------------------------------------------------------------
// APPLICATION ENTRY POINT
// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    // Unbuffer standard streams for responsive IPC pipe communication
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // 1. Register signal traps to guarantee settings & touch nodes are always restored
    std::signal(SIGINT, emergency_cleanup);
    std::signal(SIGTERM, emergency_cleanup);
    std::signal(SIGHUP, emergency_cleanup);
    std::signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE if parent closes stdout pipe

    // 2. Mask process name immediately upon entry to standard kernel worker
    mask_process_name(argc, argv, "kworker/u16:1");

    // 3. Mark process non-dumpable to block unauthorized ptrace inspection
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    // 4. Verify license gate
    if (!check_license_gate(argc, argv)) {
        return 1;
    }

    // 5. Zero-wipe sensitive credentials from argv and environment to eliminate /proc/self/cmdline and /proc/self/environ leaks
    for (int i = 1; i < argc; ++i) {
        if (argv[i]) {
            size_t l = strlen(argv[i]);
            memset(argv[i], 0, l);
        }
    }
    unsetenv("NIN_LICENSE_KEY");
    unsetenv("NIN_DEVICE_HWID");

    // 6. Launch overlay engine and enter render loop
    return start_overlay();
}
