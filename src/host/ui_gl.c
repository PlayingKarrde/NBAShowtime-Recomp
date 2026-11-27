// 2D drawing for the in-game menu (see ui_gl.h). Everything is one shader drawing quads: shapes
// are signed distance functions evaluated per pixel (rounded / slanted boxes, triangles), text
// samples the baked distance field atlases of assets/fonts/menu_font.bin (made by
// tools/font/make_menu_font.py), and the paused game frame is shown from a downscaled copy with a
// blur. Output is premultiplied alpha.
#include "ui_gl.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_funcs.h"
#include "miniz.h"

extern const unsigned char nba_asset_menu_font[];
extern const size_t nba_asset_menu_font_size;

enum { MODE_BOX, MODE_TEXT, MODE_BLUR, MODE_TRI };
enum { FIRST_CHAR = 32, NUM_CHARS = 95 };

typedef struct {
  float x, y, lx, ly;
  float r, g, b, a;
  float r2, g2, b2, a2;
  float p0[4], p1[4];
} UiVtx;

typedef struct {
  float x, y, w, h, xoff, yoff, adv;
} Glyph;

typedef struct {
  float size, ascent, descent, spread, cap;
  int aw, ah;
  Glyph g[NUM_CHARS];
  float kern[NUM_CHARS][NUM_CHARS];
  GLuint tex;
} Font;

static struct {
  int ok;
  GLuint prog, vao, vbo;
  GLint u_screen, u_tex;
  Font font[UI_FONT_COUNT];
  UiVtx* v;
  int n, cap;
  GLuint cur_tex;
  int win_w, win_h;
  /* snapshot: three successive halvings of the game frame */
  GLuint snap_tex[3], fbo_read, fbo_draw;
  int snap_w[3], snap_h[3];
} U;

static const char* kVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_loc;\n"
    "layout(location=2) in vec4 a_col; layout(location=3) in vec4 a_col2;\n"
    "layout(location=4) in vec4 a_p0; layout(location=5) in vec4 a_p1;\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_loc; out vec4 v_col; flat out vec4 v_col2; flat out vec4 v_p0; out vec4 v_p1;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "  v_loc = a_loc; v_col = a_col; v_col2 = a_col2; v_p0 = a_p0; v_p1 = a_p1;\n"
    "}\n";

static const char* kFS =
    "#version 330 core\n"
    "in vec2 v_loc; in vec4 v_col; flat in vec4 v_col2; flat in vec4 v_p0; in vec4 v_p1;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_col;\n"
    "vec3 ramp(float t, vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 c4) {\n"
    "  t = clamp(t, 0.0, 1.0);\n"
    "  if (t < 0.46) return mix(c0, c1, t / 0.46);\n"
    "  if (t < 0.54) return mix(c1, c2, (t - 0.46) / 0.08);\n"
    "  if (t < 0.80) return mix(c2, c3, (t - 0.54) / 0.26);\n"
    "  return mix(c3, c4, (t - 0.80) / 0.20);\n"
    "}\n"
    "vec3 chrome(float t) { return ramp(t, vec3(1.0), vec3(0.80, 0.85, 0.94), vec3(0.32, 0.38, 0.52), vec3(0.78, 0.83, 0.93), vec3(1.0)); }\n"
    "vec3 gold(float t) { return ramp(t, vec3(1.0, 0.98, 0.78), vec3(1.0, 0.84, 0.25), vec3(0.72, 0.40, 0.02), vec3(1.0, 0.76, 0.16), vec3(1.0, 0.95, 0.58)); }\n"
    "vec4 pm(vec4 c) { return vec4(c.rgb * c.a, c.a); }\n"
    "float sd_box(vec2 p, vec2 b, float r) { vec2 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "float sd_tri(vec2 p, vec2 b) {\n" /* pointing right, half extents b */
    "  vec2 n = normalize(vec2(b.y, 2.0 * b.x));\n"
    "  return max(-b.x - p.x, dot(vec2(p.x, abs(p.y)), n) - dot(vec2(b.x, 0.0), n));\n"
    "}\n"
    "void main() {\n"
    "  int mode = int(v_p1.x + 0.5);\n"
    "  if (mode == 0 || mode == 3) {\n"
    "    vec2 hb = v_p0.xy; float r = v_p0.z, bw = v_p0.w;\n"
    "    vec2 p = vec2(v_loc.x + v_p1.y * v_loc.y, v_loc.y);\n"
    "    float d;\n"
    "    if (mode == 0) d = sd_box(p, hb, r);\n"
    "    else {\n" /* triangle: v_p1.w = direction (left, right, up, down) */
    "      int dir = int(v_p1.w + 0.5); vec2 q = v_loc;\n"
    "      if (dir == 0) q.x = -q.x;\n"
    "      else if (dir == 2) q = vec2(-v_loc.y, v_loc.x);\n"
    "      else if (dir == 3) q = v_loc.yx;\n"
    "      d = sd_tri(q, dir >= 2 ? hb.yx : hb);\n"
    "    }\n"
    "    float cov = clamp(0.5 - d, 0.0, 1.0);\n"
    "    float t = v_loc.y / max(hb.y, 1.0) * 0.5 + 0.5;\n"
    "    vec4 fill = v_col;\n"
    "    if (mode == 0) fill.rgb = mix(fill.rgb, vec3(1.0), v_p1.z * 0.55 * (1.0 - smoothstep(0.08, 0.52, t)) * step(0.06, t));\n"
    "    vec4 c = pm(fill);\n"
    "    if (mode == 0 && bw > 0.0) {\n"
    "      vec4 bc = v_p1.w > 0.5 ? vec4(chrome(t), v_col2.a) : v_col2;\n"
    "      c = mix(pm(bc), c, clamp(0.5 - (d + bw), 0.0, 1.0));\n"
    "    }\n"
    "    o_col = c * cov;\n"
    "  } else if (mode == 1) {\n"
    "    float s = texture(u_tex, v_loc).r;\n"
    "    float d = (s - 128.0 / 255.0) * (255.0 / 127.0) * v_p0.y * v_p0.x;\n"
    "    float af = clamp(d + 0.5, 0.0, 1.0);\n"
    "    int style = int(v_p1.y + 0.5);\n"
    "    vec3 fc = style == 1 ? chrome(v_p1.z) : style == 2 ? gold(v_p1.z) : v_col.rgb;\n"
    "    vec4 c = vec4(fc * v_col.a, v_col.a) * af;\n"
    "    if (v_p0.z > 0.0 || v_p0.w > 0.0) {\n"
    "      float ao = clamp((d + v_p0.z) / (v_p0.w + 1.0) + 0.5, 0.0, 1.0);\n"
    "      c += pm(v_col2) * ao * (1.0 - c.a);\n"
    "    }\n"
    "    o_col = c;\n"
    "  } else {\n"
    "    vec2 r = v_p0.xy; vec3 acc = vec3(0.0); float wsum = 0.0;\n"
    "    for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {\n"
    "      float w = (3.0 - abs(float(i))) * (3.0 - abs(float(j)));\n"
    "      acc += texture(u_tex, v_loc + vec2(i, j) * r).rgb * w; wsum += w;\n"
    "    }\n"
    "    o_col = vec4(acc / wsum * v_col.rgb * v_col.a, v_col.a);\n"
    "  }\n"
    "}\n";

static GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "menu shader error:\n%s\n", log);
    return 0;
  }
  return s;
}

/* ---- fonts ---- */
typedef struct {
  const unsigned char* p;
  size_t left;
} Reader;

static int rd(Reader* r, void* out, size_t n) {
  if (r->left < n) return 0;
  memcpy(out, r->p, n);
  r->p += n;
  r->left -= n;
  return 1;
}

static int load_fonts(void) {
  Reader r = {nba_asset_menu_font, nba_asset_menu_font_size};
  char magic[4];
  uint32_t version, nfonts;
  if (!rd(&r, magic, 4) || memcmp(magic, "NBAF", 4) || !rd(&r, &version, 4) || !rd(&r, &nfonts, 4) || version != 1 ||
      nfonts < UI_FONT_COUNT)
    return -1;
  for (int fi = 0; fi < UI_FONT_COUNT; ++fi) {
    Font* f = &U.font[fi];
    float hdr[4];
    uint32_t cnt[5];
    if (!rd(&r, hdr, sizeof hdr) || !rd(&r, cnt, sizeof cnt)) return -1;
    f->size = hdr[0]; f->ascent = hdr[1]; f->descent = hdr[2]; f->spread = hdr[3];
    const uint32_t nglyphs = cnt[0], nkern = cnt[1], zsize = cnt[4];
    f->aw = (int)cnt[2];
    f->ah = (int)cnt[3];
    for (uint32_t i = 0; i < nglyphs; ++i) {
      uint32_t cp;
      uint16_t box[4];
      float m[3];
      if (!rd(&r, &cp, 4) || !rd(&r, box, sizeof box) || !rd(&r, m, sizeof m)) return -1;
      if (cp < FIRST_CHAR || cp >= FIRST_CHAR + NUM_CHARS) continue;
      Glyph* g = &f->g[cp - FIRST_CHAR];
      g->x = box[0]; g->y = box[1]; g->w = box[2]; g->h = box[3];
      g->xoff = m[0]; g->yoff = m[1]; g->adv = m[2];
    }
    for (uint32_t i = 0; i < nkern; ++i) {
      uint16_t ab[2];
      float k;
      if (!rd(&r, ab, sizeof ab) || !rd(&r, &k, 4)) return -1;
      if (ab[0] >= FIRST_CHAR && ab[0] < FIRST_CHAR + NUM_CHARS && ab[1] >= FIRST_CHAR && ab[1] < FIRST_CHAR + NUM_CHARS)
        f->kern[ab[0] - FIRST_CHAR][ab[1] - FIRST_CHAR] = k;
    }
    if (r.left < zsize) return -1;
    size_t out_len = 0;
    void* pix = tinfl_decompress_mem_to_heap(r.p, zsize, &out_len, TINFL_FLAG_PARSE_ZLIB_HEADER);
    r.p += zsize;
    r.left -= zsize;
    if (!pix || out_len != (size_t)f->aw * f->ah) { free(pix); return -1; }
    /* cap height: top of 'H' above the baseline (the distance field pads the box by spread + 1) */
    const Glyph* H = &f->g['H' - FIRST_CHAR];
    f->cap = -(H->yoff + f->spread + 1.0f);
    glGenTextures(1, &f->tex);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, f->aw, f->ah, 0, GL_RED, GL_UNSIGNED_BYTE, pix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    mz_free(pix);
  }
  return 0;
}

int ui_init(void) {
  if (U.ok) return 0;
  GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) return -1;
  U.prog = glCreateProgram();
  glAttachShader(U.prog, vs);
  glAttachShader(U.prog, fs);
  glLinkProgram(U.prog);
  GLint ok = 0;
  glGetProgramiv(U.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(U.prog, sizeof log, NULL, log);
    fprintf(stderr, "menu shader link error:\n%s\n", log);
    return -1;
  }
  U.u_screen = glGetUniformLocation(U.prog, "u_screen");
  U.u_tex = glGetUniformLocation(U.prog, "u_tex");
  glGenVertexArrays(1, &U.vao);
  glBindVertexArray(U.vao);
  glGenBuffers(1, &U.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, U.vbo);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVtx), (void*)offsetof(UiVtx, x));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVtx), (void*)offsetof(UiVtx, lx));
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UiVtx), (void*)offsetof(UiVtx, r));
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(UiVtx), (void*)offsetof(UiVtx, r2));
  glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(UiVtx), (void*)offsetof(UiVtx, p0));
  glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(UiVtx), (void*)offsetof(UiVtx, p1));
  for (int i = 0; i < 6; ++i) glEnableVertexAttribArray((GLuint)i);
  glGenFramebuffers(1, &U.fbo_read);
  glGenFramebuffers(1, &U.fbo_draw);
  glGenTextures(3, U.snap_tex);
  if (load_fonts() != 0) { fprintf(stderr, "menu: bad font data\n"); return -1; }
  U.ok = 1;
  return 0;
}

/* ---- batching ---- */
static void flush(void) {
  if (!U.n) return;
  glBindBuffer(GL_ARRAY_BUFFER, U.vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(U.n * sizeof(UiVtx)), U.v, GL_STREAM_DRAW);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, U.cur_tex);
  glDrawArrays(GL_TRIANGLES, 0, U.n);
  U.n = 0;
}

static void use_tex(GLuint tex) {
  if (tex != U.cur_tex) { flush(); U.cur_tex = tex; }
}

void ui_begin(int win_w, int win_h) {
  U.win_w = win_w;
  U.win_h = win_h;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, win_w, win_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glUseProgram(U.prog);
  glUniform2f(U.u_screen, (float)win_w, (float)win_h);
  glUniform1i(U.u_tex, 0);
  glBindVertexArray(U.vao);
  U.cur_tex = U.font[0].tex;
}

void ui_end(void) {
  flush();
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glActiveTexture(GL_TEXTURE0);
}

void ui_clip(float x, float y, float w, float h) {
  flush();
  if (w <= 0 || h <= 0) { glDisable(GL_SCISSOR_TEST); return; }
  glEnable(GL_SCISSOR_TEST);
  const int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
  glScissor(x0, U.win_h - y1, x1 - x0, y1 - y0);
}

static UiVtx* quad(void) {
  if (U.n + 6 > U.cap) {
    U.cap = U.cap ? U.cap * 2 : 4096;
    U.v = realloc(U.v, (size_t)U.cap * sizeof(UiVtx));
  }
  UiVtx* q = &U.v[U.n];
  U.n += 6;
  return q;
}

static void color4(uint32_t c, float* out) {
  out[0] = ((c >> 16) & 255) / 255.0f;
  out[1] = ((c >> 8) & 255) / 255.0f;
  out[2] = (c & 255) / 255.0f;
  out[3] = (c >> 24) / 255.0f;
}

/* Corners 0..3 = top-left, top-right, bottom-right, bottom-left; two triangles. */
static void emit(UiVtx* q, const UiVtx c[4]) {
  static const int idx[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) q[i] = c[idx[i]];
}

uint32_t ui_alpha(uint32_t color, float a) {
  if (a < 0) a = 0;
  if (a > 1) a = 1;
  return (color & 0xFFFFFFu) | (uint32_t)lroundf((color >> 24) * a) << 24;
}

uint32_t ui_lerp_color(uint32_t a, uint32_t b, float t) {
  uint32_t out = 0;
  for (int s = 0; s < 32; s += 8) {
    const float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
    out |= (uint32_t)lroundf(ca + (cb - ca) * t) << s;
  }
  return out;
}

static void shape(int mode, float x, float y, float w, float h, uint32_t top, uint32_t bottom, uint32_t border, float radius,
                  float border_w, float skew, float gloss, float flag) {
  use_tex(U.cur_tex); /* shapes don't sample: any texture will do */
  const float hw = w * 0.5f, hh = h * 0.5f, cx = x + hw, cy = y + hh;
  const float m = 1.5f, ex = fabsf(skew) * hh + m; /* coverage margin, slant */
  UiVtx c[4];
  memset(c, 0, sizeof c);
  const float lx[4] = {-hw - ex, hw + ex, hw + ex, -hw - ex}, ly[4] = {-hh - m, -hh - m, hh + m, hh + m};
  float ct[4], cb[4], bc[4];
  color4(top, ct);
  color4(bottom, cb);
  color4(border, bc);
  for (int i = 0; i < 4; ++i) {
    c[i].x = cx + lx[i];
    c[i].y = cy + ly[i];
    c[i].lx = lx[i];
    c[i].ly = ly[i];
    const float t = (ly[i] + hh) / (h > 0 ? h : 1);
    c[i].r = ct[0] + (cb[0] - ct[0]) * t;
    c[i].g = ct[1] + (cb[1] - ct[1]) * t;
    c[i].b = ct[2] + (cb[2] - ct[2]) * t;
    c[i].a = ct[3] + (cb[3] - ct[3]) * t;
    c[i].r2 = bc[0]; c[i].g2 = bc[1]; c[i].b2 = bc[2]; c[i].a2 = bc[3];
    c[i].p0[0] = hw; c[i].p0[1] = hh; c[i].p0[2] = fminf(radius, fminf(hw, hh)); c[i].p0[3] = border_w;
    c[i].p1[0] = (float)mode; c[i].p1[1] = skew; c[i].p1[2] = gloss; c[i].p1[3] = flag;
  }
  emit(quad(), c);
}

void ui_box(float x, float y, float w, float h, const UiBox* b) {
  shape(MODE_BOX, x, y, w, h, b->top, b->bottom, b->border, b->radius, b->border_w, b->skew, b->gloss,
        b->chrome_border ? 1.0f : 0.0f);
}

void ui_fill(float x, float y, float w, float h, uint32_t color) { shape(MODE_BOX, x, y, w, h, color, color, 0, 0, 0, 0, 0, 0); }
void ui_fill_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
  shape(MODE_BOX, x, y, w, h, top, bottom, 0, 0, 0, 0, 0, 0);
}

void ui_fill_h(float x, float y, float w, float h, uint32_t left, uint32_t right) {
  /* a horizontal gradient: two boxes would show a seam, so use per-corner colours directly */
  use_tex(U.cur_tex);
  UiVtx c[4];
  memset(c, 0, sizeof c);
  const float hw = w * 0.5f, hh = h * 0.5f;
  const float px[4] = {x, x + w, x + w, x}, py[4] = {y, y, y + h, y + h};
  float cl[4], cr[4];
  color4(left, cl);
  color4(right, cr);
  for (int i = 0; i < 4; ++i) {
    const float* cc = (i == 1 || i == 2) ? cr : cl;
    c[i].x = px[i]; c[i].y = py[i];
    c[i].lx = px[i] - (x + hw); c[i].ly = py[i] - (y + hh);
    c[i].r = cc[0]; c[i].g = cc[1]; c[i].b = cc[2]; c[i].a = cc[3];
    c[i].p0[0] = hw + 2; c[i].p0[1] = hh + 2; /* no edge anti-aliasing: the box is larger than the quad */
    c[i].p1[0] = MODE_BOX;
  }
  emit(quad(), c);
}

void ui_triangle(float cx, float cy, float w, float h, int dir, uint32_t color) {
  shape(MODE_TRI, cx - w * 0.5f, cy - h * 0.5f, w, h, color, color, 0, 0, 0, 0, 0, (float)dir);
}

/* ---- text ---- */
static const Glyph* glyph(const Font* f, unsigned char ch) {
  if (ch < FIRST_CHAR || ch >= FIRST_CHAR + NUM_CHARS) ch = '?';
  return &f->g[ch - FIRST_CHAR];
}

float ui_text_width(int font, float px, const char* s) {
  const Font* f = &U.font[font];
  const float sc = px / f->size;
  float w = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    w += glyph(f, *p)->adv;
    if (p[1] && *p >= FIRST_CHAR && *p < FIRST_CHAR + NUM_CHARS && p[1] >= FIRST_CHAR && p[1] < FIRST_CHAR + NUM_CHARS)
      w += f->kern[*p - FIRST_CHAR][p[1] - FIRST_CHAR];
  }
  return w * sc;
}

static void text_pass(const UiText* t, float x, float base, const char* s, uint32_t top, uint32_t bottom, float outline,
                      float soft, uint32_t outline_col, int style) {
  const Font* f = &U.font[t->font];
  use_tex(f->tex);
  const float sc = t->px / f->size;
  const float cap = f->cap * sc, line_top = base - cap;
  float ct[4], cb[4], oc[4];
  color4(top, ct);
  color4(bottom, cb);
  color4(outline_col, oc);
  float pen = x;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    const Glyph* g = glyph(f, *p);
    if (g->w > 0) {
      const float gx = pen + g->xoff * sc, gy = base + g->yoff * sc, gw = g->w * sc, gh = g->h * sc;
      UiVtx c[4];
      memset(c, 0, sizeof c);
      const float px[4] = {gx, gx + gw, gx + gw, gx}, py[4] = {gy, gy, gy + gh, gy + gh};
      const float u[4] = {g->x, g->x + g->w, g->x + g->w, g->x}, v[4] = {g->y, g->y, g->y + g->h, g->y + g->h};
      for (int i = 0; i < 4; ++i) {
        const float tt = (py[i] - line_top) / (cap > 0 ? cap : 1);
        const float tc = tt < 0 ? 0 : tt > 1 ? 1 : tt;
        c[i].x = px[i]; c[i].y = py[i];
        c[i].lx = u[i] / f->aw; c[i].ly = v[i] / f->ah;
        c[i].r = ct[0] + (cb[0] - ct[0]) * tc;
        c[i].g = ct[1] + (cb[1] - ct[1]) * tc;
        c[i].b = ct[2] + (cb[2] - ct[2]) * tc;
        c[i].a = ct[3] + (cb[3] - ct[3]) * tc;
        c[i].r2 = oc[0]; c[i].g2 = oc[1]; c[i].b2 = oc[2]; c[i].a2 = oc[3];
        c[i].p0[0] = sc; c[i].p0[1] = f->spread; c[i].p0[2] = outline; c[i].p0[3] = soft;
        c[i].p1[0] = MODE_TEXT; c[i].p1[1] = (float)style; c[i].p1[2] = tt;
      }
      emit(quad(), c);
    }
    pen += g->adv * sc;
    if (p[1] && *p >= FIRST_CHAR && *p < FIRST_CHAR + NUM_CHARS && p[1] >= FIRST_CHAR && p[1] < FIRST_CHAR + NUM_CHARS)
      pen += f->kern[*p - FIRST_CHAR][p[1] - FIRST_CHAR] * sc;
  }
}

float ui_text(const UiText* t, float x, float y, int align, const char* s) {
  const Font* f = &U.font[t->font];
  const float w = ui_text_width(t->font, t->px, s);
  if (align == UI_ALIGN_CENTER) x -= w * 0.5f;
  else if (align == UI_ALIGN_RIGHT) x -= w;
  const float base = y + f->cap * (t->px / f->size) * 0.5f;
  /* outlines and glows can't reach past the distance field's spread */
  const float reach = f->spread * (t->px / f->size) - 1.0f;
  float outline = t->outline, soft = t->soft;
  if (outline + soft > reach) { outline = fminf(outline, reach); soft = fmaxf(0.0f, reach - outline); }
  if (t->shadow != 0.0f)
    text_pass(t, x + t->shadow * 0.6f, base + t->shadow, s, t->shadow_col, t->shadow_col, outline, soft + t->shadow * 0.5f,
              t->shadow_col, UI_TEXT_PLAIN);
  text_pass(t, x, base, s, t->top, t->bottom, outline, soft, t->outline_col, t->style);
  return w;
}

/* ---- snapshot of the game frame ---- */
void ui_snapshot(unsigned tex, int w, int h) {
  GLuint src = tex;
  int sw = w, sh = h;
  for (int i = 0; i < 3; ++i) {
    const int dw = sw / 2 > 1 ? sw / 2 : 1, dh = sh / 2 > 1 ? sh / 2 : 1;
    glBindTexture(GL_TEXTURE_2D, U.snap_tex[i]);
    if (U.snap_w[i] != dw || U.snap_h[i] != dh) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      U.snap_w[i] = dw;
      U.snap_h[i] = dh;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, U.fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, U.fbo_draw);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, U.snap_tex[i], 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    src = U.snap_tex[i];
    sw = dw;
    sh = dh;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ui_snapshot_draw(float x, float y, float w, float h, uint32_t tint) {
  use_tex(U.snap_tex[2]);
  UiVtx c[4];
  memset(c, 0, sizeof c);
  const float px[4] = {x, x + w, x + w, x}, py[4] = {y, y, y + h, y + h};
  /* the texture is bottom-up (GL), the screen top-down */
  const float u[4] = {0, 1, 1, 0}, v[4] = {1, 1, 0, 0};
  float tc[4];
  color4(tint, tc);
  const float rx = 1.0f / (float)(U.snap_w[2] > 0 ? U.snap_w[2] : 1), ry = 1.0f / (float)(U.snap_h[2] > 0 ? U.snap_h[2] : 1);
  for (int i = 0; i < 4; ++i) {
    c[i].x = px[i]; c[i].y = py[i];
    c[i].lx = u[i]; c[i].ly = v[i];
    c[i].r = tc[0]; c[i].g = tc[1]; c[i].b = tc[2]; c[i].a = tc[3];
    c[i].p0[0] = rx; c[i].p0[1] = ry;
    c[i].p1[0] = MODE_BLUR;
  }
  emit(quad(), c);
}
