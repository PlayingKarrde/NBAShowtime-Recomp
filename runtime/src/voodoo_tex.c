// Banshee texture layout (mipmap chain in VRAM) and texel formats, shared by the renderers.
#include "voodoo_tex.h"

#include <string.h>

#include "banshee.h"

static inline uint16_t vram16(uint32_t a) {
  uint16_t v;
  memcpy(&v, g_banshee_vram + (a & (BANSHEE_VRAM_SIZE - 2)), 2);
  return v;
}
static inline uint8_t vram8(uint32_t a) { return g_banshee_vram[a & (BANSHEE_VRAM_SIZE - 1)]; }

VtColor vtex_decode(uint32_t fmt, uint32_t v) {
  VtColor c;
  const float k = 1.0f / 255.0f;
  switch (fmt) {
    case 0: c.r = ((v >> 5) & 7) / 7.0f; c.g = ((v >> 2) & 7) / 7.0f; c.b = (v & 3) / 3.0f; c.a = 1; break;
    case 2: c.r = c.g = c.b = c.a = v * k; break;                              /* A8 */
    case 3: c.r = c.g = c.b = v * k; c.a = 1; break;                           /* I8 */
    case 4: c.r = c.g = c.b = (v & 15) / 15.0f; c.a = (v >> 4) / 15.0f; break;  /* AI44 */
    case 5: { uint32_t p = g_v3d_palette[v & 255];                             /* P8 */
      c.r = ((p >> 16) & 255) * k; c.g = ((p >> 8) & 255) * k; c.b = (p & 255) * k; c.a = 1; break; }
    case 6: { uint32_t p = g_v3d_palette[v & 255];                             /* AP8: 6666 */
      c.a = ((p >> 18) & 63) / 63.0f; c.r = ((p >> 12) & 63) / 63.0f; c.g = ((p >> 6) & 63) / 63.0f; c.b = (p & 63) / 63.0f; break; }
    case 8: c.a = (v >> 8) * k; c.r = ((v >> 5) & 7) / 7.0f; c.g = ((v >> 2) & 7) / 7.0f; c.b = (v & 3) / 3.0f; break;
    case 10: c.r = ((v >> 11) & 31) / 31.0f; c.g = ((v >> 5) & 63) / 63.0f; c.b = (v & 31) / 31.0f; c.a = 1; break;
    case 11: c.a = (v >> 15) ? 1.0f : 0.0f; c.r = ((v >> 10) & 31) / 31.0f; c.g = ((v >> 5) & 31) / 31.0f; c.b = (v & 31) / 31.0f; break;
    case 12: c.a = (v >> 12) / 15.0f; c.r = ((v >> 8) & 15) / 15.0f; c.g = ((v >> 4) & 15) / 15.0f; c.b = (v & 15) / 15.0f; break;
    case 13: c.a = (v >> 8) * k; c.r = c.g = c.b = (v & 255) * k; break;       /* AI88 */
    case 14: { uint32_t p = g_v3d_palette[v & 255];                            /* AP88 */
      c.a = (v >> 8) * k; c.r = ((p >> 16) & 255) * k; c.g = ((p >> 8) & 255) * k; c.b = (p & 255) * k; break; }
    default: c.r = c.g = c.b = 0.5f; c.a = 1; break;                           /* YIQ (NCC): not used */
  }
  return c;
}


void vtex_setup(const V3dState* st, TexInfo* t) {
  const uint32_t tm = st->textureMode, lod = st->tLOD;
  t->fmt = (tm >> 8) & 15;
  t->bpp = t->fmt >= 8 ? 2 : 1;
  t->persp = tm & 1;
  t->bilinear_min = (tm >> 1) & 1;
  t->bilinear_mag = (tm >> 2) & 1;
  t->clamp_s = (tm >> 6) & 1;
  t->clamp_t = (tm >> 7) & 1;
  t->mirror_s = (lod >> 28) & 1;
  t->mirror_t = (lod >> 29) & 1;
  t->lodmin = lod & 63;
  t->lodmax = (lod >> 6) & 63;
  if (t->lodmax > 32) t->lodmax = 32;
  t->lodbias = (float)((int8_t)(((lod >> 12) & 63) << 2) >> 2) / 4.0f;
  t->wmask = t->hmask = 255;
  const int aspect = (lod >> 21) & 3;
  if (lod & (1u << 20)) t->hmask >>= aspect; else t->wmask >>= aspect;
  uint32_t mask = 0x1FF;
  if (lod & (1u << 19)) mask = (lod & (1u << 18)) ? 0x0AA : 0x155;
  const uint32_t base = st->texBaseAddr & 0xFFFFF0u;
  uint32_t off = 0;
  for (int l = 0; l <= 8; ++l) {
    uint32_t lbase = base;
    if ((lod & (1u << 24)) && (lod >> 28) == 0) /* multi-base mode */
      lbase = (l == 0 ? st->texBaseAddr : l == 1 ? st->texBaseAddr_1 : l == 2 ? st->texBaseAddr_2 : st->texBaseAddr_3_8) & 0xFFFFF0u;
    t->lodoff[l] = lbase + off;
    if (mask & (1u << l)) {
      uint32_t size = (uint32_t)(((t->wmask >> l) + 1) * ((t->hmask >> l) + 1));
      if (l >= 3 && size < 4) size = 4;
      off += size * t->bpp;
    }
  }
}

uint32_t vtex_raw(const TexInfo* t, int lod, uint32_t index) {
  return t->bpp == 2 ? vram16(t->lodoff[lod] + index * 2) : vram8(t->lodoff[lod] + index);
}
