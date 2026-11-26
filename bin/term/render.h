// SPDX-License-Identifier: GPL-2.0-or-later
/* render.h - draw a vt screen model into a gfx_surface.
 * Needs only libgfx (ttf + gfx), so it is tested natively too. */
#ifndef RENDER_H
#define RENDER_H
#include "vt.h"
#include "gfx.h"
#include "ttf.h"

typedef struct { int x0, y0, x1, y1; } render_rect; /* x1, y1 exclusive; empty when x1 <= x0 */

typedef struct {
  const ttf_font *font[3];  /* regular, bold, fallback for missing glyphs (may be NULL) */
  ttf_cache *cache[3];
  float px;
  int cw, ch, ascent;       /* cell size and baseline offset */
  unsigned pal[256], fg, bg, cursor;
  int pad_x, pad_y;         /* top-left of the grid in the surface */
  int cols, rows;           /* grid drawn last time */
  vt_cell *shadow;          /* what is on screen, per cell (pad = cursor marker) */
  int full;                 /* next draw repaints everything, padding included */
  uint8_t *missing;         /* 2 bits per BMP code point: 0 unknown, 1 in the font, 2 missing */
  /* The selection, in window rows and columns (rows may lie outside the window), drawn in
   * inverse video: from (sel_x0, sel_y0) to (sel_x1, sel_y1) inclusive, in reading order.
   * Set sel_on = 0 for none. The front end sets t->all_dirty when it changes. */
  int sel_on, sel_x0, sel_y0, sel_x1, sel_y1;
  /* Set after render_init when the fallback font is wanted but not loaded yet: called once,
   * the first time a glyph is missing from the regular font. May return NULL. */
  const ttf_font *(*load_fallback)(void);
  int cursor_default;       /* DECSCUSR style used while the program has not set one (0 = block) */
} render;

int render_init(render *r, const ttf_font *reg, const ttf_font *bold, const ttf_font *fallback, float px);
void render_free(render *r);
/* fg, bg, cursor: libgfx colours; color16: the theme's 16 terminal colours */
void render_colors(render *r, unsigned fg, unsigned bg, unsigned cursor, const unsigned color16[16]);
/* Grid that fits a w x h pixel area with `pad` pixels of padding on every side. */
void render_grid_size(const render *r, int w, int h, int pad, int *cols, int *rows);
/* Place the grid (call after a resize); forces a full redraw. */
void render_layout(render *r, int pad_x, int pad_y);
void render_invalidate(render *r);
/* Draw what changed since the last call. Returns 1 and the changed area in *damage
 * if anything was drawn. focused = 0 draws a hollow cursor. */
int render_draw(render *r, vt *t, gfx_surface *s, int focused, render_rect *damage);
/* libgfx colour of a vt colour (fg_default/bg_default for VT_DEFAULT) */
unsigned render_color(const render *r, uint32_t c, unsigned def);

#endif
