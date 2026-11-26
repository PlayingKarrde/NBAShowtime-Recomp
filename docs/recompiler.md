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

**Current state:**
- syscall 4 reads the environment file from the disk through the BIOS file system, the IDE model and
  the DMA completion interrupt;
- the game's console output appears over the emulated UART (about 800 UART interrupts delivered
  through the recompiled BIOS): environment variables, the welcome banner, the code CRC check, the
  stack check;
- the game starts talking to the Midway I/O ASIC at 0xA4800000;
- Glide probes the PCI memory windows (0xB0400000…0xB4000000) and VRC5074 +0x200 for the Voodoo
  Banshee, finds nothing, and the game halts in its Glide error handler.

**Next:** the I/O ASIC (sound handshake, inputs, coin), then PCI configuration and the Banshee probe,
which leads into the Glide replacement.

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
