// Host input: keyboard and SDL gamepads mapped onto the cabinet's I/O ASIC inputs.
//
// Cabinet (per player): 49-way joystick, A, B, Turbo, Start; plus coins, service and test.
// The bindings live in the settings (settings.c; changed from the in-game menu). Defaults:
//   P1: arrows, A = Z, B = X, Turbo = C (or Left Shift), Start = 1, Coin = 5
//   P2: I/J/K/L, A = Q, B = W, Turbo = E, Start = 2, Coin = 6
//   P3/P4: Start = 3/4, Coin = 7/8 (gamepads only for play)
//   Service credit = F9, Test switch = F10 (operator menu), Volume = - / =
// Gamepads (in connection order = players 1-4): left stick (analog, all 7 positions per axis)
// or D-pad, South = A, East = B, West or a trigger/shoulder = Turbo, Start = Start, Back = Coin.
#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../../runtime/src/ioasic.h"
#include "settings.h"

/* bit masks (active low in the registers; kept active high here) */
enum { SYS_COIN1 = 0x1, SYS_COIN2 = 0x2, SYS_START1 = 0x4, SYS_TEST = 0x10, SYS_START2 = 0x20, SYS_SERVICE = 0x40,
       SYS_COIN3 = 0x80, SYS_COIN4 = 0x100, SYS_START3 = 0x200, SYS_START4 = 0x400, SYS_VOLDN = 0x800, SYS_VOLUP = 0x1000 };
static const uint16_t kStart[4] = {SYS_START1, SYS_START2, SYS_START3, SYS_START4};
static const uint16_t kCoin[4] = {SYS_COIN1, SYS_COIN2, SYS_COIN3, SYS_COIN4};
enum { BTN_A = 1, BTN_B = 2, BTN_TURBO = 4 };

typedef struct {
  int dx, dy;        /* digital direction -1/0/1 */
  float ax, ay;      /* analog stick -1..1 */
  int analog;        /* analog stick in use */
  int buttons, start, coin;
} Player;

typedef struct {
  int port, from, to;
  uint16_t mask;
} Press;

static struct {
  SDL_Gamepad* pads[4];
  Player kb[4], pad[4];
  Press script[128];
  int nscript;
  int hold; /* after the menu: ignore buttons until all are released */
} I;

void input_init(void) {
  memset(&I, 0, sizeof I);
  SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  /* NBA_PRESS=port:mask:from:to[,...] presses bits (hex) on a port (0 = SYSTEM, 1 = IN1, 2 = IN2,
     3 = player 1 stick with 1/2/4/8 = up/down/left/right)
     during a frame range: lets tests drive the game without a keyboard. */
  const char* s = getenv("NBA_PRESS");
  while (s && *s && I.nscript < 128) {
    Press p;
    unsigned mask;
    if (sscanf(s, "%d:%x:%d:%d", &p.port, &mask, &p.from, &p.to) != 4) break;
    p.mask = (uint16_t)mask;
    I.script[I.nscript++] = p;
    s = strchr(s, ',');
    if (s) ++s;
  }
}

static void open_pad(SDL_JoystickID id) {
  for (int i = 0; i < 4; ++i)
    if (!I.pads[i]) {
      I.pads[i] = SDL_OpenGamepad(id);
      if (I.pads[i]) fprintf(stderr, "gamepad %d: %s\n", i + 1, SDL_GetGamepadName(I.pads[i]));
      return;
    }
}

static void close_pad(SDL_JoystickID id) {
  for (int i = 0; i < 4; ++i)
    if (I.pads[i] && SDL_GetGamepadID(I.pads[i]) == id) {
      SDL_CloseGamepad(I.pads[i]);
      I.pads[i] = NULL;
      memset(&I.pad[i], 0, sizeof I.pad[i]);
    }
}

void input_event(const SDL_Event* e) {
  if (e->type == SDL_EVENT_GAMEPAD_ADDED) open_pad(e->gdevice.which);
  else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(e->gdevice.which);
}

static int key_down(const bool* k, const SDL_Scancode* slots) {
  for (int i = 0; i < KEY_SLOTS; ++i)
    if (slots[i] && k[slots[i]]) return 1;
  return 0;
}

static void read_keyboard(void) {
  const bool* k = SDL_GetKeyboardState(NULL);
  for (int p = 0; p < KB_PLAYERS; ++p) {
    SDL_Scancode(*b)[KEY_SLOTS] = g_settings.keys[p];
    Player* pl = &I.kb[p];
    pl->dx = key_down(k, b[ACT_RIGHT]) - key_down(k, b[ACT_LEFT]);
    pl->dy = key_down(k, b[ACT_DOWN]) - key_down(k, b[ACT_UP]);
    pl->buttons = (key_down(k, b[ACT_A]) ? BTN_A : 0) | (key_down(k, b[ACT_B]) ? BTN_B : 0) |
                  (key_down(k, b[ACT_TURBO]) ? BTN_TURBO : 0);
    pl->start = key_down(k, b[ACT_START]);
    pl->coin = key_down(k, b[ACT_COIN]);
  }
  static const SDL_Scancode start[2] = {SDL_SCANCODE_3, SDL_SCANCODE_4};
  static const SDL_Scancode coin[2] = {SDL_SCANCODE_7, SDL_SCANCODE_8};
  for (int i = 0; i < 2; ++i) {
    I.kb[2 + i].start = k[start[i]];
    I.kb[2 + i].coin = k[coin[i]];
  }
}

/* The gamepad inputs held now, as bits (SDL_GamepadButton values, PAD_LT, PAD_RT). */
uint32_t input_pad_held(SDL_Gamepad* g) {
  uint32_t m = 0;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
    if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) m |= 1u << b;
  if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) m |= 1u << PAD_LT;
  if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) m |= 1u << PAD_RT;
  return m;
}

static void read_pads(void) {
  const float dz = g_settings.deadzone / 100.0f;
  for (int i = 0; i < 4; ++i) {
    SDL_Gamepad* g = I.pads[i];
    if (!g) continue;
    Player* p = &I.pad[i];
    p->dx = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) - SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    p->dy = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) - SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP);
    p->ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    p->ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    p->analog = p->ax * p->ax + p->ay * p->ay > dz * dz; /* outside the dead zone */
    const uint32_t held = input_pad_held(g);
    const uint32_t* m = g_settings.pad;
    p->buttons = ((held & m[ACT_A]) ? BTN_A : 0) | ((held & m[ACT_B]) ? BTN_B : 0) | ((held & m[ACT_TURBO]) ? BTN_TURBO : 0);
    p->start = (held & m[ACT_START]) != 0;
    p->coin = (held & m[ACT_COIN]) != 0;
  }
}

/* analog -1..1 -> 49-way position 0..6 (3 = centre) */
static int pos49(float v) {
  int p = (int)((v + 1.0f) * 3.5f);
  return p < 0 ? 0 : p > 6 ? 6 : p;
}

void input_hold_until_released(void) { I.hold = 1; }

void input_update(uint64_t frame) {
  read_keyboard();
  read_pads();
  if (I.hold) { /* the button that closed the menu must not reach the game */
    int any = 0;
    for (int i = 0; i < 4; ++i) {
      any |= I.kb[i].buttons | I.kb[i].start | I.kb[i].coin | I.pad[i].buttons | I.pad[i].start | I.pad[i].coin;
      I.kb[i].buttons = I.kb[i].start = I.kb[i].coin = 0;
      I.pad[i].buttons = I.pad[i].start = I.pad[i].coin = 0;
    }
    if (!any) I.hold = 0;
  }
  uint16_t sys = 0, in1 = 0, in2 = 0;
  for (int i = 0; i < 4; ++i) {
    const Player* k = &I.kb[i];
    const Player* g = &I.pad[i];
    int x, y;
    if (g->analog) { x = pos49(g->ax); y = pos49(g->ay); }
    else {
      const int dx = k->dx ? k->dx : g->dx, dy = k->dy ? k->dy : g->dy;
      x = 3 + 3 * dx;
      y = 3 + 3 * dy;
    }
    ioasic_set_stick(i, x, y);
    const int b = k->buttons | g->buttons;
    const uint16_t bits = (uint16_t)(((b & BTN_A) ? 0x10 : 0) | ((b & BTN_B) ? 0x20 : 0) | ((b & BTN_TURBO) ? 0x40 : 0));
    if (i == 0) in1 |= bits;
    else if (i == 1) in1 |= (uint16_t)(bits << 8);
    else if (i == 2) in2 |= bits;
    else in2 |= (uint16_t)(bits << 8);
    if (k->start || g->start) sys |= kStart[i];
    if (k->coin || g->coin) sys |= kCoin[i];
  }
  const bool* key = SDL_GetKeyboardState(NULL);
  if (key[SDL_SCANCODE_F9]) sys |= SYS_SERVICE;
  if (key[SDL_SCANCODE_F10]) sys |= SYS_TEST;
  if (key[SDL_SCANCODE_MINUS]) sys |= SYS_VOLDN;
  if (key[SDL_SCANCODE_EQUALS]) sys |= SYS_VOLUP;
  for (int i = 0; i < I.nscript; ++i)
    if ((int)frame >= I.script[i].from && (int)frame < I.script[i].to) {
      if (I.script[i].port == 3) { /* player 1 stick: 1 up, 2 down, 4 left, 8 right */
        const uint16_t m = I.script[i].mask;
        ioasic_set_stick(0, (m & 4) ? 0 : (m & 8) ? 6 : 3, (m & 1) ? 0 : (m & 2) ? 6 : 3);
      } else if (I.script[i].port == 0) sys |= I.script[i].mask;
      else if (I.script[i].port == 1) in1 |= I.script[i].mask;
      else in2 |= I.script[i].mask;
    }
  ioasic_set_input(IOASIC_PORT_SYSTEM, (uint16_t)~sys);
  ioasic_set_input(IOASIC_PORT_IN1, (uint16_t)~in1);
  ioasic_set_input(IOASIC_PORT_IN2, (uint16_t)(~in2 | 0x0F00)); /* bits 8-11: joystick adapter */
}
