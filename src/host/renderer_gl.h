// OpenGL renderer backend for the Banshee 3D pipeline (see renderer_gl.c).
#ifndef NBA_RENDERER_GL_H
#define NBA_RENDERER_GL_H

#include <stdint.h>

/* Called after each buffer swap (the displayed buffer is ready to present). */
typedef void (*RendererGlFrameFn)(void* user, uint64_t frame);

typedef struct {
  uint64_t frames, triangles, draw_calls, texture_uploads;
} RendererGlStats;

/* Needs a current OpenGL 3.3 core context with functions loaded (nba_gl_load). Renders
   384*scale pixels high at the given aspect ratio (4/3 = original; wider shows more of the game's
   X range around the centre) and installs itself as the 3D backend. Returns 0 on success. */
int renderer_gl_init(float scale, float aspect, RendererGlFrameFn fn, void* user);
/* Change the render scale and aspect ratio between frames (after the frame callback has presented).
   Returns 1 if anything changed. */
int renderer_gl_set_view(float scale, float aspect);
void renderer_gl_size(int* w, int* h);
void renderer_gl_present(int win_w, int win_h, int dx, int dy, int dw, int dh);
void renderer_gl_read_display(uint8_t* rgba); /* width*height*4, top row first */
RendererGlStats* renderer_gl_stats(void);

#endif
