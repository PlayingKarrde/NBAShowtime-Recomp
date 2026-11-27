# NBA Showtime Recomp

A static recompilation of the arcade game **NBA Showtime: NBA on NBC** (Midway, 1999) for
modern PCs. The game ran on Midway's Vegas hardware: a MIPS R5000 CPU, a 3dfx Voodoo Banshee
and a DCS2 sound board. Its BIOS and game code are translated from MIPS machine code into C,
then compiled into a native program that runs with a model of the original hardware.

**No game data is included.** You need your own copy of the ROM set. The recompiled code is
produced from it on your machine.

## Features
- Native code with no CPU interpreter. Hardware models cover the IDE disk, the I/O ASIC,
  interrupts, timers, NVRAM and the DCS2 sound board.
- An OpenGL renderer for the Banshee at any resolution.
- Hor+ widescreen that matches the window's aspect ratio automatically (up to 32:9), or a fixed
  ratio (`--aspect 16:9`, `21:9`, any ratio), with the 2D HUD kept at its original proportions.
- Keyboard controls and up to four gamepads, with analog sticks mapped onto the original
  49-way joysticks.
- An in-game menu (Esc, or L3 + R3 on a gamepad) that pauses the game and changes gameplay
  (free play, 2- or 4-player cabinet), graphics, audio and controls, with rebindable keys and
  buttons. Works with a keyboard, gamepad or mouse, and saves the settings.
- The game's saved data (CMOS audits and adjustments, and the records it writes to its hard disk)
  is kept between sessions.
- A launcher that turns the ROM set into a standalone install, so players need neither a
  compiler nor MAME.

## Playing
1. Get the launcher (`nbalauncher`) for your platform.
2. Choose your ROM set: the MAME `nbashowt.zip` together with `nbashowt/nbashowt.chd`, or a
   folder holding the files. Files are recognised by their content, not their names.
3. Choose an install folder and press **Build**. A build takes under a minute.
4. Press **Play**, or run `nbashowtime` from the install folder.

The ROM set must contain:

| file | check |
|---|---|
| BIOS `showtime_mar15_1999.u27` | CRC32 `ff5d620d` |
| sound boot ROM `vegassio.bin` | CRC32 `d1470e23` |
| disk `nbashowt.chd` | CHD v5, SHA-1 `f7c56bc3dcbebc434de58034986179ae01127f87` |

Most updates replace only the engine, and your existing build keeps working. If an update
changes the recompiler's output, the game tells you to run its launcher again. See
[docs/launcher.md](docs/launcher.md).

### Controls
| | Player 1 | Player 2 | Players 3/4 |
|---|---|---|---|
| Stick | arrows | I J K L | gamepad |
| A / B / Turbo | Z / X / C | Q / W / E | gamepad |
| Start / Coin | 1 / 5 | 2 / 6 | 3, 4 / 7, 8 |

Gamepads are assigned to players 1-4 in the order they are connected. Keys and buttons can be
changed in the menu (Controls).

| key | action |
|---|---|
| Esc, or L3 + R3 | menu: pause, settings, quit |
| F11 or Alt+Enter | toggle fullscreen |
| F2 | cycle aspect ratio: Auto (default), 4:3, 16:10, 16:9, 21:9, 32:9 |
| - / = | volume down / up |
| F9 | service credit |
| F10 | test switch (the operator menu is a separate program that does not run yet) |

### Options
```
nbashowtime [--aspect auto|16:9] [--fullscreen] [--window WxH] [--scale S]
            [--vsync] [--volume PERCENT] [--no-sound] [--save-dir DIR] [--no-save] [--perf]
```
The game starts in free play on a 4-player cabinet; both can be changed under Gameplay.

Settings changed in the menu are saved to `settings.ini` in your user data folder
(`~/.local/share/NBAShowtimeRecomp/` on Linux, `%APPDATA%\NBAShowtimeRecomp\` on Windows), or
next to the executable if a `settings.ini` is already there. The game's own saved data goes in the
same folder: `nvram.bin` and `disk.bin` (`--save-dir DIR` puts them elsewhere, `--no-save` keeps
nothing). Options given on the command line apply to that run only.

## Platform status
| platform | status |
|---|---|
| Linux | working, tested end to end |
| Windows | generated code builds with the bundled compiler; engine and launcher untested |
| macOS | planned |
| Android | planned (game library cross-built on desktop) |

## Building from source
You need CMake, Ninja, a C/C++ compiler (clang recommended: the runtime is about 40% slower
when built with gcc) and the SDL3 development files.

```bash
git clone --recursive https://github.com/PlayingKarrde/NBAShowtime-Recomp.git
cd NBAShowtime-Recomp
CC=clang CXX=clang++ cmake -S . -B build-clang -G Ninja
cmake --build build-clang
```

This builds the engine (`nbashowtime`) and the launcher (`build-clang/launcher/nbalauncher`).
The launcher needs [Zig](https://ziglang.org) 0.17, either unpacked into `toolchain/zig/` or
on `PATH`. Run the launcher from the build tree and it uses the engine you just built.

If you cloned without `--recursive`, fetch the libchdr submodule with
`git submodule update --init`.

### Development workflow
Recompiler development works from a MAME dump of the game's entry state in `work/`. That
folder, `rom/` and `generated/` are gitignored, because they hold copyrighted data or code
derived from it.

```bash
./build-clang/tools/recomp/nbarecomp generate-vegas work/dumps/entry_ram.bin --out generated
cmake --build build-clang          # now also builds nbagame.so from generated/
./build-clang/nbashowtime          # reads work/ when there is no data/ folder
```

`tools/mame/` has the Lua scripts used to dump and trace the game in MAME.

## Repository layout
| path | contents |
|---|---|
| `tools/recomp/` | `nbarecomp`: MIPS analysis, disassembly and C code generation |
| `runtime/` | Vegas hardware models (CPU glue, IDE, I/O ASIC, Banshee, DCS2, timers) |
| `src/host/` | the `nbashowtime` engine: SDL3 window, OpenGL renderer, audio and input |
| `launcher/` | `nbalauncher`: ROM set detection, boot snapshot rebuild, Zig build of the game library |
| `assets/boot/` | the boot snapshot recipe and entry register state |
| `tests/` | smoke test and I/O ASIC replay test |
| `tools/mame/`, `tools/bios/`, `tools/glide/`, `tools/recipe/` | research and data tools |
| `docs/` | technical notes |

## Documentation
- [docs/recompiler.md](docs/recompiler.md): running the game, widescreen, recompiler status
  and findings
- [docs/launcher.md](docs/launcher.md): the launcher, the engine/game library split, updates
  and packaging
- [docs/hardware.md](docs/hardware.md): the Vegas hardware as seen by the BIOS and game
- [docs/syscalls.md](docs/syscalls.md): the BIOS syscall table
- [docs/disk-format.md](docs/disk-format.md): the game's hard disk format

## Legal
This project is not affiliated with or endorsed by Midway, Warner Bros., the NBA or NBC.
NBA Showtime and all related names are trademarks of their respective owners. The repository
contains no ROM data, disk images or recompiled game code. Use it only with a ROM set you own.
