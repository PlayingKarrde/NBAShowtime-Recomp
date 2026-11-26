// OpenGL renderer backend for the Banshee 3D pipeline (see renderer_gl.c).
#ifndef NBA_RENDERER_GL_H
#define NBA_RENDERER_GL_H

#include <stdint.h>

/* Called after each buffer swap (the displayed buffer is ready to present). */
typedef void (*RendererGlFrameFn)(void* user, uint64_t frame);

typedef struct {
  uint64_t frames, triangles, draw_calls, texture_uploads;
} RendererGlStats;

/* Needs a current OpenGL 3.3 core context with functions loaded (nba_gl_load). Renders at
   scale x 512x384 and installs itself as the 3D backend. Returns 0 on success. */
int renderer_gl_init(float scale, RendererGlFrameFn fn, void* user);
void renderer_gl_size(int* w, int* h);
void renderer_gl_present(int win_w, int win_h, int dx, int dy, int dw, int dh);
void renderer_gl_read_display(uint8_t* rgba); /* width*height*4, top row first */
RendererGlStats* renderer_gl_stats(void);

#endif
