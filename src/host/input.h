// Host input: keyboard and gamepads -> I/O ASIC inputs (see input.c for the key map).
#ifndef NBA_INPUT_H
#define NBA_INPUT_H

#include <stdint.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

void input_init(void);
void input_event(const SDL_Event* e);
void input_update(uint64_t frame); /* once per frame, after events were pumped */
void input_hold_until_released(void); /* ignore buttons until all are up (after the menu) */
uint32_t input_pad_held(SDL_Gamepad* g); /* held inputs as bits (see settings.h PAD_LT) */

#endif
