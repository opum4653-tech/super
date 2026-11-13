# AGENTS.md — includes/

Vendored and generated headers. Treat as read-only unless the file is explicitly ours.

## Read-only (never edit, never reformat)

- `includes/draw/ImGui/**` — ImGui core and backends.
- `includes/internal/ImGui/**` — second ImGui copy used by the internal render path.
- `includes/font/**` — compiled font blobs.
- `includes/internal/Android_draw`, `Android_touch`, `native_surface` — platform glue,
  upstream-shaped; patch only when a real bug is proven, and keep the diff minimal.

## Rules

- `imgui_demo.cpp`, `imgui_impl_vulkan.cpp`, `stb_image.cpp` are excluded from the build
  by name in `CMakeLists.txt`. Do not remove those excludes and do not re-enable Vulkan.
- Two ImGui trees exist. Before adding a widget, check which one the current translation
  unit already includes. Mixing them produces duplicate-symbol link errors that look like
  unrelated failures.
- New header-only dependencies must also be wired into `jni/Android.mk` /
  `jni/Application.mk`, or the `compile.sh` path breaks silently.
- `includes/protect/` and `includes/network/` are ours — carry the same offset/obfuscation
  rules as `src/`.
