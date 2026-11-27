// Player settings, kept in settings.ini: next to the executable when a settings.ini is already
// there (a portable install), otherwise in the user's preferences folder (SDL_GetPrefPath, e.g.
// ~/.local/share/NBAShowtimeRecomp/ on Linux, %APPDATA%\NBAShowtimeRecomp\ on Windows).
//
// Plain "key = value" lines. Keyboard keys use SDL's scancode names ("Left Shift"), gamepad
// buttons SDL's mapping names ("leftshoulder", plus "lefttrigger" / "righttrigger"); several
// inputs for one action are separated by commas.
#include "settings.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

Settings g_settings;
Settings g_settings_at_boot;
static Settings g_file;      /* the values in the file (for fields overridden on the command line) */
static unsigned g_overridden;
static char g_path[1200];

static const char* const k_act_keys[ACT_COUNT] = {"up", "down", "left", "right", "a", "b", "turbo", "start", "coin"};

float settings_gain(int volume) { return 3.0f * (float)volume / 75.0f; }
int settings_volume_from_gain(float gain) {
  const int v = (int)lroundf(gain * 75.0f / 3.0f);
  return v < 0 ? 0 : v > 100 ? 100 : v;
}

void settings_default_controls(Settings* s) {
  memset(s->keys, 0, sizeof s->keys);
  memset(s->pad, 0, sizeof s->pad);
  static const SDL_Scancode p1[ACT_COUNT] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                                             SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_1, SDL_SCANCODE_5};
  static const SDL_Scancode p2[ACT_COUNT] = {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
                                             SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_2, SDL_SCANCODE_6};
  for (int a = 0; a < ACT_COUNT; ++a) {
    s->keys[0][a][0] = p1[a];
    s->keys[1][a][0] = p2[a];
  }
  s->keys[0][ACT_TURBO][1] = SDL_SCANCODE_LSHIFT;
  s->pad[ACT_A] = 1u << SDL_GAMEPAD_BUTTON_SOUTH;
  s->pad[ACT_B] = 1u << SDL_GAMEPAD_BUTTON_EAST;
  s->pad[ACT_TURBO] = 1u << SDL_GAMEPAD_BUTTON_WEST | 1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER |
                      1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER | 1u << PAD_LT | 1u << PAD_RT;
  s->pad[ACT_START] = 1u << SDL_GAMEPAD_BUTTON_START;
  s->pad[ACT_COIN] = 1u << SDL_GAMEPAD_BUTTON_BACK;
}

/* As shipped (0xBE7E, MAME's names): coinage from CMOS, USA, 49-way sticks, medium resolution,
   2-player cabinet. Free play takes coinage from the switches with both the coinage (0x000E) and
   currency (0x0030) fields at their free play setting (0); either one alone still asks for coins. */
uint16_t settings_dip_switches(const Settings* s) {
  uint16_t d = 0xBE7E;
  if (s->free_play) d = (uint16_t)((d | 0x0001) & ~0x003E);
  if (s->four_player) d &= (uint16_t)~0x2000;
  return d;
}

void settings_defaults(Settings* s) {
  memset(s, 0, sizeof *s);
  s->free_play = 1;
  s->four_player = 1;
  s->fullscreen = 0;
  s->aspect = 0.0f;
  s->render_scale = 0.0f;
  s->vsync = 0;
  s->sharp_scaling = 0;
  s->volume = 75;
  s->mute_unfocused = 0;
  s->deadzone = 20;
  settings_default_controls(s);
}

static const char* pad_input_name(int bit) {
  if (bit == PAD_LT) return "lefttrigger";
  if (bit == PAD_RT) return "righttrigger";
  return SDL_GetGamepadStringForButton((SDL_GamepadButton)bit);
}

static int pad_input_from_name(const char* name) {
  if (!SDL_strcasecmp(name, "lefttrigger")) return PAD_LT;
  if (!SDL_strcasecmp(name, "righttrigger")) return PAD_RT;
  const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
  return b == SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
}

static char* trim(char* s) {
  while (isspace((unsigned char)*s)) ++s;
  char* e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

static int on_off(const char* v) {
  return !SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "true") || !SDL_strcasecmp(v, "yes") || atoi(v) != 0;
}

static void parse_line(Settings* s, char* key, char* val) {
  if (!strcmp(key, "free_play")) s->free_play = on_off(val);
  else if (!strcmp(key, "cabinet_players")) s->four_player = atoi(val) == 4;
  else if (!strcmp(key, "fullscreen")) s->fullscreen = on_off(val);
  else if (!strcmp(key, "aspect")) {
    float a = 0, b = 0;
    if (!SDL_strcasecmp(val, "auto")) s->aspect = 0;
    else if (sscanf(val, "%f:%f", &a, &b) == 2 && a > 0 && b > 0) s->aspect = a / b;
    else if (atof(val) > 0) s->aspect = (float)atof(val);
  }
  else if (!strcmp(key, "render_scale")) s->render_scale = !SDL_strcasecmp(val, "auto") ? 0.0f : (float)atof(val);
  else if (!strcmp(key, "vsync")) s->vsync = on_off(val);
  else if (!strcmp(key, "sharp_scaling")) s->sharp_scaling = on_off(val);
  else if (!strcmp(key, "volume")) s->volume = SDL_clamp(atoi(val), 0, 100);
  else if (!strcmp(key, "mute_unfocused")) s->mute_unfocused = on_off(val);
  else if (!strcmp(key, "deadzone")) s->deadzone = SDL_clamp(atoi(val), 0, 90);
  else if (!strncmp(key, "key.p", 5) && key[5] >= '1' && key[5] < '1' + KB_PLAYERS && key[6] == '.') {
    const int p = key[5] - '1';
    for (int a = 0; a < ACT_COUNT; ++a)
      if (!strcmp(key + 7, k_act_keys[a])) {
        memset(s->keys[p][a], 0, sizeof s->keys[p][a]);
        int n = 0;
        for (char* tok = strtok(val, ","); tok && n < KEY_SLOTS; tok = strtok(NULL, ",")) {
          const SDL_Scancode sc = SDL_GetScancodeFromName(trim(tok));
          if (sc != SDL_SCANCODE_UNKNOWN) s->keys[p][a][n++] = sc;
        }
      }
  }
  else if (!strncmp(key, "pad.", 4)) {
    for (int a = ACT_A; a < ACT_COUNT; ++a)
      if (!strcmp(key + 4, k_act_keys[a])) {
        s->pad[a] = 0;
        for (char* tok = strtok(val, ","); tok; tok = strtok(NULL, ",")) {
          const int b = pad_input_from_name(trim(tok));
          if (b >= 0) s->pad[a] |= 1u << b;
        }
      }
  }
}

const char* settings_path(void) {
  if (g_path[0]) return g_path;
  const char* base = SDL_GetBasePath();
  if (base) {
    snprintf(g_path, sizeof g_path, "%ssettings.ini", base);
    FILE* f = fopen(g_path, "rb");
    if (f) { fclose(f); return g_path; }
  }
  char* pref = SDL_GetPrefPath(NULL, "NBAShowtimeRecomp");
  if (pref) {
    snprintf(g_path, sizeof g_path, "%ssettings.ini", pref);
    SDL_free(pref);
  } else {
    snprintf(g_path, sizeof g_path, "settings.ini");
  }
  return g_path;
}

const char* settings_dir(void) {
  static char dir[1200];
  snprintf(dir, sizeof dir, "%s", settings_path());
  char* slash = strrchr(dir, '/');
#ifdef _WIN32
  char* bslash = strrchr(dir, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
  if (slash) slash[1] = 0;
  else dir[0] = 0;
  return dir;
}

void settings_load(void) {
  settings_defaults(&g_settings);
  FILE* f = fopen(settings_path(), "rb");
  if (f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      char* s = trim(line);
      if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
      char* eq = strchr(s, '=');
      if (!eq) continue;
      *eq = 0;
      parse_line(&g_settings, trim(s), trim(eq + 1));
    }
    fclose(f);
  }
  g_file = g_settings;
}

void settings_override(unsigned which) { g_overridden |= which; }
void settings_touched(unsigned which) { g_overridden &= ~which; }

void settings_save(void) {
  Settings s = g_settings;
  if (g_overridden & SET_FULLSCREEN) s.fullscreen = g_file.fullscreen;
  if (g_overridden & SET_ASPECT) s.aspect = g_file.aspect;
  if (g_overridden & SET_SCALE) s.render_scale = g_file.render_scale;
  if (g_overridden & SET_VSYNC) s.vsync = g_file.vsync;
  if (g_overridden & SET_VOLUME) s.volume = g_file.volume;
  FILE* f = fopen(settings_path(), "wb");
  if (!f) { fprintf(stderr, "cannot write settings to %s\n", settings_path()); return; }
  fprintf(f, "# NBA Showtime Recomp settings (also changed from the in-game menu: Esc, or L3+R3)\n\n");
  fprintf(f, "[gameplay]\nfree_play = %s\ncabinet_players = %d\n\n", s.free_play ? "on" : "off", s.four_player ? 4 : 2);
  fprintf(f, "[video]\nfullscreen = %s\n", s.fullscreen ? "on" : "off");
  {
    /* The menu's ratios by name (as launchers write them too), anything else as a number. */
    static const struct { int w, h; } named[] = {{4, 3}, {16, 10}, {16, 9}, {21, 9}, {32, 9}};
    char buf[32] = "auto";
    if (s.aspect > 0) snprintf(buf, sizeof buf, "%.4f", s.aspect);
    for (size_t i = 0; s.aspect > 0 && i < sizeof named / sizeof named[0]; ++i)
      if (fabsf(s.aspect - (float)named[i].w / (float)named[i].h) < 0.001f)
        snprintf(buf, sizeof buf, "%d:%d", named[i].w, named[i].h);
    fprintf(f, "aspect = %s\n", buf);
  }
  if (s.render_scale > 0) fprintf(f, "render_scale = %g\n", s.render_scale);
  else fprintf(f, "render_scale = auto\n");
  fprintf(f, "vsync = %s\nsharp_scaling = %s\n\n", s.vsync ? "on" : "off", s.sharp_scaling ? "on" : "off");
  fprintf(f, "[audio]\nvolume = %d\nmute_unfocused = %s\n\n", s.volume, s.mute_unfocused ? "on" : "off");
  fprintf(f, "[controls]\ndeadzone = %d\n", s.deadzone);
  for (int p = 0; p < KB_PLAYERS; ++p)
    for (int a = 0; a < ACT_COUNT; ++a) {
      fprintf(f, "key.p%d.%s = ", p + 1, k_act_keys[a]);
      int first = 1;
      for (int k = 0; k < KEY_SLOTS; ++k)
        if (s.keys[p][a][k]) {
          fprintf(f, "%s%s", first ? "" : ", ", SDL_GetScancodeName(s.keys[p][a][k]));
          first = 0;
        }
      fputc('\n', f);
    }
  for (int a = ACT_A; a < ACT_COUNT; ++a) {
    fprintf(f, "pad.%s = ", k_act_keys[a]);
    int first = 1;
    for (int b = 0; b < 32; ++b)
      if ((s.pad[a] >> b) & 1) {
        const char* n = pad_input_name(b);
        if (!n) continue;
        fprintf(f, "%s%s", first ? "" : ", ", n);
        first = 0;
      }
    fputc('\n', f);
  }
  fclose(f);
}
