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
fixes (anything in `runtime/src` or `src/host`) live in the engine. Players unpack the new
release over their folder (RecompLauncher does the same), and their existing `nbagame` keeps
working.

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
  function table, the program list (game, operator menu) and pointers to the shared state.
- **Engine → library:** `RecompHostApi` is a table of the 18 runtime hooks the generated code
  calls: MMIO, indirect lookup, syscalls, COP0, FP exceptions, loop slices, process switches, exec and
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
   embedded in the launcher. The operator menu program (DIAG.EXE, run by the test switch) is
   rebuilt the same way from `assets/boot/diag_recipe.bin` (48 bytes: one copy from the disk, plus
   its CRC) into `.build/diag_80100000.bin`. If that fails the build goes on without the menu.
4. **Recompile.** The same code path as `nbarecomp generate-vegas`
   (`tools/recomp/src/vegas_gen.cpp`, linked into the launcher) writes the C for the BIOS, the
   game and DIAG.EXE into `.build/generated/` (see [recompiler.md](recompiler.md#other-programs-exec)).
5. **Compile the game library.** The generated code and `game_module.c` are compiled with
   `zig cc -O2 -fPIC -fvisibility=hidden -DRECOMP_GAME_MODULE`, one job per CPU, and linked
   with `-shared`.
6. **Install the engine.** The prebuilt `nbashowtime` and its SDL3 are copied next to the
   library, unless the build is in the release folder itself, where they already are. On Linux
   the engine uses rpath `$ORIGIN`.
7. **Leave a marker.** `data/build.txt` says what was built from what, and
   `data/built-<abi>.<revision>.txt` (from `recomp_version.h`) marks the build as done for
   RecompLauncher (see below).

The finished folder:

```
NBA Showtime/
  nbashowtime(.exe)  nbagame.so / nbagame.dll  libSDL3.so.0 / SDL3.dll
  data/  nbashowt.chd  vegassio.bin  entry_ram.bin  entry_regs.json  entry_nile.bin  entry_sio.bin
         build.txt  built-<abi>.<revision>.txt
```

The engine looks for `nbagame` and `data/` next to itself. `--game FILE` picks another library.
Without `data/` (a developer build run from the repository root), it falls back to `work/`. A
build takes about 16 s on a 24-thread Linux machine.

## Using it
Window: choose the ROM set (a file, a folder, or drag and drop), the install folder, then
**Build**. **Play** starts the game when the build finishes. A packaged release builds into its
own folder by default; a launcher run from a build tree defaults to `~/Games/NBA Showtime`.

The ROM set players should have is **one zip** holding MAME's `nbashowt.zip` (the BIOS and sound
ROM, read from inside it) and `nbashowt/nbashowt.chd` (streamed straight out of the zip, so it is
never unpacked twice). The CHD can't be inside a nested zip. A MAME rom folder works too.

Command line (no window), used by RecompLauncher's setup step and handy for testing:

    nbalauncher --rom PATH --out DIR [--keep-build] [--jobs N]

`--rom=PATH` and `--out=DIR` work too. `--keep-build` keeps `DIR/.build` (generated C, objects).
Besides the build log it prints `progress <0..1> <step>` lines, and exits with:

| code | meaning |
|---|---|
| 0 | built |
| 1 | the build failed (the reason is the last line on stderr) |
| 2 | bad arguments |
| 3 | not a complete NBA Showtime ROM set (a file is missing, or it's another game or version) |
| 4 | the release is incomplete (no engine, game library sources or Zig) |

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

The DIAG.EXE recipe comes from its image (see [recompiler.md](recompiler.md#other-programs-exec)):

    python3 tools/recipe/make_recipe.py --ram work/game/diag_80100000.bin --out assets/boot/diag_recipe.bin

## Packaging a release
Package from a clang build tree with tracing off:

```bash
cmake --install build-clang --prefix dist/linux-x86_64
```
The result is one folder, which is also where the game gets built:

```
nbashowtime  libSDL3.so.0     the engine (and the launcher's SDL3)
nbalauncher                   the launcher
recomplauncher.json           how RecompLauncher sets the game up
engine/include/  recomp.h  recomp_version.h
engine/module/   game_module.c
toolchain/zig/
```
A packaged launcher only uses the engine next to it, its own `engine/` and `toolchain/zig/`.
`NBA_ZIG_DIR` selects the Zig to bundle (default `toolchain/zig`). It must be the Zig for the
platform the launcher runs on. Because the game is built into the release folder, unpacking a
newer release over it updates the engine and keeps the build (`nbagame`, `data/`), settings and
saves, none of which a release contains. Never ship a `settings.ini`.

Zip it as `NBAShowtime-Recomp-<version>-Windows-x64.zip` (or `-Linux-x64.tar.gz`), with the files
at the top of the zip or in one folder, and attach it to a GitHub release.

`.github/workflows/release.yml` does all of this: push a tag (`git tag v0.2.0`,
`git push origin v0.2.0`) and it builds `NBAShowtime-Recomp-<tag>-Windows-x64.zip` (llvm-mingw,
the MinGW SDL3) and `NBAShowtime-Recomp-<tag>-Linux-x64.tar.gz` (clang on Ubuntu 22.04, so glibc
2.35 or newer, with SDL3 built from source), checks both folders, and publishes them as one
release with `packaging/release-notes.md` as its text. Run it by hand from the Actions tab to
build without publishing, to add files to an existing tag's release, or to mark a new one as a
pre-release (a RecompLauncher beta).

The unpacked size is about 420 MB, almost all of it Zig. Its `lib/libc` holds headers for every
target and could be trimmed to the release's own platform.

## RecompLauncher
`recomplauncher.json` (made from `packaging/recomplauncher.json.in` at configure time) lets
[RecompLauncher](https://recomplauncher.com) install and set up the game:

- **Input:** one `.zip` file, the ROM set as above. RecompLauncher can't look inside a zip, so it
  doesn't check the copy itself; the setup step does, and exits with 3 for a wrong one.
- **Setup step (`prepare`):** `nbalauncher --rom={input:game} --out {install}`, which reports
  `progress` lines. Its `done` file is `data/built-<abi>.<revision>.txt`, with the numbers from
  `recomp_version.h` filled in by CMake. When a release bumps `RECOMP_GAME_ABI` or
  `RECOMP_CODEGEN_REVISION`, the file names a marker that isn't there yet, so the step runs
  again. The engine also exits with 11 for an outdated library.
- **Settings:** `config` is `{install}/settings.ini`, created with the defaults on the first
  start, so the engine runs portable from then on and keeps `nvram.bin`, `disk.bin` and
  `logs/` there too. The settings use the INI sections the engine writes (`video/aspect`).
  Aspect ratios are saved by name (`16:9`), so the launcher's choices match after a change in
  the in-game menu.
- **Engine behaviour:** with `RECOMPLAUNCHER=1` the engine shows no message boxes (the launcher
  shows the `exit_codes` message instead), and with `RECOMPLAUNCHER_MODE=bigpicture` it starts
  full screen for that run. Without a console it logs to `logs/nbashowtime.log`, which
  RecompLauncher attaches to crash reports.

Engine exit codes: 0 quit, 1 other error, 2 bad arguments, 10 no game library, 11 game library
from another version, 12 `data/` missing or damaged, 13 no window or OpenGL 3.3.

To check the file, turn on Developer tools in RecompLauncher (Settings, Developers) and use
**Add a build** on a packaged release folder.

## Status by platform
- **Linux**: done and tested end to end, in both the dev and packaged layouts. Also tested:
  engine-only updates and the version check.
- **Windows**: the engine and launcher build with clang and the MinGW SDL3. Tested: a packaged
  release building the game in place from the single ROM zip on the command line (`nbagame.dll`
  with the bundled Zig), and the engine loading it and running headless. Not tested yet: playing
  it through, and the launcher's window. Both are GUI programs (no console window); run from a
  terminal, they attach to it.

  The engine/library boundary passes only plain C types and function pointers, so an engine
  built with MSVC-ABI clang can still load a MinGW-ABI DLL.
- **macOS**: the structure is ready (Zig targets macOS, and the library is `nbagame.dylib`). Still
  needed: building the engine there, and an app bundle layout.
- **Android**: compiling on the device isn't practical. The planned route is a desktop launcher
  target that builds `nbagame.so` for Android with Zig (or the NDK) and packages it into an APK
  with a prebuilt engine.
