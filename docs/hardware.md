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

`runtime/src/ioasic.c` implements the boot monitor exactly and keeps the uploaded DSP program and
data (`g_dcs`). `tests/ioasic_replay` replays a MAME trace through it: all boot-monitor replies match
the real board. The running program isn't executed yet (no audio). Its few query replies come from a
small table, and 7 of 120 replies in the trace differ. Real audio needs the ADSP-2104 itself
(emulation or recompilation of the uploaded program).

## PCI bus (behind the VRC5074)
The window at physical 0x10000000 does configuration or memory cycles depending on VRC5074 PCIINIT1
(`+0xF8`, bits 3:1: 5 = configuration type 0, 3 = memory). The BIOS switches it around every config
access. In config mode one address bit selects the slot (IDSEL):

| address | slot | device |
|---|---|---|
| 0x10400000 / 0x10800000 / 0x12000000 | — | empty (reads 0xFFFFFFFF) |
| 0x11000000 | IDSEL 24 | 3dfx Voodoo Banshee (0x0003121A), class 0x0300 rev 2, BAR0 0x10000000, BAR1 0x12000000, I/O 0x0C000001 |
| 0x14000000 | IDSEL 26 | CMD646 IDE (0x06461095), I/O BARs 0x0C000101… |

The VRC5074's own config space is at `+0x200` (NEC 0x005A1033, host bridge).

## Voodoo Banshee
Glide 3.10 (statically linked) drives the card directly. The model works at register level so that
Glide's own code runs unmodified:
- **init/video registers** with the values the real card returns (e.g. `vidScreenSize` 512×384);
- **status** idle, with the vertical-retrace bit driven by virtual time;
- **command FIFO** in VRAM (base `cmdBaseAddr0` 0xE9E pages, size from `cmdBaseSize0`). Glide writes
  packets straight into frame-buffer memory and polls the read pointer (`+0x8002C`) and depth
  (`+0x80044`). The model currently consumes instantly (read pointer = last write), so Glide never
  waits. **The packets aren't parsed yet: that is the renderer's input.**

## Timers and periodic interrupts
- VRC5074 timers `+0x1C0 + 0x10n`: CTRL low = reload value, CTRL high bit 0 = enable, CNTR at +8.
  They count down at 100 MHz. Only writing the reload value restarts a timer; toggling enable doesn't
  (the BIOS timestamp, syscall 73, pauses T3 around every read). At entry: T0 = 390 (refresh?),
  T2 = 10 Hz, **T3 = 125,000 → 800 Hz BIOS tick**, raising VRC5074 source 5 (IP4). Its handler
  increments `0x8002A270` and runs software timers, including a watchdog that raises a fatal error if
  the game doesn't wait for a frame (syscall 56) for 20 s.
- **Vertical blank** at 57 Hz: SIO sub-source 5 → VRC5074 source 10 → IP3. The BIOS handler
  (`0x800113AC`) acknowledges by clearing/setting SIO control bit 3 and increments the frame counter
  `0x8002A260` (syscalls 56/57).
- SIO sub-source 1 (about 785/s in MAME) is the I/O ASIC interrupt; not modelled yet.
- VRC5074 sources are level-triggered for devices (UART, PCI INTx: SIO, IDE) and edge-latched for
  timer reloads (cleared by INTCLR).
