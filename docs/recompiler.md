# Recompiler — status and findings

## Build
```bash
cmake -S . -B build -G Ninja && cmake --build build                       # builds nbarecomp
./build/tools/recomp/nbarecomp generate-vegas work/dumps/entry_ram.bin    # BIOS + game -> generated/*.c
cmake -S . -B build -G Ninja && cmake --build build                       # picks up generated/, builds smoke
./build/smoke work/dumps/entry_ram.bin work/dumps/entry_regs.json 400     # run the recompiled game on stubs
```
Other commands: `nbarecomp analyze …` (writes out/analysis/{report.txt,functions.csv,game.s}),
`nbarecomp disasm <bin> <addr> [count]`.
`tools/mame/run_dump.sh` regenerates `work/dumps/entry_ram.bin` + `entry_regs.json` (RAM and CPU registers
at the moment the game's crt0 starts).

## Code generation (tools/recomp/src/cfg.*, codegen.*)
- **Functions**: 1,982 in total. 1,540 come from the call and pointer analysis, plus 442 found in
  unreachable gaps after a function's last reachable instruction (static functions reached only through
  pointers we didn't spot). 103 of them are syscall stubs.
- **Control flow**: each body is walked by following branches from its entry, so strings embedded in the
  code are never emitted. 32 gcc switch tables are recovered and emitted as C `switch` statements;
  18 `jr reg` remain unresolved and go through `recomp_lookup` at runtime.
- **Emitted C**: one C function per guest function (`func_XXXXXXXX(RecompContext*)`), with gotos for
  internal labels.
  - The branch condition is evaluated before the delay slot runs; branch-likely runs its slot only when taken.
  - `jal` is a direct C call. `jalr`, unknown targets and unresolved jumps go through
    `recomp_lookup` (a binary search of `func_table.c`).
  - Jumps and branches that leave the function become tail calls.
- **Registers**: GPRs are 64-bit, with 32-bit results sign-extended. The FPU runs in FR=0 mode (doubles are
  even/odd pairs), confirmed from the SR register. FCSR condition codes are emulated.
- **Memory**: `recomp_phys(addr) = addr & 0x1FFFFFFF`. Physical addresses below 8 MB hit host RAM directly;
  anything else calls `recomp_mmio_read/write` (hardware registers).
- **Floats**: compiled with `-ffp-contract=off`, and `madd.s` is emitted unfused. Float-to-int conversions
  give defined results for NaN and overflow.
- **Not yet translated** (8 sites, all `recomp_unhandled`): 64-bit mul/div, ldl/ldr/sdl/sdr, and
  immediate traps. They probably all sit in strings or dead code.
- All 18 generated files compile cleanly with gcc `-Wall -Wextra`. `-Wmisleading-indentation` caught a
  real codegen bug: tail calls after `if (c)` weren't braced. That's fixed.

## BIOS recompilation (generate-vegas)
`generate-vegas` treats the full 8 MB RAM snapshot as one image (base 0x80000000) with two code sections:
- **BIOS** 0x80004000–0x8001BAC0, rooted at the 100 non-empty syscall table entries: 567 functions,
  18 jump tables, 2 unresolved `jr`.
- **Game** 0x80100000–0x801E5E50: 1,982 functions, as before.
- **Data-resident routines**: `jal` targets that land inside RAM but outside both code sections are
  recompiled as small blocks (target up to `jr ra`). These are the CP0 Count helpers at 0x800295E0/F8
  (BIOS) and 0x803597A8/B0 (game).
In total that's 2,553 functions. Syscalls go through `recomp_bios_syscall()`, which reads the handler
from the table in guest RAM (0x8002B3C8) and calls the recompiled function. The BIOS's assembly
exception/interrupt entry (k0/k1, eret) is not used; the runtime will deliver interrupts natively.

## Smoke test (tests/smoke/smoke.c)
```bash
./build/smoke work/dumps/entry_ram.bin work/dumps/entry_regs.json work/nbashowt.img 300 --quiet
```
Runs the recompiled BIOS and game from the game's entry point on the runtime machine model
(`runtime/src/vegas_machine.c`: CP0 and interrupts; `ide.c`: disk; `nile.c`: VRC5074 interrupt
controller and UART). See [hardware.md](hardware.md).

**Current state** (`SMOKE_SECONDS=150 ./build/smoke … 2000 --quiet`):
- Glide initialises the Banshee and streams command-FIFO packets (21 M words so far, not rendered yet).
- vblank, the BIOS tick, NVRAM, disk reads/writes and the process scheduler (44 processes, 12k
  switches) all work.
- The **emulated DCS2 sound board** boots, receives the sound program, and accepts the sound banks
  (1.5 M words). The game loads about 290 files: fonts, CMOS formats, sound banks, player/ball textures.
- **Next blocker**: about 90 s in, a game process spins in floating-point code (`func_80198F80`, a
  float search loop around `0x80199014`) and the BIOS watchdog fires. This is probably an FPU semantics
  difference in the generated code (compare/NaN/conversion); not investigated yet.

## Game processes (runtime/src/proc.c)
The game's scheduler (main loop `0x801B4270`) resumes a process by restoring its registers, swapping
`sp` with `k1` and doing `jr ra` into it (`0x801B4760`). Processes give control back from sleep
(`0x801B3F98`, 407 call sites) or exit (`0x801B4088`) by swapping stacks and jumping into the middle of
the scheduler (`0x801B4764`). C can't express that, so:
- every game process runs on its own **host fiber** (ucontext on Linux, Fibers on Windows);
- the generator emits `recomp_proc_switch_in` at the scheduler's `jr ra` (then continues at
  0x801B4764) and `recomp_proc_switch_out` at the jumps back;
- a fiber is resumed when its saved resume address and stack match what the scheduler restored;
  otherwise a new fiber starts at that address. All guest register save/restore stays guest code.

## Timing (loop edges)
Every backward branch charges its body size to an instruction budget (`RECOMP_LOOP`, 2,223 edges).
When the budget runs out the runtime advances virtual time and delivers interrupts. This lets loops
that only poll RAM (e.g. waiting for the vblank counter) see interrupts, and keeps timing
deterministic (useful for netplay later).

## Debugging aids
- `cmake -DRECOMP_TRACE_CALLS=ON` records every function entry in a ring buffer. The smoke test dumps
  it at `exit()` or at `BREAK_AT=<hex address>`.
- Smoke env vars: `TRACE_FILES` (open/read/close), `TRACE_IDE` (disk commands), `TRACE_IOASIC`,
  `IDE_DISCARD_WRITES`. On timeout it prints a host backtrace with `func_XXXXXXXX` frames, and any
  console text still queued.

Earlier run, with stub syscalls:
Loads the MAME RAM and register snapshot and calls `func_80100000` (crt0) with stub hooks
(syscalls return 0, hardware reads return 0, Count advances 125,000 per read).
Result: the recompiled code runs crt0 and the game's init through **about 300 BIOS syscalls**, then spins
waiting for something (probably an interrupt-driven counter), and the 10 s watchdog stops it.
Evidence that the generated code is correct:
- syscall 72 is called with `(0x80100000, 0xE5E50)`, exactly the code section's start and size (probably
  a cache flush).
- the game's own printf prints readable messages through syscall 20, including the formatted line
  "The STACK POINTER is OK (807fffa8)".
- the failures that follow are the expected ones for stubbed services: malloc fails (syscall 4 probably
  returns the heap or memory size and we return 0), and the Glide error callback fires (no Banshee).
- **The game made no direct hardware (MMIO) accesses at all**. Hardware goes through BIOS syscalls, so the
  syscall layer is the main job of the runtime.

Syscalls seen so far: 4, 20 (write: fd, buf, len), 21 (many calls with ioctl-like codes 0x69xx),
47, 54 (called for 0..31, probably installing interrupt or exception handlers), 72 (cache flush?), 94.

## Decoder
`tools/recomp/src/mips_decoder.*` decodes the full MIPS IV / R5000 set (integer, 64-bit, COP0, COP1, COP1X).
I cross-checked it against MAME's `unidasm -arch mips3le` over the entire code section (235,408 words).
Every word matches except string bytes embedded in code and one padding word that is never executed
(`0x80140e5c`, which we decode as `tlbwi` and MAME shows as raw cop0).

## Game image layout
| range | contents |
|---|---|
| 0x80100000–0x801E5E50 | code (about 920 KB, 235k instructions) |
| 0x801E5E50–0x8035A470 | data / rodata (starts with the copyright banners and the Glide 3.10 version string) |
| 0x8035A470–0x807FC000 | BSS (zeroed by crt0) |

## Analysis results
- **1,540 functions**: 1,162 found from `jal` targets, 249 from lui/addiu function pointers in code,
  149 from function pointers in data, 2 from tail jumps.
- **Indirect control flow**: 80 `jalr` (function-pointer calls) and 50 `jr` through non-`ra` registers
  (jump tables). The recompiler needs a runtime lookup table from guest address to host function, plus
  jump-table recovery.
- **Strings inside the code section**: 11 places. Code discovery must be recursive (follow control flow),
  not a linear sweep.
- **Floating point**: mostly single precision (S 11,431 vs D 219, W 851). It uses `madd.s`/`msub.s`
  (R5000 fused multiply-add instructions), `trunc.w.s` and `cvt`. crt0 sets FCSR = 0x01000C00:
  flush-to-zero on, and traps on invalid operations and divide-by-zero. Flush and rounding behaviour matter for
  deterministic results, which online play will need later.
- **Calls outside the code**: `0x803597B0` (×20) is `mfc0 v0,$9`, which reads the CP0 Count register, and
  `0x803597A8` (×2) resets Count with `mtc0 zero,$9`. The game measures time with the Count register, which
  runs at half the CPU clock (125 MHz on a 250 MHz R5000). The runtime has to provide it.
- **`break` ×173**: gcc's divide-by-zero / overflow checks after `div`.

## Next steps
1. **Map the BIOS syscall table.** Disassemble the BIOS syscall dispatcher (the BIOS lives in low RAM in
   `entry_ram.bin`, and the exception vector is at 0x80000180) to learn what each of the 103 syscalls does
   (memory, files on the hard disk, PCI, interrupts, timers). Then implement them natively in the runtime.
   This is the biggest single piece.
2. **Interrupts / vblank.** Whatever the game spins on is presumably updated by an interrupt handler
   registered through syscall 54. The runtime has to call those handlers at the right times
   (vblank at 57 Hz, timers).
3. **Process dispatcher.** The game has a cooperative multitasking system ("Starting process dispatcher").
   Its stack-switching code can't map onto plain C calls and will need host fibers or coroutines.
4. **Glide 3 high-level replacement.** Find the Glide entry points in the image (strings such as
   `grTexChromaRangeExt` and the 3.10.00.0126 build tag) and replace them with a modern renderer.
   That is where higher resolution and widescreen come in.
5. Audit the 442 gap-discovered functions and the remaining unreached code for missed entry points.
   Resolve the 18 unresolved `jr`.
