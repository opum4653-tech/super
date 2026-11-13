# AGENTS.md — src/

First-party source. Everything here is ours; everything outside is third-party.

## Rules

- New `.cpp` files here are globbed into the build automatically. New `.h/.hpp` are not —
  add the include dir to `CMakeLists.txt` if you create a new subdirectory.
- One concern per directory: `aim/` targeting, `func/` feature rendering, `game/` world
  reads, `offsets/` addresses, `other/` platform plumbing, `protect/` license + obfuscation,
  `ui/` ImGui.
- `src/game/*` must not call ImGui. Expose data; let `src/ui/menu.cpp` draw.
- `src/aim/*` must not allocate per frame. Preallocate at attach.
- Every read goes through `src/other/memory.hpp`. Do not call `process_vm_readv`,
  `pread64` on `/proc/<pid>/mem`, or raw `ReadProcessMemory`-equivalents directly.
- Every offset in `src/offsets/offsets.hpp` and `src/game/Offsets.h` carries a trailing
  comment naming the game build it was taken from. No anonymous constants.
- Literals that must not survive in the binary: `oxorany(...)`.
  License endpoints, keys, and file paths are always obfuscated.
- `src/Font.h`, `src/ui/png/images/*.h`, `src/ui/png/stb_image.h` are generated or
  vendored. Never edit by hand, never reformat.
- `src/main.cpp` owns lifecycle: attach → license gate → overlay thread → teardown.
  Do not start threads from feature code.

## Threading

The overlay runs on its own thread. The render loop and the game-read loop are separate.
Shared state is written by the read loop and read by the draw pass — guard it or make it
single-writer with seqlock-style snapshots. Do not add a mutex to the per-frame draw path.
