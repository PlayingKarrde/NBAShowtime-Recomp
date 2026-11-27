# Vegas BIOS syscall table (first pass)

## How syscalls work
- The game calls one-instruction stubs at `0x80100060 + 4*n` (`syscall n`); 105 stubs, codes 0–108.
- The BIOS exception handler (`0x80000180` → `0x80004384`) checks Cause.ExcCode == 8, takes the code from
  the instruction (`word >> 6`) and jumps to `table[code]`. The table is at **`0x8002B3C8`** (104 entries,
  codes 0–103; 26–29 are empty). It re-enables interrupts first, except for code 53.
- The handler is entered with the game's `ra` and `a0..a3` untouched, so **each syscall is an ordinary
  function call**: arguments in a0–a3, result in v0 (v0/v1 for 64-bit), returning to the game's caller.
- Regenerate the raw profile: `python3 tools/bios/profile_syscalls.py work/dumps/entry_ram.bin
  work/game/game_80100000.bin build/tools/recomp/nbarecomp`

Confidence: **C** = confirmed from code or strings, **L** = likely (strong evidence), **?** = guess.
"Calls" = number of direct call sites in the game.

## File / stream I/O (fd-based, BIOS file system on the hard disk)
| # | name | calls | conf | evidence |
|---|---|---|---|---|
| 13 | open(path, flags) | 1 | L | parses a `file:` device prefix; returns an fd |
| 0 | close(fd) | 1 | L | fd lookup (`0x80006688`) only |
| 14 | read(fd, buf, len) | 4 | L | fd lookup + device read, same shape as 20 |
| 20 | write(fd, buf, len) | 16 | C | game writes printf text to fd 1/2 |
| 21 | ioctl(fd, cmd, arg) | 52 | L | always fd 4 with cmd `0x69xx` (I/O-board device?) |
| 24 | lseek(fd, off, …) | 1 | ? | fd lookup + position update |
| 2, 3, 9, 10, 18, 50 | other fd / path ops (create, stat, …) | 0–1 | ? | call open/close or fd lookup |
| 5 | load executable | 0 | L | references `GAME.EXE` |
| 45 | exec(file) with CRC check | 0 | C | "Can not open file", "Could not read CRC", "execute address" |
| 91 | install_disk_info(...) | 1 | C | string `install_disk_info()` |
| 33–39 | disk request queue (read/write blocks) | 0–1 | L | 35/36/38/39 go through the request queue (`0x80014390`) |

## Memory, system, misc
| # | name | calls | conf | evidence |
|---|---|---|---|---|
| 4 | process startup / environment setup | 1 | ? | called first by crt0 init; malloc and "environment strings" errors follow when stubbed |
| 47 | get boot/env block | 1 | C | returns const `0x800E1FB8`; crt0 passes it to main |
| 46 | get CPU/board id string | 0 | C | returns a pointer to a BIOS id string (`R5000 … MGVEGAS1.00`) |
| 99, 100 | CPU type / cache info | 0 | L | strings RM5000 / RM5271 / RM7000 |
| 72 | crc32(buf, len) | 7 | C | table CRC (POSIX cksum variant); game CRCs its own code |
| 23 | exit(code) | 1 | C | prints "EXIT Called" |
| 92 / 93 | get / set a BIOS global | 1 / 1 | C | trivial load/store of `0x80029810` |
| 12, 17, 19, 52 | constant stubs (return 0 / 1) | 0 | C | |
| 51 | BIOS version string | 0 | C | `$Revision` string |
| 57 | get 64-bit tick/time (v0:v1) | 3 | L | returns two words of a BIOS counter |
| 56 | timer / delay(n) | 4 | ? | uses the disk/request lock; called with 1, 10 |

## Interrupts / Galileo (GT64010 at 0xBFA00000)
| # | name | calls | conf | evidence |
|---|---|---|---|---|
| 54 | set interrupt handler(slot, fn, …) | 3 | L | touches Galileo int registers; game loops slots 0–31 |
| 55 | (companion of 54: enable/mask?) | 1 | ? | same register set |
| 73 | Galileo DMA / timer op | 3 | ? | GT regs `+0x1F4/0x1F8` |
| 89 | Galileo config (called with all -1) | 1 | ? | GT reg `+0xF8` |
| 53 | big SIO-board routine (no interrupt re-enable) | 0 | ? | SIO regs `0xA4000000–0xA4006000` |

## NVRAM: audits and adjustments
All of 59–71 work on a table of 8-byte records {value, crc32(value)} and return -1 (not initialised),
-6 (bad CRC) or -7 (bad index). They match Midway's audit (coin/play counters) and adjustment (operator
settings) storage.
| # | name | calls | conf |
|---|---|---|---|
| 59–63 | init / count / validate record(s) | 0 | L |
| 64 | get_audit(i, &val) | 33 | L |
| 65 | set_audit(i, val) | 56 | L |
| 66 | increment_audit(i, n) | 59 | L |
| 67–69 | adjustments: validate / get / set | 0 / 37 / 11 | L |
| 70, 71 | interrupt-safe record ops | 17 / 14 | L |
| 74, 75 | NVRAM/RTC block access (`0xA4400000`, top at `+0x7FF7`) | 0 | L |

The records live in a RAM copy of NVRAM offsets 0x800-0x7FF0 at `0x8002C508 + offset` (syscall 58
sets the table up from the game's 0x94-byte layout: audits start at `0x8002CD08`). The BIOS fills
the copy from the chip at power-on (`0x80008968`) and writes changed bytes back from a dirty map at
`0x800344F8 + offset` (`0x80008A30`). The boot snapshot starts after power-on, so the engine
fills the copy itself from the saved `nvram.bin` (`sync_cmos_shadow` in `src/host/main.c`).
The game also keeps records on its hard disk (about 240 sectors written per session, e.g. the
power-up count is restored from there at boot); the engine saves those in `disk.bin`.

The test switch makes the game `exec("diag.exe")` (syscall 45): the operator menu is a separate
program on the disk, not part of the recompiled `GAME.EXE`, so it does not run yet.

## Sound (DCS2 board ports at 0xA4800000: data +0x10, status +0x14/+0x16)
| # | name | calls | conf |
|---|---|---|---|
| 80 | snd_send(cmd) | 118 | L |
| 78 | snd_send_data / read | 23 | L |
| 76, 77 | snd_reset / init | 2 / 6 | L |
| 81, 83 | snd_send_with_reply / read reply | 2 / 11 | L |
| 82 | snd_dump_port() | 0 | C |
| 84–88, 96 | status polls / acks | 0–5 | L |

## Unidentified
1, 6, 7, 8, 11, 15, 16, 22, 25, 30–32, 40–44, 48, 49, 58, 94, 95, 97, 98, 101–103.
They're mostly small, and most have 0–3 game call sites.

## Implications for the runtime
The BIOS isn't a thin layer: it contains a file system, an IDE driver, a disk request queue, interrupt
dispatch and the NVRAM record format. Two ways to provide it:
1. **Recompile the BIOS too** (it's in the same RAM snapshot, 0x80000000–0x800FFFFF) and emulate the few
   devices it touches (IDE disk image, Galileo, SIO, NVRAM, DCS ports). Everything works as on the real
   machine, including the hard-disk file system we haven't decoded.
2. **High-level replacements**: implement each syscall natively (files from an extracted folder, NVRAM in
   a host file, sound and controls straight to SDL). Cleaner and faster, but it needs the file system
   format solved first, and every syscall gets reimplemented.
**Chosen: (1).** The BIOS is recompiled alongside the game (`nbarecomp generate-vegas`), and
syscalls dispatch through the real table. Native replacements come later, one syscall at a time
(sound, controls, NVRAM first).

Hardware the recompiled BIOS has touched so far:
| address | device | notes |
|---|---|---|
| 0xAC000100–0xAC000107 | IDE task file (data, error, count, LBA0-2, drive/head, status/cmd) | READ DMA 0xC8 |
| 0xAC00010A | IDE alternate status / device control | polled while waiting |
| 0xAC000120–0xAC000127 | IDE bus-master DMA (cmd, status, PRD table address) | |
