// OpenGL 3.3 renderer backend for the Banshee 3D pipeline.
//
// Triangles from the setup unit are batched while the pipeline state stays the same and drawn at
// an arbitrary resolution (render scale x 512x384). The fragment shader implements the same
// pixel pipeline as the software reference renderer (runtime/src/raster_sw.c): texture combine,
// colour/alpha combine, chroma key, alpha mask/test and the Voodoo W/Z depth values; blending,
// depth comparison, colour/depth write masks and clipping map onto fixed-function GL state.
#include "renderer_gl.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../runtime/src/banshee.h"
#include "../../runtime/src/voodoo3d.h"
#include "../../runtime/src/voodoo_tex.h"
#include "gl_funcs.h"

#define NBA_GL_DEFINE(type, name) type nba_##name;
NBA_GL_FUNCS(NBA_GL_DEFINE)
#undef NBA_GL_DEFINE

int nba_gl_load(void) {
#define NBA_GL_LOAD(type, name)                                              \
  nba_##name = (type)SDL_GL_GetProcAddress(#name);                           \
  if (!nba_##name) { fprintf(stderr, "OpenGL: missing %s\n", #name); return -1; }
  NBA_GL_FUNCS(NBA_GL_LOAD)
#undef NBA_GL_LOAD
  return 0;
}

/* ---- shaders ---- */
static const char* kVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec3 a_zw;\n" /* z, 1/w (depth), 1/w (TMU) */
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec2 a_st;\n"
    "uniform float u_yorigin; uniform int u_flip; uniform vec2 u_xrange;\n"
    "noperspective out vec4 v_col; noperspective out vec3 v_zw; noperspective out vec2 v_st;\n"
    "void main() {\n"
    "  float y = u_flip != 0 ? (u_yorigin + 1.0 - a_pos.y) : a_pos.y;\n"
    "  gl_Position = vec4((a_pos.x - u_xrange.x) * u_xrange.y - 1.0, 1.0 - y / 192.0, 0.0, 1.0);\n"
    "  v_col = a_col / 255.0; v_zw = a_zw; v_st = a_st;\n"
    "}\n";

static const char* kFS =
    "#version 330 core\n"
    "noperspective in vec4 v_col; noperspective in vec3 v_zw; noperspective in vec2 v_st;\n"
    "uniform uint u_cp, u_fbz, u_am, u_tm, u_chroma, u_za;\n"
    "uniform vec4 u_color0, u_color1;\n"
    "uniform vec2 u_texsize;\n" /* LOD 0 width/height in texels */
    "uniform int u_tex;\n"
    "uniform sampler2D u_sampler;\n"
    "out vec4 o_col;\n"
    "uint bits(uint v, int lo, int n) { return (v >> uint(lo)) & ((1u << uint(n)) - 1u); }\n"
    "float combine(bool zero_other, bool sub_local, float other, float local, float f, bool rev,\n"
    "              uint add, float add_c, float add_a, bool inv) {\n"
    "  float v = zero_other ? 0.0 : other;\n"
    "  if (sub_local) v -= local;\n"
    "  if (!rev) f = 1.0 - f;\n"
    "  v *= f;\n"
    "  if (add == 1u) v += add_c; else if (add == 2u) v += add_a;\n"
    "  v = clamp(v, 0.0, 1.0);\n"
    "  return inv ? 1.0 - v : v;\n"
    "}\n"
    "float wdepth(float oow) {\n" /* Voodoo W-buffer pseudo-float, see raster_sw.c */
    "  if (!(oow > 0.0)) return 65535.0;\n"
    "  if (oow >= 1.0) return 0.0;\n"
    "  float e = floor(-log2(oow));\n"            /* leading zeros of the 16.48 value minus 16 */
    "  if (e >= 16.0) return 65535.0;\n"
    "  float norm = oow * exp2(e + 1.0);\n"       /* [1, 2) */
    "  return min(e * 4096.0 + (2.0 - norm) * 4096.0, 65535.0);\n"
    "}\n"
    "void main() {\n"
    "  vec4 it = clamp(v_col, 0.0, 1.0);\n"
    "  vec4 texc = vec4(0.0);\n"
    "  if (u_tex != 0) {\n"
    "    vec2 st = (bits(u_tm, 0, 1) != 0u) ? v_st / v_zw.z : v_st;\n"
    "    vec4 tl = texture(u_sampler, st / u_texsize);\n"
    "    float lodfrac = 0.0;\n"
    "    uint tcm = bits(u_tm, 14, 3), tcam = bits(u_tm, 23, 3);\n"
    "    vec3 f = vec3(0.0);\n"
    "    if (tcm == 1u) f = tl.rgb; else if (tcm == 3u) f = vec3(tl.a); else if (tcm >= 4u && tcm <= 5u) f = vec3(lodfrac);\n"
    "    bool tz = bits(u_tm, 12, 1) != 0u, ts = bits(u_tm, 13, 1) != 0u, trv = bits(u_tm, 17, 1) != 0u, tinv = bits(u_tm, 20, 1) != 0u;\n"
    "    uint tad = bits(u_tm, 18, 2);\n"
    "    texc.r = combine(tz, ts, 0.0, tl.r, f.r, trv, tad, tl.r, tl.a, tinv);\n"
    "    texc.g = combine(tz, ts, 0.0, tl.g, f.g, trv, tad, tl.g, tl.a, tinv);\n"
    "    texc.b = combine(tz, ts, 0.0, tl.b, f.b, trv, tad, tl.b, tl.a, tinv);\n"
    "    float fa = (tcam == 1u || tcam == 3u) ? tl.a : (tcam == 4u || tcam == 5u) ? lodfrac : 0.0;\n"
    "    uint aad = bits(u_tm, 27, 2);\n"
    "    texc.a = combine(bits(u_tm, 21, 1) != 0u, bits(u_tm, 22, 1) != 0u, 0.0, tl.a, fa, bits(u_tm, 26, 1) != 0u,\n"
    "                     aad != 0u ? 2u : 0u, tl.a, tl.a, bits(u_tm, 29, 1) != 0u);\n"
    "  }\n"
    "  uint cp = u_cp;\n"
    "  uint rsel = bits(cp, 0, 2), asel = bits(cp, 2, 2);\n"
    "  vec4 other = rsel == 0u ? it : rsel == 1u ? texc : u_color1;\n"
    "  other.a = asel == 0u ? it.a : asel == 1u ? texc.a : u_color1.a;\n"
    "  if (bits(u_fbz, 1, 1) != 0u) {\n"
    "    uvec3 o8 = uvec3(other.rgb * 255.0 + 0.5);\n"
    "    if (o8 == uvec3(bits(u_chroma, 16, 8), bits(u_chroma, 8, 8), bits(u_chroma, 0, 8))) discard;\n"
    "  }\n"
    "  bool lsel = bits(cp, 7, 1) != 0u ? (texc.a >= 0.5) : (bits(cp, 4, 1) != 0u);\n"
    "  vec4 local = lsel ? u_color0 : it;\n"
    "  uint las = bits(cp, 5, 2);\n"
    "  local.a = las == 0u ? it.a : las == 1u ? u_color0.a : las == 2u ? clamp(v_zw.x / 65535.0, 0.0, 1.0) : clamp(v_zw.y, 0.0, 1.0);\n"
    "  uint ms = bits(cp, 10, 3);\n"
    "  vec3 f = ms == 1u ? local.rgb : ms == 2u ? vec3(other.a) : ms == 3u ? vec3(local.a) : ms == 4u ? vec3(texc.a) : ms == 5u ? texc.rgb : vec3(0.0);\n"
    "  uint mas = bits(cp, 19, 3);\n"
    "  float fa = (mas == 1u || mas == 3u) ? local.a : mas == 2u ? other.a : mas == 4u ? texc.a : 0.0;\n"
    "  bool cz = bits(cp, 8, 1) != 0u, cs = bits(cp, 9, 1) != 0u, crv = bits(cp, 13, 1) != 0u, cinv = bits(cp, 16, 1) != 0u;\n"
    "  uint cad = bits(cp, 14, 2);\n"
    "  vec4 c;\n"
    "  c.r = combine(cz, cs, other.r, local.r, f.r, crv, cad, local.r, local.a, cinv);\n"
    "  c.g = combine(cz, cs, other.g, local.g, f.g, crv, cad, local.g, local.a, cinv);\n"
    "  c.b = combine(cz, cs, other.b, local.b, f.b, crv, cad, local.b, local.a, cinv);\n"
    "  c.a = combine(bits(cp, 17, 1) != 0u, bits(cp, 18, 1) != 0u, other.a, local.a, fa, bits(cp, 22, 1) != 0u,\n"
    "                bits(cp, 23, 2) != 0u ? 2u : 0u, local.a, local.a, bits(cp, 25, 1) != 0u);\n"
    "  if (bits(u_fbz, 13, 1) != 0u && c.a < 0.5) discard;\n"
    "  if (bits(u_am, 0, 1) != 0u) {\n"
    "    uint a8 = uint(c.a * 255.0 + 0.5), ref = bits(u_am, 24, 8), fn = bits(u_am, 1, 3);\n"
    "    bool pass = fn == 1u ? a8 < ref : fn == 2u ? a8 == ref : fn == 3u ? a8 <= ref : fn == 4u ? a8 > ref :\n"
    "                fn == 5u ? a8 != ref : fn == 6u ? a8 >= ref : fn == 7u;\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "  float depth = bits(u_fbz, 3, 1) != 0u ? wdepth(v_zw.y) : v_zw.x;\n"
    "  if (bits(u_fbz, 16, 1) != 0u) depth += float(int(u_za << 16) >> 16);\n"
    "  if (bits(u_fbz, 20, 1) != 0u) depth = float(u_za & 0xFFFFu);\n"
    "  gl_FragDepth = clamp(depth, 0.0, 65535.0) / 65535.0;\n"
    "  o_col = vec4(c.rgb, c.a);\n"
    "}\n";

/* ---- render targets ---- */
#define MAX_TARGETS 8
typedef struct { uint32_t addr; GLuint tex; } Target;

/* ---- texture cache ---- */
#define TEX_CACHE 512
typedef struct {
  int used;
  uint32_t key_base, key_fmt, key_lod, key_pal;
  uint32_t vram_lo, vram_hi; /* byte range */
  uint32_t stamp;            /* g_vram_stamp at upload */
  uint64_t last_use;
  GLuint tex;
  int levels, w0, h0;
} TexEntry;

typedef struct {
  float x, y, z, wb, w0, r, g, b, a, s, t;
} Vtx;

static struct {
  int width, height; /* render target size */
  float scale;
  float xmin, xmax; /* game X range shown (0..512 at 4:3; wider for widescreen) */
  GLuint prog, vao, vbo, fbo, blit_fbo;
  GLint u_xrange, u_cp, u_fbz, u_am, u_tm, u_chroma, u_za, u_color0, u_color1, u_texsize, u_tex, u_sampler, u_yorigin, u_flip;
  Target col[MAX_TARGETS], aux[MAX_TARGETS];
  TexEntry tex[TEX_CACHE];
  uint64_t use_clock;
  Vtx* verts;
  int nverts, cap;
  V3dState batch_state;
  int batch_valid;
  int batch_sprite; /* the batch holds 2D sprites (see is_sprite) */
  uint32_t display_addr;
  RendererGlFrameFn on_frame;
  void* on_frame_user;
  uint64_t frame;
  RendererGlStats stats;
} G;

RendererGlStats* renderer_gl_stats(void) { return &G.stats; }

static GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "OpenGL shader error:\n%s\n", log);
    return 0;
  }
  return s;
}

static Target* target(Target* list, uint32_t addr, int depth) {
  for (int i = 0; i < MAX_TARGETS; ++i)
    if (list[i].tex && list[i].addr == addr) return &list[i];
  for (int i = 0; i < MAX_TARGETS; ++i)
    if (!list[i].tex) {
      list[i].addr = addr;
      glGenTextures(1, &list[i].tex);
      glBindTexture(GL_TEXTURE_2D, list[i].tex);
      if (depth)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, G.width, G.height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
      else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, G.width, G.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      return &list[i];
    }
  return &list[0];
}

static void bind_targets(const V3dState* st) {
  Target* c = target(G.col, st->colBufferAddr & 0xFFFFF0u, 0);
  Target* a = target(G.aux, st->auxBufferAddr & 0xFFFFF0u, 1);
  glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, a->tex, 0);
  glViewport(0, 0, G.width, G.height);
}

/* Scissor from a clip rectangle in game coordinates (before the Y-origin flip). */
static void set_scissor(const V3dState* st, int use_clip) {
  float x0 = 0, x1 = 512, y0 = 0, y1 = 384;
  if (use_clip) {
    x0 = (float)((st->clipLeftRight >> 16) & 0x3FF); x1 = (float)(st->clipLeftRight & 0x3FF);
    y0 = (float)((st->clipLowYHighY >> 16) & 0x3FF); y1 = (float)(st->clipLowYHighY & 0x3FF);
  }
  /* widescreen: a full-width clip window opens up to the whole widened view, except for 2D
     sprites (HUD, text), which are laid out for the 4:3 screen: the game parks unused player
     panels just outside it and relies on the clip window to hide them. */
  if (x0 <= 0 && x1 >= 512 && !G.batch_sprite) { x0 = G.xmin; x1 = G.xmax; }
  if (st->fbzMode & (1u << 17)) { /* rows = yorigin + 1 - y */
    const float yo = (float)((banshee_io_reg(0x10) >> 18) & 0xFFF) + 1.0f;
    const float a = yo - y1, b = yo - y0;
    y0 = a; y1 = b;
  }
  /* GL window coordinates are bottom-up; our image row 0 is at the top (NDC +1). */
  const int sx0 = (int)lroundf((x0 - G.xmin) * G.scale), sx1 = (int)lroundf((x1 - G.xmin) * G.scale);
  const int top = (int)lroundf(y0 * G.scale), bot = (int)lroundf(y1 * G.scale);
  glEnable(GL_SCISSOR_TEST);
  glScissor(sx0, G.height - bot, sx1 > sx0 ? sx1 - sx0 : 0, bot > top ? bot - top : 0);
}

/* ---- textures ---- */
static int tex_valid(const TexEntry* e) {
  for (uint32_t p = e->vram_lo >> BANSHEE_PAGE_SHIFT; p <= (e->vram_hi - 1) >> BANSHEE_PAGE_SHIFT; ++p)
    if (g_vram_page_stamp[p & ((BANSHEE_VRAM_SIZE >> BANSHEE_PAGE_SHIFT) - 1)] > e->stamp) return 0;
  return 1;
}

static TexEntry* texture_for(const V3dState* st, TexInfo* ti) {
  vtex_setup(st, ti);
  const int lmin = ti->lodmin >> 2;
  int lmax = ti->lodmax >> 2;
  if (lmax > 8) lmax = 8;
  if (lmax < lmin) lmax = lmin;
  const uint32_t key_base = ti->lodoff[lmin];
  const uint32_t key_lod = (st->tLOD & 0x01FC0FFFu); /* range, aspect, split, multibase */
  const uint32_t key_pal = (ti->fmt == 5 || ti->fmt == 6 || ti->fmt == 14) ? g_v3d_palette_version : 0;
  TexEntry* free_e = NULL;
  TexEntry* lru = &G.tex[0];
  for (int i = 0; i < TEX_CACHE; ++i) {
    TexEntry* e = &G.tex[i];
    if (!e->used) { if (!free_e) free_e = e; continue; }
    if (e->key_base == key_base && e->key_fmt == ti->fmt && e->key_lod == key_lod && e->key_pal == key_pal) {
      if (tex_valid(e)) { e->last_use = ++G.use_clock; return e; }
      free_e = e; /* stale: re-upload in place */
      break;
    }
    if (e->last_use < lru->last_use) lru = e;
  }
  TexEntry* e = free_e ? free_e : lru;
  if (!e->tex) glGenTextures(1, &e->tex);
  e->used = 1;
  e->key_base = key_base; e->key_fmt = ti->fmt; e->key_lod = key_lod; e->key_pal = key_pal;
  e->stamp = g_vram_stamp;
  e->last_use = ++G.use_clock;
  e->levels = lmax - lmin + 1;
  e->w0 = (ti->wmask >> lmin) + 1;
  e->h0 = (ti->hmask >> lmin) + 1;
  e->vram_lo = ti->lodoff[lmin];
  e->vram_hi = ti->lodoff[lmax] + (uint32_t)(((ti->wmask >> lmax) + 1) * ((ti->hmask >> lmax) + 1)) * ti->bpp + 4;
  glBindTexture(GL_TEXTURE_2D, e->tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  uint8_t* buf = malloc((size_t)e->w0 * e->h0 * 4);
  for (int l = lmin; l <= lmax; ++l) {
    const int w = (ti->wmask >> l) + 1, h = (ti->hmask >> l) + 1;
    for (int i = 0; i < w * h; ++i) {
      const VtColor c = vtex_decode(ti->fmt, vtex_raw(ti, l, (uint32_t)i));
      buf[i * 4 + 0] = (uint8_t)(c.r * 255.0f + 0.5f);
      buf[i * 4 + 1] = (uint8_t)(c.g * 255.0f + 0.5f);
      buf[i * 4 + 2] = (uint8_t)(c.b * 255.0f + 0.5f);
      buf[i * 4 + 3] = (uint8_t)(c.a * 255.0f + 0.5f);
    }
    glTexImage2D(GL_TEXTURE_2D, l - lmin, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  }
  free(buf);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, e->levels - 1);
  G.stats.texture_uploads++;
  return e;
}

static void apply_sampler(const TexInfo* ti, const TexEntry* e) {
  const int mip = e->levels > 1;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ti->bilinear_mag ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  ti->bilinear_min ? (mip ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR)
                                   : (mip ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ti->clamp_s ? GL_CLAMP_TO_EDGE : ti->mirror_s ? GL_MIRRORED_REPEAT : GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, ti->clamp_t ? GL_CLAMP_TO_EDGE : ti->mirror_t ? GL_MIRRORED_REPEAT : GL_REPEAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, ti->lodbias);
}

/* ---- state ---- */
static GLenum blend_gl(int f, int src) {
  switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_SRC_ALPHA;
    case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return GL_DST_ALPHA;
    case 4: return GL_ONE;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 7: return GL_ONE_MINUS_DST_ALPHA;
    case 15: return src ? GL_SRC_ALPHA_SATURATE : GL_ONE;
    default: return GL_ZERO;
  }
}

static void uniform_color(GLint loc, uint32_t argb) {
  glUniform4f(loc, ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f, (argb >> 24) / 255.0f);
}

static void apply_state(const V3dState* st) {
  bind_targets(st);
  glUseProgram(G.prog);
  const uint32_t fbz = st->fbzMode, am = st->alphaMode;
  glUniform1ui(G.u_cp, st->fbzColorPath);
  glUniform1ui(G.u_fbz, fbz);
  glUniform1ui(G.u_am, am);
  glUniform1ui(G.u_tm, st->textureMode);
  glUniform1ui(G.u_chroma, st->chromaKey);
  glUniform1ui(G.u_za, st->zaColor);
  uniform_color(G.u_color0, st->color0);
  uniform_color(G.u_color1, st->color1);
  glUniform1f(G.u_yorigin, (float)((banshee_io_reg(0x10) >> 18) & 0xFFF));
  glUniform1i(G.u_flip, (fbz >> 17) & 1);
  glUniform2f(G.u_xrange, G.xmin, 2.0f / (G.xmax - G.xmin));
  const int texturing = (st->fbzColorPath >> 27) & 1;
  glUniform1i(G.u_tex, texturing);
  if (texturing) {
    TexInfo ti;
    TexEntry* e = texture_for(st, &ti);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    apply_sampler(&ti, e);
    glUniform1i(G.u_sampler, 0);
    glUniform2f(G.u_texsize, (float)(ti.wmask + 1), (float)(ti.hmask + 1));
  }
  /* depth: test always enabled so that the aux write mask works without a depth test */
  glEnable(GL_DEPTH_TEST);
  static const GLenum dfunc[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
  glDepthFunc((fbz & (1u << 4)) ? dfunc[(fbz >> 5) & 7] : GL_ALWAYS);
  glDepthMask((fbz & (1u << 10)) && !(fbz & (1u << 18)) ? GL_TRUE : GL_FALSE);
  const GLboolean rgb = (fbz & (1u << 9)) ? GL_TRUE : GL_FALSE;
  glColorMask(rgb, rgb, rgb, GL_FALSE); /* destination alpha stays 1 (no alpha planes) */
  if (am & (1u << 4)) {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(blend_gl((am >> 8) & 15, 1), blend_gl((am >> 12) & 15, 0), GL_ONE, GL_ZERO);
  } else {
    glDisable(GL_BLEND);
  }
  set_scissor(st, fbz & 1);
}

static void flush(void) {
  if (!G.nverts) return;
  apply_state(&G.batch_state);
  glBindVertexArray(G.vao);
  glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(G.nverts * sizeof(Vtx)), G.verts, GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, G.nverts);
  G.stats.draw_calls++;
  G.nverts = 0;
}

static int same_state(const V3dState* a, const V3dState* b) {
  return memcmp(a, b, offsetof(V3dState, fog_table)) == 0;
}

/* 2D sprites (HUD, text) are drawn at fixed layer depths: the same 1/w on all three vertices and
   much larger than any world geometry's (measured: sprites 1/18 .. 1/1.6, world <= 0.0112). */
static int is_sprite(const V3dVertex v[3]) {
  return v[0].wb == v[1].wb && v[1].wb == v[2].wb && v[0].wb >= 0.02f;
}

/* A 2D sprite covering the whole 512x384 screen is an overlay (the white flash on camera cuts,
   fades): in widescreen it is stretched over the widened view instead of leaving the sides bare. */
static int is_fullscreen(const V3dVertex v[3]) {
  float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
  for (int i = 1; i < 3; ++i) {
    x0 = fminf(x0, v[i].x); x1 = fmaxf(x1, v[i].x);
    y0 = fminf(y0, v[i].y); y1 = fmaxf(y1, v[i].y);
  }
  return x0 <= 0.5f && x1 >= 511.5f && y0 <= 0.5f && y1 >= 383.5f;
}

static void gl_triangle(void* user, const V3dState* st, const V3dVertex v[3]) {
  (void)user;
  int sprite = is_sprite(v);
  const int widen = sprite && G.xmin < 0.0f && is_fullscreen(v);
  if (widen) sprite = 0; /* not clipped to the 4:3 screen (see set_scissor) */
  if (G.batch_valid && (!same_state(st, &G.batch_state) || sprite != G.batch_sprite)) flush();
  if (!G.batch_valid || G.nverts == 0) { G.batch_state = *st; G.batch_valid = 1; G.batch_sprite = sprite; }
  /* textures written since the batch began must not be used stale */
  if (G.nverts + 3 > G.cap) {
    G.cap = G.cap ? G.cap * 2 : 65536;
    G.verts = realloc(G.verts, (size_t)G.cap * sizeof(Vtx));
  }
  for (int i = 0; i < 3; ++i) {
    Vtx* o = &G.verts[G.nverts++];
    o->x = v[i].x; o->y = v[i].y; o->z = v[i].z; o->wb = v[i].wb; o->w0 = v[i].w0;
    o->r = v[i].r; o->g = v[i].g; o->b = v[i].b; o->a = v[i].a; o->s = v[i].s0; o->t = v[i].t0;
    if (widen) o->x = o->x <= 256.0f ? G.xmin : G.xmax;
  }
  G.stats.triangles++;
}

static void gl_fastfill(void* user, const V3dState* st) {
  (void)user;
  flush();
  bind_targets(st);
  G.batch_sprite = 0;
  set_scissor(st, 1);
  const uint32_t fbz = st->fbzMode;
  GLbitfield mask = 0;
  if (fbz & (1u << 9)) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(((st->color1 >> 16) & 255) / 255.0f, ((st->color1 >> 8) & 255) / 255.0f, (st->color1 & 255) / 255.0f, 1.0f);
    mask |= GL_COLOR_BUFFER_BIT;
  }
  if ((fbz & (1u << 10)) && !(fbz & (1u << 18))) {
    glDepthMask(GL_TRUE);
    glClearDepth((st->zaColor & 0xFFFF) / 65535.0);
    mask |= GL_DEPTH_BUFFER_BIT;
  }
  if (mask) glClear(mask);
}

/* Texture memory changed: batches already queued were built with the old contents. */
static void gl_texture_written(void* user, uint32_t addr, uint32_t bytes) {
  (void)user; (void)addr; (void)bytes;
  if (G.nverts && (G.batch_state.fbzColorPath & (1u << 27))) flush();
}

static void gl_swap(void* user, uint32_t display_addr) {
  (void)user;
  flush();
  G.display_addr = display_addr & 0xFFFFF0u;
  G.stats.frames++;
  if (G.on_frame) G.on_frame(G.on_frame_user, G.frame);
  G.frame++;
}

/* The render targets are created lazily at the current size (see target), so a new size only
   needs the old ones dropped: the game redraws every buffer before showing it again. */
int renderer_gl_set_view(float scale, float aspect) {
  if (scale < 1.0f) scale = 1.0f;
  if (aspect < 4.0f / 3.0f) aspect = 4.0f / 3.0f;
  const float xmin = 256.0f - 192.0f * aspect, xmax = 256.0f + 192.0f * aspect;
  const int w = (int)lroundf((xmax - xmin) * scale), h = (int)lroundf(384 * scale);
  if (scale == G.scale && xmin == G.xmin && xmax == G.xmax && w == G.width && h == G.height) return 0;
  flush();
  G.scale = scale;
  G.xmin = xmin;
  G.xmax = xmax;
  if (w != G.width || h != G.height) {
    for (int i = 0; i < MAX_TARGETS; ++i) {
      if (G.col[i].tex) glDeleteTextures(1, &G.col[i].tex);
      if (G.aux[i].tex) glDeleteTextures(1, &G.aux[i].tex);
    }
    memset(G.col, 0, sizeof G.col);
    memset(G.aux, 0, sizeof G.aux);
    G.width = w;
    G.height = h;
  }
  return 1;
}

int renderer_gl_init(float scale, float aspect, RendererGlFrameFn fn, void* user) {
  memset(&G, 0, sizeof G);
  renderer_gl_set_view(scale, aspect);
  G.on_frame = fn;
  G.on_frame_user = user;
  GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) return -1;
  G.prog = glCreateProgram();
  glAttachShader(G.prog, vs);
  glAttachShader(G.prog, fs);
  glLinkProgram(G.prog);
  GLint ok = 0;
  glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(G.prog, sizeof log, NULL, log);
    fprintf(stderr, "OpenGL link error:\n%s\n", log);
    return -1;
  }
#define U(n) G.u_##n = glGetUniformLocation(G.prog, "u_" #n)
  U(xrange); U(cp); U(fbz); U(am); U(tm); U(chroma); U(za); U(color0); U(color1); U(texsize); U(tex); U(sampler); U(yorigin); U(flip);
#undef U
  glGenVertexArrays(1, &G.vao);
  glBindVertexArray(G.vao);
  glGenBuffers(1, &G.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, x));
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, z));
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, r));
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, s));
  for (int i = 0; i < 4; ++i) glEnableVertexAttribArray((GLuint)i);
  glGenFramebuffers(1, &G.fbo);
  glGenFramebuffers(1, &G.blit_fbo);
  V3dBackend be = {gl_triangle, gl_fastfill, gl_swap, gl_texture_written, NULL};
  v3d_set_backend(&be);
  return 0;
}

void renderer_gl_size(int* w, int* h) { *w = G.width; *h = G.height; }

/* Draw the displayed buffer into the window's framebuffer (dst rectangle in window pixels). */
void renderer_gl_present(int win_w, int win_h, int dx, int dy, int dw, int dh) {
  Target* c = target(G.col, G.display_addr, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, G.blit_fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, win_w, win_h);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlitFramebuffer(0, 0, G.width, G.height, dx, win_h - dy - dh, dx + dw, win_h - dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Read the displayed buffer back (RGBA8, top row first). */
void renderer_gl_read_display(uint8_t* rgba) {
  Target* c = target(G.col, G.display_addr, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, G.blit_fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  uint8_t* tmp = malloc((size_t)G.width * G.height * 4);
  glReadPixels(0, 0, G.width, G.height, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
  for (int y = 0; y < G.height; ++y)
    memcpy(rgba + (size_t)y * G.width * 4, tmp + (size_t)(G.height - 1 - y) * G.width * 4, (size_t)G.width * 4);
  free(tmp);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
