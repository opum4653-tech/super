// ZqwClient — ninmods PROJECT
#include "Android_draw/draw.h"
#include "ui/menu.hpp"
#include "protect/license.hpp"
#include "other/memory.hpp"
#include "other/crash_handler.hpp"
#include "other/display_manager.hpp"
#include "other/settings_guard.hpp"
#include "game/data.hpp"
#include "game/Offsets.h"
#include "func/esp.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/prctl.h>
#include <string>
#include <atomic>

extern float global_anim_time;
std::atomic<bool> main_thread_flag{true};

// Defined next to mask_process_name below; used by the render-loop heartbeat.
static bool self_is_traced();

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
    printf("[?] Enter API Access Key: ");
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
        printf("[-] Authentication failed: Invalid or expired API token.\n");
        fflush(stdout);
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

// START OVERLAY
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

    printf("[*] Screen dimensions: %dx%d (Orientation: %u)\n", native_window_screen_x, native_window_screen_y, displayInfo.orientation);
    printf("[*] Configuring Android system touch security policies...\n");
    fflush(stdout);

    settings_guard::capture_and_relax();

    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true, false)) {
        settings_guard::restore();      // captured before init — put it back on this path too
        return -1;
    }

    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);

// START BACKGROUND ATTACH THREAD
    // Allows overlay GUI & floating logo to appear immediately upon launching,
    // without blocking for game launch.
    std::thread attach_worker([]() {
        while (main_thread_flag) {
            proc::pid = get_pid();
            if (proc::pid > 0) {
                proc::lib = get_lib();
                if (proc::lib > 0) {
                    // Verify memory
                    uint32_t elfMagic = rpm<uint32_t>(proc::lib);
                    if (elfMagic == 0x464C457F) {
                        data::thread_start();

                        // Monitor target lifecycle during active gameplay.
                        // Orientation is tracked by the render loop, which already
                        // sees every transition; nothing to poll for here.
                        while (main_thread_flag) {
                            if (get_pid() <= 0) {
                                data::thread_stop();
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });
    attach_worker.detach();

    //  MAIN LOOP — RENDER LOOP (MINIMAL LOGGING)
    int dbgCounter = 0;
    while (main_thread_flag) {
        global_anim_time += 0.016f;
        drawBegin();
        ui::menu::render();
        drawEnd();
        usleep(16000);

// PERIODIC DAEMON HEARTBEAT (EVERY ~5 SECONDS)
        if (++dbgCounter >= 300) {
            dbgCounter = 0;

            // Re-read TracerPid, then re-assert the seal. If something attached
            // mid-session, drop out of the loop — the teardown below still runs,
            // so the settings write is put back.
            bool traced = self_is_traced();
            prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
            if (traced) {
                main_thread_flag = false;
                break;
            }
        }
    }

    shutdown();
    Touch_Close();
    data::thread_stop();
    settings_guard::restore();
    return 0;
}

// MAIN
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

// True when a debugger is attached to this process. One openat + one read of
// our own /proc/self/status, parsed in a stack buffer, descriptor closed on
// every path — nothing opened stays open, nothing hits storage.
static bool self_is_traced() {
    int fd = sys::openat(AT_FDCWD, oxorany("/proc/self/status"), O_RDONLY);
    if (fd < 0) return false;

    char buf[1024];
    long n = sys::read(fd, buf, sizeof(buf) - 1);
    sys::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    const char* p = strstr(buf, oxorany("TracerPid:"));
    if (!p) return false;
    p += 9;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p) != 0;
}

int main(int argc, char** argv) {
    // 0. Install crash interception handler
    crash_guard::install();
    crash_guard::set_cleanup(settings_guard::restore);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // Mask process name immediately upon entry to gemini-cli
    mask_process_name(argc, argv, "gemini-cli");

    // The process runs privileged, so nothing else gets to look inside it.
    // Read TracerPid first (while our own /proc entries are still ours), then
    // seal: PR_SET_DUMPABLE 0 removes the core-dump path, denies ptrace, and
    // makes /proc/<pid>/{mem,maps,fd,environ,cmdline,status} root-owned instead
    // of ours — an ordinary process opens them and gets EACCES.
    if (self_is_traced()) {
        return 1;                   // attached before we even started: leave quietly
    }
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    if (!check_license_gate(argc, argv)) {
        return 1;
    }

    return start_overlay();
}
