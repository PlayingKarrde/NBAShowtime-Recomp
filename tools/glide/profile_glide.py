#!/usr/bin/env python3
"""Profile the Glide 3 entry points that the game calls, to identify them.

For each Glide function called from game code it reports: size, argument registers and stack
arguments read, whether v0 is set, GC state fields written (relative to the GC base), "invalid"
bits set, Glide source file (from assert strings) and direct callees.

usage: profile_glide.py <game.bin> <functions.csv> [--glide-lo 0x801bf700 --glide-hi 0x801e0600]
"""
import bisect
import csv
import re
import struct
import sys

B = 0x80100000
TEXT_END = 0x801E5E50
GC_BASE = 0x80406E68       # Glide graphics context (from the state setters)
INVALID = 0x804076AC        # gc->state.invalid flags


def u32(g, a):
    return struct.unpack_from("<I", g, a - B)[0]


def cstr(g, a):
    o = a - B
    if not (0 <= o < len(g)):
        return None
    e = g.find(b"\0", o, o + 80)
    s = g[o:e]
    return s.decode() if e > o and all(32 <= c < 127 for c in s) and len(s) >= 3 else None


def main():
    g = open(sys.argv[1], "rb").read()
    funcs = sorted(int(r["start"], 16) for r in csv.DictReader(open(sys.argv[2])))
    lo, hi = 0x801BF700, 0x801E0600
    if "--glide-lo" in sys.argv:
        lo = int(sys.argv[sys.argv.index("--glide-lo") + 1], 16)
    if "--glide-hi" in sys.argv:
        hi = int(sys.argv[sys.argv.index("--glide-hi") + 1], 16)

    def func_end(f):
        i = bisect.bisect_right(funcs, f)
        return funcs[i] if i < len(funcs) else TEXT_END

    # entry points called from outside the Glide range
    entries = {}
    for pc in range(B, TEXT_END, 4):
        w = u32(g, pc)
        if w >> 26 == 3:
            t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if lo <= t < hi and not (lo <= pc < hi):
                entries[t] = entries.get(t, 0) + 1

    for f in sorted(entries):
        end = func_end(f)
        regs_hi, args, stack_args, writes, inv_bits, files, callees = {}, set(), set(), set(), set(), set(), []
        v0 = False
        last_ori = {}
        written = set()
        for pc in range(f, end, 4):
            w = u32(g, pc)
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            srcs = []
            if op == 0:
                srcs = [rs, rt]
                if rd == 2: v0 = True
                written.add(rd)
            elif op in (4, 5):
                srcs = [rs, rt]
            elif op in (40, 41, 43, 57):  # stores
                srcs = [rs, rt]
            elif op in (8, 9, 10, 11, 12, 13, 14, 32, 33, 35, 36, 37, 49):
                srcs = [rs]
                if rt == 2: v0 = True
                written.add(rt)
            elif op == 15:
                regs_hi[rt] = imm << 16
                written.add(rt)
            elif op == 3:
                callees.append(((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
            for r in srcs:
                if 4 <= r <= 7 and r not in written:
                    args.add(f"a{r - 4}")
            if op == 35 and rs == 29 and simm >= 0x10:  # lw from caller frame: stack args
                pass
            if op in (35, 43, 49, 57) and rs in regs_hi:
                a = (regs_hi[rs] + simm) & 0xFFFFFFFF
                if op in (43, 57) and GC_BASE - 0x400 <= a < GC_BASE + 0x2000:
                    writes.add(a - GC_BASE)
                s = cstr(g, a)
                if s and s.endswith(".c"):
                    files.add(s)
            if op == 9 and rs in regs_hi:
                a = (regs_hi[rs] + simm) & 0xFFFFFFFF
                s = cstr(g, a)
                if s and s.endswith(".c"):
                    files.add(s)
            if op == 13:
                last_ori[rt] = imm
            if op == 43 and rs in regs_hi and ((regs_hi[rs] + simm) & 0xFFFFFFFF) == INVALID and rt in last_ori:
                inv_bits.add(last_ori[rt])
        # stack arguments: lw rX, off(sp) with off >= frame size + 0x10
        frame = 0
        w0 = u32(g, f)
        for pc in range(f, min(end, f + 32), 4):
            w = u32(g, pc)
            if w >> 16 == 0x27BD and (w & 0x8000):
                frame = 0x10000 - (w & 0xFFFF)
                break
        for pc in range(f, end, 4):
            w = u32(g, pc)
            if w >> 26 == 35 and (w >> 21) & 31 == 29:
                off = w & 0xFFFF
                if off >= frame + 0x10 and off < frame + 0x40:
                    stack_args.add(f"arg{(off - frame) // 4}")
        print(f"{f:08x} size={end - f:5d} calls={entries[f]:3d} args={','.join(sorted(args)) or '-'}"
              f"{(' +' + ','.join(sorted(stack_args))) if stack_args else ''} ret={'v0' if v0 else '-'}"
              f" file={','.join(sorted(files)) or '?'}"
              f" gc_writes={','.join(hex(x) for x in sorted(writes)[:6]) or '-'}"
              f" invalid={','.join(hex(x) for x in sorted(inv_bits)) or '-'}"
              f" callees={len(callees)}")


if __name__ == "__main__":
    main()
