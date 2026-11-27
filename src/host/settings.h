// Player settings (video, audio, controls), saved to settings.ini (see settings.c).
#ifndef NBA_SETTINGS_H
#define NBA_SETTINGS_H

#include <stdint.h>

#include <SDL3/SDL_scancode.h>

/* Cabinet inputs a player can bind. The directions are keyboard only: gamepads always steer with
   the D-pad and the left stick. */
enum { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_A, ACT_B, ACT_TURBO, ACT_START, ACT_COIN, ACT_COUNT };
enum { KEY_SLOTS = 2, KB_PLAYERS = 2 };
/* Gamepad inputs as bits: SDL_GamepadButton values, plus the two triggers. */
enum { PAD_LT = 28, PAD_RT = 29 };

typedef struct {
  /* gameplay: cabinet DIP switches, read by the game when it boots */
  int free_play;      /* no coins needed */
  int four_player;    /* 4-player cabinet (else 2) */
  /* video */
  int fullscreen;
  float aspect;       /* 0 = auto (match the window), else width / height */
  float render_scale; /* 0 = match the window, else x 384 lines */
  int vsync;
  int sharp_scaling;  /* nearest-neighbour upscaling of the rendered image */
  /* audio */
  int volume;         /* 0..100 */
  int mute_unfocused;
  /* controls */
  int deadzone;       /* left stick dead zone, percent */
  SDL_Scancode keys[KB_PLAYERS][ACT_COUNT][KEY_SLOTS];
  uint32_t pad[ACT_COUNT]; /* bit masks of gamepad inputs (see PAD_LT); directions unused */
} Settings;

/* Settings that can also come from the command line: those values are used for this run only and
   the file keeps its own unless they are changed in the menu. */
enum { SET_FULLSCREEN = 1, SET_ASPECT = 2, SET_SCALE = 4, SET_VSYNC = 8, SET_VOLUME = 16 };

extern Settings g_settings;
extern Settings g_settings_at_boot; /* as the game booted with (DIP switches need a restart) */

void settings_defaults(Settings* s);
void settings_default_controls(Settings* s);
void settings_load(void);                  /* defaults, then the file if there is one */
void settings_override(unsigned which);    /* the command line set these fields */
void settings_touched(unsigned which);     /* the menu changed these fields: save them */
void settings_save(void);
const char* settings_path(void);
const char* settings_dir(void); /* the folder settings.ini is in, with a trailing separator */

/* The DIP switch word for the I/O board (see docs/hardware.md). */
uint16_t settings_dip_switches(const Settings* s);

/* Volume 0..100 -> SDL stream gain (the board's output is quiet: 75 = gain 3). */
float settings_gain(int volume);
int settings_volume_from_gain(float gain);

#endif
