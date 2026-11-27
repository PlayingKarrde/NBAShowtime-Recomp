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
//   --save-dir DIR where the game's saved data goes: nvram.bin (the battery-backed CMOS: audits,
//                  operator adjustments) and disk.bin (sectors the game wrote to its hard disk);
//                  default: next to settings.ini
//   --no-save      start from the boot snapshot's state every run and keep nothing
//   --scale S      render scale (x 512x384); default: follow the window size
//   --window WxH   initial window size           (default 1280x960)
//   --fullscreen   desktop fullscreen
//   --aspect A     render aspect ratio: auto (default: match the window, Hor+ widescreen up to
//                  32:9), or a fixed ratio such as 4:3, 16:9 or 21:9 (black bars around)
//   --vsync        present frames on the display's refresh (no tearing; best on 120 Hz+ displays,
//                  on a 60 Hz display a frame can wait almost a whole refresh)
//   --volume P     sound volume in percent (default 300; the board's output is quiet)
//   Window, aspect, scale, vsync and volume default to the saved settings (settings.ini, changed
//   from the in-game menu: Esc or L3+R3); options given here apply to this run only.
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
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> /* WinMain on Windows, where the engine is a GUI program */

#ifdef _WIN32
/* The engine is built as a GUI program on Windows (no console window behind the game). Run from a
   terminal, it borrows that terminal's console so messages still show; redirected output is
   left alone. */
__declspec(dllimport) int __stdcall AttachConsole(unsigned long process_id);
static void attach_parent_console(void) {
  if (_fileno(stdout) >= 0 && _fileno(stderr) >= 0) return; /* already redirected */
  if (!AttachConsole((unsigned long)-1 /* ATTACH_PARENT_PROCESS */)) return;
  if (_fileno(stdout) < 0) freopen("CONOUT$", "w", stdout);
  if (_fileno(stderr) < 0) freopen("CONOUT$", "w", stderr);
}
#endif

#include "../../runtime/src/dcs2.h"
#include "../../runtime/src/ioasic.h"
#include "../../runtime/src/ide.h"
#include "../../runtime/src/nvram.h"
#include "../../runtime/src/raster_sw.h"
#include "../../runtime/src/widescreen.h"
#ifndef NBA_STATIC_GAME
#include "game_loader.h"
#endif
#include "gl_funcs.h"
#include "input.h"
#include "menu.h"
#include "recomp.h"
#include "renderer_gl.h"
#include "settings.h"
#include "vegas.h"

static struct {
  SDL_Window* window;
  SDL_GLContext gl;
  SDL_AudioStream* audio;
  int throttle, verbose, perf;
  /* vsync, the requested aspect ratio and render scale are in g_settings */
  float aspect;      /* render aspect ratio in use */
  int focused;       /* the window has keyboard focus (mute in background) */
  int menu_request;  /* open the menu at the next buffer swap */
  uint64_t menu_at;
  char disk_path[1300]; /* saved disk sectors ("" = not kept) */  /* NBA_MENU_AT=N: open the menu at swap N (with NBA_MENU_SCRIPT, see menu.c) */
  uint64_t frame; /* buffer swaps so far */
  const char* dump_dir;
  uint64_t dump_every, dump_start, max_frames;
  uint64_t wall_base_ns;
  double virt_base;
  uint8_t* readback;
} H;

/* ---- aspect ratio ---- */
#define ASPECT_MIN (4.0f / 3.0f)
#define ASPECT_MAX (32.0f / 9.0f)
static const struct { float aspect; const char* name; } k_aspects[] = {
    {0.0f, "Auto"}, {4.0f / 3.0f, "4:3"}, {16.0f / 10.0f, "16:10"}, {16.0f / 9.0f, "16:9"}, {21.0f / 9.0f, "21:9"}, {32.0f / 9.0f, "32:9"},
};
enum { N_ASPECTS = sizeof k_aspects / sizeof k_aspects[0] };

static float clamp_aspect(float a) { return a < ASPECT_MIN ? ASPECT_MIN : a > ASPECT_MAX ? ASPECT_MAX : a; }

static void aspect_name(float a, char* out, size_t n) {
  for (int i = 1; i < N_ASPECTS; ++i)
    if (fabsf(a - k_aspects[i].aspect) < 0.01f) { snprintf(out, n, "%s", k_aspects[i].name); return; }
  snprintf(out, n, "%.2f:1", a);
}

/* The render aspect ratio and scale wanted for the window's current size. */
static void wanted_view(float* aspect, float* scale) {
  int ww, wh;
  SDL_GetWindowSizeInPixels(H.window, &ww, &wh);
  if (ww < 1) ww = 1;
  if (wh < 1) wh = 1;
  /* Widescreen is the game's (its patches, widescreen.c); the operator menu shows at 4:3. */
  const int in_game = !recomp_program_count || recomp_program() == &recomp_programs[0];
  const float a = in_game ? clamp_aspect(g_settings.aspect > 0 ? g_settings.aspect : (float)ww / wh) : 4.0f / 3.0f;
  *aspect = a;
  /* render at the displayed size: the window height, or less when bars go above and below */
  const float shown_h = ww < wh * a ? ww / a : (float)wh;
  *scale = g_settings.render_scale > 0 ? g_settings.render_scale : (shown_h / 384.0f < 1.0f ? 1.0f : shown_h / 384.0f);
}

static void update_title(float aspect) {
  char cur[32], title[96];
  aspect_name(aspect, cur, sizeof cur);
  if (g_settings.aspect > 0) snprintf(title, sizeof title, "NBA Showtime  [%s]  Esc: menu", cur);
  else snprintf(title, sizeof title, "NBA Showtime  [Auto: %s]  Esc: menu", cur);
  SDL_SetWindowTitle(H.window, title);
}

/* Follow the window (auto aspect, render scale) and the F2 choice; between frames only. */
static void apply_view(void) {
  float a, sc;
  wanted_view(&a, &sc);
  const int aspect_changed = a != H.aspect;
  if (renderer_gl_set_view(sc, a) || aspect_changed) {
    H.aspect = a;
    widescreen_patch(a);
    free(H.readback); /* --dump: the frame size changed */
    H.readback = NULL;
    if (aspect_changed) update_title(a);
  }
}

static void cycle_aspect(void) {
  int cur = 0;
  for (int i = 0; i < N_ASPECTS; ++i)
    if (g_settings.aspect == k_aspects[i].aspect) cur = i;
  g_settings.aspect = k_aspects[(cur + 1) % N_ASPECTS].aspect;
  settings_touched(SET_ASPECT);
  settings_save();
  float a, sc;
  wanted_view(&a, &sc);
  update_title(a); /* the view itself follows at the next frame (apply_view) */
  char name[32];
  aspect_name(a, name, sizeof name);
  fprintf(stderr, "aspect ratio: %s%s%s\n", g_settings.aspect > 0 ? "" : "Auto (", name, g_settings.aspect > 0 ? "" : ")");
}

/* ---- settings ---- */
static void apply_gain(void) {
  if (!H.audio) return;
  const int muted = g_settings.mute_unfocused && !H.focused;
  SDL_SetAudioStreamGain(H.audio, muted ? 0.0f : settings_gain(g_settings.volume));
}

static void apply_vsync(void) {
  if (g_settings.vsync && !SDL_GL_SetSwapInterval(1)) {
    fprintf(stderr, "vsync: %s\n", SDL_GetError());
    g_settings.vsync = 0;
  }
  if (!g_settings.vsync) SDL_GL_SetSwapInterval(0); /* paced by the emulated vblank instead */
}

static void apply_fullscreen(void) {
  const int fs = (SDL_GetWindowFlags(H.window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (fs != g_settings.fullscreen) SDL_SetWindowFullscreen(H.window, g_settings.fullscreen);
}

/* ---- runtime hooks ---- */
static void quiet_log(const char* fmt, ...) {
  if (!H.verbose) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void save_game_data(void) {
  nvram_save();
  if (H.disk_path[0]) ide_overlay_save(H.disk_path);
}

static void quit(int code) {
  save_game_data();
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
    switch (e.type) {
      case SDL_EVENT_QUIT: quit(0); break;
      case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
          g_settings.fullscreen = !((SDL_GetWindowFlags(H.window) & SDL_WINDOW_FULLSCREEN) != 0);
          settings_touched(SET_FULLSCREEN);
          apply_fullscreen();
          settings_save();
        }
        if (e.key.key == SDLK_F2) cycle_aspect();
        /* not a repeat: a held Esc that just closed the menu must not reopen it */
        if (e.key.key == SDLK_ESCAPE && !e.key.repeat) H.menu_request = 1;
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK || e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
          /* L3 + R3 together open the menu */
          SDL_Gamepad* g = SDL_GetGamepadFromID(e.gbutton.which);
          if (g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK) && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK))
            H.menu_request = 1;
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        H.focused = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        apply_gain();
        break;
      case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
      case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        g_settings.fullscreen = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        break;
      default: break;
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

/* Where the rendered image goes in the window: its aspect ratio kept, black bars around. */
static void frame_rect(int ww, int wh, int* dx, int* dy, int* dw, int* dh) {
  int rw, rh;
  renderer_gl_size(&rw, &rh);
  const float ra = (float)rw / rh;
  *dw = ww, *dh = wh, *dx = 0, *dy = 0;
  if (ww > wh * ra) { *dw = (int)lroundf(wh * ra); *dx = (ww - *dw) / 2; }
  else { *dh = (int)lroundf(ww / ra); *dy = (wh - *dh) / 2; }
}

/* ---- the in-game menu: runs inside the buffer swap, the game is paused meanwhile ---- */
static void menu_present(int ww, int wh, float rect[4]) {
  int dx, dy, dw, dh;
  frame_rect(ww, wh, &dx, &dy, &dw, &dh);
  renderer_gl_present(ww, wh, dx, dy, dw, dh, g_settings.sharp_scaling);
  rect[0] = (float)dx, rect[1] = (float)dy, rect[2] = (float)dw, rect[3] = (float)dh;
}

static unsigned menu_frame_texture(int* w, int* h) {
  renderer_gl_size(w, h);
  return renderer_gl_display_texture();
}

static void menu_apply(unsigned what) {
  if (what & APPLY_DISPLAY) apply_fullscreen();
  if (what & APPLY_VSYNC) apply_vsync();
  if (what & APPLY_AUDIO) apply_gain();
  if (what & APPLY_VIEW) { /* the renderer follows when the game runs again (apply_view) */
    float a, sc;
    wanted_view(&a, &sc);
    update_title(a);
  }
}

static void menu_quit(void) { quit(0); }

/* Start the program again with the same arguments (the DIP switches are read at boot). */
static char** g_argv;
static void menu_restart(void) {
  save_game_data();
  if (H.audio) SDL_DestroyAudioStream(H.audio);
  if (H.gl) SDL_GL_DestroyContext(H.gl);
  if (H.window) SDL_DestroyWindow(H.window);
  SDL_Quit();
  fflush(NULL);
#if defined(_WIN32)
  _execv(g_argv[0], (const char* const*)g_argv);
#elif defined(__linux__)
  execv("/proc/self/exe", g_argv);
#else
  execv(g_argv[0], g_argv);
#endif
  fprintf(stderr, "restart failed; start the game again to apply the new settings\n");
  exit(1);
}

static void run_menu(void) {
  H.menu_request = 0;
  if (H.audio) SDL_PauseAudioStreamDevice(H.audio);
  const MenuHost host = {H.window, menu_present, menu_frame_texture, menu_apply, menu_quit, menu_restart};
  if (menu_run(&host) != 0) fprintf(stderr, "the menu is unavailable (see above)\n");
  if (H.audio) {
    SDL_ClearAudioStream(H.audio);
    SDL_ResumeAudioStreamDevice(H.audio);
  }
  /* game time continues from here: no catching up on the time spent paused */
  H.wall_base_ns = SDL_GetTicksNS();
  H.virt_base = g_vegas.cop0.cycles / 125e6;
  g_frame_done_ns = 0;
}

static void on_frame(void* user, uint64_t frame) {
  (void)user;
  const uint64_t t_emu_end = SDL_GetTicksNS();
  if (H.perf && g_frame_done_ns) {
    const double e = (t_emu_end - g_frame_done_ns) / 1e6;
    if (e > g_emu_worst) g_emu_worst = e;
  }
  int ww, wh, dx, dy, dw, dh;
  SDL_GetWindowSizeInPixels(H.window, &ww, &wh);
  frame_rect(ww, wh, &dx, &dy, &dw, &dh);
  renderer_gl_present(ww, wh, dx, dy, dw, dh, g_settings.sharp_scaling);
  SDL_GL_SwapWindow(H.window);
  if (g_settings.vsync) glFinish(); /* wait for this vblank here: don't let the driver queue frames (latency) */
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
  if (H.menu_at && H.frame == H.menu_at) H.menu_request = 1; /* NBA_MENU_AT (tests) */
  if (H.menu_request) run_menu();
  apply_view(); /* the next frame renders at the window's current aspect ratio / size */
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
  { /* keep the CMOS (scores, audits, adjustments) on disk: at most every 5 s */
    static uint64_t last_save;
    const uint64_t now = SDL_GetTicksNS();
    if (now - last_save > 5000000000ull) { save_game_data(); last_save = now; }
  }
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
  if (!recomp_rdram) recomp_rdram = calloc(1, RECOMP_RAM_SIZE);
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

/* The BIOS keeps a RAM copy of the CMOS records (NVRAM offsets 0x800-0x7FF0, at RAM 0x2C508 +
   offset) and only writes changed bytes back to the chip. It fills that copy at power-on
   (0x80008968), which happened before the boot snapshot was taken, so do it here: from the saved
   NVRAM if there is one, else save the snapshot's copy as the starting NVRAM contents. */
#define CMOS_SHADOW 0x0002C508u
#define CMOS_FROM 0x800u
#define CMOS_TO 0x7FF0u
static void sync_cmos_shadow(int loaded) {
  uint32_t insn;
  memcpy(&insn, recomp_rdram + 0x8968, 4);
  if (insn != 0x24030800u) { fprintf(stderr, "nvram: unknown BIOS, CMOS not restored\n"); return; }
  uint8_t* nv = nvram_mem();
  if (loaded) memcpy(recomp_rdram + CMOS_SHADOW + CMOS_FROM, nv + CMOS_FROM, CMOS_TO - CMOS_FROM);
  else {
    memcpy(nv + CMOS_FROM, recomp_rdram + CMOS_SHADOW + CMOS_FROM, CMOS_TO - CMOS_FROM);
    nvram_touch();
  }
}

/* Cabinet DIP switches (free play, 2 or 4 players): the game reads them as it starts. */
static void apply_dip_switches(void) {
  g_settings_at_boot = g_settings;
  ioasic_set_input(IOASIC_IN_DIP, getenv("NBA_DIP") ? (uint16_t)strtoul(getenv("NBA_DIP"), NULL, 16) /* tests */
                                                     : settings_dip_switches(&g_settings));
}

/* ---- exec: the operator menu and back ---- */
static struct { const char *ram, *regs; } g_boot;

static void on_exec_started(RecompContext* ctx, const RecompProgram* p) {
  (void)ctx;
  fprintf(stderr, "exec: %s\n", p == &recomp_programs[0] ? "back to the game" : p->name);
  if (p == &recomp_programs[0]) { /* a fresh game image: patch it again */
    widescreen_reset();
    widescreen_patch(H.aspect);
  }
}

/* The operator menu (DIAG.EXE) leaves by exec("BOOT.COM"), the disk's boot loader, which goes on
   to load GAME.EXE through more programs that aren't recompiled. Start the game the way the boot
   snapshot does instead: a warm reboot into the RAM and CPU state at the game's entry point, with
   the BIOS's CMOS copy taken from the NVRAM (where the menu saved its adjustments). The disk
   overlay, sound board and video keep running; the game resets them as it starts. */
static void reboot_to_game(RecompContext* ctx, uint32_t entry) {
  (void)entry;
  fprintf(stderr, "exec: rebooting into the game\n");
  save_game_data();
  if (load_entry(ctx, g_boot.ram, g_boot.regs) != 0) quit(1);
  sync_cmos_shadow(1);
  apply_dip_switches(); /* a restart: DIP changes from the settings menu take effect now */
  recomp_set_program(&recomp_programs[0]);
  on_exec_started(ctx, &recomp_programs[0]);
  recomp_proc_exec(ctx, RECOMP_PROGRAM_BASE);
}

static int file_exists(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f) fclose(f);
  return f != NULL;
}

int main(int argc, char** argv) {
#ifdef _WIN32
  attach_parent_console();
#endif
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
  const char* save_dir = NULL;
  int no_save = 0;
  int win_w = 1280, win_h = 960, hidden = 0, no_sound = 0;
  settings_load();
  g_argv = argv;
  H.throttle = 1;
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
    else if (ARG("--save-dir")) save_dir = next;
    else if (ARG("--scale")) { g_settings.render_scale = (float)atof(next); settings_override(SET_SCALE); }
    else if (ARG("--window")) sscanf(next, "%dx%d", &win_w, &win_h);
    else if (ARG("--aspect")) {
      float a = 0, b = 0;
      if (!SDL_strcasecmp(next, "auto")) g_settings.aspect = 0;
      else if (sscanf(next, "%f:%f", &a, &b) == 2 && b > 0) g_settings.aspect = clamp_aspect(a / b);
      else if (atof(next) > 0) g_settings.aspect = clamp_aspect((float)atof(next));
      else { fprintf(stderr, "--aspect: expected auto, W:H or a number, got %s\n", next); return 2; }
      settings_override(SET_ASPECT);
    }
    else if (ARG("--dump")) H.dump_dir = next;
    else if (ARG("--volume")) { g_settings.volume = settings_volume_from_gain((float)atof(next) / 100.0f); settings_override(SET_VOLUME); }
    else if (ARG("--dump-every")) H.dump_every = strtoull(next, NULL, 0);
    else if (ARG("--dump-start")) H.dump_start = strtoull(next, NULL, 0);
    else if (ARG("--frames")) H.max_frames = strtoull(next, NULL, 0);
    else if (!strcmp(a, "--fullscreen")) { g_settings.fullscreen = 1; settings_override(SET_FULLSCREEN); }
    else if (!strcmp(a, "--vsync")) { g_settings.vsync = 1; settings_override(SET_VSYNC); }
    else if (!strcmp(a, "--no-sound")) no_sound = 1;
    else if (!strcmp(a, "--unthrottled")) H.throttle = 0;
    else if (!strcmp(a, "--hidden")) hidden = 1;
    else if (!strcmp(a, "--no-save")) no_save = 1;
    else if (!strcmp(a, "--verbose")) H.verbose = 1;
    else if (!strcmp(a, "--perf")) H.perf = 1;
    else { fprintf(stderr, "unknown option %s (see the comment at the top of src/host/main.c)\n", a); return 2; }
#undef ARG
  }
  if (!H.dump_every) H.dump_every = 1;
  if (getenv("NBA_MENU_AT")) {
    H.menu_at = strtoull(getenv("NBA_MENU_AT"), NULL, 0);
    SDL_unsetenv_unsafe("NBA_MENU_AT"); /* not again after a restart from the menu */
  }

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
    g_boot.ram = ram, g_boot.regs = regs;
  }
  if (load_entry(&ctx, ram, regs) != 0) return 1;
  apply_dip_switches();
  recomp_exec_started_hook = on_exec_started;
  recomp_exec_unknown_hook = reboot_to_game;
  if (no_save) sync_cmos_shadow(0); /* the NVRAM starts as the snapshot's CMOS copy */
  else {
    static char nv_path[1300];
    char dir[1200];
    if (save_dir) {
      const size_t n = strlen(save_dir);
      snprintf(dir, sizeof dir, "%s%s", save_dir, n && (save_dir[n - 1] == '/' || save_dir[n - 1] == '\\') ? "" : "/");
    } else snprintf(dir, sizeof dir, "%s", settings_dir());
    snprintf(nv_path, sizeof nv_path, "%snvram.bin", dir);
    snprintf(H.disk_path, sizeof H.disk_path, "%sdisk.bin", dir);
    sync_cmos_shadow(nvram_init(nv_path));
    const int n = ide_overlay_load(H.disk_path);
    if (n > 0 && H.verbose) fprintf(stderr, "%d saved disk sectors from %s\n", n, H.disk_path);
  }

  if (!SDL_Init(SDL_INIT_VIDEO | (no_sound ? 0 : SDL_INIT_AUDIO))) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (g_settings.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  if (hidden) flags |= SDL_WINDOW_HIDDEN;
  H.window = SDL_CreateWindow("NBA Showtime", win_w, win_h, flags);
  if (!H.window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  H.gl = SDL_GL_CreateContext(H.window);
  if (!H.gl) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
  SDL_SyncWindow(H.window); /* let fullscreen take effect before sizing the render target */
  if (nba_gl_load() != 0) return 1;
  apply_vsync();
  H.focused = 1;
  {
    float scale;
    wanted_view(&H.aspect, &scale);
    if (renderer_gl_init(scale, H.aspect, on_frame, NULL) != 0) return 1;
    widescreen_patch(H.aspect);
    update_title(H.aspect);
    int w, h;
    renderer_gl_size(&w, &h);
    char name[32];
    aspect_name(H.aspect, name, sizeof name);
    fprintf(stderr, "OpenGL %s; rendering at %dx%d (%s%s)\n", (const char*)glGetString(GL_VERSION), w, h,
            g_settings.aspect > 0 ? "" : "auto ", name);
  }
  if (!no_sound) {
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, dcs2_audio_rate()};
    H.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (H.audio) {
      apply_gain();
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
