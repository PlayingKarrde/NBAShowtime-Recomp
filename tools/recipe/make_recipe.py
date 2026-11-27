#!/usr/bin/env python3
"""Build assets/boot/entry_recipe.bin: instructions that rebuild the game's entry RAM snapshot
(work/dumps/entry_ram.bin, dumped from MAME) out of the user's own ROM files.

Almost every non-zero byte of the snapshot is a straight copy of the BIOS ROM
(showtime_mar15_1999.u27) or of the hard disk (nbashowt.chd). The recipe stores those as
(destination, length, source, source offset) copies; only the few KB the BIOS computed at boot
(pointers, tables, the clock) are stored as literal bytes. The launcher replays it.

The same works for any image made of ROM / disk bytes: --ram work/game/diag_80100000.bin
--out assets/boot/diag_recipe.bin gives the recipe for the operator menu program (DIAG.EXE).

Format (little endian):
  "NBARCP1\\0"  u32 ram_size  u32 crc32(ram)  u32 nsegs  u32 literal_bytes
  nsegs x { u32 dst, u32 len, u32 src (1 = BIOS ROM, 2 = disk, 3 = literal), u32 pad, u64 src_off }
  literal bytes
Bytes no segment covers are zero.

usage: tools/recipe/make_recipe.py [--ram work/dumps/entry_ram.bin] [--rom work/bios/showtime_mar15_1999.u27]
                                   [--disk work/nbashowt.img] [--out assets/boot/entry_recipe.bin]
"""
import argparse, hashlib, mmap, struct, zlib

SRC_ROM, SRC_DISK, SRC_LIT = 1, 2, 3
W = 512        # match window
MIN_COPY = 16  # shorter matches are stored as literals


def h(b):
    return hashlib.blake2b(b, digest_size=8).digest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ram", default="work/dumps/entry_ram.bin")
    ap.add_argument("--rom", default="work/bios/showtime_mar15_1999.u27")
    ap.add_argument("--disk", default="work/nbashowt.img")
    ap.add_argument("--out", default="assets/boot/entry_recipe.bin")
    a = ap.parse_args()
    ram = open(a.ram, "rb").read()
    rom = open(a.rom, "rb").read()
    df = open(a.disk, "rb")
    disk = mmap.mmap(df.fileno(), 0, access=mmap.ACCESS_READ)
    srcs = {SRC_ROM: rom, SRC_DISK: disk}
    N = len(ram)

    # 1) Index every non-zero 512-byte RAM window (8-byte steps) and look for it in the sources.
    want = {}
    for off in range(0, N - W + 1, 8):
        w = ram[off:off + W]
        if w.count(0) == W:
            continue
        want.setdefault(h(w), []).append(off)
    cands = {}  # RAM word index -> set of (src, delta)

    def hit(src, src_off):
        for off in want.get(h(srcs[src][src_off:src_off + W]), ()):
            d = (src, src_off - off)
            for wi in range(off // 8, (off + W) // 8):
                s = cands.setdefault(wi, [])
                if d not in s and len(s) < 4:
                    s.append(d)
    for o in range(0, len(rom) - W + 1, 4):
        hit(SRC_ROM, o)
    for o in range(0, len(disk) - W + 1, W):
        if o % (256 << 20) == 0:
            print(f"scanning disk {o >> 20} MB", flush=True)
        hit(SRC_DISK, o)

    # 2) Walk the snapshot, extending matches as far as they go.
    def match_len(p, src, delta):
        s = srcs[src]
        q = p + delta
        if q < 0 or q >= len(s):
            return 0
        n = 0
        step = 4096
        while p + n < N and q + n < len(s):
            k = min(step, N - p - n, len(s) - q - n)
            if ram[p + n:p + n + k] == s[q + n:q + n + k]:
                n += k
                continue
            if k == 1:
                break
            step = max(1, k // 8)
        return n

    segs, lit = [], bytearray()
    cur = None
    lit_start = None
    p = 0

    def flush_lit(end):
        nonlocal lit_start
        if lit_start is None:
            return
        chunk = ram[lit_start:end]
        # trim zero bytes at both ends (the buffer starts zeroed)
        a, b = 0, len(chunk)
        while a < b and chunk[a] == 0: a += 1
        while b > a and chunk[b - 1] == 0: b -= 1
        if b > a:
            segs.append((lit_start + a, b - a, SRC_LIT, len(lit)))
            lit.extend(chunk[a:b])
        lit_start = None

    while p < N:
        best, bl = None, 0
        # Matches found a little further on may also start here (windows are sector-aligned on
        # disk, so the bytes before the first whole window would otherwise be missed).
        tries = [cur] if cur else []
        for k in (0, 1, 8, 16, 32, 48, 64):
            for d in cands.get(p // 8 + k, ()):
                if d not in tries:
                    tries.append(d)
        for d in tries:
            L = match_len(p, *d)
            if L > bl:
                best, bl = d, L
        if bl >= MIN_COPY:
            flush_lit(p)
            # skip copying zero tails: they are already zero
            segs.append((p, bl, best[0], p + best[1]))
            cur = best
            p += bl
            continue
        if ram[p] == 0:
            z = p
            while z < N and ram[z] == 0 and z - p < 64: z += 1
            if z - p >= 64 or z == N:
                flush_lit(p)
                while z < N and ram[z] == 0: z += 1
                p = z
                continue
        if lit_start is None:
            lit_start = p
        p += 1
    flush_lit(N)

    # 3) Verify by replaying.
    out = bytearray(N)
    for dst, ln, src, so in segs:
        out[dst:dst + ln] = (lit if src == SRC_LIT else srcs[src])[so:so + ln]
    assert out == ram, "replay mismatch"
    crc = zlib.crc32(ram) & 0xFFFFFFFF
    with open(a.out, "wb") as f:
        f.write(b"NBARCP1\0" + struct.pack("<IIII", N, crc, len(segs), len(lit)))
        for dst, ln, src, so in segs:
            f.write(struct.pack("<IIIIQ", dst, ln, src, 0, so))
        f.write(lit)
    by = {s: sum(x[1] for x in segs if x[2] == s) for s in (SRC_ROM, SRC_DISK, SRC_LIT)}
    print(f"{len(segs)} segments: {by[SRC_ROM]} bytes from ROM, {by[SRC_DISK]} from disk, "
          f"{by[SRC_LIT]} literal; crc32 {crc:08x} -> {a.out}")


if __name__ == "__main__":
    main()
