// Small OpenGL 2D drawing layer for the in-game menu: rounded / slanted boxes with gradients,
// gloss and chrome borders, triangles, signed distance field text and a blurred copy of the game
// frame. Coordinates are window pixels, top-left origin. Colours are 0xAARRGGBB.
#ifndef NBA_UI_GL_H
#define NBA_UI_GL_H

#include <stdint.h>

enum { UI_FONT_DISPLAY, UI_FONT_BODY, UI_FONT_COUNT };
enum { UI_TEXT_PLAIN, UI_TEXT_CHROME, UI_TEXT_GOLD };
enum { UI_ALIGN_LEFT, UI_ALIGN_CENTER, UI_ALIGN_RIGHT };

typedef struct {
  uint32_t top, bottom;   /* fill gradient */
  uint32_t border;        /* border colour (unless chrome_border) */
  float radius, border_w;
  float skew;             /* x shift per pixel up: slanted like italic type */
  float gloss;            /* 0..1 highlight on the upper half */
  int chrome_border;
} UiBox;

typedef struct {
  int font;
  float px;               /* font size in pixels */
  int style;              /* UI_TEXT_PLAIN uses top/bottom, otherwise a metallic gradient */
  uint32_t top, bottom;
  float outline;          /* outline width in pixels */
  float soft;             /* outline softness (glow) in pixels */
  uint32_t outline_col;
  float shadow;           /* drop shadow offset in pixels, 0 = none */
  uint32_t shadow_col;
} UiText;

int ui_init(void);
void ui_begin(int win_w, int win_h);
void ui_end(void);
void ui_clip(float x, float y, float w, float h); /* w <= 0: no clipping */

void ui_box(float x, float y, float w, float h, const UiBox* b);
void ui_fill(float x, float y, float w, float h, uint32_t color);
void ui_fill_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void ui_fill_h(float x, float y, float w, float h, uint32_t left, uint32_t right);
enum { UI_DIR_LEFT, UI_DIR_RIGHT, UI_DIR_UP, UI_DIR_DOWN };
/* Filled triangle in the w x h box, pointing in direction dir. */
void ui_triangle(float cx, float cy, float w, float h, int dir, uint32_t color);

/* Text on a line centred at y (on the capital letters); returns the width. */
float ui_text(const UiText* t, float x, float y, int align, const char* s);
float ui_text_width(int font, float px, const char* s);

/* Copy the game's displayed frame (texture `tex`, w x h) into a blurred, downscaled snapshot. */
void ui_snapshot(unsigned tex, int w, int h);
/* Draw the snapshot over the rectangle, tinted. */
void ui_snapshot_draw(float x, float y, float w, float h, uint32_t tint);

uint32_t ui_alpha(uint32_t color, float a); /* scale a colour's alpha */
uint32_t ui_lerp_color(uint32_t a, uint32_t b, float t);

#endif
