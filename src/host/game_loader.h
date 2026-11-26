// Loads the game library the launcher built (nbagame.so / .dll / .dylib) into the engine.
#ifndef NBA_GAME_LOADER_H
#define NBA_GAME_LOADER_H

#include <stddef.h>

/* Default location: next to the executable. */
void game_library_default_path(char* out, size_t n);

/* Loads and links the library. On failure fills `why` (for the player) and returns -1, or -2
   when the library is from another version (the player should run the launcher again). */
int game_library_load(const char* path, char* why, size_t n);

#endif
