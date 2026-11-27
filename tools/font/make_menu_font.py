#!/usr/bin/env python3
"""Bake the settings menu's fonts into signed distance field atlases (assets/fonts/menu_font.bin).

usage: make_menu_font.py OUT.bin DISPLAY.ttf BODY.ttf

The menu uses Noto Sans (SIL Open Font License, assets/fonts/OFL.txt): Condensed Black Italic for
titles and items, Condensed SemiBold for help text. Needs Pillow (with raqm for kerning) and numpy.

File layout (little endian):
  "NBAF" u32 version=1 u32 nfonts
  per font: f32 size f32 ascent f32 descent f32 spread u32 nglyphs u32 nkern u32 atlas_w u32 atlas_h
            u32 zlib_size
            glyphs: u32 codepoint u16 x,y,w,h  f32 xoff,yoff (top-left vs. pen at baseline) f32 advance
            kerns:  u16 left,right f32 adjust
            atlas: zlib-compressed atlas_w*atlas_h bytes (128 = edge, spread px per 127 steps)
  All metrics are in pixels at `size`.
"""
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

CHARS = [chr(c) for c in range(32, 127)]
SS = 4  # supersampling for the distance computation


def edt_sq(inside, r, border_inside):
    """Squared distance (hi-res px) from each pixel to the nearest pixel where `inside` is False,
    exact up to r (separable brute force, r is small). border_inside: whether the pixels beyond
    the image count as inside."""
    big = float(r * r * 4 + 1)
    h, w = inside.shape
    f = np.where(inside, big, 0.0)
    edge = big if border_inside else 0.0
    g = np.full_like(f, big)
    pad = np.pad(f, ((0, 0), (r, r)), constant_values=edge)
    for k in range(-r, r + 1):
        g = np.minimum(g, pad[:, r + k:r + k + w] + k * k)
    out = np.full_like(g, big)
    pad = np.pad(g, ((r, r), (0, 0)), constant_values=edge)
    for k in range(-r, r + 1):
        out = np.minimum(out, pad[r + k:r + k + h, :] + k * k)
    return out


def bake(path, size, spread):
    hi = ImageFont.truetype(path, size * SS)
    lo = ImageFont.truetype(path, size)
    ascent, descent = lo.getmetrics()
    pad = spread + 1
    glyphs = []
    for ch in CHARS:
        adv = lo.getlength(ch)
        if ch == ' ':
            glyphs.append((ord(ch), None, 0, 0, adv))
            continue
        l, t, r, b = hi.getbbox(ch, anchor='ls')
        # glyph box in output pixels, padded for the distance field
        x0 = int(np.floor(l / SS)) - pad
        y0 = int(np.floor(t / SS)) - pad
        x1 = int(np.ceil(r / SS)) + pad
        y1 = int(np.ceil(b / SS)) + pad
        W, H = (x1 - x0) * SS, (y1 - y0) * SS
        img = Image.new('L', (W, H), 0)
        ImageDraw.Draw(img).text((-x0 * SS, -y0 * SS), ch, font=hi, fill=255, anchor='ls')
        m = np.asarray(img, dtype=np.float32) / 255.0 >= 0.5
        R = spread * SS + SS
        d_out = np.sqrt(edt_sq(~m, R, True))  # outside pixels: distance to the glyph
        d_in = np.sqrt(edt_sq(m, R, False))   # inside pixels: distance to the background
        sd = (np.where(m, d_in - 0.5, -(d_out - 0.5))) / SS  # signed, output px, + inside
        sd = sd.reshape(H // SS, SS, W // SS, SS).mean(axis=(1, 3))
        v = np.clip(128 + sd * (127.0 / spread), 0, 255).astype(np.uint8)
        glyphs.append((ord(ch), v, x0, y0, adv))
    # shelf packing
    aw = 512
    x = y = row = 0
    placed = []
    for cp, v, x0, y0, adv in sorted(glyphs, key=lambda g: -(g[1].shape[0] if g[1] is not None else 0)):
        if v is None:
            placed.append((cp, 0, 0, 0, 0, 0.0, 0.0, adv, None))
            continue
        h, w = v.shape
        if x + w > aw:
            x, y, row = 0, y + row + 1, 0
        placed.append((cp, x, y, w, h, float(x0), float(y0), adv, v))
        x += w + 1
        row = max(row, h)
    ah = y + row + 1
    ah = (ah + 3) & ~3
    atlas = np.zeros((ah, aw), np.uint8)
    for cp, gx, gy, w, h, *_rest, v in placed:
        if v is not None:
            atlas[gy:gy + h, gx:gx + w] = v
    placed.sort(key=lambda g: g[0])
    kerns = []
    for a in CHARS:
        for b in CHARS:
            k = lo.getlength(a + b) - lo.getlength(a) - lo.getlength(b)
            if abs(k) >= 0.25:
                kerns.append((ord(a), ord(b), k))
    blob = zlib.compress(atlas.tobytes(), 9)
    out = struct.pack('<4fIIIII', size, ascent, descent, spread, len(placed), len(kerns), aw, ah, len(blob))
    for cp, gx, gy, w, h, xo, yo, adv, _ in placed:
        out += struct.pack('<I4H3f', cp, gx, gy, w, h, xo, yo, adv)
    for a, b, k in kerns:
        out += struct.pack('<2Hf', a, b, k)
    print(f'{path}: {len(placed)} glyphs, {len(kerns)} kerning pairs, atlas {aw}x{ah}, {len(blob)} bytes compressed')
    return out + blob


def main():
    out, display, body = sys.argv[1:4]
    fonts = [bake(display, 56, 8), bake(body, 32, 5)]
    data = b'NBAF' + struct.pack('<II', 1, len(fonts)) + b''.join(fonts)
    with open(out, 'wb') as f:
        f.write(data)
    print(f'{out}: {len(data)} bytes')


if __name__ == '__main__':
    main()
