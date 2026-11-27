# Launcher and game library

`nbalauncher` turns a player's own NBA Showtime ROM set into a playable build. It produces a
standalone folder, and the game runs from there afterwards. Settings belong in the game, not the
launcher.

The game comes in two parts:

| part | contains | where it comes from |
|---|---|---|
| **engine** `nbashowtime(.exe)` + SDL3 | runtime (hardware models), renderer, audio, input, in-game menu | shipped prebuilt; nothing from the ROM |
| **game library** `nbagame.so` / `.dll` / `.dylib` | the recompiled BIOS + game code only | built by the launcher on the player's machine |

The engine loads the library at startup (`src/host/game_loader.c`). Nothing derived from the ROM
ships with the launcher: the recompiled C, the boot snapshot and the disk image are all produced
on the player's machine. The C is compiled there with a bundled [Zig](https://ziglang.org)
toolchain (clang plus a C library for every target), so players don't install a compiler.

## Updates
**Most updates only replace the engine.** Renderer, audio, input, settings and hardware-model
fixes (anything in `runtime/src` or `src/host`) live in the engine. Players copy the new
`nbashowtime` and SDL3 over their install, and their existing `nbagame` keeps working.

**A rebuild is only needed when the recompiler's output changes:** codegen fixes, new or
changed instruction patches (`Program::insn_override`), or new runtime hooks called from
generated code. When you make such a change, bump `RECOMP_CODEGEN_REVISION` in
`runtime/include/recomp_version.h`. The new engine then refuses an old library and tells the
player to run the launcher that came with it. Rebuilding into the same folder skips the CHD copy.
On an outdated library the player sees:

> This version of NBA Showtime needs its game files rebuilt. Run the launcher that came with this
> version and build into the same folder.

`RECOMP_GAME_ABI` covers the interface itself. Bump it when `RecompContext`, `RecompHostApi` or
`RecompGameExports` change. Either mismatch gets the same message.

### The interface (runtime/include/recomp.h)
- **Library → engine:** `recomp_game_init(const RecompHostApi*)` is the library's only exported
  symbol (`runtime/module/game_module.c`). It returns `RecompGameExports`: both versions, the
  function table, and pointers to the shared state.
- **Engine → library:** `RecompHostApi` is a table of the 17 runtime hooks the generated code
  calls: MMIO, indirect lookup, syscalls, COP0, FP exceptions, loop slices, process switches and
  float conversions. It is listed once in `RECOMP_HOST_FUNCS`. Compiled with
  `RECOMP_GAME_MODULE`, the hook names become calls through the library's copy of the table.
- **Shared state:** `recomp_rdram`, `recomp_budget` and `recomp_ws_xmin` live in the library,
  because generated code touches them on every memory access and loop. Inside the library
  they're hidden symbols with direct PC-relative access, as in a static build. The engine,
  compiled with `RECOMP_ENGINE_DYNAMIC`, reaches them through `recomp_game`.
- **Static build:** with neither define, code and runtime link into one program. The smoke test
  and `RECOMP_TRACE_CALLS` builds use this.

### Performance
Times are for 6000 frames unthrottled, on the same machine, with identical output:

| build | time |
|---|---|
| engine + library, both clang/Zig | 41-42 s |
| everything clang/Zig, one static executable | 39-41 s |
| engine gcc + library Zig | 56 s |
| everything gcc, static | 55 s |

The split costs about 3-5%. The compiler matters far more: **the runtime built with gcc is about
40% slower than with clang**, so build release engines with clang (`CC=clang CXX=clang++`). Why
gcc is slower hasn't been investigated yet.

## What a launcher build does
1. **Find the ROM set.** The input can be the distributed zip (`nbashowt.zip` +
   `nbashowt/nbashowt.chd`), a MAME rom folder, or any folder holding the files. Files are
   recognised by content, not name:

   | file | check |
   |---|---|
   | BIOS `showtime_mar15_1999.u27` | 512 KB, CRC32 `ff5d620d` (also inside a nested zip) |
   | sound boot ROM `vegassio.bin` | 32 KB, CRC32 `d1470e23` |
   | disk `nbashowt.chd` | CHD v5 header SHA-1 `f7c56bc3dcbebc434de58034986179ae01127f87` (ver 2.0, Apr 25 1999) |

2. **Install the disk.** The CHD is copied or unpacked to `data/nbashowt.chd`, and the zip CRC
   is checked. The game reads the CHD directly through libchdr (`runtime/src/ide.c`), so the
   3.2 GB raw image isn't needed. If an earlier build left the right CHD there, the copy is
   skipped.
3. **Rebuild the boot snapshot.** The game starts from the 8 MB RAM image the BIOS leaves at
   the game's entry point (`entry_ram.bin`). Developers dump it from MAME, but players don't
   need MAME: almost every byte is a copy of the BIOS ROM or the disk.
   `assets/boot/entry_recipe.bin` (12 KB) lists those copies (141 segments: 213 KB from the ROM,
   3.1 MB from the disk), plus 8.6 KB of bytes the BIOS and loader compute at boot (pointers,
   tables, the clock, the stack top). The launcher replays the recipe and checks the result's
   CRC32. The register state at entry (`entry_regs.json`, `entry_nile.bin`, `entry_sio.bin`) is
   embedded in the launcher.
4. **Recompile.** The same code path as `nbarecomp generate-vegas`
   (`tools/recomp/src/vegas_gen.cpp`, linked into the launcher) writes the C into
   `.build/generated/`.
5. **Compile the game library.** The generated code and `game_module.c` are compiled with
   `zig cc -O2 -fPIC -fvisibility=hidden -DRECOMP_GAME_MODULE`, one job per CPU, and linked
   with `-shared`.
6. **Install the engine.** The prebuilt `nbashowtime` and its SDL3 are copied next to the
   library. On Linux the engine uses rpath `$ORIGIN`.

The finished folder:

```
NBA Showtime/
  nbashowtime(.exe)  nbagame.so / nbagame.dll  libSDL3.so.0 / SDL3.dll
  data/  nbashowt.chd  vegassio.bin  entry_ram.bin  entry_regs.json  entry_nile.bin  entry_sio.bin  build.txt
```

The engine looks for `nbagame` and `data/` next to itself. `--game FILE` picks another library.
Without `data/` (a developer build run from the repository root), it falls back to `work/`. A
build takes about 16 s on a 24-thread Linux machine.

## Using it
Window: choose the ROM set (file, folder, or drag and drop), the install folder (default
`~/Games/NBA Showtime`), then **Build**. **Play** starts the game when the build finishes.

Command line (no window), handy for testing:

    nbalauncher --rom PATH --out DIR [--keep-build] [--jobs N]

`--keep-build` keeps `DIR/.build` (generated C, objects).

## Developing
```bash
git submodule update --init                     # third_party/libchdr
CC=clang CXX=clang++ cmake -S . -B build-clang -G Ninja
cmake --build build-clang                       # nbashowtime, nbagame.so (if generated/ exists), nbalauncher
./build-clang/nbashowtime                       # runs build-clang/nbagame.so with work/ data
./build-clang/launcher/nbalauncher
```
The launcher needs SDL3 development files. CMake fetches Dear ImGui (v1.92.9b) into the build
tree. Run from a build tree, the launcher installs the engine built there and compiles the
library from `runtime/include` and `runtime/module`. A build tree configured with
`RECOMP_TRACE_CALLS=ON` links the game into the executable statically, so it has no engine to
offer the launcher. The launcher finds Zig in `toolchain/zig/` (gitignored: unpack a release
from ziglang.org there), or on `PATH`.

If the boot snapshot ever changes (a new MAME dump), regenerate the recipe:

    python3 tools/recipe/make_recipe.py      # reads work/dumps/entry_ram.bin, work/bios, work/nbashowt.img
    cp work/dumps/entry_{regs.json,nile.bin,sio.bin} assets/boot/

## Packaging a release
Package from a clang build tree with tracing off:

```bash
cmake --install build-clang --prefix dist/linux-x86_64
```
The result:

```
nbalauncher  libSDL3.so.0               the launcher
engine/bin/      nbashowtime  libSDL3.so.0   copied into every install
engine/include/  recomp.h  recomp_version.h
engine/module/   game_module.c
toolchain/zig/
```
A packaged launcher only uses its own `engine/` and `toolchain/zig/`. `NBA_ZIG_DIR` selects
the Zig to bundle (default `toolchain/zig`). It must be the Zig for the platform the launcher
runs on. An engine-only update is just `engine/bin`.

The unpacked size is about 420 MB, almost all of it Zig. Its `lib/libc` holds headers for every
target and could be trimmed to the release's own platform.

## Status by platform
- **Linux**: done and tested end to end, in both the dev and packaged layouts. Also tested:
  engine-only updates and the version check.
- **Windows**: the generated code compiles for `x86_64-windows-gnu` with the bundled Zig. Not
  tested yet:
  - building the engine on Windows (use clang or MinGW; the runtime uses GCC extensions), then
    linking and loading `nbagame.dll`;
  - building the launcher itself.

  The engine/library boundary passes only plain C types and function pointers, so an engine
  built with MSVC-ABI clang can still load a MinGW-ABI DLL. The engine is linked as a console
  program, so a console window opens next to it.
- **macOS**: the structure is ready (Zig targets macOS, and the library is `nbagame.dylib`). Still
  needed: building the engine there, and an app bundle layout.
- **Android**: compiling on the device isn't practical. The planned route is a desktop launcher
  target that builds `nbagame.so` for Android with Zig (or the NDK) and packages it into an APK
  with a prebuilt engine.
