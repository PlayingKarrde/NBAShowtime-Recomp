# NBA Showtime disk image — findings so far

Source: `work/nbashowt.img`, extracted with `chdman extracthd` from `rom/…zip:nbashowt/nbashowt.chd`.
The CHD checks out against MAME's record for `nbashowt` (SHA1 f7c56bc3dcbebc434de58034986179ae01127f87),
which MAME lists as "NBA Showtime NBA on NBC (ver 2.0, Apr 25 1999)". The logical size is 3,228,696,576 bytes (6,306,048 sectors).

Boot ROM `showtime_mar15_1999.u27`: little-endian MIPS, banner "BIOS FOR SHOWTIME USING BANSHEE".
MAME shows a 640x480 display at 57 Hz.

## Partition table (sector 0), all fields u32 LE
| off | value | meaning |
|---|---|---|
| 0x00 | 'TRAP' (0x50415254) | magic |
| 0x04 | 5 | ? (version or slot count) |
| 0x0C | 0x603900 | total sectors |
| 0x14.. | {start, size, type} × 4 | partitions, in 512-byte sectors |

| # | start sector | sectors | size | type | notes |
|---|---|---|---|---|---|
| 0 | 0x10 | 0x186A00 | 800 MB | 1 (file) | main FS, has GAME.EXE (616,733 per dir) |
| 1 | 0x186A10 | 0x30D400 | 1.6 GB | 0 (raw) | starts with raw 16-bit pixel-like data — probably streamed assets |
| 2 | 0x493E10 | 0x155CC0 | 700 MB | 1 (file) | second FS, has GAME.EXE (424,455 per dir) |
| 3 | 0x5E9AD0 | 0x186A0 | 50 MB | 0 (raw) | all zero (scratch/audits?) |

## File-system partitions (type 1)
- Superblock at partition offset 0x400: `80 12 01 34` magic (0x34011280), then 8, 0x43, 0x351/0x2EF, 0x10, 4, 0x40, 0x18000.
  - 0x43 = sector of a FAT-like table (partition offset 0x8600) with u16 next-links, 0 = end of chain.
  - 8 is probably sectors per cluster (4 KB).
- Directory entries are 24 bytes: name[12] (each 4-byte word byte-reversed, e.g. "EMAG" "EXE." = GAME.EXE),
  u32 size, u32 timestamp, u32 start index.
- The start index steps by ceil(size/1KB) between neighbouring files, and FAT chains step by +4 per link,
  but the **mapping from index to physical offset is not solved yet**. GAME.EXE's chain is 151 links long.
- File data seen on disk usually begins at +0x200 within a 4 KB-aligned extent.

## Executables found (partition 0, partition-relative offsets)
Both start with `u32 checksum?`, `u32 load address = 0x80100000`, then raw MIPS code (crt0 clears BSS up to 0x807FC000).
- **0x0CA44200**: main game. Contains "Phoenix Vegas", "Mother GOOSE - R5000", "NBA SHOWTIME",
  "Diagnostics - Version", "Apr 21 1999", and Glide 3 (`@#%3.10.00.0126`, "fxPCI for DOS"). Code region is about 1.4 MB.
- **0x004CD200**: second executable (about 1.15 MB). Contains "Voodoo2 InitCode $Revision: 52 $" and Glide env strings —
  possibly an older build or a utility.
- Raw `start*4096` / contiguous reads do **not** reproduce clean file boundaries, so files are fragmented per the FAT.

## Game image dumped from MAME (current source of truth)
`tools/mame/run_dump.sh` boots MAME from a **clean state** every time (its own nvram/diff/cfg dirs under
`work/mame-state`). This matters because the game writes to its hard disk on first boot (MAME keeps that in a
`.dif` file), and later boots then take a different path. `tools/mame/dump_game.lua` saves RAM the first time the
game's crt0 runs.

Boot chain observed: BIOS (ROM, copies itself to low RAM) → 16 KB loader at 0x80100000 → intermediate program at
0x80100000 (crt0 `lui t0,0x8014`) → **game** at 0x80100000 (crt0 `lui t0,0x8036`).

Output: `work/game/game_80100000.bin` + `.json`
- load/entry 0x80100000, image end 0x8035A470 (2,466,928 bytes), BSS 0x8035A470–0x807FC000 (all zero at entry)
- code runs to about 0x801E6000 (about 920 KB of text); the rest is data. Only 41 of 602 image pages change during play.
- sha1 25b81a0d8f01d810cd2090d1f66fda564723df8f
- The executable copy found on disk at partition offset 0x0CA44200 is **not** this build (it is a different program).

## Next steps
1. Disassemble the boot ROM's disk/FS loader. It loads GAME.EXE, so it defines the exact format.
2. Cross-check by booting in MAME (`mame nbashowt`) and dumping RAM 0x80100000+ after load
   (gives the in-memory image directly, a good reference for the recompiler).
