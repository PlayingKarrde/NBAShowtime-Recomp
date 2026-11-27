// The in-game menu (Esc, or L3 + R3 on a gamepad): pauses the game over a blurred copy of the
// frame and changes the settings. Styled after the game's own HUD: bold italic type, chrome
// lettering and rims, glossy royal blue bars, a red / white / blue stripe.
//
// Works with a keyboard (arrows, Enter, Esc, Q / E for the tabs), any gamepad (D-pad or left
// stick, A / B, LB / RB, Start) and the mouse. Layout is done on a 1280x720 canvas scaled to fit
// the window. Settings apply at once and are saved when the menu closes.
#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../runtime/src/raster_sw.h"
#include "gl_funcs.h"
#include "input.h"
#include "settings.h"
#include "ui_gl.h"

enum { PAGE_GAMEPLAY, PAGE_VIDEO, PAGE_AUDIO, PAGE_CONTROLS, PAGE_COUNT };
enum { ROOT_RESUME, ROOT_GAMEPLAY, ROOT_VIDEO, ROOT_AUDIO, ROOT_CONTROLS, ROOT_QUIT, ROOT_COUNT };
#define ROOT_FIRST_PAGE ROOT_GAMEPLAY
#define ROOT_LAST_PAGE ROOT_CONTROLS
static const char* const k_root_names[ROOT_COUNT] = {"RESUME", "GAMEPLAY", "GRAPHICS", "AUDIO", "CONTROLS", "QUIT GAME"};
static const char* const k_page_names[PAGE_COUNT] = {"GAMEPLAY", "GRAPHICS", "AUDIO", "CONTROLS"};

enum { R_FREE_PLAY, R_CABINET, R_RESTART, R_DISPLAY, R_ASPECT, R_RES, R_VSYNC, R_FILTER, R_VOLUME, R_MUTE_BG, R_DEVICE, R_BIND, R_DEADZONE, R_RESET };
enum { K_CHOICE, K_TOGGLE, K_SLIDER, K_BIND, K_ACTION };
typedef struct {
  int id, arg, kind;
} Row;

enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_ACCEPT, NAV_BACK, NAV_PREV_TAB, NAV_NEXT_TAB, NAV_CLOSE };
enum { DEV_KB1, DEV_KB2, DEV_PADS, DEV_COUNT };

enum { HIT_ROOT, HIT_ROW, HIT_VALUE, HIT_TAB, HIT_DLG, HIT_HINT };
typedef struct {
  float x, y, w, h;
  int kind, index;
} Hit;

enum { MAX_ROWS = 16, MAX_HITS = 64 };

static const struct { float aspect; const char* name; } k_aspects[] = {
    {0.0f, "Auto"}, {4.0f / 3.0f, "4:3"}, {16.0f / 10.0f, "16:10"}, {16.0f / 9.0f, "16:9"}, {21.0f / 9.0f, "21:9"}, {32.0f / 9.0f, "32:9"},
};
enum { N_ASPECTS = sizeof k_aspects / sizeof k_aspects[0] };
static const float k_scales[] = {0, 1, 2, 3, 4};
enum { N_SCALES = sizeof k_scales / sizeof k_scales[0] };

static const char* const k_act_names[ACT_COUNT] = {"Joystick Up", "Joystick Down", "Joystick Left", "Joystick Right",
                                                   "A Button", "B Button", "Turbo", "Start", "Insert Coin"};

/* palette */
#define C_WHITE 0xFFFFFFFFu
#define C_TEXT 0xFFE6ECFAu
#define C_DIM 0xFF8D9BBBu
#define C_NAVY 0xFF071233u
#define C_GOLD 0xFFFFD23Cu
#define C_BLUE_HI 0xFF3F82FFu
#define C_BLUE_LO 0xFF0B2E9Eu
#define C_RED_HI 0xFFF0484Au
#define C_RED_LO 0xFF8A0E14u

static struct {
  const MenuHost* host;
  double now, opened;
  int closing, quit_dialog, dlg_sel;
  int root_sel, in_page, page, row_sel, device;
  int capturing;
  double capture_t0;
  float fade, root_y, row_y, scroll, panel_a, slide, dlg_a, focus_a;
  int used_pad;
  SDL_GamepadType pad_type;
  int held_dir;
  double held_next;
  char toast[96];
  double toast_t;
  Hit hits[MAX_HITS];
  int nhits;
  float mx, my;
  int drag_row; /* slider being dragged with the mouse, -1 = none */
  int dirty;
  float ox, oy, k; /* canvas -> window: x = ox + cx * k */
  int ww, wh;
} M;

/* canvas units -> window pixels */
#define X(v) (M.ox + (v) * M.k)
#define Y(v) (M.oy + (v) * M.k)
#define S(v) ((v) * M.k)

static float approach(float cur, float target, float rate, float dt) { return cur + (target - cur) * (1.0f - expf(-rate * dt)); }

static void toast(const char* msg) {
  snprintf(M.toast, sizeof M.toast, "%s", msg);
  M.toast_t = M.now;
}

/* ---- rows ---- */
/* The DIP switches are read when the game boots: changes wait for a restart. */
static int restart_pending(void) {
  return settings_dip_switches(&g_settings) != settings_dip_switches(&g_settings_at_boot);
}

static int build_rows(int page, Row* r) {
  int n = 0;
  switch (page) {
    case PAGE_GAMEPLAY:
      r[n++] = (Row){R_FREE_PLAY, 0, K_TOGGLE};
      r[n++] = (Row){R_CABINET, 0, K_CHOICE};
      if (restart_pending()) r[n++] = (Row){R_RESTART, 0, K_ACTION};
      break;
    case PAGE_VIDEO:
      r[n++] = (Row){R_DISPLAY, 0, K_CHOICE};
      r[n++] = (Row){R_ASPECT, 0, K_CHOICE};
      r[n++] = (Row){R_RES, 0, K_CHOICE};
      r[n++] = (Row){R_VSYNC, 0, K_TOGGLE};
      r[n++] = (Row){R_FILTER, 0, K_CHOICE};
      break;
    case PAGE_AUDIO:
      r[n++] = (Row){R_VOLUME, 0, K_SLIDER};
      r[n++] = (Row){R_MUTE_BG, 0, K_TOGGLE};
      break;
    case PAGE_CONTROLS:
      r[n++] = (Row){R_DEVICE, 0, K_CHOICE};
      for (int a = M.device == DEV_PADS ? ACT_A : 0; a < ACT_COUNT; ++a) r[n++] = (Row){R_BIND, a, K_BIND};
      if (M.device == DEV_PADS) r[n++] = (Row){R_DEADZONE, 0, K_SLIDER};
      r[n++] = (Row){R_RESET, 0, K_ACTION};
      break;
  }
  return n;
}

static const char* row_label(const Row* r) {
  switch (r->id) {
    case R_FREE_PLAY: return "Free Play";
    case R_CABINET: return "Cabinet";
    case R_RESTART: return "Restart Game to Apply";
    case R_DISPLAY: return "Display Mode";
    case R_ASPECT: return "Aspect Ratio";
    case R_RES: return "Render Resolution";
    case R_VSYNC: return "VSync";
    case R_FILTER: return "Scaling Filter";
    case R_VOLUME: return "Master Volume";
    case R_MUTE_BG: return "Mute in Background";
    case R_DEVICE: return "Configure";
    case R_BIND: return k_act_names[r->arg];
    case R_DEADZONE: return "Stick Dead Zone";
    case R_RESET: return "Reset Controls to Default";
  }
  return "";
}

static const char* row_help(const Row* r) {
  switch (r->id) {
    case R_FREE_PLAY:
      return "Start and continue games without inserting coins. When off, the coin settings from the operator "
             "menu apply. Takes effect when the game restarts.";
    case R_CABINET:
      return "A 4-player cabinet lets players 3 and 4 join in; a 2-player cabinet has two sets of controls. "
             "Takes effect when the game restarts.";
    case R_RESTART: return "Restart now to use the new cabinet settings. Any game in progress will be lost.";
    case R_DISPLAY: return "Play in a window or fill the whole screen. F11 or Alt+Enter also switch.";
    case R_ASPECT:
      return "Auto matches your screen with Hor+ widescreen, up to 32:9. The 2D HUD keeps its original "
             "proportions. F2 cycles this in game.";
    case R_RES:
      return "Lines drawn by the 3D renderer. Match Window renders at your window's size; 1x is the arcade's own "
             "384 lines. Takes effect when you resume.";
    case R_VSYNC: return "Wait for the display's refresh: no tearing, a little more latency. Best on 120 Hz+ displays.";
    case R_FILTER: return "How the image is scaled to the window: Smooth (bilinear) or Sharp (nearest neighbour, for crisp pixels at 1x).";
    case R_VOLUME: return "Overall volume of the sound board. The game's own volume (the - and = keys) is applied on top.";
    case R_MUTE_BG: return "Silence the game while its window is not focused.";
    case R_DEVICE:
      return M.device == DEV_PADS ? "Gamepads join as players 1-4 in the order they connect. They always steer with the D-pad "
                                    "or left stick. L3 + R3 opens this menu."
                                  : "Choose which player's keys to change. Players 3 and 4 play with gamepads.";
    case R_BIND:
      return M.device == DEV_PADS ? "Press A, then the button to use; it replaces the current ones. Wait a few seconds (or press Esc) to cancel."
                                  : "Press Enter, then the key to use. Esc cancels, Backspace clears.";
    case R_DEADZONE: return "How far the left stick must move before it counts. Raise it if a worn stick drifts.";
    case R_RESET: return "Restore the default keys and buttons for every player.";
  }
  return "";
}

static int aspect_index(void) {
  for (int i = 0; i < N_ASPECTS; ++i)
    if (fabsf(g_settings.aspect - k_aspects[i].aspect) < 0.01f) return i;
  return -1;
}

static int scale_index(void) {
  for (int i = 0; i < N_SCALES; ++i)
    if (fabsf(g_settings.render_scale - k_scales[i]) < 0.01f) return i;
  return -1;
}

static void ratio_name(float a, char* out, size_t n) {
  for (int i = 1; i < N_ASPECTS; ++i)
    if (fabsf(a - k_aspects[i].aspect) < 0.01f) { snprintf(out, n, "%s", k_aspects[i].name); return; }
  snprintf(out, n, "%.2f:1", a);
}

static float window_aspect(void) {
  const float a = M.wh > 0 ? (float)M.ww / M.wh : 4.0f / 3.0f;
  return a < 4.0f / 3.0f ? 4.0f / 3.0f : a > 32.0f / 9.0f ? 32.0f / 9.0f : a;
}

/* Gamepad input name for the controller type in use. */
static const char* pad_name(int bit, SDL_GamepadType type) {
  const int ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
  const int nin = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR ||
                  type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
  if (bit == PAD_LT) return ps ? "L2" : nin ? "ZL" : "LT";
  if (bit == PAD_RT) return ps ? "R2" : nin ? "ZR" : "RT";
  switch ((SDL_GamepadButton)bit) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH:
      switch (SDL_GetGamepadButtonLabelForType(type, (SDL_GamepadButton)bit)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
      }
      return bit == SDL_GAMEPAD_BUTTON_SOUTH ? "A" : bit == SDL_GAMEPAD_BUTTON_EAST ? "B" : bit == SDL_GAMEPAD_BUTTON_WEST ? "X" : "Y";
    case SDL_GAMEPAD_BUTTON_BACK: return ps ? "Share" : nin ? "-" : "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : nin ? "+" : "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nin ? "L" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nin ? "R" : "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Pad Right";
    case SDL_GAMEPAD_BUTTON_MISC1: return ps ? "Mic" : "Share";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "P1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "P3";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "P2";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "P4";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    default: return "Misc";
  }
}

static void choice_value(const Row* r, char* out, size_t n, int* index, int* count) {
  *index = 0;
  *count = 0;
  switch (r->id) {
    case R_CABINET:
      snprintf(out, n, "%s", g_settings.four_player ? "4 Players" : "2 Players");
      *index = g_settings.four_player;
      *count = 2;
      break;
    case R_DISPLAY:
      snprintf(out, n, "%s", g_settings.fullscreen ? "Fullscreen" : "Windowed");
      *index = g_settings.fullscreen;
      *count = 2;
      break;
    case R_ASPECT: {
      const int i = aspect_index();
      *count = N_ASPECTS;
      *index = i;
      char nm[32];
      if (i == 0) { ratio_name(window_aspect(), nm, sizeof nm); snprintf(out, n, "Auto (%s)", nm); }
      else if (i > 0) snprintf(out, n, "%s", k_aspects[i].name);
      else { ratio_name(g_settings.aspect, nm, sizeof nm); snprintf(out, n, "Custom (%s)", nm); }
      break;
    }
    case R_RES: {
      const int i = scale_index();
      *count = N_SCALES;
      *index = i;
      if (i == 0) {
        const float a = g_settings.aspect > 0 ? g_settings.aspect : window_aspect();
        const float shown = M.ww < M.wh * a ? M.ww / a : (float)M.wh;
        snprintf(out, n, "Match Window (%dp)", shown < 384 ? 384 : (int)lroundf(shown));
      }
      else if (i == 1) snprintf(out, n, "1x  384p  (Arcade)");
      else if (i > 1) snprintf(out, n, "%dx  %dp", (int)k_scales[i], (int)k_scales[i] * 384);
      else snprintf(out, n, "Custom (%gx)", g_settings.render_scale);
      break;
    }
    case R_FILTER:
      snprintf(out, n, "%s", g_settings.sharp_scaling ? "Sharp" : "Smooth");
      *index = g_settings.sharp_scaling;
      *count = 2;
      break;
    case R_DEVICE: {
      static const char* const names[DEV_COUNT] = {"Keyboard  -  Player 1", "Keyboard  -  Player 2", "Gamepads"};
      snprintf(out, n, "%s", names[M.device]);
      *index = M.device;
      *count = DEV_COUNT;
      break;
    }
    default: out[0] = 0;
  }
}

static int* toggle_ptr(const Row* r) {
  return r->id == R_VSYNC ? &g_settings.vsync : r->id == R_MUTE_BG ? &g_settings.mute_unfocused
         : r->id == R_FREE_PLAY ? &g_settings.free_play : NULL;
}

static int slider_value(const Row* r, int* lo, int* hi, int* step) {
  if (r->id == R_VOLUME) { *lo = 0; *hi = 100; *step = 5; return g_settings.volume; }
  *lo = 5; *hi = 60; *step = 5;
  return g_settings.deadzone;
}

static void set_slider(const Row* r, int v) {
  int lo, hi, step;
  slider_value(r, &lo, &hi, &step);
  v = v < lo ? lo : v > hi ? hi : v;
  if (r->id == R_VOLUME) {
    if (v == g_settings.volume) return;
    g_settings.volume = v;
    settings_touched(SET_VOLUME);
    M.host->apply(APPLY_AUDIO);
  } else {
    if (v == g_settings.deadzone) return;
    g_settings.deadzone = v;
  }
  M.dirty = 1;
}

/* Change a choice / toggle / slider by one step; wrap: cycle past the ends (Enter / A). */
static void change(const Row* r, int dir, int wrap) {
  if (r->kind == K_TOGGLE) {
    int* p = toggle_ptr(r);
    *p = !*p;
    if (r->id == R_VSYNC) { settings_touched(SET_VSYNC); M.host->apply(APPLY_VSYNC); }
    M.dirty = 1;
    return;
  }
  if (r->kind == K_SLIDER) {
    int lo, hi, step;
    const int v = slider_value(r, &lo, &hi, &step);
    set_slider(r, v + dir * step);
    return;
  }
  if (r->kind != K_CHOICE) return;
  char tmp[64];
  int i, n;
  choice_value(r, tmp, sizeof tmp, &i, &n);
  int j = i < 0 ? (dir > 0 ? 0 : n - 1) : i + dir;
  if (j < 0 || j >= n) {
    if (!wrap) return;
    j = (j + n) % n;
  }
  switch (r->id) {
    case R_CABINET:
      g_settings.four_player = j;
      break;
    case R_DISPLAY:
      g_settings.fullscreen = j;
      settings_touched(SET_FULLSCREEN);
      M.host->apply(APPLY_DISPLAY);
      break;
    case R_ASPECT:
      g_settings.aspect = k_aspects[j].aspect;
      settings_touched(SET_ASPECT);
      M.host->apply(APPLY_VIEW);
      break;
    case R_RES:
      g_settings.render_scale = k_scales[j];
      settings_touched(SET_SCALE);
      M.host->apply(APPLY_VIEW);
      break;
    case R_FILTER:
      g_settings.sharp_scaling = j;
      M.host->apply(APPLY_SCALING);
      break;
    case R_DEVICE:
      M.device = j;
      return; /* not a setting */
  }
  M.dirty = 1;
}

static int reserved_key(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F2: case SDL_SCANCODE_F9: case SDL_SCANCODE_F10:
    case SDL_SCANCODE_F11: case SDL_SCANCODE_MINUS: case SDL_SCANCODE_EQUALS:
      return 1;
    default: return 0;
  }
}

static void bind_key(int player, int act, SDL_Scancode sc) {
  for (int p = 0; p < KB_PLAYERS; ++p)
    for (int a = 0; a < ACT_COUNT; ++a) {
      SDL_Scancode* s = g_settings.keys[p][a];
      for (int k = 0; k < KEY_SLOTS; ++k)
        if (s[k] == sc) {
          for (int j = k; j + 1 < KEY_SLOTS; ++j) s[j] = s[j + 1];
          s[KEY_SLOTS - 1] = 0;
          --k;
        }
    }
  memset(g_settings.keys[player][act], 0, sizeof g_settings.keys[player][act]);
  g_settings.keys[player][act][0] = sc;
  M.dirty = 1;
}

static void bind_pad(int act, int bit) {
  for (int a = 0; a < ACT_COUNT; ++a) g_settings.pad[a] &= ~(1u << bit);
  g_settings.pad[act] = 1u << bit;
  M.dirty = 1;
}

static void activate(const Row* r) {
  switch (r->kind) {
    case K_CHOICE: change(r, 1, 1); break;
    case K_TOGGLE: change(r, 1, 1); break;
    case K_BIND:
      M.capturing = 1;
      M.capture_t0 = M.now;
      break;
    case K_ACTION:
      if (r->id == R_RESTART) {
        settings_save();
        M.host->restart();
      }
      if (r->id == R_RESET) {
        settings_default_controls(&g_settings);
        g_settings.deadzone = 20;
        M.dirty = 1;
        toast("Controls reset to defaults");
      }
      break;
    default: break;
  }
}

/* ---- navigation ---- */
static void quit_game(void) {
  if (M.dirty) settings_save();
  M.host->quit();
}

static void close_menu(void) {
  M.closing = 1;
  M.capturing = 0;
}

static void open_page(int page, int dir) {
  if (page != M.page) { M.row_sel = 0; M.slide = (float)dir; }
  M.page = page;
  M.root_sel = ROOT_FIRST_PAGE + page;
}

static void nav(int n) {
  if (n == NAV_NONE || M.closing) return;
  if (M.quit_dialog) {
    if (n == NAV_LEFT || n == NAV_RIGHT || n == NAV_UP || n == NAV_DOWN) M.dlg_sel ^= 1;
    else if (n == NAV_ACCEPT) { if (M.dlg_sel) quit_game(); else M.quit_dialog = 0; }
    else if (n == NAV_BACK || n == NAV_CLOSE) M.quit_dialog = 0;
    return;
  }
  if (n == NAV_CLOSE) { close_menu(); return; }
  if (!M.in_page) {
    switch (n) {
      case NAV_UP: M.root_sel = (M.root_sel + ROOT_COUNT - 1) % ROOT_COUNT; break;
      case NAV_DOWN: M.root_sel = (M.root_sel + 1) % ROOT_COUNT; break;
      case NAV_RIGHT:
      case NAV_ACCEPT:
        if (M.root_sel == ROOT_RESUME) { if (n == NAV_ACCEPT) close_menu(); }
        else if (M.root_sel == ROOT_QUIT) { if (n == NAV_ACCEPT) { M.quit_dialog = 1; M.dlg_sel = 0; } }
        else { open_page(M.root_sel - ROOT_FIRST_PAGE, 0); M.in_page = 1; }
        break;
      case NAV_BACK: close_menu(); break;
      case NAV_PREV_TAB:
      case NAV_NEXT_TAB: {
        const int cur = M.root_sel >= ROOT_FIRST_PAGE && M.root_sel <= ROOT_LAST_PAGE ? M.root_sel - ROOT_FIRST_PAGE : (n == NAV_NEXT_TAB ? -1 : 0);
        const int d = n == NAV_NEXT_TAB ? 1 : -1;
        open_page((cur + d + PAGE_COUNT) % PAGE_COUNT, d);
        M.in_page = 1;
        break;
      }
      default: break;
    }
    if (M.root_sel >= ROOT_FIRST_PAGE && M.root_sel <= ROOT_LAST_PAGE && M.page != M.root_sel - ROOT_FIRST_PAGE) {
      M.page = M.root_sel - ROOT_FIRST_PAGE;
      M.row_sel = 0;
    }
    return;
  }
  Row rows[MAX_ROWS];
  const int nr = build_rows(M.page, rows);
  if (M.row_sel >= nr) M.row_sel = nr - 1;
  const Row* r = &rows[M.row_sel];
  switch (n) {
    case NAV_UP: M.row_sel = (M.row_sel + nr - 1) % nr; break;
    case NAV_DOWN: M.row_sel = (M.row_sel + 1) % nr; break;
    case NAV_LEFT: change(r, -1, 0); break;
    case NAV_RIGHT: change(r, 1, 0); break;
    case NAV_ACCEPT: activate(r); break;
    case NAV_BACK: M.in_page = 0; break;
    case NAV_PREV_TAB: open_page((M.page + PAGE_COUNT - 1) % PAGE_COUNT, -1); break;
    case NAV_NEXT_TAB: open_page((M.page + 1) % PAGE_COUNT, 1); break;
    default: break;
  }
}

static Row current_row(void) {
  Row rows[MAX_ROWS];
  const int nr = build_rows(M.page, rows);
  return rows[M.row_sel < nr ? M.row_sel : nr - 1];
}

static void capture_key(SDL_Scancode sc) {
  const Row r = current_row();
  if (sc == SDL_SCANCODE_ESCAPE) { M.capturing = 0; return; }
  if (M.device == DEV_PADS) return; /* waiting for a gamepad button */
  if (sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE) {
    memset(g_settings.keys[M.device][r.arg], 0, sizeof g_settings.keys[M.device][r.arg]);
    M.dirty = 1;
    M.capturing = 0;
    return;
  }
  if (reserved_key(sc)) { toast("That key is reserved for a hotkey"); return; }
  bind_key(M.device, r.arg, sc);
  M.capturing = 0;
}

static void capture_pad(int bit) {
  const Row r = current_row();
  if (M.device != DEV_PADS) { /* configuring the keyboard from a gamepad: B cancels */
    if (bit == SDL_GAMEPAD_BUTTON_EAST) M.capturing = 0;
    return;
  }
  if (bit >= SDL_GAMEPAD_BUTTON_DPAD_UP && bit <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT) { toast("The D-pad always steers"); return; }
  bind_pad(r.arg, bit);
  M.capturing = 0;
}

static int key_nav(const SDL_KeyboardEvent* k) {
  switch (k->key) {
    case SDLK_UP: return NAV_UP;
    case SDLK_DOWN: return NAV_DOWN;
    case SDLK_LEFT: return NAV_LEFT;
    case SDLK_RIGHT: return NAV_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return k->repeat ? NAV_NONE : NAV_ACCEPT;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return k->repeat ? NAV_NONE : NAV_BACK;
    case SDLK_Q: case SDLK_PAGEUP: return NAV_PREV_TAB;
    case SDLK_E: case SDLK_PAGEDOWN: return NAV_NEXT_TAB;
    case SDLK_TAB: return (k->mod & SDL_KMOD_SHIFT) ? NAV_PREV_TAB : NAV_NEXT_TAB;
    default: return NAV_NONE;
  }
}

static int button_nav(int b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return NAV_ACCEPT;
    case SDL_GAMEPAD_BUTTON_EAST: return NAV_BACK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return NAV_PREV_TAB;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return NAV_NEXT_TAB;
    case SDL_GAMEPAD_BUTTON_START: return NAV_CLOSE;
    default: return NAV_NONE;
  }
}

/* D-pad / left stick on any gamepad, with auto-repeat. */
static int held_direction(void) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  int dir = NAV_NONE;
  for (int i = 0; ids && i < count && dir == NAV_NONE; ++i) {
    SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
    if (!g) continue;
    const float ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    const float ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || ay < -0.55f) dir = NAV_UP;
    else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ay > 0.55f) dir = NAV_DOWN;
    else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || ax < -0.55f) dir = NAV_LEFT;
    else if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > 0.55f) dir = NAV_RIGHT;
    if (dir != NAV_NONE) { M.used_pad = 1; M.pad_type = SDL_GetGamepadType(g); }
  }
  SDL_free(ids);
  return dir;
}

static void poll_held(void) {
  const int dir = M.capturing ? NAV_NONE : held_direction();
  if (dir != M.held_dir) {
    M.held_dir = dir;
    M.held_next = M.now + 0.38;
    nav(dir);
  } else if (dir != NAV_NONE && M.now >= M.held_next) {
    M.held_next = M.now + 0.085;
    nav(dir);
  }
}

/* ---- mouse ---- */
static void add_hit(float x, float y, float w, float h, int kind, int index) {
  if (M.nhits < MAX_HITS) M.hits[M.nhits++] = (Hit){x, y, w, h, kind, index};
}

static const Hit* hit_at(float x, float y) {
  for (int i = M.nhits - 1; i >= 0; --i) {
    const Hit* h = &M.hits[i];
    if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return h;
  }
  return NULL;
}

static void slider_from_mouse(int row, float x) {
  Row rows[MAX_ROWS];
  const int nr = build_rows(M.page, rows);
  if (row < 0 || row >= nr || rows[row].kind != K_SLIDER) return;
  for (int i = 0; i < M.nhits; ++i)
    if (M.hits[i].kind == HIT_VALUE && M.hits[i].index == row) {
      int lo, hi, step;
      slider_value(&rows[row], &lo, &hi, &step);
      const float t = (x - M.hits[i].x) / (M.hits[i].w > 1 ? M.hits[i].w : 1);
      const int v = lo + (int)lroundf((t < 0 ? 0 : t > 1 ? 1 : t) * (hi - lo) / step) * step;
      set_slider(&rows[row], v);
    }
}

static void mouse_move(float x, float y) {
  M.mx = x;
  M.my = y;
  if (M.drag_row >= 0) { slider_from_mouse(M.drag_row, x); return; }
  if (M.capturing) return;
  const Hit* h = hit_at(x, y);
  if (!h) return;
  if (M.quit_dialog) { if (h->kind == HIT_DLG) M.dlg_sel = h->index; return; }
  if (h->kind == HIT_ROOT && M.root_sel != h->index) {
    M.in_page = 0;
    M.root_sel = h->index;
    if (M.root_sel >= ROOT_FIRST_PAGE && M.root_sel <= ROOT_LAST_PAGE && M.page != M.root_sel - ROOT_FIRST_PAGE) {
      M.page = M.root_sel - ROOT_FIRST_PAGE;
      M.row_sel = 0;
    }
  } else if ((h->kind == HIT_ROW || h->kind == HIT_VALUE) && M.root_sel != ROOT_RESUME && M.root_sel != ROOT_QUIT) {
    M.in_page = 1;
    M.row_sel = h->index;
  }
}

static void mouse_click(float x, float y) {
  if (M.capturing) { M.capturing = 0; return; }
  const Hit* h = hit_at(x, y);
  if (!h) return;
  if (M.quit_dialog) {
    if (h->kind == HIT_DLG) { M.dlg_sel = h->index; nav(NAV_ACCEPT); }
    return;
  }
  switch (h->kind) {
    case HIT_ROOT:
      M.in_page = 0;
      M.root_sel = h->index;
      nav(NAV_ACCEPT);
      break;
    case HIT_TAB:
      open_page(h->index, h->index > M.page ? 1 : -1);
      M.in_page = 1;
      break;
    case HIT_ROW:
    case HIT_VALUE: {
      Row rows[MAX_ROWS];
      const int nr = build_rows(M.page, rows);
      if (h->index >= nr) break;
      M.in_page = 1;
      M.row_sel = h->index;
      const Row* r = &rows[h->index];
      if (r->kind == K_SLIDER && h->kind == HIT_VALUE) { M.drag_row = h->index; slider_from_mouse(h->index, x); }
      else if (r->kind == K_CHOICE && h->kind == HIT_VALUE) change(r, x < h->x + h->w * 0.5f ? -1 : 1, 1);
      else if (r->kind != K_SLIDER && r->kind != K_CHOICE) activate(r);
      break;
    }
    case HIT_HINT: nav(h->index); break;
  }
}

/* ---- drawing helpers ---- */
static UiText text_style(int font, float px, uint32_t color) {
  UiText t;
  memset(&t, 0, sizeof t);
  t.font = font;
  t.px = S(px);
  t.top = t.bottom = color;
  return t;
}

static float draw_text(int font, float px, uint32_t color, float x, float y, int align, const char* s) {
  UiText t = text_style(font, px, color);
  return ui_text(&t, X(x), Y(y), align, s) / M.k;
}

/* A label in the game's style: bold italic, dark outline. */
static float draw_label(float px, uint32_t color, float alpha, float x, float y, int align, const char* s) {
  UiText t = text_style(UI_FONT_DISPLAY, px, ui_alpha(color, alpha));
  t.outline = S(2.0f);
  t.outline_col = ui_alpha(0xD0030820u, alpha);
  return ui_text(&t, X(x), Y(y), align, s) / M.k;
}

static void glossy_bar(float x, float y, float w, float h, uint32_t hi, uint32_t lo, float alpha, float skew) {
  UiBox b = {ui_alpha(hi, alpha), ui_alpha(lo, alpha), ui_alpha(0xFFFFFFFFu, alpha), S(9), S(2.0f), skew, 0.85f, 1};
  ui_box(X(x), Y(y), S(w), S(h), &b);
}

static void keycap(float x, float cy, float h, const char* label, float alpha, int right_align, float* out_w) {
  const float px = h * 0.62f;
  const float tw = ui_text_width(UI_FONT_BODY, S(px), label) / M.k;
  const float w = tw + h * 0.7f < h ? h : tw + h * 0.7f;
  if (right_align) x -= w;
  UiBox b = {ui_alpha(0xFFF4F6FBu, alpha), ui_alpha(0xFFB9C2D6u, alpha), ui_alpha(0xFF4A5470u, alpha), S(h * 0.22f), S(1.5f), 0, 0, 0};
  ui_box(X(x), Y(cy - h * 0.5f), S(w), S(h), &b);
  draw_text(UI_FONT_BODY, px, ui_alpha(0xFF141A2Au, alpha), x + w * 0.5f, cy, UI_ALIGN_CENTER, label);
  if (out_w) *out_w = w;
}

/* A gamepad button glyph centred at (cx, cy); returns its width. */
static float pad_glyph(int bit, float cx, float cy, float h, float alpha) {
  const SDL_GamepadType type = M.pad_type;
  if (bit == SDL_GAMEPAD_BUTTON_SOUTH || bit == SDL_GAMEPAD_BUTTON_EAST || bit == SDL_GAMEPAD_BUTTON_WEST ||
      bit == SDL_GAMEPAD_BUTTON_NORTH) {
    const SDL_GamepadButtonLabel lab = SDL_GetGamepadButtonLabelForType(type, (SDL_GamepadButton)bit);
    uint32_t col = 0xFFC8D0E0u;
    const char* letter = NULL;
    switch (lab) {
      case SDL_GAMEPAD_BUTTON_LABEL_A: col = 0xFF62C447u; letter = "A"; break;
      case SDL_GAMEPAD_BUTTON_LABEL_B: col = 0xFFE5483Fu; letter = "B"; break;
      case SDL_GAMEPAD_BUTTON_LABEL_X: col = 0xFF3D8CF0u; letter = "X"; break;
      case SDL_GAMEPAD_BUTTON_LABEL_Y: col = 0xFFF2C230u; letter = "Y"; break;
      case SDL_GAMEPAD_BUTTON_LABEL_CROSS: col = 0xFF86ACEEu; break;
      case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: col = 0xFFF07880u; break;
      case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: col = 0xFFDB90D6u; break;
      case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: col = 0xFF52CDAEu; break;
      default: letter = pad_name(bit, type); break;
    }
    if (type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) col = 0xFFD8DDE8u;
    UiBox b = {ui_alpha(0xFF262C3Cu, alpha), ui_alpha(0xFF10141Eu, alpha), ui_alpha(col, alpha), S(h), S(1.6f), 0, 0.5f, 0};
    ui_box(X(cx - h * 0.5f), Y(cy - h * 0.5f), S(h), S(h), &b);
    const uint32_t c = ui_alpha(col, alpha);
    const float s = h * 0.24f;
    if (letter) draw_text(UI_FONT_BODY, h * 0.62f, c, cx, cy, UI_ALIGN_CENTER, letter);
    else if (lab == SDL_GAMEPAD_BUTTON_LABEL_CROSS) {
      UiBox l = {c, c, 0, 0, 0, 1.0f, 0, 0};
      ui_box(X(cx - s * 0.18f), Y(cy - s), S(s * 0.36f), S(2 * s), &l);
      l.skew = -1.0f;
      ui_box(X(cx - s * 0.18f), Y(cy - s), S(s * 0.36f), S(2 * s), &l);
    } else if (lab == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE) {
      UiBox o = {0, 0, c, S(s), S(h * 0.08f), 0, 0, 0};
      ui_box(X(cx - s), Y(cy - s), S(2 * s), S(2 * s), &o);
    } else if (lab == SDL_GAMEPAD_BUTTON_LABEL_SQUARE) {
      UiBox o = {0, 0, c, S(1.5f), S(h * 0.08f), 0, 0, 0};
      ui_box(X(cx - s * 0.9f), Y(cy - s * 0.9f), S(1.8f * s), S(1.8f * s), &o);
    } else {
      ui_triangle(X(cx), Y(cy + s * 0.08f), S(s * 2.0f), S(s * 1.8f), UI_DIR_UP, c);
    }
    return h;
  }
  /* shoulders, triggers and the rest: a rounded tag */
  const char* label = pad_name(bit, type);
  const float tw = ui_text_width(UI_FONT_BODY, S(h * 0.55f), label) / M.k;
  const float w = tw + h * 0.6f < h * 1.3f ? h * 1.3f : tw + h * 0.6f;
  UiBox b = {ui_alpha(0xFF2A3042u, alpha), ui_alpha(0xFF12161Fu, alpha), ui_alpha(0xFFAEB8CCu, alpha), S(h * 0.3f), S(1.5f), 0, 0.5f, 0};
  ui_box(X(cx - w * 0.5f), Y(cy - h * 0.5f), S(w), S(h), &b);
  draw_text(UI_FONT_BODY, h * 0.55f, ui_alpha(0xFFE8ECF4u, alpha), cx, cy, UI_ALIGN_CENTER, label);
  return w;
}

/* D-pad glyph */
static float dpad_glyph(float cx, float cy, float h, float alpha) {
  const uint32_t c = ui_alpha(0xFFDDE3EEu, alpha);
  const float t = h * 0.34f;
  UiBox b = {c, c, 0, S(2), 0, 0, 0, 0};
  ui_box(X(cx - t * 0.5f), Y(cy - h * 0.5f), S(t), S(h), &b);
  ui_box(X(cx - h * 0.5f), Y(cy - t * 0.5f), S(h), S(t), &b);
  return h;
}

static void wrap_text(int font, float px, uint32_t color, float x, float y, float width, float line_h, const char* s) {
  char line[256];
  const char* p = s;
  while (*p) {
    int best = 0, n = 0;
    while (p[n] && n < (int)sizeof line - 1) {
      while (p[n] && p[n] != ' ') ++n;
      memcpy(line, p, (size_t)n);
      line[n] = 0;
      if (ui_text_width(font, S(px), line) / M.k > width && best > 0) break;
      best = n;
      if (!p[n]) break;
      ++n;
    }
    if (best == 0) best = n;
    memcpy(line, p, (size_t)best);
    line[best] = 0;
    draw_text(font, px, color, x, y, UI_ALIGN_LEFT, line);
    p += best;
    while (*p == ' ') ++p;
    y += line_h;
  }
}

/* ---- drawing ---- */
static void draw_background(const float rect[4]) {
  const float a = M.fade;
  ui_snapshot_draw(rect[0], rect[1], rect[2], rect[3], ui_alpha(0xFF56627Eu, a));
  /* legibility: navy wash from the left, vignette at the bottom */
  ui_fill_h(0, 0, (float)M.ww * 0.62f, (float)M.wh, ui_alpha(0xD8040A1Cu, a), 0);
  ui_fill_v(0, (float)M.wh * 0.72f, (float)M.ww, (float)M.wh * 0.28f, 0, ui_alpha(0xB0020610u, a));
}

static void draw_title(float slide) {
  const float a = M.fade;
  const float x = 64 + slide;
  UiText sub = text_style(UI_FONT_DISPLAY, 24, ui_alpha(C_WHITE, a));
  sub.style = UI_TEXT_GOLD;
  sub.outline = S(1.5f);
  sub.outline_col = ui_alpha(0xFF3A1A00u, a);
  ui_text(&sub, X(x + 4), Y(62), UI_ALIGN_LEFT, "NBA SHOWTIME");
  UiText t = text_style(UI_FONT_DISPLAY, 84, ui_alpha(C_WHITE, a));
  t.style = UI_TEXT_CHROME;
  t.outline = S(3.0f);
  t.outline_col = ui_alpha(0xFF0A1C55u, a);
  t.shadow = S(5);
  t.shadow_col = ui_alpha(0x90000000u, a);
  ui_text(&t, X(x), Y(116), UI_ALIGN_LEFT, M.quit_dialog ? "PAUSED" : M.in_page ? "SETTINGS" : "PAUSED");
  /* red / white / blue stripe */
  const float sy = 162, sh = 6, sk = 1.0f;
  UiBox blue = {ui_alpha(0xFF2F62F0u, a), ui_alpha(0xFF1A3CB0u, a), 0, 0, 0, sk, 0, 0};
  UiBox white = {ui_alpha(0xFFFFFFFFu, a), ui_alpha(0xFFD0D6E4u, a), 0, 0, 0, sk, 0, 0};
  UiBox red = {ui_alpha(0xFFE8343Au, a), ui_alpha(0xFFA81820u, a), 0, 0, 0, sk, 0, 0};
  ui_box(X(x + 4), Y(sy), S(150), S(sh), &blue);
  ui_box(X(x + 162), Y(sy), S(26), S(sh), &white);
  ui_box(X(x + 196), Y(sy), S(150), S(sh), &red);
}

static void draw_root(float slide) {
  const float a = M.fade;
  const float x = 60 + slide, w = 340, h = 50, y0 = 222, step = 60;
  /* selection bar */
  const int danger = M.root_sel == ROOT_QUIT;
  const float focus = 1.0f - M.focus_a * 0.55f;
  glossy_bar(x, M.root_y - h * 0.5f, w, h, danger ? C_RED_HI : C_BLUE_HI, danger ? C_RED_LO : C_BLUE_LO, a * focus, 0.22f);
  for (int i = 0; i < ROOT_COUNT; ++i) {
    const float cy = y0 + i * step;
    const int sel = i == M.root_sel;
    UiText t = text_style(UI_FONT_DISPLAY, 34, ui_alpha(sel ? C_WHITE : 0xFFB6C2DCu, a));
    t.outline = S(2.0f);
    t.outline_col = ui_alpha(sel ? 0xFF061340u : 0xC0040A1Cu, a);
    if (sel && !M.in_page) { t.style = UI_TEXT_CHROME; t.shadow = S(2.5f); t.shadow_col = ui_alpha(0x80000000u, a); }
    ui_text(&t, X(x + 26), Y(cy), UI_ALIGN_LEFT, k_root_names[i]);
    if (i >= ROOT_FIRST_PAGE && i <= ROOT_LAST_PAGE)
      ui_triangle(X(x + w - 28), Y(cy), S(9), S(14), UI_DIR_RIGHT, ui_alpha(sel ? C_WHITE : 0x60FFFFFFu, a));
    add_hit(X(x), Y(cy - step * 0.5f), S(w), S(step), HIT_ROOT, i);
  }
}

static void draw_choice(const Row* r, int sel, float xl, float xr, float cy, float alpha, int row_index) {
  char v[64];
  int i, n;
  choice_value(r, v, sizeof v, &i, &n);
  const float mid = (xl + xr) * 0.5f;
  draw_label(25, C_WHITE, alpha, mid, cy - (n <= 6 ? 4 : 0), UI_ALIGN_CENTER, v);
  const uint32_t on = ui_alpha(C_WHITE, alpha), off = ui_alpha(0x40FFFFFFu, alpha);
  ui_triangle(X(xl + 8), Y(cy), S(10), S(16), UI_DIR_LEFT, sel && i != 0 ? on : off);
  ui_triangle(X(xr - 8), Y(cy), S(10), S(16), UI_DIR_RIGHT, sel && i != n - 1 ? on : off);
  if (n > 1 && n <= 6) { /* position pips */
    const float pw = 14, gap = 6, total = n * pw + (n - 1) * gap;
    for (int k = 0; k < n; ++k) {
      const uint32_t c = k == i ? ui_alpha(C_GOLD, alpha) : ui_alpha(0x50FFFFFFu, alpha);
      UiBox b = {c, c, 0, S(2), 0, 0.3f, 0, 0};
      ui_box(X(mid - total * 0.5f + k * (pw + gap)), Y(cy + 15), S(pw), S(3.5f), &b);
    }
  }
  add_hit(X(xl - 10), Y(cy - 24), S(xr - xl + 20), S(48), HIT_VALUE, row_index);
}

static void draw_toggle(const Row* r, float xr, float cy, float alpha, int row_index) {
  const int on = *toggle_ptr(r);
  const float w = 66, h = 32;
  UiBox track = {ui_alpha(on ? 0xFF3F82FFu : 0xFF2A3350u, alpha), ui_alpha(on ? 0xFF1440B8u : 0xFF161C30u, alpha),
                 ui_alpha(on ? 0xFFFFFFFFu : 0xFF5A6684u, alpha), S(h), S(1.5f), 0, on ? 0.7f : 0.2f, on};
  ui_box(X(xr - w), Y(cy - h * 0.5f), S(w), S(h), &track);
  const float kx = on ? xr - h * 0.5f : xr - w + h * 0.5f, kr = 12;
  UiBox knob = {ui_alpha(0xFFFFFFFFu, alpha), ui_alpha(0xFFC8D0E0u, alpha), ui_alpha(0x60000000u, alpha), S(kr), S(1), 0, 0, 0};
  ui_box(X(kx - kr), Y(cy - kr), S(2 * kr), S(2 * kr), &knob);
  draw_label(23, on ? C_WHITE : C_DIM, alpha, xr - w - 16, cy, UI_ALIGN_RIGHT, on ? "ON" : "OFF");
  add_hit(X(xr - w - 70), Y(cy - 24), S(w + 70), S(48), HIT_VALUE, row_index);
}

static void draw_slider(const Row* r, float xl, float xr, float cy, float alpha, int row_index) {
  int lo, hi, step;
  const int v = slider_value(r, &lo, &hi, &step);
  const float tx0 = xl + 10, tx1 = xr - 70, th = 10;
  const float t = (float)(v - lo) / (float)(hi - lo);
  UiBox track = {ui_alpha(0xFF0E1428u, alpha), ui_alpha(0xFF1C2440u, alpha), ui_alpha(0xFF4A5674u, alpha), S(th), S(1.2f), 0, 0, 0};
  ui_box(X(tx0), Y(cy - th * 0.5f), S(tx1 - tx0), S(th), &track);
  if (t > 0) {
    UiBox fill = {ui_alpha(0xFFC4F2FFu, alpha), ui_alpha(0xFF4AB8FFu, alpha), 0, S(th), 0, 0, 0.6f, 0};
    ui_box(X(tx0), Y(cy - th * 0.5f), S((tx1 - tx0) * t), S(th), &fill);
  }
  const float kx = tx0 + (tx1 - tx0) * t, kr = 12;
  UiBox knob = {ui_alpha(0xFFFFFFFFu, alpha), ui_alpha(0xFFC0C8DAu, alpha), ui_alpha(0xFFFFFFFFu, alpha), S(kr), S(2), 0, 0, 1};
  ui_box(X(kx - kr), Y(cy - kr), S(2 * kr), S(2 * kr), &knob);
  char num[16];
  snprintf(num, sizeof num, "%d%%", v);
  draw_label(25, C_WHITE, alpha, xr, cy, UI_ALIGN_RIGHT, num);
  add_hit(X(tx0), Y(cy - 24), S(tx1 - tx0), S(48), HIT_VALUE, row_index);
}

static void draw_binding(const Row* r, int sel, float xr, float cy, float alpha) {
  if (sel && M.capturing) {
    const float pulse = 0.55f + 0.45f * sinf((float)(M.now - M.capture_t0) * 7.0f);
    UiText t = text_style(UI_FONT_DISPLAY, 24, ui_alpha(C_WHITE, alpha * pulse));
    t.style = UI_TEXT_GOLD;
    t.outline = S(1.5f);
    t.outline_col = ui_alpha(0xFF2A1400u, alpha * pulse);
    ui_text(&t, X(xr), Y(cy - 3), UI_ALIGN_RIGHT, M.device == DEV_PADS ? "PRESS A BUTTON..." : "PRESS A KEY...");
    const float left = 1.0f - (float)(M.now - M.capture_t0) / 8.0f;
    ui_fill(X(xr - 200 * left), Y(cy + 15), S(200 * left), S(2.5f), ui_alpha(C_GOLD, alpha * 0.8f));
    return;
  }
  float x = xr;
  int any = 0;
  if (M.device == DEV_PADS) {
    const uint32_t m = g_settings.pad[r->arg];
    for (int b = 31; b >= 0; --b)
      if ((m >> b) & 1) {
        const char* nm = pad_name(b, M.pad_type);
        const float w = ui_text_width(UI_FONT_BODY, S(36 * 0.55f), nm) / M.k;
        const int face = b <= SDL_GAMEPAD_BUTTON_NORTH;
        const float gw = face ? 34 : (w + 34 * 0.6f < 34 * 1.3f ? 34 * 1.3f : w + 34 * 0.6f);
        pad_glyph(b, x - gw * 0.5f, cy, 34, alpha);
        x -= gw + 8;
        any = 1;
      }
  } else {
    for (int k = KEY_SLOTS - 1; k >= 0; --k) {
      const SDL_Scancode sc = g_settings.keys[M.device][r->arg][k];
      if (!sc) continue;
      float w;
      keycap(x, cy, 34, SDL_GetScancodeName(sc), alpha, 1, &w);
      x -= w + 8;
      any = 1;
    }
  }
  if (!any) draw_label(23, C_DIM, alpha, xr, cy, UI_ALIGN_RIGHT, "Unbound");
}

static void draw_panel(float slide) {
  const float a = M.fade * M.panel_a;
  if (a <= 0.002f) return;
  const float px = 450 + slide + (1.0f - M.panel_a) * 30, py = 150, pw = 770, ph = 470;
  /* panel body */
  UiBox body = {ui_alpha(0xEC13204Au, a), ui_alpha(0xF0060B1Fu, a), ui_alpha(0xFFFFFFFFu, a), S(16), S(2.0f), 0, 0, 1};
  ui_box(X(px), Y(py), S(pw), S(ph), &body);
  /* tabs */
  const float tab_y = py + 36;
  float tx = px + 74;
  for (int i = 0; i < PAGE_COUNT; ++i) {
    const int cur = i == M.page;
    UiText t = text_style(UI_FONT_DISPLAY, 27, ui_alpha(cur ? C_WHITE : 0xFF8090B4u, a));
    if (cur) { t.style = UI_TEXT_GOLD; t.outline = S(1.5f); t.outline_col = ui_alpha(0xFF2A1400u, a); }
    const float w = ui_text(&t, X(tx), Y(tab_y), UI_ALIGN_LEFT, k_page_names[i]) / M.k;
    if (cur) {
      UiBox ul = {ui_alpha(0xFFFFE070u, a), ui_alpha(0xFFE0A010u, a), 0, S(2), 0, 1.0f, 0, 0};
      ui_box(X(tx), Y(tab_y + 19), S(w), S(4), &ul);
    }
    add_hit(X(tx - 10), Y(py + 8), S(w + 20), S(56), HIT_TAB, i);
    tx += w + 40;
  }
  if (M.in_page) {
    if (M.used_pad) {
      pad_glyph(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, px + 40, tab_y, 28, a);
      pad_glyph(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, px + pw - 40, tab_y, 28, a);
    } else {
      keycap(px + 28, tab_y, 28, "Q", a, 0, NULL);
      keycap(px + pw - 28, tab_y, 28, "E", a, 1, NULL);
    }
  }
  ui_fill_h(X(px + 20), Y(py + 66), S(pw * 0.5f - 20), S(1.5f), ui_alpha(0x00FFFFFFu, a), ui_alpha(0x50FFFFFFu, a));
  ui_fill_h(X(px + pw * 0.5f), Y(py + 66), S(pw * 0.5f - 20), S(1.5f), ui_alpha(0x50FFFFFFu, a), ui_alpha(0x00FFFFFFu, a));

  /* rows */
  Row rows[MAX_ROWS];
  const int nr = build_rows(M.page, rows);
  if (M.row_sel >= nr) M.row_sel = nr - 1;
  const float list_y = py + 80, list_h = ph - 80 - 96, row_h = 50;
  const float content = nr * row_h;
  float want = M.row_y - list_h * 0.5f;
  if (want > content - list_h) want = content - list_h;
  if (want < 0) want = 0;
  M.scroll = want;
  const float ca = a * (1.0f - fabsf(M.slide));
  const float cx_off = M.slide * 40;
  ui_clip(X(px + 4), Y(list_y), S(pw - 8), S(list_h));
  if (M.in_page) {
    UiBox hb = {ui_alpha(C_BLUE_HI, ca), ui_alpha(C_BLUE_LO, ca), ui_alpha(0xFFFFFFFFu, ca), S(8), S(1.6f), 0.18f, 0.75f, 1};
    ui_box(X(px + 20 + cx_off), Y(list_y + M.row_y - M.scroll - row_h * 0.5f + 3), S(pw - 40), S(row_h - 6), &hb);
  }
  const float xl = px + pw - 40 - 320, xr = px + pw - 40;
  for (int i = 0; i < nr; ++i) {
    const float cy = list_y + row_h * (i + 0.5f) - M.scroll;
    if (cy < list_y - row_h || cy > list_y + list_h + row_h) continue;
    const int sel = M.in_page && i == M.row_sel;
    const Row* r = &rows[i];
    const float ra = ca * (M.in_page ? 1.0f : 0.75f);
    draw_label(27, sel ? C_WHITE : 0xFFD4DCF0u, ra, px + 44 + cx_off, cy, UI_ALIGN_LEFT, row_label(r));
    switch (r->kind) {
      case K_CHOICE: draw_choice(r, sel, xl + cx_off, xr + cx_off, cy, ra, i); break;
      case K_TOGGLE: draw_toggle(r, xr + cx_off, cy, ra, i); break;
      case K_SLIDER: draw_slider(r, xl + cx_off, xr + cx_off, cy, ra, i); break;
      case K_BIND: draw_binding(r, sel, xr + cx_off, cy, ra); break;
      case K_ACTION:
        if (sel) draw_label(22, C_GOLD, ra, xr + cx_off, cy, UI_ALIGN_RIGHT, r->id == R_RESTART ? "PRESS TO RESTART" : "PRESS TO RESET");
        else if (r->id == R_RESTART) draw_label(22, C_GOLD, ra, xr + cx_off, cy, UI_ALIGN_RIGHT, "RESTART REQUIRED");
        break;
    }
    if (cy > list_y && cy < list_y + list_h) add_hit(X(px + 20), Y(cy - row_h * 0.5f), S(pw - 40), S(row_h), HIT_ROW, i);
  }
  ui_clip(0, 0, 0, 0);
  /* scroll hints */
  if (M.scroll > 1) ui_triangle(X(px + pw * 0.5f), Y(list_y + 2), S(16), S(9), UI_DIR_UP, ui_alpha(0xA0FFFFFFu, ca));
  if (M.scroll < content - list_h - 1)
    ui_triangle(X(px + pw * 0.5f), Y(list_y + list_h - 2), S(16), S(9), UI_DIR_DOWN, ui_alpha(0xA0FFFFFFu, ca));

  /* help */
  const float hy = py + ph - 92;
  ui_fill_h(X(px + 20), Y(hy), S(pw - 40), S(1.5f), ui_alpha(0x40FFFFFFu, a), ui_alpha(0x00FFFFFFu, a));
  const Row* cur = &rows[M.row_sel];
  UiBox info = {ui_alpha(0xFF3F82FFu, a), ui_alpha(0xFF1440B8u, a), 0, S(3), 0, 0.3f, 0, 0};
  ui_box(X(px + 28), Y(hy + 20), S(5), S(56), &info);
  wrap_text(UI_FONT_BODY, 21, ui_alpha(0xFFC4D0E8u, a * (M.in_page ? 1.0f : 0.7f)), px + 46, hy + 32, pw - 90, 26, row_help(cur));
}

typedef struct {
  int nav;
  const char* label;
} Hint;

static void draw_hints(void) {
  Hint h[5];
  int n = 0;
  if (M.capturing) {
    if (M.device == DEV_PADS) h[n++] = (Hint){NAV_BACK, "Cancel (Esc)"};
    else { h[n++] = (Hint){NAV_BACK, "Cancel"}; }
  } else if (M.quit_dialog) {
    h[n++] = (Hint){NAV_ACCEPT, "Confirm"};
    h[n++] = (Hint){NAV_BACK, "Cancel"};
  } else if (!M.in_page) {
    h[n++] = (Hint){NAV_UP, "Navigate"};
    h[n++] = (Hint){NAV_ACCEPT, "Select"};
    h[n++] = (Hint){NAV_BACK, "Resume"};
  } else {
    const Row r = current_row();
    h[n++] = (Hint){NAV_UP, "Navigate"};
    if (r.kind == K_CHOICE || r.kind == K_SLIDER || r.kind == K_TOGGLE) h[n++] = (Hint){NAV_LEFT, "Change"};
    if (r.kind == K_BIND) h[n++] = (Hint){NAV_ACCEPT, "Rebind"};
    else if (r.kind == K_ACTION) h[n++] = (Hint){NAV_ACCEPT, "Select"};
    h[n++] = (Hint){NAV_BACK, "Back"};
  }
  const float a = M.fade, cy = 672, gh = 30;
  float x = 1216;
  for (int i = n - 1; i >= 0; --i) {
    const float tw = draw_text(UI_FONT_BODY, 21, ui_alpha(0xFFE0E6F2u, a), x, cy, UI_ALIGN_RIGHT, h[i].label);
    x -= tw + 10;
    float gw = gh;
    if (M.capturing && M.device == DEV_PADS) gw = 0;
    else if (M.used_pad) {
      switch (h[i].nav) {
        case NAV_UP: dpad_glyph(x - gh * 0.5f, cy, gh * 0.9f, a); break;
        case NAV_LEFT: dpad_glyph(x - gh * 0.5f, cy, gh * 0.9f, a); break;
        case NAV_ACCEPT: pad_glyph(SDL_GAMEPAD_BUTTON_SOUTH, x - gh * 0.5f, cy, gh, a); break;
        case NAV_BACK: pad_glyph(SDL_GAMEPAD_BUTTON_EAST, x - gh * 0.5f, cy, gh, a); break;
      }
    } else {
      const char* key = h[i].nav == NAV_ACCEPT ? "Enter" : h[i].nav == NAV_BACK ? "Esc" : NULL;
      if (key) keycap(x, cy, gh, key, a, 1, &gw);
      else { /* arrow keys */
        const float s = gh * 0.8f;
        UiBox b = {ui_alpha(0xFFF4F6FBu, a), ui_alpha(0xFFB9C2D6u, a), ui_alpha(0xFF4A5470u, a), S(5), S(1.5f), 0, 0, 0};
        const int vertical = h[i].nav == NAV_UP;
        for (int k = 0; k < 2; ++k) {
          const float bx = x - (2 - k) * (s + 3);
          ui_box(X(bx), Y(cy - s * 0.5f), S(s), S(s), &b);
          ui_triangle(X(bx + s * 0.5f), Y(cy), S(s * 0.4f), S(s * 0.4f),
                      vertical ? (k ? UI_DIR_DOWN : UI_DIR_UP) : (k ? UI_DIR_RIGHT : UI_DIR_LEFT), ui_alpha(0xFF141A2Au, a));
        }
        gw = 2 * (s + 3) - 3;
      }
    }
    add_hit(X(x - gw), Y(cy - 20), S(gw + 10 + 200), S(40), HIT_HINT, h[i].nav == NAV_UP || h[i].nav == NAV_LEFT ? NAV_NONE : h[i].nav);
    x -= gw + 30;
  }
  draw_text(UI_FONT_BODY, 18, ui_alpha(0x90FFFFFFu, a), 64, cy, UI_ALIGN_LEFT, "Settings are saved automatically");
}

static void draw_dialog(void) {
  const float a = M.dlg_a * M.fade;
  if (a <= 0.002f) return;
  ui_fill(0, 0, (float)M.ww, (float)M.wh, ui_alpha(0xA0000008u, a));
  const float w = 580, h = 260, x = 640 - w * 0.5f, y = 360 - h * 0.5f + (1.0f - M.dlg_a) * 20;
  UiBox body = {ui_alpha(0xF41A2A5Cu, a), ui_alpha(0xF8070C22u, a), ui_alpha(0xFFFFFFFFu, a), S(18), S(2.5f), 0, 0, 1};
  ui_box(X(x), Y(y), S(w), S(h), &body);
  UiText t = text_style(UI_FONT_DISPLAY, 50, ui_alpha(C_WHITE, a));
  t.style = UI_TEXT_CHROME;
  t.outline = S(2.5f);
  t.outline_col = ui_alpha(0xFF0A1C55u, a);
  t.shadow = S(3);
  t.shadow_col = ui_alpha(0x90000000u, a);
  ui_text(&t, X(640), Y(y + 62), UI_ALIGN_CENTER, "QUIT GAME?");
  draw_text(UI_FONT_BODY, 23, ui_alpha(0xFFC8D4EAu, a), 640, y + 116, UI_ALIGN_CENTER, "Any game in progress will be lost.");
  static const char* const labels[2] = {"CANCEL", "QUIT"};
  for (int i = 0; i < 2; ++i) {
    const float bw = 210, bh = 54, bx = 640 + (i ? 14 : -14 - bw), by = y + h - 86;
    const int sel = i == M.dlg_sel;
    if (sel) glossy_bar(bx, by, bw, bh, i ? C_RED_HI : C_BLUE_HI, i ? C_RED_LO : C_BLUE_LO, a, 0.2f);
    else {
      UiBox b = {ui_alpha(0x40FFFFFFu, a), ui_alpha(0x20FFFFFFu, a), ui_alpha(0x60FFFFFFu, a), S(9), S(1.5f), 0.2f, 0, 0};
      ui_box(X(bx), Y(by), S(bw), S(bh), &b);
    }
    draw_label(28, sel ? C_WHITE : 0xFFB0BCD4u, a, bx + bw * 0.5f, by + bh * 0.5f, UI_ALIGN_CENTER, labels[i]);
    add_hit(X(bx), Y(by), S(bw), S(bh), HIT_DLG, i);
  }
}

static void draw_toast(void) {
  const double age = M.now - M.toast_t;
  if (!M.toast[0] || age > 2.2) return;
  const float a = M.fade * (float)(age < 0.15 ? age / 0.15 : age > 1.8 ? (2.2 - age) / 0.4 : 1.0);
  const float tw = ui_text_width(UI_FONT_BODY, S(22), M.toast) / M.k;
  const float w = tw + 48, h = 44, x = 835 - w * 0.5f, y = 94;
  UiBox b = {ui_alpha(0xF0203A80u, a), ui_alpha(0xF00C1A48u, a), ui_alpha(0xFFFFFFFFu, a), S(22), S(1.5f), 0, 0.5f, 1};
  ui_box(X(x), Y(y), S(w), S(h), &b);
  draw_text(UI_FONT_BODY, 22, ui_alpha(C_WHITE, a), x + w * 0.5f, y + h * 0.5f, UI_ALIGN_CENTER, M.toast);
}

static void draw(const float rect[4]) {
  M.nhits = 0;
  /* fit the 1280x720 canvas into the window */
  M.k = fminf(M.ww / 1280.0f, M.wh / 720.0f);
  M.ox = (M.ww - 1280.0f * M.k) * 0.5f;
  M.oy = (M.wh - 720.0f * M.k) * 0.5f;
  draw_background(rect);
  const float slide = (1.0f - M.fade) * -30.0f;
  draw_title(slide);
  draw_root(slide);
  draw_panel(-slide);
  draw_dialog();
  draw_hints();
  draw_toast();
}

/* ---- test script ----
   NBA_MENU_SCRIPT="Down Return shot ..." drives the menu for tests: every 0.4 s one token, either
   an SDL key name (pressed and released), "pad:<button>" (an SDL gamepad button name pressed on
   the first gamepad, e.g. pad:a) or "shot", which writes the window to
   $NBA_MENU_SHOTS/menu_NN.png (default the current folder). */
static struct {
  const char* p;
  double next;
  int shots;
} T;

static void script_step(int ww, int wh) {
  if (!T.p || M.now < T.next) return;
  while (*T.p == ' ') ++T.p;
  if (!*T.p) { T.p = NULL; return; }
  char tok[32];
  int n = 0;
  while (T.p[n] && T.p[n] != ' ' && n < 31) { tok[n] = T.p[n]; ++n; }
  tok[n] = 0;
  T.p += n;
  T.next = M.now + 0.4;
  if (!strcmp(tok, "shot")) {
    uint8_t* px = SDL_malloc((size_t)ww * wh * 4);
    uint8_t* flip = SDL_malloc((size_t)ww * wh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < wh; ++y) memcpy(flip + (size_t)y * ww * 4, px + (size_t)(wh - 1 - y) * ww * 4, (size_t)ww * 4);
    char path[1024];
    const char* dir = SDL_getenv("NBA_MENU_SHOTS");
    snprintf(path, sizeof path, "%s/menu_%02d.png", dir ? dir : ".", T.shots++);
    raster_sw_write_png(path, flip, ww, wh);
    fprintf(stderr, "[menu] %s\n", path);
    SDL_free(px);
    SDL_free(flip);
    return;
  }
  if (!strncmp(tok, "pad:", 4)) { /* a gamepad button on the first gamepad */
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(tok + 4);
    if (ids && count > 0 && b != SDL_GAMEPAD_BUTTON_INVALID) {
      SDL_Event e;
      SDL_zero(e);
      e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      e.gbutton.which = ids[0];
      e.gbutton.button = (Uint8)b;
      e.gbutton.down = true;
      SDL_PushEvent(&e);
    } else fprintf(stderr, "[menu] no gamepad for %s\n", tok);
    SDL_free(ids);
    return;
  }
  const SDL_Keycode key = SDL_GetKeyFromName(tok);
  if (key == SDLK_UNKNOWN) { fprintf(stderr, "[menu] unknown key %s\n", tok); return; }
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.key = key;
  e.key.scancode = SDL_GetScancodeFromKey(key, NULL);
  e.key.down = true;
  SDL_PushEvent(&e);
  e.type = SDL_EVENT_KEY_UP;
  e.key.down = false;
  SDL_PushEvent(&e);
}

/* ---- loop ---- */
static void handle_event(const SDL_Event* e) {
  switch (e->type) {
    case SDL_EVENT_QUIT: quit_game(); break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED: input_event(e); break;
    case SDL_EVENT_KEY_DOWN:
      M.used_pad = 0;
      if (M.capturing) { if (!e->key.repeat) capture_key(e->key.scancode); break; }
      if (e->key.key == SDLK_F11 || (e->key.key == SDLK_RETURN && (e->key.mod & SDL_KMOD_ALT))) {
        g_settings.fullscreen = !g_settings.fullscreen;
        settings_touched(SET_FULLSCREEN);
        M.host->apply(APPLY_DISPLAY);
        M.dirty = 1;
        break;
      }
      nav(key_nav(&e->key));
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
      M.used_pad = 1;
      M.pad_type = SDL_GetGamepadTypeForID(e->gbutton.which);
      const int b = e->gbutton.button;
      if (M.capturing) { capture_pad(b); break; }
      if (b == SDL_GAMEPAD_BUTTON_LEFT_STICK || b == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(e->gbutton.which);
        const int other = b == SDL_GAMEPAD_BUTTON_LEFT_STICK ? SDL_GAMEPAD_BUTTON_RIGHT_STICK : SDL_GAMEPAD_BUTTON_LEFT_STICK;
        if (g && SDL_GetGamepadButton(g, (SDL_GamepadButton)other)) nav(NAV_CLOSE);
        break;
      }
      nav(button_nav(b));
      break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      if (M.capturing && M.device == DEV_PADS && e->gaxis.value > 16000 &&
          (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        capture_pad(e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PAD_LT : PAD_RT);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e->button.button == SDL_BUTTON_LEFT) mouse_click(M.mx, M.my);
      else if (e->button.button == SDL_BUTTON_RIGHT) { if (M.capturing) M.capturing = 0; else nav(NAV_BACK); }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e->button.button == SDL_BUTTON_LEFT) M.drag_row = -1;
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (!M.capturing) nav(e->wheel.y > 0 ? NAV_UP : e->wheel.y < 0 ? NAV_DOWN : NAV_NONE);
      break;
    default: break;
  }
}

int menu_run(const MenuHost* host) {
  if (ui_init() != 0) return -1;
  const int keep_page = M.page, keep_device = M.device;
  memset(&M, 0, sizeof M);
  M.host = host;
  M.page = keep_page;
  M.device = keep_device;
  M.drag_row = -1;
  M.root_sel = ROOT_RESUME;
  M.root_y = 222;
  M.now = M.opened = SDL_GetTicksNS() / 1e9;
  M.held_dir = held_direction(); /* a stick already held must not move the cursor at once */
  M.used_pad = M.held_dir != NAV_NONE;
  {
    int tw, th;
    const unsigned tex = host->frame_texture(&tw, &th);
    ui_snapshot(tex, tw, th);
  }
  if (!T.p && !T.shots) T.p = SDL_getenv("NBA_MENU_SCRIPT");
  T.next = M.now + 0.5;
  double last = M.now;
  for (;;) {
    SDL_GetWindowSizeInPixels(host->window, &M.ww, &M.wh);
    int pt_w, pt_h; /* mouse coordinates are in points, drawing in pixels */
    SDL_GetWindowSize(host->window, &pt_w, &pt_h);
    const float mouse_scale = pt_w > 0 ? (float)M.ww / pt_w : 1.0f;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_MOUSE_MOTION) { mouse_move(e.motion.x * mouse_scale, e.motion.y * mouse_scale); continue; }
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { M.mx = e.button.x * mouse_scale; M.my = e.button.y * mouse_scale; }
      handle_event(&e);
    }
    M.now = SDL_GetTicksNS() / 1e9;
    const float dt = (float)(M.now - last);
    last = M.now;
    poll_held();
    if (M.capturing && M.now - M.capture_t0 > 8.0) M.capturing = 0;

    /* animation */
    M.fade = M.closing ? fmaxf(0.0f, M.fade - dt / 0.14f) : fminf(1.0f, M.fade + dt / 0.16f);
    M.root_y = approach(M.root_y, 222 + M.root_sel * 60.0f, 22, dt);
    M.row_y = approach(M.row_y, 50.0f * (M.row_sel + 0.5f), 22, dt);
    const int panel_visible = M.root_sel >= ROOT_FIRST_PAGE && M.root_sel <= ROOT_LAST_PAGE;
    M.panel_a = approach(M.panel_a, panel_visible ? 1.0f : 0.0f, 16, dt);
    M.slide = approach(M.slide, 0.0f, 18, dt);
    if (fabsf(M.slide) < 0.002f) M.slide = 0;
    M.dlg_a = approach(M.dlg_a, M.quit_dialog ? 1.0f : 0.0f, 20, dt);
    M.focus_a = approach(M.focus_a, M.in_page ? 1.0f : 0.0f, 18, dt);

    float rect[4];
    host->present(M.ww, M.wh, rect);
    ui_begin(M.ww, M.wh);
    draw(rect);
    ui_end();
    script_step(M.ww, M.wh);
    SDL_GL_SwapWindow(host->window);
    if (M.closing && M.fade <= 0.0f) break;
    if (!g_settings.vsync) { /* no swap interval: pace to the display */
      const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(host->window));
      const float hz = dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f;
      const double spent = SDL_GetTicksNS() / 1e9 - M.now;
      if (spent < 1.0 / hz) SDL_DelayPrecise((uint64_t)((1.0 / hz - spent) * 1e9));
    }
  }
  if (M.dirty) settings_save();
  input_hold_until_released();
  return 0;
}
