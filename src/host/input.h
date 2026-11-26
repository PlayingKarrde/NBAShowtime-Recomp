// Host input: keyboard and gamepads -> I/O ASIC inputs (see input.c for the key map).
#ifndef NBA_INPUT_H
#define NBA_INPUT_H

#include <stdint.h>

#include <SDL3/SDL_events.h>

void input_init(void);
void input_event(const SDL_Event* e);
void input_update(uint64_t frame); /* once per frame, after events were pumped */

#endif
