// Banshee texture layout and texel formats (shared by the software and GPU renderers).
#ifndef NBA_VOODOO_TEX_H
#define NBA_VOODOO_TEX_H

#include <stdint.h>

#include "voodoo3d.h"

typedef struct { float r, g, b, a; } VtColor;

typedef struct {
  uint32_t fmt, bpp;
  uint32_t lodoff[9];
  int wmask, hmask;
  int lodmin, lodmax; /* in 1/4 LOD units */
  float lodbias;
  int clamp_s, clamp_t, mirror_s, mirror_t, bilinear_min, bilinear_mag, persp;
} TexInfo;

/* Decode the texture registers of a triangle's state: format, LOD range, mip offsets in VRAM. */
void vtex_setup(const V3dState* st, TexInfo* t);
/* Raw texel `index` (row * width + column) of mip level `lod`. */
uint32_t vtex_raw(const TexInfo* t, int lod, uint32_t index);
/* Expand a raw texel of format `fmt` (textureMode bits 11:8). */
VtColor vtex_decode(uint32_t fmt, uint32_t raw);

#endif
