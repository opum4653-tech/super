# AGENTS.md — crack_master (NINMODS External Mod)

Native Android ARM64 read/write overlay. `gemini-cli` is the external mod binary:
it attaches to a game process, resolves offsets at runtime, draws an ImGui overlay,
and gates startup behind a license check.

Two engines exist in the NINMODS system. This repo is the **external** one
(`gemini-cli`). The kernel engine (`ninmods-kernel`) is not here.

## Build

Two independent build paths. Keep both working when you touch source layout.

```sh
# ndk-build path — this is the authoritative one (jni/Android.mk)
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk \
          NDK_APPLICATION_MK=jni/Application.mk

# CMake path — second opinion, currently mirrors the NDK flags only partially
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
cmake --build build
```

- `compile.sh` runs `ndk-build` and then `./package_sh.sh`. **`package_sh.sh` does not exist
  in this repo** — that second step fails. Either it was never committed or it lives
  elsewhere; do not assume `./compile.sh` produces a package.
- `jni/Android.mk` links prebuilt static `libcurl.a`, `libssl.a`, `libcrypto.a` from
  `includes/network/{curl,openssl}/lib/$(TARGET_ARCH_ABI)/`. `CMakeLists.txt` does **not**
  link them. Anything that touches `includes/network/` builds under ndk-build and may not
  build under CMake. Keep that asymmetry in mind before you trust a CMake build.
- Target: `APP_ABI := arm64-v8a` only, `APP_PLATFORM := android-24`, `APP_STL := c++_static`,
  `APP_OPTIM := release`.
- C++17. Note `jni/Android.mk` sets `-frtti` and `jni/Application.mk` sets `-fno-rtti`;
  the later `APP_CPPFLAGS` wins. Code that uses `dynamic_cast` or `typeid` will not link.
- ndk-build links: `curl ssl crypto libevdev` (static) + `log EGL GLESv3 z dl` (system).
  CMake links: `log android EGL GLESv3 z dl` only.
- Link flags that must stay: `--gc-sections --strip-all -pie -z relro -z now -z noexecstack --max-page-size=16384`.
  ndk-build also sets `--build-id=none --no-undefined`. The `16384` page size is required
  for 16 KB-page Android builds — do not drop it.
- Sources are globbed from `src/**/*.cpp` plus `includes/draw/**/*.cpp`.
  Explicitly excluded: `imgui_demo.cpp`, `imgui_impl_vulkan.cpp`, `stb_image.cpp`.
  A new `.cpp` under `src/` is picked up automatically; a new `.cpp` elsewhere is not.
- Build outputs (`obj/`, `libs/`, `*.o`, `*.d`) are gitignored. Never commit them.

## Layout

```
src/main.cpp            entry, boot sequence, license gate, overlay thread
src/aim/aim.hpp         aim assist logic
src/func/esp.hpp        ESP rendering
src/game/data.*         entity/player collection and caching
src/game/math.hpp       view matrix, world-to-screen
src/game/player.hpp     player record shape
src/game/Offsets.h      small offset constants
src/offsets/offsets.hpp the main offset table
src/other/memory.hpp    process attach, RPM/WPM, module base
src/other/display_manager.*, settings_guard.hpp, crash_handler.*, utils.hpp, vector3.h
src/protect/license.cpp license validation (network, RAM-only)
src/protect/oxorany.*   compile-time string obfuscation
src/ui/menu.cpp         ImGui menu, config binding
src/ui/cfg.hpp          config struct and defaults
src/ui/png/             image loading + embedded image headers
includes/draw/          ImGui core and backends (third-party, do not restyle)
includes/font/          embedded font data (generated, do not hand-edit)
includes/internal/      Android_draw, Android_touch, native_surface, ImGui copy
includes/network/       network helpers
includes/protect/       protection helpers
jni/vendor/             glm, libevdev (third-party, read-only)
jni/minitouch/          touch injection
```

## Conventions

- Third-party code is read-only: `includes/draw/ImGui`, `includes/internal/ImGui`,
  `jni/vendor/**`, `src/ui/png/stb_image.h`, `includes/font/*`, `src/ui/png/images/*.h`.
  Never "clean up" or reformat these. Never hand-edit the generated image/font headers.
- Offsets are game-build specific. Treat every numeric offset as version-pinned.
  When you change or add one, record the game version and the signature/derivation
  that produced it in a comment on the same line or directly above it.
  Prefer a runtime derivation (pattern scan / pointer chain) over a bare constant;
  if you ship a bare constant, say which build it was taken from.
- Every `OpenProcess` handle, `mmap`/`malloc`, and thread gets a paired release on
  the failure path, not only the success path.
- Use the existing helpers: `memory.hpp` for read/write, plain `printf` to stdout
  for output (there is no logger and no log file),
  `crash_handler` for SEH/signal capture. Do not add a second memory layer.
- Strings that must not appear in the binary go through `oxorany`.
  Do not put license endpoints, keys, or paths in plain literals.
- UI code lives in `src/ui`. Do not add ImGui calls from `src/game` or `src/aim`;
  expose state and let `menu.cpp` draw it.
- No new dependencies. Adding one means editing both `CMakeLists.txt` and
  `jni/Android.mk` / `jni/Application.mk`, and it must be ARM64-only and header-only
  or statically built.

## Verification

- Build the ARM64 target. A host x86 build is not proof of anything here.
- After touching offsets: enumerate the resolved value at runtime and log it before
  trusting it. A wrong offset that happens to read zero looks like "feature broken".
- After touching `license.cpp`: verify both the online-verify path and the cached-key
  path, plus cancel.
- Compare against `DOCS.md` before changing anything under the dashboard/download flow;
  `DOCS.md` carries the server URL matrix and the forbidden-path rules.
- `MENU_MAINTENANCE.md` describes the menu widget contract. Read it before editing
  `src/ui/menu.cpp`.

## Do not

- Do not commit API keys, license keys, server credentials, or keystores.
- Do not bundle mod packages into the APK assets — see `DOCS.md` §2.2.
- Do not switch download sources off `https://ninmods.smmtor.com`.
- Do not remove the license gate to "test faster".
