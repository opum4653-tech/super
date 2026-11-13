# AGENTS.md — jni/

Build system for the `gemini-cli` executable. `Android.mk` is authoritative;
`Application.mk` supplies ABI and APP_* flags; `vendor/` and `minitouch/` are third-party.

## Android.mk facts

- Module name is `gemini-cli`, type `BUILD_EXECUTABLE`.
- Sources come from `find` over `../src` and `../includes/draw` — not a hardcoded list.
  `imgui_demo.cpp`, `backends/imgui_impl_vulkan.cpp`, and `stb_image.cpp` are filtered out
  by full path. If a file is renamed, update the `filter-out` list or it enters the build.
- Prebuilt static libs declared here, all from `includes/network/`:
  `curl` → `lib/$(TARGET_ARCH_ABI)/libcurl.a`, `ssl` → `libssl.a`, `crypto` → `libcrypto.a`.
  Their include dirs are exported via `LOCAL_EXPORT_C_INCLUDES`.
- `vendor/libevdev/Android.mk` is included as a sub-module; it redefines `LOCAL_PATH`, so the
  main module resets `LOCAL_PATH := $(ROOT_JNI_PATH)` afterwards. Preserve that restore line.
- `LOCAL_CPPFLAGS` is a hard assignment (`:=`), not `+=`. Appending with `+=` below it is
  required; using `:=` again will silently drop every earlier flag (`-Oz`, `-fvisibility=hidden`,
  `-ffunction-sections`, `-fmerge-all-constants`).
- `-fvisibility=hidden` plus `-fvisibility-inlines-hidden` — anything that must be exported
  needs an explicit visibility attribute.

## Gotchas

- `Application.mk` sets `APP_CPPFLAGS := -std=c++17 -fno-rtti`. This wins over the `-frtti`
  in `Android.mk` and over the `-frtti` in `LOCAL_CPPFLAGS`. No exceptions? Exceptions are
  enabled (`-fexceptions`); RTTI is not. Do not introduce `dynamic_cast` or `typeid`.
- `APP_STL := c++_static` — the C++ runtime is inside the binary. Do not switch to `c++_shared`
  without checking that no other component links a different STL.
- `APP_PLATFORM := android-24`. API calls newer than 24 need a runtime `dlsym` probe, not a
  direct symbol reference.
- `APP_ABI := arm64-v8a` only. There is no armeabi-v7a or x86 build. Do not add one.
- `-Wl,--no-undefined` is on. A missing static lib surfaces at link time, not at runtime.
- There is no `package_sh.sh` in this repo even though root `compile.sh` calls it.

## vendor/ and minitouch/

- `vendor/glm`, `vendor/libevdev` — upstream, read-only. Do not patch; wrap instead.
- `minitouch/` — touch injection helper. Same rule: treat as vendored.
