// Software reference renderer for the Banshee 3D pipeline.
//
// Rasterises setup-unit triangles at an integer multiple of the game's 512x384 resolution into
// private RGBA8 colour / float depth buffers (keyed by the game's colour/aux buffer addresses;
// the game never reads the frame buffer back). Implements the Voodoo pixel pipeline: texture
// fetch (all 16-bit and 8-bit formats, mipmaps, bilinear), texture combine, colour/alpha combine,
// chroma key, alpha mask/test, W/Z depth test, alpha blending, clipping and Y origin.
// It favours clarity over speed: it is the reference for the GPU renderer.
#include "raster_sw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "banshee.h"
#include "voodoo3d.h"

#define MAX_BUFS 8
typedef struct {
  uint32_t addr;
  uint8_t* rgba; /* W*H*4 */
  float* depth;  /* W*H, aux buffer (depth or alpha planes) */
} Buf;

static struct {
  int scale, w, h; /* output size = 512*scale x 384*scale */
  Buf col[MAX_BUFS], aux[MAX_BUFS];
  int enabled;     /* rasterise the current frame */
  RasterSwFrameFn on_frame;
  void* on_frame_user;
  uint64_t frame;
} R;

static Buf* find_buf(Buf* list, uint32_t addr, int depth) {
  for (int i = 0; i < MAX_BUFS; ++i)
    if ((list[i].rgba || list[i].depth) && list[i].addr == addr) return &list[i];
  for (int i = 0; i < MAX_BUFS; ++i)
    if (!list[i].rgba && !list[i].depth) {
      list[i].addr = addr;
      const size_t n = (size_t)R.w * R.h;
      if (depth) list[i].depth = calloc(n, sizeof(float));
      else list[i].rgba = calloc(n * 4, 1);
      return &list[i];
    }
  return &list[0];
}

/* ---- texture fetch ---- */
typedef struct { float r, g, b, a; } C4;
static int sw_debug; /* SW_DEBUG: log fast fills and sampled depth/alpha-test failures */
uint64_t g_sw_stats[8]; /* pixels: inside, chroma fail, alpha mask, alpha test, depth fail, written, offscreen */

static inline uint16_t vram16(uint32_t a) {
  uint16_t v;
  memcpy(&v, g_banshee_vram + (a & (BANSHEE_VRAM_SIZE - 2)), 2);
  return v;
}
static inline uint8_t vram8(uint32_t a) { return g_banshee_vram[a & (BANSHEE_VRAM_SIZE - 1)]; }

static C4 texel_decode(uint32_t fmt, uint32_t v) {
  C4 c;
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

typedef struct {
  uint32_t fmt, bpp;
  uint32_t lodoff[9];
  int wmask, hmask;
  int lodmin, lodmax; /* in 1/4 LOD units */
  float lodbias;
  int clamp_s, clamp_t, mirror_s, mirror_t, bilinear_min, bilinear_mag, persp;
} TexInfo;

static void tex_setup(const V3dState* st, TexInfo* t) {
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

static inline int wrap_coord(int c, int mask, int clamp, int mirror) {
  if (clamp) return c < 0 ? 0 : c > mask ? mask : c;
  if (mirror && (c & (mask + 1))) c = ~c;
  return c & mask;
}

static C4 tex_fetch1(const TexInfo* t, int l, int s, int tt) {
  const int wm = t->wmask >> l, hm = t->hmask >> l;
  s = wrap_coord(s, wm, t->clamp_s, t->mirror_s);
  tt = wrap_coord(tt, hm, t->clamp_t, t->mirror_t);
  const uint32_t idx = (uint32_t)(tt * (wm + 1) + s);
  const uint32_t v = t->bpp == 2 ? vram16(t->lodoff[l] + idx * 2) : vram8(t->lodoff[l] + idx);
  return texel_decode(t->fmt, v);
}

/* s, t in LOD 0 texel units; lod in LOD units */
static C4 tex_sample(const TexInfo* t, float s, float tt, float lod) {
  lod += t->lodbias;
  const float lmin = t->lodmin / 4.0f, lmax = t->lodmax / 4.0f;
  const int magnify = lod <= lmin;
  if (lod < lmin) lod = lmin;
  if (lod > lmax) lod = lmax;
  int l = (int)lod;
  if (l > 8) l = 8;
  const float sc = 1.0f / (float)(1 << l);
  s *= sc;
  tt *= sc;
  const int bil = magnify ? t->bilinear_mag : t->bilinear_min;
  if (!bil) return tex_fetch1(t, l, (int)floorf(s), (int)floorf(tt));
  s -= 0.5f;
  tt -= 0.5f;
  const float fs = floorf(s), ft = floorf(tt);
  const int s0 = (int)fs, t0 = (int)ft;
  const float ws = s - fs, wt = tt - ft;
  const C4 a = tex_fetch1(t, l, s0, t0), b = tex_fetch1(t, l, s0 + 1, t0);
  const C4 c = tex_fetch1(t, l, s0, t0 + 1), d = tex_fetch1(t, l, s0 + 1, t0 + 1);
  C4 o;
#define LERP2(f) ((a.f * (1 - ws) + b.f * ws) * (1 - wt) + (c.f * (1 - ws) + d.f * ws) * wt)
  o.r = LERP2(r); o.g = LERP2(g); o.b = LERP2(b); o.a = LERP2(a);
#undef LERP2
  return o;
}

static inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static C4 rgba_of(uint32_t argb) {
  C4 c = {((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f, (argb >> 24) / 255.0f};
  return c;
}

/* One combine unit (texture combine or colour combine), channel-generic.
   other/local: inputs; factor: blend factor before the reverse-blend inversion. */
static inline float combine(int zero_other, int sub_local, float other, float local, float factor,
                            int reverse, int add, float add_c, float add_a, int invert) {
  float v = zero_other ? 0.0f : other;
  if (sub_local) v -= local;
  if (!reverse) factor = 1.0f - factor;
  v *= factor;
  if (add == 1) v += add_c;
  else if (add == 2) v += add_a;
  v = clamp01(v);
  return invert ? 1.0f - v : v;
}

/* Voodoo W-buffer pseudo-float (16-bit) from 1/w. The iterated W is 16.48 fixed point; the
   depth is (leading-zero count - 16) in the top 4 bits and the inverted 12 bits below the
   leading one. Returned as float (with the bits below the mantissa) for sub-step precision. */
static float wdepth(float oow) {
  if (!(oow > 0)) return 65535.0f;
  if (oow >= 1.0f) return 0.0f;                         /* integer part set: exponent < 0 */
  const double iterw = (double)oow * 281474976710656.0; /* * 2^48 */
  const uint64_t w = (uint64_t)iterw;
  if (w < (1ull << 32)) return 65535.0f;                /* exponent >= 16 */
  const int exp = __builtin_clzll(w) - 16;              /* 0..15 */
  const double norm = iterw / (double)(1ull << (47 - exp)); /* [1, 2) */
  double d = exp * 4096.0 + (2.0 - norm) * 4096.0;
  if (d > 65535.0) d = 65535.0;
  return (float)d;
}

static inline int depth_pass(int func, float src, float dst) {
  switch (func) {
    case 0: return 0;
    case 1: return src < dst;
    case 2: return src == dst;
    case 3: return src <= dst;
    case 4: return src > dst;
    case 5: return src != dst;
    case 6: return src >= dst;
    default: return 1;
  }
}

static inline float blend_factor(int f, int is_src, const C4* s, const C4* d, float da, int ch) {
  const float sc = ch == 0 ? s->r : ch == 1 ? s->g : s->b;
  const float dc = ch == 0 ? d->r : ch == 1 ? d->g : d->b;
  switch (f) {
    case 0: return 0;
    case 1: return s->a;
    case 2: return is_src ? dc : sc;
    case 3: return da;
    case 4: return 1;
    case 5: return 1 - s->a;
    case 6: return is_src ? 1 - dc : 1 - sc;
    case 7: return 1 - da;
    case 15: return is_src ? (s->a < 1 - da ? s->a : 1 - da) : 1; /* alpha saturate / pre-fog colour */
    default: return 0;
  }
}

typedef struct { float a, b, c; } Plane; /* q = a*x + b*y + c (native pixel coords) */

static Plane plane(const V3dVertex v[3], const float q[3], float inv_area) {
  const float x0 = v[0].x, y0 = v[0].y;
  const float dx1 = v[1].x - x0, dy1 = v[1].y - y0, dx2 = v[2].x - x0, dy2 = v[2].y - y0;
  const float dq1 = q[1] - q[0], dq2 = q[2] - q[0];
  Plane p;
  p.a = (dq1 * dy2 - dq2 * dy1) * inv_area;
  p.b = (dq2 * dx1 - dq1 * dx2) * inv_area;
  p.c = q[0] - p.a * x0 - p.b * y0;
  return p;
}
#define PL(p, x, y) ((p).a * (x) + (p).b * (y) + (p).c)

static void sw_triangle(void* user, const V3dState* st, const V3dVertex vin[3]) {
  (void)user;
  if (!R.enabled) return;
  const float area = (vin[1].x - vin[0].x) * (vin[2].y - vin[0].y) - (vin[2].x - vin[0].x) * (vin[1].y - vin[0].y);
  if (area == 0 || !isfinite(area)) return;
  const float inv_area = 1.0f / area;

  const uint32_t fbz = st->fbzMode, cp = st->fbzColorPath, am = st->alphaMode, tm = st->textureMode;
  Buf* cb = find_buf(R.col, st->colBufferAddr & 0xFFFFF0u, 0);
  Buf* ab = find_buf(R.aux, st->auxBufferAddr & 0xFFFFF0u, 1);

  /* bounding box in native coordinates, clipped */
  float minx = fminf(vin[0].x, fminf(vin[1].x, vin[2].x)), maxx = fmaxf(vin[0].x, fmaxf(vin[1].x, vin[2].x));
  float miny = fminf(vin[0].y, fminf(vin[1].y, vin[2].y)), maxy = fmaxf(vin[0].y, fmaxf(vin[1].y, vin[2].y));
  float cx0 = 0, cx1 = 512, cy0 = 0, cy1 = 384;
  if (fbz & 1) {
    cx0 = (float)((st->clipLeftRight >> 16) & 0x3FF); cx1 = (float)(st->clipLeftRight & 0x3FF);
    cy0 = (float)((st->clipLowYHighY >> 16) & 0x3FF); cy1 = (float)(st->clipLowYHighY & 0x3FF);
  }
  if (minx < cx0) minx = cx0;
  if (maxx > cx1) maxx = cx1;
  if (miny < cy0) miny = cy0;
  if (maxy > cy1) maxy = cy1;
  if (minx >= maxx || miny >= maxy) return;
  const float S = (float)R.scale;
  int px0 = (int)floorf(minx * S), px1 = (int)ceilf(maxx * S), py0 = (int)floorf(miny * S), py1 = (int)ceilf(maxy * S);
  if (px0 < 0) px0 = 0;
  if (py0 < 0) py0 = 0;
  if (px1 > R.w) px1 = R.w;
  if (py1 > R.h) py1 = R.h;

  float q[3];
#define MKPLANE(field) (q[0] = vin[0].field, q[1] = vin[1].field, q[2] = vin[2].field, plane(vin, q, inv_area))
  const Plane pr = MKPLANE(r), pg = MKPLANE(g), pb = MKPLANE(b), pa = MKPLANE(a), pz = MKPLANE(z);
  const Plane pwb = MKPLANE(wb), pw0 = MKPLANE(w0), ps = MKPLANE(s0), pt = MKPLANE(t0);
#undef MKPLANE

  const int texturing = (cp >> 27) & 1;
  TexInfo tex;
  if (texturing) tex_setup(st, &tex);
  const C4 color0 = rgba_of(st->color0), color1 = rgba_of(st->color1);
  const int yorigin_on = (fbz >> 17) & 1;
  const float yorigin = (float)((banshee_io_reg(0x10) >> 18) & 0xFFF);
  const int dfunc = (fbz >> 5) & 7;
  const float zbias = (fbz & (1u << 16)) ? (float)(int16_t)(st->zaColor & 0xFFFF) : 0.0f;

  for (int py = py0; py < py1; ++py) {
    const float y = (py + 0.5f) / S;
    for (int px = px0; px < px1; ++px) {
      const float x = (px + 0.5f) / S;
      /* inside test with a top-left rule via edge functions */
      int inside = 1;
      for (int e = 0; e < 3 && inside; ++e) {
        const V3dVertex* p0 = &vin[e];
        const V3dVertex* p1 = &vin[(e + 1) % 3];
        float ef = (p1->x - p0->x) * (y - p0->y) - (p1->y - p0->y) * (x - p0->x);
        if (area < 0) ef = -ef;
        if (ef < 0) inside = 0;
        else if (ef == 0) { /* top-left rule */
          const float dy = (area < 0) ? -(p1->y - p0->y) : (p1->y - p0->y);
          const float dx = (area < 0) ? -(p1->x - p0->x) : (p1->x - p0->x);
          if (!(dy < 0 || (dy == 0 && dx > 0))) inside = 0;
        }
      }
      if (!inside) continue;
      g_sw_stats[0]++;
      if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1) continue;

      /* iterated values */
      C4 it = {clamp01(PL(pr, x, y) / 255.0f), clamp01(PL(pg, x, y) / 255.0f), clamp01(PL(pb, x, y) / 255.0f),
               clamp01(PL(pa, x, y) / 255.0f)};
      const float iz = PL(pz, x, y), iwb = PL(pwb, x, y);

      /* texture unit */
      C4 texc = {0, 0, 0, 0};
      if (texturing) {
        const float w0 = PL(pw0, x, y);
        float s = PL(ps, x, y), t = PL(pt, x, y);
        float dsdx = ps.a, dtdx = pt.a, dsdy = ps.b, dtdy = pt.b;
        if (tex.persp) {
          const float iw = (w0 != 0) ? 1.0f / w0 : 0.0f;
          dsdx = (ps.a - s * iw * pw0.a) * iw; dtdx = (pt.a - t * iw * pw0.a) * iw;
          dsdy = (ps.b - s * iw * pw0.b) * iw; dtdy = (pt.b - t * iw * pw0.b) * iw;
          s *= iw;
          t *= iw;
        }
        const float fx = sqrtf(dsdx * dsdx + dtdx * dtdx), fy = sqrtf(dsdy * dsdy + dtdy * dtdy);
        const float foot = fmaxf(fx, fy) / S;
        const float lod = foot > 0 ? log2f(foot) : -8.0f;
        const C4 tl = tex_sample(&tex, s, t, lod);
        /* texture combine: c_other/a_other = 0 (no upstream TMU), local = texel */
        const float lodfrac = lod - floorf(lod);
        float f;
        const int tcm = (tm >> 14) & 7, tcam = (tm >> 23) & 7;
        f = 0;
        float fr = 0, fg = 0, fb = 0;
        switch (tcm) { case 1: fr = tl.r; fg = tl.g; fb = tl.b; break; case 3: fr = fg = fb = tl.a; break;
                       case 4: case 5: fr = fg = fb = lodfrac; break; default: break; }
        const int tz = (tm >> 12) & 1, ts = (tm >> 13) & 1, trv = (tm >> 17) & 1, tad = (tm >> 18) & 3, tinv = (tm >> 20) & 1;
        texc.r = combine(tz, ts, 0, tl.r, fr, trv, tad, tl.r, tl.a, tinv);
        texc.g = combine(tz, ts, 0, tl.g, fg, trv, tad, tl.g, tl.a, tinv);
        texc.b = combine(tz, ts, 0, tl.b, fb, trv, tad, tl.b, tl.a, tinv);
        switch (tcam) { case 1: case 3: f = tl.a; break; case 4: case 5: f = lodfrac; break; default: f = 0; break; }
        const int az = (tm >> 21) & 1, as = (tm >> 22) & 1, arv = (tm >> 26) & 1, aad = (tm >> 27) & 3, ainv = (tm >> 29) & 1;
        texc.a = combine(az, as, 0, tl.a, f, arv, aad ? 2 : 0, tl.a, tl.a, ainv);
      }

      /* colour combine */
      C4 other, local;
      switch (cp & 3) { case 0: other = it; break; case 1: other = texc; break; default: other = color1; break; }
      switch ((cp >> 2) & 3) { case 0: other.a = it.a; break; case 1: other.a = texc.a; break; default: other.a = color1.a; break; }
      if ((fbz >> 1) & 1) { /* chroma key on the "other" colour */
        const uint32_t ck = st->chromaKey;
        const int r8 = (int)(other.r * 255 + 0.5f), g8 = (int)(other.g * 255 + 0.5f), b8 = (int)(other.b * 255 + 0.5f);
        if (r8 == (int)((ck >> 16) & 255) && g8 == (int)((ck >> 8) & 255) && b8 == (int)(ck & 255)) { g_sw_stats[1]++; continue; }
      }
      const int lsel = ((cp >> 7) & 1) ? (texc.a >= 0.5f) : ((cp >> 4) & 1);
      local = lsel ? color0 : it;
      switch ((cp >> 5) & 3) {
        case 0: local.a = it.a; break;
        case 1: local.a = color0.a; break;
        case 2: local.a = clamp01(iz / 65535.0f); break;
        default: local.a = clamp01(iwb); break;
      }
      float fr, fg, fb, fa;
      switch ((cp >> 10) & 7) {
        case 1: fr = local.r; fg = local.g; fb = local.b; break;
        case 2: fr = fg = fb = other.a; break;
        case 3: fr = fg = fb = local.a; break;
        case 4: fr = fg = fb = texc.a; break;
        case 5: fr = texc.r; fg = texc.g; fb = texc.b; break;
        default: fr = fg = fb = 0; break;
      }
      switch ((cp >> 19) & 7) {
        case 1: case 3: fa = local.a; break;
        case 2: fa = other.a; break;
        case 4: fa = texc.a; break;
        default: fa = 0; break;
      }
      const int cz = (cp >> 8) & 1, cs = (cp >> 9) & 1, crv = (cp >> 13) & 1, cad = (cp >> 14) & 3, cinv = (cp >> 16) & 1;
      C4 c;
      c.r = combine(cz, cs, other.r, local.r, fr, crv, cad, local.r, local.a, cinv);
      c.g = combine(cz, cs, other.g, local.g, fg, crv, cad, local.g, local.a, cinv);
      c.b = combine(cz, cs, other.b, local.b, fb, crv, cad, local.b, local.a, cinv);
      const int az = (cp >> 17) & 1, as = (cp >> 18) & 1, arv = (cp >> 22) & 1, aad = (cp >> 23) & 3, ainv = (cp >> 25) & 1;
      c.a = combine(az, as, other.a, local.a, fa, arv, aad ? 2 : 0, local.a, local.a, ainv);

      /* alpha mask / alpha test */
      if ((fbz >> 13) & 1 && c.a < 0.5f) { g_sw_stats[2]++; continue; }
      if (am & 1) {
        const int a8 = (int)(c.a * 255 + 0.5f), ref = (int)(am >> 24);
        int pass;
        switch ((am >> 1) & 7) {
          case 0: pass = 0; break; case 1: pass = a8 < ref; break; case 2: pass = a8 == ref; break;
          case 3: pass = a8 <= ref; break; case 4: pass = a8 > ref; break; case 5: pass = a8 != ref; break;
          case 6: pass = a8 >= ref; break; default: pass = 1; break;
        }
        if (!pass) { if ((g_sw_stats[3]++ % 20000) == 0 && sw_debug) fprintf(stderr, "atest fail: a %d ref %d func %d am %08X cp %08X tm %08X\n", a8, ref, (am >> 1) & 7, am, cp, tm); continue; }
      }

      /* destination pixel */
      int sy = py;
      if (yorigin_on) sy = (int)((yorigin + 1.0f) * S) - 1 - py; /* lower-left origin: row = yorigin - y */
      if (sy < 0 || sy >= R.h) { g_sw_stats[6]++; continue; }
      const size_t pi = (size_t)sy * R.w + px;

      /* depth */
      float depth = ((fbz >> 3) & 1) ? wdepth(iwb) : iz;
      depth += zbias;
      if (depth < 0) depth = 0;
      if (depth > 65535) depth = 65535;
      const float src_depth = ((fbz >> 20) & 1) ? (float)(st->zaColor & 0xFFFF) : depth;
      if ((fbz >> 4) & 1) {
        if (!depth_pass(dfunc, src_depth, ab->depth[pi])) { if ((g_sw_stats[4]++ % 20000) == 0 && sw_debug) fprintf(stderr, "depth fail: src %.1f dst %.1f func %d fbz %08X wb %g z %g za %08X\n", src_depth, ab->depth[pi], dfunc, fbz, iwb, iz, st->zaColor); continue; }
      }

      /* alpha blend */
      uint8_t* d8 = cb->rgba + pi * 4;
      if ((am >> 4) & 1) {
        const C4 d = {d8[0] / 255.0f, d8[1] / 255.0f, d8[2] / 255.0f, d8[3] / 255.0f};
        const float da = ((fbz >> 18) & 1) ? ab->depth[pi] / 255.0f : 1.0f;
        const int sf = (am >> 8) & 15, df = (am >> 12) & 15;
        C4 o;
        o.r = clamp01(c.r * blend_factor(sf, 1, &c, &d, da, 0) + d.r * blend_factor(df, 0, &c, &d, da, 0));
        o.g = clamp01(c.g * blend_factor(sf, 1, &c, &d, da, 1) + d.g * blend_factor(df, 0, &c, &d, da, 1));
        o.b = clamp01(c.b * blend_factor(sf, 1, &c, &d, da, 2) + d.b * blend_factor(df, 0, &c, &d, da, 2));
        o.a = c.a;
        c = o;
      }
      g_sw_stats[5]++;
      if ((fbz >> 9) & 1) {
        d8[0] = (uint8_t)(c.r * 255 + 0.5f);
        d8[1] = (uint8_t)(c.g * 255 + 0.5f);
        d8[2] = (uint8_t)(c.b * 255 + 0.5f);
        d8[3] = 255;
      }
      if ((fbz >> 10) & 1) {
        if ((fbz >> 18) & 1) ab->depth[pi] = c.a * 255.0f; /* alpha planes */
        else ab->depth[pi] = depth;
      }
    }
  }
}

static void sw_fastfill(void* user, const V3dState* st) {
  (void)user;
  if (!R.enabled) return;
  const uint32_t fbz = st->fbzMode;
  Buf* cb = find_buf(R.col, st->colBufferAddr & 0xFFFFF0u, 0);
  Buf* ab = find_buf(R.aux, st->auxBufferAddr & 0xFFFFF0u, 1);
  int x0 = (int)((st->clipLeftRight >> 16) & 0x3FF), x1 = (int)(st->clipLeftRight & 0x3FF);
  int y0 = (int)((st->clipLowYHighY >> 16) & 0x3FF), y1 = (int)(st->clipLowYHighY & 0x3FF);
  x0 *= R.scale; x1 *= R.scale; y0 *= R.scale; y1 *= R.scale;
  if (x1 > R.w) x1 = R.w;
  if (y1 > R.h) y1 = R.h;
  const uint32_t c1 = st->color1;
  if (sw_debug) fprintf(stderr, "fastfill: fbz %08X za %08X color1 %08X clip %08X %08X col %06X aux %06X\n", fbz, st->zaColor, c1, st->clipLeftRight, st->clipLowYHighY, st->colBufferAddr, st->auxBufferAddr);
  const float z = (float)(st->zaColor & 0xFFFF);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const size_t pi = (size_t)y * R.w + x;
      if ((fbz >> 9) & 1) {
        cb->rgba[pi * 4 + 0] = (c1 >> 16) & 255;
        cb->rgba[pi * 4 + 1] = (c1 >> 8) & 255;
        cb->rgba[pi * 4 + 2] = c1 & 255;
        cb->rgba[pi * 4 + 3] = 255;
      }
      if ((fbz >> 10) & 1) ab->depth[pi] = ((fbz >> 18) & 1) ? (float)(st->zaColor >> 24) : z;
    }
}

static int frame_selected(uint64_t frame);

static void sw_swap(void* user, uint32_t display_addr) {
  (void)user;
  if (R.enabled && R.on_frame) {
    Buf* cb = find_buf(R.col, display_addr & 0xFFFFF0u, 0);
    R.on_frame(R.on_frame_user, R.frame, cb->rgba, R.w, R.h);
  }
  R.frame++;
  R.enabled = frame_selected(R.frame);
}

static uint64_t sel_start, sel_every = 1;
static int frame_selected(uint64_t frame) { return frame >= sel_start && (frame - sel_start) % sel_every == 0; }

void raster_sw_init(int scale, uint64_t first_frame, uint64_t every, RasterSwFrameFn fn, void* user) {
  memset(&R, 0, sizeof R);
  R.scale = scale < 1 ? 1 : scale;
  R.w = 512 * R.scale;
  R.h = 384 * R.scale;
  R.on_frame = fn;
  sw_debug = getenv("SW_DEBUG") != NULL;
  R.on_frame_user = user;
  sel_start = first_frame;
  sel_every = every ? every : 1;
  R.enabled = frame_selected(0);
  V3dBackend be = {sw_triangle, sw_fastfill, sw_swap, NULL, NULL};
  v3d_set_backend(&be);
}

/* ---- PNG output (uncompressed deflate, enough for debugging) ---- */
static uint32_t crc_table[256];
static uint32_t crc32_upd(uint32_t c, const uint8_t* p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t k = i;
      for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
      crc_table[i] = k;
    }
  c ^= 0xFFFFFFFFu;
  while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
static void put32(FILE* f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
  put32(f, len);
  uint8_t* tmp = malloc((size_t)len + 4);
  memcpy(tmp, type, 4);
  if (len) memcpy(tmp + 4, data, len);
  fwrite(tmp, 1, (size_t)len + 4, f);
  put32(f, crc32_upd(0, tmp, (size_t)len + 4));
  free(tmp);
}

int raster_sw_write_png(const char* path, const uint8_t* rgba, int w, int h) {
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
  chunk(f, "IHDR", ihdr, 13);
  const size_t row = (size_t)w * 3 + 1, raw_len = row * h;
  uint8_t* raw = malloc(raw_len);
  for (int y = 0; y < h; ++y) {
    raw[y * row] = 0;
    for (int x = 0; x < w; ++x) memcpy(raw + y * row + 1 + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
  }
  const size_t nblocks = (raw_len + 65534) / 65535;
  const size_t z_len = 2 + raw_len + nblocks * 5 + 4;
  uint8_t* z = malloc(z_len);
  size_t o = 0;
  z[o++] = 0x78; z[o++] = 0x01;
  uint32_t a = 1, bsum = 0;
  for (size_t i = 0; i < raw_len; ++i) { a = (a + raw[i]) % 65521; bsum = (bsum + a) % 65521; }
  for (size_t p = 0; p < raw_len; p += 65535) {
    const size_t n = raw_len - p < 65535 ? raw_len - p : 65535;
    z[o++] = (p + n >= raw_len) ? 1 : 0;
    z[o++] = n & 255; z[o++] = n >> 8; z[o++] = ~n & 255; z[o++] = (~n >> 8) & 255;
    memcpy(z + o, raw + p, n);
    o += n;
  }
  const uint32_t adler = (bsum << 16) | a;
  z[o++] = adler >> 24; z[o++] = adler >> 16; z[o++] = adler >> 8; z[o++] = adler;
  chunk(f, "IDAT", z, (uint32_t)o);
  chunk(f, "IEND", NULL, 0);
  free(raw);
  free(z);
  fclose(f);
  return 0;
}
