// Widescreen patches for the game's visibility tests (see widescreen.c).
#ifndef NBA_WIDESCREEN_H
#define NBA_WIDESCREEN_H

/* Call after the game image is in RAM. aspect = width / height of the rendered view. */
void widescreen_patch(float aspect);

#endif
