# Vegas hardware as seen by the recompiled BIOS/game

Everything here was found by disassembling the BIOS in the RAM snapshot and checking it against MAME
traces (`tools/mame/trace_irqs.lua`).

## Physical address map (kseg1 address = physical | 0xA0000000)
| physical | device | runtime model |
|---|---|---|
| 0x00000000–0x007FFFFF | main RAM (8 MB) | host array (`recomp_rdram`) |
| 0x04000000–0x04006FFF | SIO board: control +0x0000, interrupt enable +0x1000, status +0x2000 (one byte each) | `runtime/src/sio.c` |
| 0x04400000–0x04407FFF | NVRAM / RTC (timekeeper; clock in the top 8 bytes) | `runtime/src/nvram.c` |
| 0x04600000 | diagnostic LED / status code (written on every exception) | accepted, stored |
| 0x04800000–0x0480001F | Midway I/O ASIC: control, inputs, DCS2 sound board ports | `runtime/src/ioasic.c` |
| 0x0C000000–0x0C0000FF | Banshee I/O BAR (init/video registers) | `runtime/src/banshee.c` |
| 0x0C000100–0x0C000107 | IDE task file (CMD646) | `runtime/src/ide.c` |
| 0x0C00010A | IDE alternate status / device control | `ide.c` |
| 0x0C000120–0x0C000127 | IDE bus-master DMA (cmd, status, PRD table) | `ide.c` |
| 0x10000000–0x17FFFFFF | PCI window (VRC5074 PCIINIT1: config or memory cycles) | `runtime/src/pci.c` |
| 0x10000000 (memory) | Banshee BAR0: registers (init 0x0, command FIFO 0x80000, 2D 0x100000, 3D 0x200000) | `banshee.c` |
| 0x12000000 (memory) | Banshee BAR1: 16 MB frame buffer / texture memory (+16 MB 3D LFB window) | `banshee.c` |
| 0x1FA00000–0x1FA001FF | NEC VRC5074 ("Nile 4") system controller registers | `runtime/src/nile.c` |
| 0x1FA00300–0x1FA0033F | VRC5074 16550 UART (registers 8 bytes apart) | `nile.c` (output → log) |

## CPU exceptions and interrupts
- The BIOS exception entry (`0x80000180` → `0x80004384`) saves all registers to a frame at
  `0x8002B710` (GPR n at 8n, LO 0x100, HI 0x108, EPC 0x110, CP0 reg n at 0x118 + 8n).
- **Syscalls** (ExcCode 8) jump to `table[code]` at `0x8002B3C8`. See [syscalls.md](syscalls.md).
- **Everything else** goes to `0x80013728(cause)`. An optional hook at `0x800DE3F8` runs first, then a
  per-ExcCode table at `0x8002A388`. ExcCode 0 (interrupt) → `0x800120B4` → `0x8001204C`, which
  computes `pending = Cause & SR` and walks `{mask, ?, handler}` entries at `0x8002A1FC`:

| CPU line | Cause bit | BIOS handler | source |
|---|---|---|---|
| IP4 | 0x1000 | 0x80011D70 → 0x80011C00 | VRC5074 INT2 (UART = source 4, source 5) |
| IP2 | 0x0400 | 0x80010774 | VRC5074 INT0 (source 11 = PCI IDE) |
| IP3 | 0x0800 | 0x80011524 → 0x80011484 | VRC5074 INT1 (source 10 = SIO board, 6 sub-sources at 0xA4001000) |
| IP6 | 0x4000 | 0x80011FA4 | VRC5074 INT4 (sources 8, 12) |
| IP7 | 0x8000 | 0x80012010 | CP0 Count/Compare timer |
| IP5 | 0x2000 | 0x80011DC8 | VRC5074 INT3 |
| IP1 / IP0 | 0x0200 / 0x0100 | 0x80010718 / 0x800106BC | software interrupts |

The runtime (`vegas_poll_interrupts`) enters this path natively. It fills the frame, sets EXL, calls
the recompiled `0x80013728` with a0 = Cause, then restores the context with SR taken from the frame.
Interrupts are delivered at safe points (Count reads, MMIO reads, syscall return).

### VRC5074 interrupt controller
- `+0x88` / `+0x8C` INTCTRL: one 4-bit field per source (0–15): bit 3 = enable, bits 0–2 = CPU line
  (INT0..INT4 = IP2..IP6). Values at game entry: `0x20AA2222` / `0x220C890C`.
- `+0x90` / `+0x94` / `+0x98` INTSTAT: 16-bit pending masks for INT0..INT4.
- `+0xA0` INTCLR: write 1 to clear a latched source.
- The BIOS programs these before the game starts, so `tools/mame/dump_game.lua` saves the register
  block (`work/dumps/entry_nile.bin`) alongside the RAM snapshot.

Measured in MAME (25 s from game entry): IP3 and IP4 each fire about 630 times a second, and IP2 once
per IDE DMA command.

### IDE
Standard ATA task file plus bus-master DMA (PRD table: {u32 address, u16 bytes (0 = 64 KB),
bit 31 = end of table}). The BIOS uses READ DMA (C8) / WRITE DMA (CA) and waits for the completion
interrupt. Status register reads acknowledge it. The model serves sectors from `work/nbashowt.img`;
writes go to an in-memory overlay.

### Floating-point exceptions
The game runs with FCSR = `0x01000C00`: FS (denormals flush to zero) plus the Invalid and
Divide-by-zero enables. Those exceptions arrive as ExcCode 15; the BIOS handler (`0x800135CC`)
dispatches each set cause bit through the table at `0x8002A370` (Inexact, Underflow, Overflow,
Divide-by-zero `0x80012758`, Invalid `0x800130D0`, Unimplemented). The Divide-by-zero and Invalid
handlers first call an optional game hook (`0x800DE474` / `0x800DE478`), then emulate the faulting
instruction from the frame. Frame layout beyond the integer part: FPR n as a 64-bit slot at
`0x218 + 8n` (saved with FR=1, so slot 2k holds the FR=0 pair f2k/f2k+1), FIR at `0x318`, FCSR at
`0x31C`.

## Midway I/O ASIC (0x04800000) and DCS2 sound board
Measured on the real game with `tools/mame/trace_mmio.lua` (memory taps, installed after boot because
the VRC5074 remaps its windows during BIOS start-up). The registers are 16-bit, 2 bytes apart:

| offset | access | role |
|---|---|---|
| +0x00 | R/W | control (reads back 0x4000) |
| +0x04 | W | 4-bit select. Bits 8–11 of +0x1A read 0 for selects 0–7, 0xF for 8/9, the select value for 0xA–0xF |
| +0x10 | W | data word to the sound board |
| +0x12 | R/W | sound board reset: 0 = held in reset, 1 = run |
| +0x14 | R | sound status: 0x80 = board ready for data, 0x40 = reply available, 0x08 always set |
| +0x16 | R/W | reply word from the sound board; written 0 to acknowledge |
| +0x18 / +0x1A / +0x1C | R | input ports, active low (idle: FFFF / F0FF / FFFF) |
| +0x1E | R | DIP switches (0xBE7E as shipped) |

### DCS2 boot monitor protocol
`vegassio.bin` from the ROM set is the sound board's boot ROM (MAME loads it into the `dcs` region).
After reset is released the board replies `0x000A` (ready), and then:

| command | reply |
|---|---|
| 0x6A | 0xD106 (version), 0x000A |
| 0x7A | 0x005A, 0x000A |
| 0x8A, w | ~w, 0x000A (walking-bit bus test) |
| 0x9A | 0x6528 (ROM checksum), 0x000A |
| 0x3A / 0x4A / 0xAA | 0xCC01 / 0xCC02 / 0xCC04 (memory tests), 0x000A |
| 0x1A, start, end, type, data… | 16-bit sum of the data words, 0x000A. Type 0 = program memory (24-bit words sent as 2 × 16), type 1 = data memory |
| 0x2A | starts the uploaded program, replies 0x000C |

The game boots the board twice and uploads 8 blocks (10,752 program words + 25,088 data words). After
the program starts, the CPU streams sound data and only occasionally sends queries (0x55D2–0x55D4).

### Emulated DCS2 board (runtime/src/adsp2100.c, dcs2.c)
The sound board is now emulated: an ADSP-2104 core (written from the ADSP-2100 family instruction set)
runs the real boot ROM and the game's uploaded sound program. Board details (memory map, SDRC,
latches, control registers) follow MAME's DCS2 2104 notes:

| DSP address | function |
|---|---|
| PM 0x0000–0x01FF | internal program RAM. On reset the first boot-ROM page is copied here ((byte3+1)×8 words, 4 bytes per word) |
| PM 0x0800–0x3FFF | external program SRAM (when SDRC SM_EN) |
| DM 0x0400 | input latch from the host (read). Writing it acknowledges and clears IRQ2 |
| DM 0x0401 / 0x0402 | output latch to the host / output control (high byte shows in SOUNDSTAT) |
| DM 0x0403 | latch status: 0x80 input full, 0x40 output empty, 0x08 FIFO empty |
| DM 0x0480–0x0483 | SDRC: SRAM banks, boot-ROM page window, 4 MB DRAM page window, security register |
| DM 0x0800–0x37FF | external data SRAM (two banks) |
| DM 0x3800–0x39FF | internal data RAM |
| DM 0x3FE0–0x3FFF | ADSP control registers: SPORT1 autobuffer, timer, system control |

- Host write → input latch + **IRQ2**. SOUNDSTAT bit 7 = input empty, bit 6 = output full.
- **SPORT0 receive interrupt at 1 kHz** while system control bit 12 is set: the sound program's tick.
  Its handler sends one queued reply per tick, when the output latch is empty.
- **Timer**: TSCALE/TCOUNT/TPERIOD. The ROM's and the program's handlers only refresh DRAM.
- **SPORT1 autobuffered transmit** → audio. The rate is 16 MHz / (2×(SCLKDIV+1)) / 32 with internal
  clock, else 31,250 Hz. IRQ1 fires on buffer wrap. Samples go to a ring buffer (`dcs2_audio_read`).
- The DSP runs at 16 MHz in step with virtual time (`dcs2_sync`, converting absolute time).

Verification: `./build/ioasic_replay work/dumps/sound_trace.txt work/bios/vegassio.bin` replays the MAME
sound-port trace (400k words) through the emulated board. **All 120 replies match the real board**
(boot monitor, 8 upload checksums, program start, and the program's query replies).
`tools/mame/trace_dsp.lua` records MAME's DSP instruction trace for comparison.

Without the boot ROM the I/O ASIC falls back to the stand-in board (boot monitor only).
