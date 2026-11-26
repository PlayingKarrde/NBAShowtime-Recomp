// Voodoo Banshee 3D engine state and triangle setup (see voodoo3d.h).
// Register meanings follow the Voodoo2/Banshee specs (cross-checked against MAME's register
// documentation); the implementation is our own.
#include "voodoo3d.h"

#include <string.h>

V3dStats g_v3d_stats;
uint32_t g_v3d_palette[256];

static struct {
  uint32_t reg[V3D_REG_COUNT];
  uint8_t* vram;
  uint32_t vram_mask;
  V3dBackend be;
  V3dVertex cur;     /* setup-unit vertex being assembled from sVx.. registers */
  V3dVertex sv[3];
  int nverts;
  uint8_t fog_table[64][2];
} v;

static float reg_f(uint32_t r) { float f; memcpy(&f, &v.reg[r], 4); return f; }

void v3d_init(uint8_t* vram, uint32_t vram_mask) {
  memset(&v, 0, sizeof v);
  memset(&g_v3d_stats, 0, sizeof g_v3d_stats);
  v.vram = vram;
  v.vram_mask = vram_mask;
}

void v3d_set_backend(const V3dBackend* be) {
  if (be) v.be = *be;
  else memset(&v.be, 0, sizeof v.be);
}

uint32_t v3d_reg_read(uint32_t regnum) { return regnum < V3D_REG_COUNT ? v.reg[regnum] : 0; }

static void snapshot(V3dState* s) {
  s->fbzColorPath = v.reg[V3D_fbzColorPath];
  s->fogMode = v.reg[V3D_fogMode];
  s->alphaMode = v.reg[V3D_alphaMode];
  s->fbzMode = v.reg[V3D_fbzMode];
  s->clipLeftRight = v.reg[V3D_clipLeftRight];
  s->clipLowYHighY = v.reg[V3D_clipLowYHighY];
  s->fogColor = v.reg[V3D_fogColor];
  s->zaColor = v.reg[V3D_zaColor];
  s->chromaKey = v.reg[V3D_chromaKey];
  s->chromaRange = v.reg[V3D_chromaRange];
  s->stipple = v.reg[V3D_stipple];
  s->color0 = v.reg[V3D_color0];
  s->color1 = v.reg[V3D_color1];
  s->colBufferAddr = v.reg[V3D_colBufferAddr];
  s->colBufferStride = v.reg[V3D_colBufferStride];
  s->auxBufferAddr = v.reg[V3D_auxBufferAddr];
  s->auxBufferStride = v.reg[V3D_auxBufferStride];
  s->textureMode = v.reg[V3D_textureMode];
  s->tLOD = v.reg[V3D_tLOD];
  s->tDetail = v.reg[V3D_tDetail];
  s->texBaseAddr = v.reg[V3D_texBaseAddr];
  s->texBaseAddr_1 = v.reg[V3D_texBaseAddr_1];
  s->texBaseAddr_2 = v.reg[V3D_texBaseAddr_2];
  s->texBaseAddr_3_8 = v.reg[V3D_texBaseAddr_3_8];
  s->ncc_select_table = (v.reg[V3D_textureMode] >> 5) & 1;
  memcpy(s->fog_table, v.fog_table, sizeof s->fog_table);
}

/* ---- triangle setup ---- */
static void draw_setup_triangle(void) {
  const V3dVertex* a = &v.sv[0];
  const V3dVertex* b = &v.sv[1];
  const V3dVertex* c = &v.sv[2];
  const uint32_t mode = v.reg[V3D_sSetupMode];
  const float area = (a->x - b->x) * (a->y - c->y) - (a->x - c->x) * (a->y - b->y);
  if (mode & (1u << 17)) { /* backface culling */
    int sign = (mode >> 18) & 1;
    if (!(mode & (1u << 16)) && !(mode & (1u << 19))) sign ^= (v.nverts - 3) & 1; /* strip ping-pong */
    if ((area < 0) == sign) { g_v3d_stats.culled++; return; }
  }
  g_v3d_stats.triangles++;
  const uint32_t tm = v.reg[V3D_textureMode];
  if (v.reg[V3D_fbzColorPath] & (1u << 27)) g_v3d_stats.tex_format_tris[(tm >> 8) & 15]++;
  if (v.be.triangle) {
    V3dState st;
    snapshot(&st);
    v.be.triangle(v.be.user, &st, v.sv);
  }
}

/* A vertex arrives (FIFO packet 3 or sDrawTriCMD). `start` begins a new strip/fan/list. */
static void add_vertex(const V3dVertex* vert, int start, int fan) {
  if (start) {
    v.sv[0] = v.sv[1] = v.sv[2] = *vert;
    v.nverts = 1;
    return;
  }
  if (!fan) v.sv[0] = v.sv[1];
  v.sv[1] = v.sv[2];
  v.sv[2] = *vert;
  if (++v.nverts >= 3) draw_setup_triangle();
}

/* FIFO packet type 3: `data` points at the words after the header. Returns words consumed. */
uint32_t v3d_packet3(uint32_t cmd, const uint32_t* w) {
  const uint32_t count = (cmd >> 6) & 15, code = (cmd >> 3) & 7;
  const int fan = (cmd >> 22) & 1;
  v.reg[V3D_sSetupMode] = ((cmd >> 10) & 0xFF) | (((cmd >> 22) & 15) << 16);
  uint32_t n = 0;
  V3dVertex sv = v.cur;
#define F() (memcpy(&tmp, &w[n++], 4), tmp)
  float tmp;
  for (uint32_t i = 0; i < count; ++i) {
    sv.x = F();
    sv.y = F();
    if (cmd & (1u << 28)) {
      if (cmd & (3u << 10)) {
        const uint32_t argb = w[n++];
        if (cmd & (1u << 10)) { sv.r = (float)((argb >> 16) & 255); sv.g = (float)((argb >> 8) & 255); sv.b = (float)(argb & 255); }
        if (cmd & (1u << 11)) sv.a = (float)(argb >> 24);
      }
    } else {
      if (cmd & (1u << 10)) { sv.r = F(); sv.g = F(); sv.b = F(); }
      if (cmd & (1u << 11)) sv.a = F();
    }
    if (cmd & (1u << 12)) sv.z = F();
    if (cmd & (1u << 13)) sv.wb = sv.w0 = sv.w1 = F();
    if (cmd & (1u << 14)) sv.w0 = sv.w1 = F();
    if (cmd & (1u << 15)) { sv.s0 = sv.s1 = F(); sv.t0 = sv.t1 = F(); }
    if (cmd & (1u << 16)) sv.w1 = F();
    if (cmd & (1u << 17)) { sv.s1 = F(); sv.t1 = F(); }
    const int start = (code == 1 && i == 0) || (code == 0 && i % 3 == 0);
    add_vertex(&sv, start, fan);
  }
#undef F
  v.cur = sv;
  return n + ((cmd >> 29) & 7);
}

/* words needed by a packet-3 body (for completeness checks before parsing) */
uint32_t v3d_packet3_size(uint32_t cmd) {
  uint32_t per = 2;
  if (cmd & (1u << 28)) per += (cmd & (3u << 10)) ? 1 : 0;
  else per += ((cmd >> 10) & 1) * 3 + ((cmd >> 11) & 1);
  per += ((cmd >> 12) & 1) + ((cmd >> 13) & 1) + ((cmd >> 14) & 1) + ((cmd >> 15) & 1) * 2 +
         ((cmd >> 16) & 1) + ((cmd >> 17) & 1) * 2;
  return per * ((cmd >> 6) & 15) + ((cmd >> 29) & 7);
}

static void fastfill(void) {
  g_v3d_stats.fastfills++;
  if (v.be.fastfill) {
    V3dState st;
    snapshot(&st);
    v.be.fastfill(v.be.user, &st);
  }
}

void v3d_reg_write(uint32_t regnum, uint32_t chipmask, uint32_t data) {
  (void)chipmask; /* single TMU: FBI and TMU registers do not overlap */
  regnum &= 0xFF;
  g_v3d_stats.reg_writes[regnum]++;
  v.reg[regnum] = data;
  float f;
  memcpy(&f, &data, 4);
  switch (regnum) {
    case V3D_sVx: v.cur.x = f; break;
    case V3D_sVy: v.cur.y = f; break;
    case V3D_sARGB:
      v.cur.a = (float)(data >> 24); v.cur.r = (float)((data >> 16) & 255);
      v.cur.g = (float)((data >> 8) & 255); v.cur.b = (float)(data & 255);
      break;
    case V3D_sRed: v.cur.r = f; break;
    case V3D_sGreen: v.cur.g = f; break;
    case V3D_sBlue: v.cur.b = f; break;
    case V3D_sAlpha: v.cur.a = f; break;
    case V3D_sVz: v.cur.z = f; break;
    case V3D_sWb: v.cur.wb = v.cur.w0 = v.cur.w1 = f; break;
    case V3D_sWtmu0: v.cur.w0 = v.cur.w1 = f; break;
    case V3D_sS_W0: v.cur.s0 = v.cur.s1 = f; break;
    case V3D_sT_W0: v.cur.t0 = v.cur.t1 = f; break;
    case V3D_sWtmu1: v.cur.w1 = f; break;
    case V3D_sS_Wtmu1: v.cur.s1 = f; break;
    case V3D_sT_Wtmu1: v.cur.t1 = f; break;
    case V3D_sBeginTriCMD: add_vertex(&v.cur, 1, 0); break;
    case V3D_sDrawTriCMD: add_vertex(&v.cur, 0, (v.reg[V3D_sSetupMode] >> 16) & 1); break;
    case V3D_fastfillCMD: fastfill(); break;
    case V3D_swapbufferCMD:
      g_v3d_stats.swaps++;
      if (v.be.swap) v.be.swap(v.be.user, v.reg[V3D_leftOverlayBuf]);
      break;
    case V3D_triangleCMD:
    case V3D_ftriangleCMD:
      g_v3d_stats.triangles++; /* gradient-form triangles: not used by this game's Glide so far */
      break;
    default:
      if (regnum >= V3D_fogTable && regnum < V3D_fogTable + 32) {
        const uint32_t i = (regnum - V3D_fogTable) * 2;
        v.fog_table[i][1] = data & 0xFF;          /* delta for entry i */
        v.fog_table[i][0] = (data >> 8) & 0xFF;   /* fog value entry i */
        v.fog_table[i + 1][1] = (data >> 16) & 0xFF;
        v.fog_table[i + 1][0] = data >> 24;
      } else if (regnum >= V3D_nccTable + 4 && regnum < V3D_nccTable + 12 && (data & 0x80000000u)) {
        const uint32_t idx = (((data >> 24) & 0x7F) << 1) | ((regnum - V3D_nccTable) & 1);
        g_v3d_palette[idx] = data & 0xFFFFFFu;
      }
      break;
  }
  (void)reg_f;
}

void v3d_texture_write(uint32_t word_offset, uint32_t data) {
  g_v3d_stats.texture_words++;
  word_offset &= 0x7FFFFu; /* bits 20:19 select the TMU (only TMU 0 exists) */
  const uint32_t lod = v.reg[V3D_tLOD];
  if (lod & (1u << 25)) data = __builtin_bswap32(data);
  if (lod & (1u << 26)) data = (data >> 16) | (data << 16);
  const uint32_t base = v.reg[V3D_texBaseAddr] & 0xFFFFF0u;
  const uint32_t a = (base + word_offset * 4) & v.vram_mask & ~3u;
  memcpy(v.vram + a, &data, 4);
  if (v.be.texture_written) v.be.texture_written(v.be.user, a, 4);
}

void v3d_lfb_write(uint32_t word_offset, uint32_t data, uint32_t mem_mask) {
  (void)word_offset; (void)data; (void)mem_mask;
  g_v3d_stats.lfb_words++; /* 3D LFB writes: implemented when the game is seen to use them */
}
