#!/usr/bin/env python3
"""Profile the Vegas BIOS syscall handlers found in a MAME RAM snapshot.

For each entry of the BIOS syscall table it disassembles the handler (using
nbarecomp) up to its first top-level `jr ra`, and reports:
  - strings it references (lui/addiu pairs that point at printable text)
  - constants that look like hardware addresses (kseg1 / PCI / ROM space)
  - BIOS functions it calls, and whether it reads a0..a3 / writes v0
  - how often the game calls the syscall (direct jal sites to the stub)

usage: profile_syscalls.py <entry_ram.bin> <game.bin> <nbarecomp> [--md out.md]
"""
import re
import struct
import subprocess
import sys

TABLE = 0x8002B3C8      # syscall table (found from the exception handler at 0x80004384)
NUM = 104
RAM_BASE = 0x80000000
GAME_BASE = 0x80100000


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def cstring(ram, addr, maxlen=80):
    off = addr - RAM_BASE
    if not (0 <= off < len(ram)):
        return None
    s = bytearray()
    while off < len(ram) and len(s) < maxlen:
        c = ram[off]
        if c == 0:
            break
        if not (32 <= c < 127 or c in (9, 10, 13)):
            return None
        s.append(c)
        off += 1
    return s.decode() if len(s) >= 4 else None


def disasm(tool, image, base, addr, count):
    out = subprocess.run([tool, "disasm", image, hex(addr), str(count), "--base", hex(base)],
                         capture_output=True, text=True, check=True).stdout
    lines = []
    for l in out.splitlines():
        p = l.split(None, 2)
        lines.append((int(p[0], 16), int(p[1], 16), p[2] if len(p) > 2 else ""))
    return lines


def handler_body(lines):
    """Linear body up to the first jr ra (+ delay slot) not skipped by a forward branch."""
    body = []
    furthest = 0
    for i, (pc, w, text) in enumerate(lines):
        body.append((pc, w, text))
        m = re.search(r"0x([0-9a-f]{8})$", text)
        if m and re.match(r"(b|j )", text) and not text.startswith("jal"):
            furthest = max(furthest, int(m.group(1), 16))
        if text.startswith("jr      ra") and pc >= furthest:
            if i + 1 < len(lines):
                body.append(lines[i + 1])
            break
        if text.startswith("j ") and pc >= furthest:  # tail jump
            if i + 1 < len(lines):
                body.append(lines[i + 1])
            break
    return body


def profile(ram, body):
    regs_hi = {}
    strings, hw, calls = [], set(), []
    reads_args = set()
    writes_v0 = False
    for pc, w, text in body:
        op = text.split()[0] if text else ""
        args = text[len(op):].replace(" ", "").split(",") if text else []
        for a in ("a0", "a1", "a2", "a3"):
            # register used as a source operand (not as the destination)
            if a in args[1:] or (op in ("sw", "sh", "sb", "sd", "beq", "bne", "move", "jr") and a in args):
                reads_args.add(a)
            if re.search(r"\(" + a + r"\)", text):
                reads_args.add(a)
        if args and args[0] == "v0" and op not in ("sw", "sh", "sb", "sd", "beq", "bne"):
            writes_v0 = True
        if op == "lui":
            regs_hi[args[0]] = int(args[1], 16) << 16
            v = regs_hi[args[0]]
            if (v >> 28) in (0xA, 0xB) or (0x08000000 <= v < 0x20000000):
                hw.add(v)
        elif op in ("addiu", "ori") and len(args) == 3 and args[1] in regs_hi:
            imm = int(args[2], 16) if not args[2].startswith("-") else -int(args[2][1:], 16)
            v = (regs_hi[args[1]] + imm) & 0xFFFFFFFF if op == "addiu" else regs_hi[args[1]] | imm
            s = cstring(ram, v)
            if s:
                strings.append(s)
            elif (v >> 28) in (0xA, 0xB):
                hw.add(v)
        elif op == "jal":
            calls.append(int(args[0], 16))
        m = re.match(r"(l[bhwd]u?|s[bhwd])\s", text)
        if m:
            mm = re.search(r"(-?0x[0-9a-f]+)\((\w+)\)", text)
            if mm and mm.group(2) in regs_hi:
                off = int(mm.group(1), 16)
                v = (regs_hi[mm.group(2)] + off) & 0xFFFFFFFF
                if (v >> 28) in (0xA, 0xB):
                    hw.add(v)
                s = cstring(ram, v)
                if s and op.startswith("l") is False:
                    pass
    return strings, sorted(hw), calls, sorted(reads_args), writes_v0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    ram_path, game_path, tool = sys.argv[1:4]
    md = sys.argv[sys.argv.index("--md") + 1] if "--md" in sys.argv else None
    ram = open(ram_path, "rb").read()
    game = open(game_path, "rb").read()

    # syscall stubs in the game and how many jal sites target each
    stub_for = {}
    for off in range(0x60, 0x300, 4):
        w = u32(game, off)
        if w & 0xFC00003F == 0x0000000C:
            stub_for[(w >> 6) & 0xFFFFF] = GAME_BASE + off
    jal_count = {}
    for off in range(0, 0xE5E50, 4):
        w = u32(game, off)
        if w >> 26 == 3:
            t = ((GAME_BASE + off + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            jal_count[t] = jal_count.get(t, 0) + 1

    rows = []
    for n in range(NUM):
        h = u32(ram, TABLE - RAM_BASE + 4 * n)
        stub = stub_for.get(n)
        uses = jal_count.get(stub, 0) if stub else 0
        if h == 0:
            rows.append((n, h, stub, uses, [], [], [], [], False, 0))
            continue
        body = handler_body(disasm(tool, ram_path, RAM_BASE, h, 400))
        s, hw, calls, args, v0 = profile(ram, body)
        rows.append((n, h, stub, uses, s, hw, calls, args, v0, len(body)))

    for n, h, stub, uses, s, hw, calls, args, v0, size in rows:
        print(f"#{n:3d} handler {h:08x} stub {stub or 0:08x} game-calls {uses:3d} size {size:3d} "
              f"args {','.join(args) or '-'} v0 {'y' if v0 else '-'}")
        if s:
            print("      strings:", " | ".join(x.replace('\n', '\\n')[:60] for x in s[:4]))
        if hw:
            print("      hw:", " ".join(f"{x:08x}" for x in hw[:8]))
        if calls:
            print("      calls:", " ".join(f"{x:08x}" for x in calls[:10]))

    if md:
        with open(md, "w") as f:
            f.write("| # | handler | game stub | game call sites | args | returns | strings | hardware |\n")
            f.write("|---|---|---|---|---|---|---|---|\n")
            for n, h, stub, uses, s, hw, calls, args, v0, size in rows:
                f.write(f"| {n} | {h:08x} | {stub or 0:08x} | {uses} | {','.join(args)} | "
                        f"{'v0' if v0 else ''} | {'; '.join(x.strip()[:40] for x in s[:2])} | "
                        f"{' '.join(f'{x:08x}' for x in hw[:4])} |\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
