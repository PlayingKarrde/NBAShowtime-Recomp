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
interrupt. Status register reads acknowledge it. The BIOS uses PIO instead (READ/WRITE MULTIPLE,
C4/C5) for requests it flags as not DMA-able, such as some game saves and large streaming reads.
PIO follows the ATA protocol:
- data moves in blocks of the SET MULTIPLE size, which is 16 sectors (the BIOS sets it during boot
  from 0x8003C92C, before the snapshot, so the model starts with it);
- a read raises an interrupt when each block is ready;
- a write starts with DRQ and no interrupt, then interrupts after every block and at the end.

Interrupting per sector instead stalled the BIOS request queue (128 entries at 0x80029B70) at the
"Tonight's Matchup" save. The model serves sectors from `work/nbashowt.img`;
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
| +0x18 | R | IN1, active low: P1 stick bits 0-3, A 0x10, B 0x20, Turbo 0x40; P2 the same << 8 |
| +0x1A | R | IN2: P3 like P1, P4 like P2; bits 8-11 come from the 49-way joystick adapter |
| +0x1C | R | SYSTEM: coin 1/2 0x1/0x2, start 1/2 0x4/0x20, tilt 0x8, test 0x10, service 0x40, coin 3/4 0x80/0x100, start 3/4 0x200/0x400, volume -/+ 0x800/0x1000 |
| +0x1E | R | DIP switches (dumped as BE7E: bit 8 clear = 49-way joysticks) |
| +0x1E | R | DIP switches (0xBE7E as shipped) |

### 49-way joysticks
With DIP bit 8 clear, the stick positions are read through IN2 bits 8-11. The game writes an index
to the select register (+0x04): 0-7 select P1 X, P1 Y, P2 X, …, P4 Y. Each axis has 7 positions
(X 0 = left, Y 0 = up, 3 = centre), and the adapter returns `~{8,C,E,F,3,1,0}[pos] & 0xF`.
Selects 8 and 9 read 0xF. Selects 0xA-0xF read the select value back, which the game uses to
detect the adapter. These are the idle values measured earlier.
The register order was found by test: a coin press on +0x1C credits the game.

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
- **Bank uploads are taken over by the board model** (`upload_write` in dcs2.c), as MAME does
  (`preprocess_stage_2`). After the boot program runs (host word 0x002A), host command 0x55D0 or
  0x55D1, a start and a stop address (two words each) and stop − start + 1 data words go straight
  into sound DRAM; their 16-bit sum is posted as the reply with tag 0x03 in the output-control high
  byte. The sound program never sees them. Its own loader made uploads timing-fragile:
  - It points its IRQ2 vector at a bare RTI and polls the latch itself, then restores the vector
    2–25 DSP cycles after its checksum reply. IRQ2 is edge-triggered (ICNTL bit 2), so a command
    written in that window is lost; nothing acknowledges the latch after that and the game spins in
    sound timeouts (no gameplay sound, missed frames). An I/O ASIC cost of about 0.5 µs avoided it.
  - The game overwrites sound data that is still playing (the matchup music, while the team banks
    load). The decoder (PM 0x0114–0x0164) then queues a status reply (0xEDDE, tag 0x0F) that the
    1 kHz tick sends while the upload is running. The host read it as the checksum and reloaded the
    bank in the next sound directory slot, so every later bank moved 8 sounds along (MAME's run
    ends its directory at DRAM 0x0B38–0x0B3B, ours ended at 0x0B48–0x0B4B).
  Replies the DSP writes during an upload are now held until the host has read the checksum (the
  game drains stray replies after each bank). `DCS_NO_HLE=1` restores the loader path for
  debugging; it needs `IOASIC_COST` at 64.
- I/O ASIC accesses cost about 0.25 µs each (`IOASIC_COST` in vegas_machine.c).
- `DCS_LOG_IO=1` logs latch traffic, uploads, checksum replies and vector writes.
  `DCS_TRACE_ON_CMD=<word> DCS_TRACE_AFTER=<dsp cycle> [DCS_TRACE_LEN=<n>]` single-steps the DSP
  after a given host word.

Verification: `./build/ioasic_replay work/dumps/sound_trace.txt work/bios/vegassio.bin` replays the MAME
sound-port trace (400k words) through the emulated board. **All 120 replies match the real board**
(boot monitor, 8 upload checksums, program start, and the program's query replies).
`tools/mame/trace_dsp.lua` records MAME's DSP instruction trace for comparison.

Without the boot ROM the I/O ASIC falls back to the stand-in board (boot monitor only).

## Voodoo Banshee 3D (runtime/src/banshee.c, voodoo3d.c, raster_sw.c)
Glide streams everything through command FIFO 0 (a 256 KB ring in VRAM at `0xE9E000`, wrapped
with a JMP packet). `banshee.c` executes each packet as soon as its last word is written:

| Packet | Use in this game |
|---|---|
| 0 | JMP back to the FIFO base |
| 1 | register runs (bit 11 of the register address selects 2D space) |
| 3 | setup-unit vertices: float X/Y, packed or float ARGB, Z, Wb/W0, S0/T0; strips, fans, lists |
| 4 | masked register writes (render state) |
| 5 | texture downloads (space 3, linear from `texBaseAddr & 0xFFFFF0`) |

Observed state: 512x384, three colour buffers plus one aux (depth) buffer, a W-buffer with
the "less" depth function, alpha blending (src alpha / 1 - src alpha), alpha test as cut-outs,
textures in RGB565 / ARGB1555 / ARGB4444 modulated by `color0`, no fog, no 3D LFB access.
Frames start with a fast fill and end with `swapbufferCMD`; the displayed buffer is
`leftOverlayBuf`.

Facts used by the renderer:
- The iterated W is 16.48 fixed point. The W-buffer value is a 16-bit pseudo-float: the
  leading-zero count minus 16 in bits 15:12, then the inverted 12 bits below the leading one.
- Texel coordinates are (s/w) / (1/w) in LOD 0 units (LOD 0 is 256 texels on the long side).
  Mip levels follow LOD 0 in VRAM, each at least 4 texels from LOD 3 down.
- The game uses the lower-left origin (fbzMode bit 17). Rows are `yorigin - y`, with
  `yorigin` = miscInit0 bits 29:18 (383).
- Palette entries are written through nccTable 0 I/Q registers with bit 31 set: the index is
  bits 30:24 shifted left 1, plus the register's low bit.

`raster_sw.c` is the reference software renderer. It draws at any integer multiple of
512x384 into private RGBA8 and depth buffers and implements the whole pixel pipeline. The
smoke test dumps frames with
`FRAME_DUMP=dir [FRAME_START=n FRAME_EVERY=n FRAME_SCALE=s FRAME_MAX=n]`.

The OpenGL renderer (`src/host/renderer_gl.c`) uses the same pipeline:
- Triangles are batched while the state snapshot is unchanged.
- A fragment shader implements texture and colour combine, chroma key, alpha test, and the Voodoo
  W/Z depth values (written to `gl_FragDepth`). Attributes use `noperspective` interpolation, as on
  the Voodoo, and textures are divided per fragment.
- Fixed-function state handles blending, the depth function, write masks and scissor clipping.
- Decoded textures are cached per mip chain. A cache entry is invalidated when any 4 KB VRAM page it
  covers is written, which `banshee_vram_touched` tracks.

