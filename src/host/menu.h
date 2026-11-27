// The in-game menu: pauses the game and changes the settings (see menu.c).
#ifndef NBA_MENU_H
#define NBA_MENU_H

#include <SDL3/SDL_video.h>

/* What changed, for MenuHost.apply. */
enum { APPLY_DISPLAY = 1, APPLY_VIEW = 2, APPLY_VSYNC = 4, APPLY_AUDIO = 8, APPLY_SCALING = 16 };

typedef struct {
  SDL_Window* window;
  /* Draw the paused game frame into the window; returns its rectangle (x, y, w, h). */
  void (*present)(int win_w, int win_h, float rect[4]);
  unsigned (*frame_texture)(int* w, int* h); /* the paused frame, for the blurred backdrop */
  void (*apply)(unsigned what);
  void (*quit)(void);
  void (*restart)(void); /* start the game again (DIP switch changes) */
} MenuHost;

/* Runs the menu until the player resumes (returns) or quits (host->quit). Returns -1 if the
   menu can't be drawn (no font / shader). */
int menu_run(const MenuHost* host);

#endif
