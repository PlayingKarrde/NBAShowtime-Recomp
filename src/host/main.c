// NBA Showtime host program: runs the recompiled BIOS + game on the Vegas machine model, renders
// the Banshee output with OpenGL in an SDL3 window and plays the DCS2 sound board's audio.
//
// usage: nbashowtime [options]
//   --ram FILE     game entry RAM snapshot      (default work/dumps/entry_ram.bin)
//   --regs FILE    entry register JSON           (default work/dumps/entry_regs.json)
//   --disk FILE    hard disk image               (default work/nbashowt.img)
//   --sound FILE   DCS2 boot ROM (vegassio.bin)  (default work/bios/vegassio.bin)
//   --scale S      render scale (x 512x384); default: fit the window height
//   --window WxH   initial window size           (default 1280x960)
//   --fullscreen   desktop fullscreen
//   --aspect A     render aspect ratio, e.g. 16:9 or 21:9 (default 4:3): widescreen
//   --stretch      fill the window (ignore the aspect ratio)
//   --no-sound     skip audio output
//   --unthrottled  run as fast as possible
//   --dump DIR     write rendered frames as PNG (with --dump-every N, --dump-start N)
//   --frames N     quit after N frames; --hidden: no visible window (testing)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../runtime/src/dcs2.h"
#include "../../runtime/src/raster_sw.h"
#include "../../runtime/src/widescreen.h"
#include "gl_funcs.h"
#include "input.h"
#include "recomp.h"
#include "recomp_funcs.h"
#include "renderer_gl.h"
#include "vegas.h"

static struct {
  SDL_Window* window;
  SDL_GLContext gl;
  SDL_AudioStream* audio;
  int stretch, throttle, verbose;
  float aspect; /* render aspect ratio */
  const char* dump_dir;
  uint64_t dump_every, dump_start, max_frames;
  uint64_t wall_base_ns;
  double virt_base;
  uint8_t* readback;
} H;

/* ---- runtime hooks ---- */
static void quiet_log(const char* fmt, ...) {
  if (!H.verbose) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void quit(int code) {
  if (H.audio) SDL_DestroyAudioStream(H.audio);
  if (H.gl) SDL_GL_DestroyContext(H.gl);
  if (H.window) SDL_DestroyWindow(H.window);
  SDL_Quit();
  exit(code);
}

void recomp_syscall(RecompContext* ctx, uint32_t code, uint32_t pc) {
  (void)pc;
  if (code == 23) { /* exit() */
    fprintf(stderr, "game called exit(%d)\n", (int)(int32_t)ctx->r[4]);
    quit(1);
  }
  if (!recomp_bios_syscall(ctx, code)) fprintf(stderr, "syscall %u has no BIOS handler\n", code);
  vegas_poll_interrupts(ctx);
}
void recomp_break(RecompContext* ctx, uint32_t code, uint32_t pc) {
  fprintf(stderr, "BREAK %u at %08X (ra %08X)\n", code, pc, (uint32_t)ctx->r[31]);
  quit(2);
}
void recomp_trap(RecompContext* ctx, uint32_t pc) {
  fprintf(stderr, "TRAP at %08X (ra %08X)\n", pc, (uint32_t)ctx->r[31]);
  quit(2);
}
void recomp_unhandled(RecompContext* ctx, uint32_t pc, const char* what) {
  fprintf(stderr, "unhandled %s at %08X (ra %08X)\n", what, pc, (uint32_t)ctx->r[31]);
  quit(3);
}
#ifdef RECOMP_TRACE_CALLS
void recomp_break_hit(RecompContext* ctx, uint32_t addr) { (void)ctx; (void)addr; }
void recomp_watch_hit(RecompContext* ctx, uint32_t addr, uint32_t value) { (void)ctx; (void)addr; (void)value; }
#endif

/* ---- per frame (called from the game's buffer swap) ---- */
static void pump_audio(void) {
  if (!H.audio) return;
  int16_t buf[4096 * 2];
  int n;
  while ((n = dcs2_audio_read(buf, 4096)) > 0) SDL_PutAudioStreamData(H.audio, buf, n * 4);
}

static void handle_events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    input_event(&e);
    if (e.type == SDL_EVENT_QUIT) quit(0);
    if (e.type == SDL_EVENT_KEY_DOWN) {
      if (e.key.key == SDLK_ESCAPE) quit(0);
      if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
        const int fs = (SDL_GetWindowFlags(H.window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreen(H.window, !fs);
      }
      if (e.key.key == SDLK_F2) H.stretch = !H.stretch;
    }
  }
}

static void pace(void) {
  const double virt = g_vegas.cop0.cycles / 125e6;
  const uint64_t now = SDL_GetTicksNS();
  const double wall = (now - H.wall_base_ns) / 1e9;
  const double ahead = (virt - H.virt_base) - wall;
  if (ahead > 0) SDL_DelayPrecise((uint64_t)(ahead * 1e9));
  else if (ahead < -0.25) { /* fell behind (loading, slow host): don't try to catch up */
    H.wall_base_ns = now;
    H.virt_base = virt;
  }
}

static void on_frame(void* user, uint64_t frame) {
  (void)user;
  int ww, wh;
  SDL_GetWindowSizeInPixels(H.window, &ww, &wh);
  int dw = ww, dh = wh, dx = 0, dy = 0;
  if (!H.stretch) { /* keep the render aspect ratio, black bars around */
    if (ww > wh * H.aspect) { dw = (int)(wh * H.aspect); dx = (ww - dw) / 2; }
    else { dh = (int)(ww / H.aspect); dy = (wh - dh) / 2; }
  }
  renderer_gl_present(ww, wh, dx, dy, dw, dh);
  SDL_GL_SwapWindow(H.window);

  if (H.dump_dir && frame >= H.dump_start && (frame - H.dump_start) % H.dump_every == 0) {
    int w, h;
    renderer_gl_size(&w, &h);
    if (!H.readback) H.readback = malloc((size_t)w * h * 4);
    renderer_gl_read_display(H.readback);
    char path[1024];
    snprintf(path, sizeof path, "%s/gl_%06llu.png", H.dump_dir, (unsigned long long)frame);
    raster_sw_write_png(path, H.readback, w, h);
    fprintf(stderr, "[frame] %s\n", path);
  }
  if (H.max_frames && frame + 1 >= H.max_frames) {
    RendererGlStats* s = renderer_gl_stats();
    fprintf(stderr, "%llu frames, %llu triangles, %llu draw calls, %llu texture uploads, virtual %.2f s\n",
            (unsigned long long)s->frames, (unsigned long long)s->triangles, (unsigned long long)s->draw_calls,
            (unsigned long long)s->texture_uploads, g_vegas.cop0.cycles / 125e6);
    quit(0);
  }
  pump_audio();
  handle_events();
  input_update(frame);
  if (H.throttle) pace();
}

/* ---- boot ---- */
static const char* gpr_names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                    "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

static int json_u64(const char* json, const char* key, uint64_t* out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\": \"", key);
  const char* p = strstr(json, pat);
  if (!p) return 0;
  *out = strtoull(p + strlen(pat), NULL, 16);
  return 1;
}

static int load_entry(RecompContext* ctx, const char* ram_path, const char* regs_path) {
  recomp_rdram = calloc(1, RECOMP_RAM_SIZE);
  FILE* f = fopen(ram_path, "rb");
  if (!f || fread(recomp_rdram, 1, RECOMP_RAM_SIZE, f) != RECOMP_RAM_SIZE) {
    fprintf(stderr, "cannot read RAM snapshot %s (dump it with tools/mame/run_dump.sh)\n", ram_path);
    if (f) fclose(f);
    return -1;
  }
  fclose(f);
  static char json[65536];
  f = fopen(regs_path, "rb");
  if (!f) { fprintf(stderr, "cannot read %s\n", regs_path); return -1; }
  json[fread(json, 1, sizeof json - 1, f)] = 0;
  fclose(f);
  memset(ctx, 0, sizeof *ctx);
  for (int i = 1; i < 32; ++i) json_u64(json, gpr_names[i], &ctx->r[i]);
  json_u64(json, "HI", &ctx->hi);
  json_u64(json, "LO", &ctx->lo);
  for (int i = 0; i < 32; ++i) {
    char k[16];
    uint64_t v = 0;
    snprintf(k, sizeof k, "FPR%d", i);
    if (json_u64(json, k, &v)) ctx->f[i] = (uint32_t)v;
  }
  uint64_t v = 0;
  if (json_u64(json, "CCR31", &v)) ctx->fcsr = (uint32_t)v;
  if (json_u64(json, "SR", &v)) g_vegas.cop0.sr = (uint32_t)v;
  if (json_u64(json, "Count", &v)) g_vegas.cop0.count_base = (uint32_t)v;
  if (json_u64(json, "Compare", &v)) g_vegas.cop0.compare = (uint32_t)v;
  return 0;
}

int main(int argc, char** argv) {
  const char* ram = "work/dumps/entry_ram.bin";
  const char* regs = "work/dumps/entry_regs.json";
  const char* disk = "work/nbashowt.img";
  const char* sound = "work/bios/vegassio.bin";
  float scale = 0;
  int win_w = 1280, win_h = 960, fullscreen = 0, hidden = 0, no_sound = 0;
  H.throttle = 1;
  H.aspect = 4.0f / 3.0f;
  H.dump_every = 60;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && next && (++i, 1))
    if (ARG("--ram")) ram = next;
    else if (ARG("--regs")) regs = next;
    else if (ARG("--disk")) disk = next;
    else if (ARG("--sound")) sound = next;
    else if (ARG("--scale")) scale = (float)atof(next);
    else if (ARG("--window")) sscanf(next, "%dx%d", &win_w, &win_h);
    else if (ARG("--aspect")) {
      float a = 0, b = 0;
      if (sscanf(next, "%f:%f", &a, &b) == 2 && b > 0) H.aspect = a / b;
      else H.aspect = (float)atof(next);
    }
    else if (ARG("--dump")) H.dump_dir = next;
    else if (ARG("--dump-every")) H.dump_every = strtoull(next, NULL, 0);
    else if (ARG("--dump-start")) H.dump_start = strtoull(next, NULL, 0);
    else if (ARG("--frames")) H.max_frames = strtoull(next, NULL, 0);
    else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
    else if (!strcmp(a, "--stretch")) H.stretch = 1;
    else if (!strcmp(a, "--no-sound")) no_sound = 1;
    else if (!strcmp(a, "--unthrottled")) H.throttle = 0;
    else if (!strcmp(a, "--hidden")) hidden = 1;
    else if (!strcmp(a, "--verbose")) H.verbose = 1;
    else { fprintf(stderr, "unknown option %s (see the comment at the top of src/host/main.c)\n", a); return 2; }
#undef ARG
  }
  if (!H.dump_every) H.dump_every = 1;

  vegas_log = quiet_log;
  RecompContext ctx;
  {
    /* the VRC5074 / SIO dumps live next to the register JSON */
    char dir[1024], nile[1100], sio[1100];
    snprintf(dir, sizeof dir, "%s", regs);
    char* slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    snprintf(nile, sizeof nile, "%sentry_nile.bin", dir);
    snprintf(sio, sizeof sio, "%sentry_sio.bin", dir);
    if (vegas_init(disk, nile, sio, sound) != 0) { fprintf(stderr, "cannot open disk image %s\n", disk); return 1; }
  }
  if (load_entry(&ctx, ram, regs) != 0) return 1;
  widescreen_patch(H.aspect);

  if (!SDL_Init(SDL_INIT_VIDEO | (no_sound ? 0 : SDL_INIT_AUDIO))) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  if (hidden) flags |= SDL_WINDOW_HIDDEN;
  H.window = SDL_CreateWindow("NBA Showtime", win_w, win_h, flags);
  if (!H.window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  H.gl = SDL_GL_CreateContext(H.window);
  if (!H.gl) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
  SDL_GL_SetSwapInterval(0); /* paced by the emulated vblank instead */
  if (nba_gl_load() != 0) return 1;
  if (scale <= 0) {
    int pw, ph;
    SDL_GetWindowSizeInPixels(H.window, &pw, &ph);
    scale = ph / 384.0f;
    if (scale < 1) scale = 1;
  }
  if (renderer_gl_init(scale, H.aspect, on_frame, NULL) != 0) return 1;
  {
    int w, h;
    renderer_gl_size(&w, &h);
    fprintf(stderr, "OpenGL %s; rendering at %dx%d\n", (const char*)glGetString(GL_VERSION), w, h);
  }
  if (!no_sound) {
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, dcs2_audio_rate()};
    H.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (H.audio) SDL_ResumeAudioStreamDevice(H.audio);
    else fprintf(stderr, "no audio: %s\n", SDL_GetError());
  }
  input_init();
  H.wall_base_ns = SDL_GetTicksNS();
  H.virt_base = g_vegas.cop0.cycles / 125e6;

  func_80100000(&ctx); /* never returns: the host work happens at each buffer swap */
  quit(0);
  return 0;
}
