// Software reference renderer for the Banshee 3D pipeline (see raster_sw.c).
#ifndef NBA_RASTER_SW_H
#define NBA_RASTER_SW_H

#include <stdint.h>

/* Called at each selected buffer swap with the displayed buffer (RGBA8, w x h). */
typedef void (*RasterSwFrameFn)(void* user, uint64_t frame, const uint8_t* rgba, int w, int h);

/* Installs itself as the 3D backend. Frames first_frame, first_frame + every, ... are rasterised
   (each frame starts with a fast fill, so the others can be skipped). scale multiplies 512x384. */
void raster_sw_init(int scale, uint64_t first_frame, uint64_t every, RasterSwFrameFn fn, void* user);
int raster_sw_write_png(const char* path, const uint8_t* rgba, int w, int h);

#endif
