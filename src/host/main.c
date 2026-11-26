// NBA Showtime host program: runs the recompiled BIOS + game on the Vegas machine model, renders
// the Banshee output with OpenGL in an SDL3 window and plays the DCS2 sound board's audio.
//
// usage: nbashowtime [options]
//   --ram FILE     game entry RAM snapshot      (default data/entry_ram.bin, else work/dumps/entry_ram.bin)
//   --regs FILE    entry register JSON           (default data/entry_regs.json, else work/dumps/entry_regs.json)
//   --disk FILE    hard disk, .chd or raw image  (default data/nbashowt.chd, else work/nbashowt.img)
//   --sound FILE   DCS2 boot ROM (vegassio.bin)  (default data/vegassio.bin, else work/bios/vegassio.bin)
//   --game FILE    game library built by the launcher (default nbagame.so/.dll/.dylib next to the executable)
//   The data/ defaults are next to the executable, where the launcher puts them; the work/ ones
//   are the developer tree (run from the repository root).
//   --scale S      render scale (x 512x384); default: fit the window height
//   --window WxH   initial window size           (default 1280x960)
//   --fullscreen   desktop fullscreen
//   --aspect A     render aspect ratio, e.g. 16:9 or 21:9 (default 4:3): widescreen
//   --stretch      fill the window (ignore the aspect ratio)
//   --vsync        present frames on the display's refresh (no tearing; best on 120 Hz+ displays,
//                  on a 60 Hz display a frame can wait almost a whole refresh)
//   --volume P     sound volume in percent (default 300; the board's output is quiet)
//   --no-sound     skip audio output
//   --unthrottled  run as fast as possible
//   --dump DIR     write rendered frames as PNG (with --dump-every N, --dump-start N)
//   --frames N     quit after N frames; --hidden: no visible window (testing)
#include <math.h>
#include <stdarg.h>
#if defined(__linux__)
#include <execinfo.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../runtime/src/dcs2.h"
#include "../../runtime/src/ide.h"
#include "../../runtime/src/raster_sw.h"
#include "../../runtime/src/widescreen.h"
#ifndef NBA_STATIC_GAME
#include "game_loader.h"
#endif
#include "gl_funcs.h"
#include "input.h"
#include "recomp.h"
#include "renderer_gl.h"
#include "vegas.h"

static struct {
  SDL_Window* window;
  SDL_GLContext gl;
  SDL_AudioStream* audio;
  int stretch, throttle, verbose, perf;
  int vsync;     /* present on the display's refresh (time-based pacing still sets the speed) */
  float aspect; /* render aspect ratio */
  uint64_t frame; /* buffer swaps so far */
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
/* Keep at most ~80 ms of audio queued: anything beyond that would only add latency (e.g. audio
   produced while the emulation briefly ran ahead), so it is dropped. */
static double g_audio_sq; /* --perf: sum of squares and peak of the samples played */
static uint64_t g_audio_n;
static int g_audio_peak;

static void pump_audio(void) {
  int16_t buf[4096 * 2];
  int n;
  const int max_queued = dcs2_audio_rate() * 4 * 80 / 1000;
  while ((n = dcs2_audio_read(buf, 4096)) > 0) {
    if (H.perf)
      for (int i = 0; i < n * 2; ++i) {
        const int v = buf[i];
        g_audio_sq += (double)v * v;
        if (abs(v) > g_audio_peak) g_audio_peak = abs(v);
      }
    g_audio_n += (uint64_t)n * 2;
    if (H.audio && SDL_GetAudioStreamQueued(H.audio) < max_queued) SDL_PutAudioStreamData(H.audio, buf, n * 4);
  }
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

/* Keep game time in step with wall time: the arcade's 57 Hz. This is the only clock; with
   --vsync the swap also waits for the display's next refresh, which only delays a frame by up to
   one refresh (pacing then sleeps less). Vsync is not used as the clock because what drivers do
   with swap intervals varies (on Wayland an interval of 2 is ignored, the reported refresh rate
   can differ from the real one), and two clocks fighting hold frames for extra refreshes. */
static double g_pace_sleep_ms; /* --perf */

static void pace(void) {
  const double virt = g_vegas.cop0.cycles / 125e6;
  const uint64_t now = SDL_GetTicksNS();
  const double wall = (now - H.wall_base_ns) / 1e9;
  const double ahead = (virt - H.virt_base) - wall;
  if (ahead > 0) {
    SDL_DelayPrecise((uint64_t)(ahead * 1e9));
    g_pace_sleep_ms += ahead * 1e3;
  }
  else if (ahead < -0.25) { /* fell behind (loading, slow host): don't try to catch up */
    H.wall_base_ns = now;
    H.virt_base = virt;
  }
}

static uint64_t g_frame_done_ns; /* --perf: end of the previous present */
static double g_emu_worst, g_present_worst;

static void on_frame(void* user, uint64_t frame) {
  (void)user;
  const uint64_t t_emu_end = SDL_GetTicksNS();
  if (H.perf && g_frame_done_ns) {
    const double e = (t_emu_end - g_frame_done_ns) / 1e6;
    if (e > g_emu_worst) g_emu_worst = e;
  }
  int ww, wh;
  SDL_GetWindowSizeInPixels(H.window, &ww, &wh);
  int dw = ww, dh = wh, dx = 0, dy = 0;
  if (!H.stretch) { /* keep the render aspect ratio, black bars around */
    if (ww > wh * H.aspect) { dw = (int)(wh * H.aspect); dx = (ww - dw) / 2; }
    else { dh = (int)(ww / H.aspect); dy = (wh - dh) / 2; }
  }
  renderer_gl_present(ww, wh, dx, dy, dw, dh);
  SDL_GL_SwapWindow(H.window);
  if (H.vsync) glFinish(); /* wait for this vblank here: don't let the driver queue frames (latency) */
  g_frame_done_ns = SDL_GetTicksNS();
  if (H.perf && (g_frame_done_ns - t_emu_end) / 1e6 > g_present_worst) g_present_worst = (g_frame_done_ns - t_emu_end) / 1e6;

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
  H.frame = frame + 1;
  if (H.perf) { /* frame pacing: wall time between presented frames */
    static uint64_t prev;
    static double worst;
    static int late;
    const uint64_t t = SDL_GetTicksNS();
    if (prev) {
      const double dt = (t - prev) / 1e6;
      if (dt > worst) worst = dt;
      if (dt > 1.25 * 1000.0 / 57.0) late++;
    }
    prev = t;
    if (H.frame % 60 == 0) {
      fprintf(stderr, "[perf]   present: worst gap %.1f ms, %d late frames; worst emulation %.1f ms, worst present+swap %.1f ms\n",
              worst, late, g_emu_worst, g_present_worst);
      fprintf(stderr, "[perf]   paced sleep %.0f ms\n", g_pace_sleep_ms);
      g_pace_sleep_ms = 0;
      g_emu_worst = g_present_worst = 0;
      worst = 0;
      late = 0;
    }
  }
  if (H.perf && H.frame % 60 == 0) { /* speed report: game frames vs host time */
    static uint64_t last_ns, last_cyc;
    const uint64_t now = SDL_GetTicksNS(), cyc = g_vegas.cop0.cycles;
    if (last_ns)
      fprintf(stderr, "[perf] swap %llu: 60 swaps in %.0f ms host, %.0f ms game (%.2fx)\n", (unsigned long long)H.frame,
              (now - last_ns) / 1e6, (cyc - last_cyc) / 125e3, ((cyc - last_cyc) / 125e3) / ((now - last_ns) / 1e6));
    static Dcs2Stats ld;
    const Dcs2Stats* ds = &g_dcs2_stats;
    fprintf(stderr, "[perf]   dcs2: %llu host words, %llu replies, %llu samples, %llu irq1, %.1f M dsp insns\n",
            (unsigned long long)(ds->host_writes - ld.host_writes), (unsigned long long)(ds->output_writes - ld.output_writes),
            (unsigned long long)(ds->samples - ld.samples), (unsigned long long)(ds->irq1 - ld.irq1),
            (ds->instructions - ld.instructions) / 1e6);
    ld = *ds;
    fprintf(stderr, "[perf]   audio: rms %.0f, peak %d\n", g_audio_n ? sqrt(g_audio_sq / g_audio_n) : 0.0, g_audio_peak);
    {
      static uint64_t lv[VT_COUNT];
      static const char* names[VT_COUNT] = {"insn", "count", "ide", "banshee", "ioasic", "mmio"};
      fprintf(stderr, "[perf]   vtime ms:");
      for (int k = 0; k < VT_COUNT; ++k) { fprintf(stderr, " %s %.0f", names[k], (g_vtime[k] - lv[k]) / 125e3); lv[k] = g_vtime[k]; }
      fprintf(stderr, "\n");
    }
    g_audio_sq = 0;
    g_audio_n = 0;
    g_audio_peak = 0;
    if (H.frame % 600 == 0) dcs2_debug_dump();
    last_ns = now;
    last_cyc = cyc;
  }
}

/* Every emulated vblank, also during loading screens where the game does not swap buffers. */
static void stall_report(void) {
  fprintf(stderr, "-- no buffer swap for 10 s of game time (virtual %.1f s, swaps %llu) --\n",
          g_vegas.cop0.cycles / 125e6, (unsigned long long)H.frame);
  fprintf(stderr, "IDE: %llu commands, %llu sectors read; DCS2: %llu host words in, %llu replies out\n",
          (unsigned long long)g_ide_stats.commands, (unsigned long long)g_ide_stats.sectors_read,
          (unsigned long long)g_dcs2_stats.host_writes, (unsigned long long)g_dcs2_stats.output_writes);
#ifdef RECOMP_TRACE_CALLS
  fprintf(stderr, "recent guest functions (newest first):");
  for (int k = 0; k < 96; ++k) fprintf(stderr, "%s%08X", k % 12 ? " " : "\n  ", recomp_call_ring[(recomp_call_pos - 1 - (uint32_t)k) & (RECOMP_CALL_RING - 1)]);
  fprintf(stderr, "\n");
#endif
#if defined(__linux__)
  {
    /* host stack: the generated func_XXXXXXXX frames show where the game is waiting */
    void* frames[64];
    const int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
  }
#endif
  if (getenv("NBA_STALL_EXIT")) quit(5);
}

static void on_vblank(void) {
  static uint64_t last_swap_frame, vblanks_since;
  if (H.frame != last_swap_frame) { last_swap_frame = H.frame; vblanks_since = 0; }
  else if (++vblanks_since == 570) stall_report();
  pump_audio();
  handle_events();
  input_update(H.frame);
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

static int file_exists(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f) fclose(f);
  return f != NULL;
}

int main(int argc, char** argv) {
  const char* ram = "work/dumps/entry_ram.bin";
  const char* regs = "work/dumps/entry_regs.json";
  const char* disk = "work/nbashowt.img";
  const char* sound = "work/bios/vegassio.bin";
  static char data_ram[1100], data_regs[1100], data_disk[1100], data_sound[1100];
  {
    /* A launcher build keeps its data in data/ next to the executable. */
    const char* base = SDL_GetBasePath();
    if (base) {
      snprintf(data_ram, sizeof data_ram, "%sdata/entry_ram.bin", base);
      if (file_exists(data_ram)) {
        snprintf(data_regs, sizeof data_regs, "%sdata/entry_regs.json", base);
        snprintf(data_disk, sizeof data_disk, "%sdata/nbashowt.chd", base);
        snprintf(data_sound, sizeof data_sound, "%sdata/vegassio.bin", base);
        ram = data_ram, regs = data_regs, disk = data_disk, sound = data_sound;
      }
    }
  }
  const char* game_lib = NULL;
  float scale = 0;
  int win_w = 1280, win_h = 960, fullscreen = 0, hidden = 0, no_sound = 0;
  float volume = 3.0f;
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
    else if (ARG("--game")) game_lib = next;
    else if (ARG("--scale")) scale = (float)atof(next);
    else if (ARG("--window")) sscanf(next, "%dx%d", &win_w, &win_h);
    else if (ARG("--aspect")) {
      float a = 0, b = 0;
      if (sscanf(next, "%f:%f", &a, &b) == 2 && b > 0) H.aspect = a / b;
      else H.aspect = (float)atof(next);
    }
    else if (ARG("--dump")) H.dump_dir = next;
    else if (ARG("--volume")) volume = (float)atof(next) / 100.0f;
    else if (ARG("--dump-every")) H.dump_every = strtoull(next, NULL, 0);
    else if (ARG("--dump-start")) H.dump_start = strtoull(next, NULL, 0);
    else if (ARG("--frames")) H.max_frames = strtoull(next, NULL, 0);
    else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
    else if (!strcmp(a, "--stretch")) H.stretch = 1;
    else if (!strcmp(a, "--vsync")) H.vsync = 1;
    else if (!strcmp(a, "--no-sound")) no_sound = 1;
    else if (!strcmp(a, "--unthrottled")) H.throttle = 0;
    else if (!strcmp(a, "--hidden")) hidden = 1;
    else if (!strcmp(a, "--verbose")) H.verbose = 1;
    else if (!strcmp(a, "--perf")) H.perf = 1;
    else { fprintf(stderr, "unknown option %s (see the comment at the top of src/host/main.c)\n", a); return 2; }
#undef ARG
  }
  if (!H.dump_every) H.dump_every = 1;

#ifndef NBA_STATIC_GAME
  {
    /* The recompiled game lives in a library the launcher built from the player's ROM. */
    static char lib_path[1200], why[1024];
    if (!game_lib) {
      game_library_default_path(lib_path, sizeof lib_path);
      game_lib = lib_path;
    }
    if (game_library_load(game_lib, why, sizeof why) != 0) {
      fprintf(stderr, "%s\n", why);
      if (!hidden) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NBA Showtime", why, NULL);
      return 1;
    }
  }
#else
  (void)game_lib;
#endif
  vegas_log = quiet_log;
  {
    extern int ide_trace;
    ide_trace = getenv("TRACE_IDE") != NULL; /* with --verbose */
  }
  RecompContext ctx;
  {
    /* the VRC5074 / SIO dumps live next to the register JSON */
    char dir[1200], nile[1300], sio[1300];
    snprintf(dir, sizeof dir, "%s", regs);
    char* slash = strrchr(dir, '/');
#ifdef _WIN32
    char* bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
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
  SDL_SyncWindow(H.window); /* let fullscreen take effect before sizing the render target */
  if (nba_gl_load() != 0) return 1;
  if (!H.vsync || !SDL_GL_SetSwapInterval(1)) {
    H.vsync = 0;
    SDL_GL_SetSwapInterval(0); /* paced by the emulated vblank instead */
  }
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
    if (H.audio) {
      SDL_SetAudioStreamGain(H.audio, volume);
      SDL_ResumeAudioStreamDevice(H.audio);
    }
    else fprintf(stderr, "no audio: %s\n", SDL_GetError());
  }
  input_init();
  vegas_vblank_hook = on_vblank;
  H.wall_base_ns = SDL_GetTicksNS();
  H.virt_base = g_vegas.cop0.cycles / 125e6;

  recomp_lookup(&ctx, 0x80100000)(&ctx); /* game entry; never returns: the host work happens at each buffer swap */
  quit(0);
  return 0;
}
