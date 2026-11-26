// SPDX-License-Identifier: GPL-2.0-or-later
/* render.c - draw a vt screen model into a gfx_surface. See render.h.
 * Only cells that differ from what was drawn last time are repainted. */
#include "render.h"
#include <stdlib.h>
#include <string.h>

int render_init(render *r, const ttf_font *reg, const ttf_font *bold, const ttf_font *fallback, float px) {
  for (int i = 0; i < 3; i++) if (r->cache[i]) ttf_cache_free(r->cache[i]);
  free(r->shadow);
  uint8_t *missing = r->missing;
  memset(r, 0, sizeof *r);
  r->missing = missing ? memset(missing, 0, 0x4000) : calloc(1, 0x4000);
  r->font[0] = reg; r->font[1] = bold ? bold : reg; r->font[2] = fallback;
  r->px = px;
  for (int i = 0; i < 3; i++) if (r->font[i]) r->cache[i] = ttf_cache_new(r->font[i], px, i == 2 ? 256 : 1024);
  if (!r->cache[0] || !r->cache[1] || !r->missing) return -1;
  ttf_metrics m;
  ttf_metrics_px(reg, px, &m);
  r->cw = m.advance > 0 ? m.advance : 1;
  r->ch = m.ascent + m.descent > 0 ? m.ascent + m.descent : 1; /* no line gap: cell glyphs must touch */
  r->ascent = m.ascent;
  r->full = 1;
  return 0;
}

void render_free(render *r) {
  for (int i = 0; i < 3; i++) if (r->cache[i]) ttf_cache_free(r->cache[i]);
  free(r->shadow); free(r->missing);
  memset(r, 0, sizeof *r);
}

void render_colors(render *r, unsigned fg, unsigned bg, unsigned cursor, const unsigned color16[16]) {
  static const int lv[6] = {0, 95, 135, 175, 215, 255};
  r->fg = fg; r->bg = bg; r->cursor = cursor;
  for (int i = 0; i < 16; i++) r->pal[i] = color16[i] | 0xff000000u;
  for (int i = 0; i < 216; i++) r->pal[16 + i] = GFX_RGB(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
  for (int i = 0; i < 24; i++) r->pal[232 + i] = GFX_RGB(8 + 10 * i, 8 + 10 * i, 8 + 10 * i);
  r->full = 1;
}

void render_grid_size(const render *r, int w, int h, int pad, int *cols, int *rows) {
  *cols = (w - 2 * pad) / r->cw;
  *rows = (h - 2 * pad) / r->ch;
  if (*cols < 1) *cols = 1;
  if (*rows < 1) *rows = 1;
}
void render_layout(render *r, int pad_x, int pad_y) { r->pad_x = pad_x; r->pad_y = pad_y; r->full = 1; }
void render_invalidate(render *r) { r->full = 1; }

unsigned render_color(const render *r, uint32_t c, unsigned def) {
  if (VT_IS_PAL(c)) return r->pal[c & 255];
  if (VT_IS_RGB(c)) return GFX_RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
  return def;
}

static unsigned mix(unsigned a, unsigned b, unsigned w) { return gfx_blend_px(b, a, w); } /* w/255 of a over b */

/* U+2580..259F drawn as exact rectangles so that neighbouring cells never show seams */
static int draw_block(gfx_surface *s, int x, int y, int w, int h, uint32_t c, unsigned fg, unsigned bg) {
  static const uint8_t quad[10] = {4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8};
  if (c < 0x2580 || c > 0x259f) return 0;
  int hw = w / 2, hh = h / 2;
  if (c == 0x2580) gfx_fill_rect(s, x, y, w, hh, fg);
  else if (c <= 0x2588) { int n = (h * (int)(c - 0x2580) + 4) / 8; gfx_fill_rect(s, x, y + h - n, w, n, fg); }
  else if (c <= 0x258f) { int n = (w * (int)(0x2590 - c) + 4) / 8; gfx_fill_rect(s, x, y, n, h, fg); }
  else if (c == 0x2590) gfx_fill_rect(s, x + hw, y, w - hw, h, fg);
  else if (c <= 0x2593) gfx_fill_rect(s, x, y, w, h, mix(fg, bg, 64 * (c - 0x2590)));
  else if (c == 0x2594) gfx_fill_rect(s, x, y, w, (h + 4) / 8, fg);
  else if (c == 0x2595) { int n = (w + 4) / 8; gfx_fill_rect(s, x + w - n, y, n, h, fg); }
  else {
    int q = quad[c - 0x2596];
    if (q & 1) gfx_fill_rect(s, x, y, hw, hh, fg);
    if (q & 2) gfx_fill_rect(s, x + hw, y, w - hw, hh, fg);
    if (q & 4) gfx_fill_rect(s, x, y + hh, hw, h - hh, fg);
    if (q & 8) gfx_fill_rect(s, x + hw, y + hh, w - hw, h - hh, fg);
  }
  return 1;
}

/* which cache draws code point c: the regular/bold one, or the fallback if the glyph is missing */
static void load_fallback(render *r) {
  const ttf_font *(*load)(void) = r->load_fallback;
  r->load_fallback = NULL; /* only ever once */
  const ttf_font *f = load();
  if (f && (r->cache[2] = ttf_cache_new(f, r->px, 256))) r->font[2] = f;
}
static ttf_cache *pick(render *r, uint32_t c, int bold) {
  ttf_cache *main = r->cache[bold ? 1 : 0];
  if (c < 0x80 || (!r->cache[2] && !r->load_fallback)) return main;
  int st = 0;
  if (c < 0x10000) st = (r->missing[c >> 2] >> ((c & 3) * 2)) & 3;
  if (!st) {
    if (ttf_glyph_index(r->font[0], c)) st = 1;
    else {
      if (r->load_fallback) load_fallback(r);
      st = r->font[2] && ttf_glyph_index(r->font[2], c) ? 2 : 1;
    }
    if (c < 0x10000) r->missing[c >> 2] |= (uint8_t)(st << ((c & 3) * 2));
  }
  return st == 2 ? r->cache[2] : main;
}

/* Marks stored in the shadow cell's pad: the low byte is the cursor (0 none, else
 * 1 + focused * 8 + style), PAD_SEL a selected cell. */
#define PAD_SEL 0x100
static int selected(const render *r, int x, int y) {
  if (!r->sel_on || y < r->sel_y0 || y > r->sel_y1) return 0;
  if (y == r->sel_y0 && x < r->sel_x0) return 0;
  if (y == r->sel_y1 && x > r->sel_x1) return 0;
  return 1;
}
static void draw_cell(render *r, gfx_surface *s, int px, int py, vt_cell c, int cur) {
  unsigned fg = render_color(r, c.fg, r->fg), bg = render_color(r, c.bg, r->bg);
  int w = r->cw, h = r->ch;
  if (!(c.attr & VT_INVERSE) != !(c.pad & PAD_SEL)) { unsigned t = fg; fg = bg; bg = t; }
  if (c.attr & VT_DIM) fg = mix(fg, bg, 150);
  if (c.attr & VT_HIDDEN) fg = bg;
  int style = cur ? (cur - 1) & 7 : 0, focused = cur && ((cur - 1) & 8);
  int block = cur && focused && style <= 2;
  if (block) { bg = r->cursor; fg = r->bg; }
  gfx_set_clip(s, px, py, w, h);
  gfx_fill_rect(s, px, py, w, h, bg);
  if (c.ch != ' ' && !draw_block(s, px, py, w, h, c.ch, fg, bg)) {
    ttf_cache *k = pick(r, c.ch, c.attr & VT_BOLD);
    gfx_glyph(s, px, py + r->ascent, ttf_cache_get(k, c.ch), fg);
  }
  int lt = h >= 28 ? 2 : 1; /* line thickness */
  if (c.attr & VT_UNDERLINE) gfx_fill_rect(s, px, py + r->ascent + (h - r->ascent) / 2 - lt / 2, w, lt, fg);
  if (c.attr & VT_STRIKE) gfx_fill_rect(s, px, py + r->ascent - r->ascent * 3 / 10 - lt / 2, w, lt, fg);
  if (cur && !block) {
    int bw = h >= 28 ? 3 : 2;
    if (!focused) gfx_round_rect_stroke(s, px, py, w, h, 0, 1, r->cursor);
    else if (style <= 4) gfx_fill_rect(s, px, py + h - bw, w, bw, r->cursor); /* 3, 4: underline */
    else gfx_fill_rect(s, px, py, bw, h, r->cursor);                          /* 5, 6: bar */
  }
  gfx_reset_clip(s);
}

int render_draw(render *r, vt *t, gfx_surface *s, int focused, render_rect *dmg) {
  int cols = t->cols, rows = t->rows;
  dmg->x0 = dmg->y0 = 1 << 30; dmg->x1 = dmg->y1 = -1;
  if (!r->shadow || r->cols != cols || r->rows != rows) {
    free(r->shadow);
    r->shadow = malloc(sizeof(vt_cell) * (size_t)cols * (size_t)rows);
    if (!r->shadow) return 0;
    r->cols = cols; r->rows = rows; r->full = 1;
  }
  int full = r->full;
  if (full) {
    gfx_fill_rect(s, 0, 0, s->w, s->h, r->bg);
    *dmg = (render_rect){0, 0, s->w, s->h};
  }
  int cx = -1, cy = -1, cmark = 0;
  if (vt_cursor_in_view(t)) {
    cx = t->x; cy = t->y;
    int st = t->cursor_style;
    cmark = 1 + (focused ? 8 : 0) + (st >= 0 && st <= 6 ? st : 0);
  }
  vt_cell blank = {' ', VT_DEFAULT, VT_DEFAULT, 0, 0};
  for (int y = 0; y < rows; y++) {
    vt_cell *sh = r->shadow + (size_t)y * (size_t)cols;
    int row_has_cursor = y == cy;
    if (!full && !t->all_dirty && !t->dirty[y] && !row_has_cursor) {
      /* the cursor may have left this row: check only that cell */
      int k;
      for (k = 0; k < cols; k++) if (sh[k].pad) break;
      if (k == cols) continue;
    }
    t->dirty[y] = 0;
    int w;
    const vt_cell *src = vt_view_row(t, y, &w);
    int py = r->pad_y + y * r->ch;
    for (int x = 0; x < cols; x++) {
      vt_cell c = (src && x < w) ? src[x] : blank;
      c.pad = (uint16_t)(((y == cy && x == cx) ? cmark : 0) | (selected(r, x, y) ? PAD_SEL : 0));
      if (!full && !memcmp(&c, &sh[x], sizeof c)) continue;
      sh[x] = c;
      int px = r->pad_x + x * r->cw;
      draw_cell(r, s, px, py, c, c.pad & 0xff);
      if (px < dmg->x0) dmg->x0 = px;
      if (py < dmg->y0) dmg->y0 = py;
      if (px + r->cw > dmg->x1) dmg->x1 = px + r->cw;
      if (py + r->ch > dmg->y1) dmg->y1 = py + r->ch;
    }
  }
  t->all_dirty = 0;
  r->full = 0;
  if (dmg->x1 <= dmg->x0) { dmg->x0 = dmg->y0 = dmg->x1 = dmg->y1 = 0; return 0; }
  if (full) *dmg = (render_rect){0, 0, s->w, s->h};
  return 1;
}
