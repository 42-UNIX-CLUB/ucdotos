// SPDX-License-Identifier: GPL-2.0-or-later
/* clipboard: a window on the clipboard history kept by clipd. Newest first. Up/Down to
 * choose, Enter or a click to make that entry the newest (Ctrl+Shift+V then pastes it),
 * Delete to remove it, Escape to close. It closes by itself when it loses the focus,
 * like the launcher. It talks only to clipd (libclip), never to the window manager. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"
#include "clip.h"

#define MAXE 32
#define ROWS 8
typedef struct { char size[16], text[96]; } entry;
static entry ents[MAXE];
static int nents, sel, top, failed;
static ui_window *win;
static ui_theme th;
static ttf_font font, bold;
static unsigned char *font_data, *bold_data;
static ttf_cache *reg, *bld;
static ttf_metrics met;
static int pad = 14, row_h, head_h;

/* Ask clipd for the history ("SIZE\tPREVIEW\n" per entry). */
static void load(void) {
  char *t;
  size_t n;
  nents = 0;
  failed = clip_list(&t, &n);
  if (failed) return;
  for (char *line = t, *next; *line && nents < MAXE; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0; else next = line + strlen(line);
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    unsigned long b = strtoul(line, NULL, 10);
    entry *e = &ents[nents++];
    if (b < 1024) snprintf(e->size, sizeof e->size, "%lu B", b);
    else if (b < 1024 * 1024) snprintf(e->size, sizeof e->size, "%lu KiB", (b + 512) / 1024);
    else snprintf(e->size, sizeof e->size, "%lu.%lu MiB", b >> 20, ((b & 0xfffff) * 10) >> 20);
    strlcpy(e->text, tab + 1, sizeof e->text);
  }
  free(t);
  if (sel >= nents) sel = nents ? nents - 1 : 0;
  if (top > sel) top = sel;
}

static void load_theme(void) {
  ui_theme_load(&th, NULL);
  if (reg) ttf_cache_free(reg);
  if (bld) ttf_cache_free(bld);
  free(font_data); free(bold_data);
  reg = bld = NULL;
  unsigned n;
  font_data = ui_read_file(th.font, &n);
  if (font_data && ttf_open(&font, font_data, n) == 0) { reg = ttf_cache_new(&font, th.font_size, 160); ttf_metrics_px(&font, th.font_size, &met); }
  bold_data = ui_read_file(th.font_bold, &n);
  if (bold_data && ttf_open(&bold, bold_data, n) == 0) bld = ttf_cache_new(&bold, th.font_size, 160);
  if (!bld && reg) bld = ttf_cache_new(&font, th.font_size, 160);
  row_h = (reg ? met.line_height : th.font_size) + 8;
  head_h = row_h + 10;
}

/* Text that fits in width w: the longest start of s that does. */
static int fit(ttf_cache *c, const char *s, int w) {
  int len = strlen(s);
  while (len > 0 && gfx_text_width(c, s, len) > w) {
    len--;
    while (len > 0 && (s[len] & 0xc0) == 0x80) len--; /* never half a character */
  }
  return len;
}

static void draw(void) {
  int w = ui_width(win), h = ui_height(win);
  gfx_surface s;
  gfx_init(&s, ui_pixels(win), w, h, w);
  gfx_clear(&s, th.panel);
  if (!reg) { ui_present(win, 0, 0, w, h); return; }
  int base = pad + (head_h + met.ascent - met.descent) / 2 - 4;
  int x = gfx_text(&s, bld, pad, base, "\xe2\x80\xba", -1, th.border_float) + met.advance;
  gfx_text(&s, bld, x, base, "clipboard", -1, th.fg);
  char cnt[32];
  snprintf(cnt, sizeof cnt, "%d copied", nents);
  gfx_text(&s, reg, w - pad - gfx_text_width(reg, cnt, -1), base, cnt, -1, th.dim);
  gfx_fill_rect(&s, pad, pad + head_h - 4, w - 2 * pad, 1, gfx_blend_px(th.panel, th.dim, 90));
  int y0 = pad + head_h + 4;
  if (!nents) {
    const char *msg = failed ? "the clipboard (clipd) is not answering" : "nothing copied yet: select text in a terminal, then Ctrl+Shift+C";
    gfx_text(&s, reg, pad + met.advance * 2, y0 + (row_h + met.ascent) / 2, msg, fit(reg, msg, w - 2 * pad - met.advance * 2), th.dim);
  }
  for (int r = 0; r < ROWS && top + r < nents; r++) {
    entry *e = &ents[top + r];
    int y = y0 + r * row_h, b = y + (row_h + met.ascent - met.descent) / 2;
    int tx = pad + met.advance * 2, sw = gfx_text_width(reg, e->size, -1), room = w - pad - sw - met.advance * 2 - tx;
    int n = fit(top + r == sel ? bld : reg, e->text, room);
    if (top + r == sel) {
      gfx_text(&s, bld, pad, b, "\xe2\x80\xba", -1, th.border_float);
      gfx_fill_rect(&s, tx - 6, y + 2, gfx_text_width(bld, e->text, n) + 12, row_h - 4, th.border_float);
      gfx_text(&s, bld, tx, b, e->text, n, th.panel);
    } else gfx_text(&s, reg, tx, b, e->text, n, th.fg);
    gfx_text(&s, reg, w - pad - sw, b, e->size, -1, th.dim);
  }
  ui_present(win, 0, 0, w, h);
}

static void keep_visible(void) {
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;
}

int main(void) {
  load_theme();
  load();
  int w = 640, h = 2 * pad + head_h + 4 + ROWS * row_h;
  win = ui_open("clipboard", "clipboard", w, h, UI_FLOATING);
  if (!win) { fprintf(stderr, "clipboard: no window manager (try the clip command)\n"); return 1; }
  ui_event ev;
  int had_focus = 0;
  while (ui_wait_event(win, &ev, -1) > 0) {
    switch (ev.type) {
    case UI_EV_RESIZE: load_theme(); draw(); break;
    case UI_EV_FOCUS: had_focus = 1; break;
    case UI_EV_BLUR: if (had_focus) goto out; break; /* clicked elsewhere: go away */
    case UI_EV_CLOSE: goto out;
    case UI_EV_KEY_DOWN:
      if (ev.key == 1) goto out;                                                                  /* Escape */
      if (ev.key == 28 || ev.key == 96) { if (nents) clip_select(sel); goto out; }                /* Enter */
      if (ev.key == 111 && nents) { clip_remove(sel); load(); }                                    /* Delete */
      else if (ev.key == 103 || (ev.key == 15 && (ev.mods & UI_SHIFT))) { if (sel > 0) sel--; }   /* Up, Shift+Tab */
      else if (ev.key == 108 || ev.key == 15) { if (sel + 1 < nents) sel++; }                     /* Down, Tab */
      else if (ev.key == 104) sel = sel > ROWS ? sel - ROWS : 0;                                   /* PageUp */
      else if (ev.key == 109) sel = sel + ROWS < nents ? sel + ROWS : (nents ? nents - 1 : 0);     /* PageDown */
      else break;
      keep_visible();
      draw();
      break;
    case UI_EV_WHEEL:
      top -= ev.wheel;
      if (top > nents - ROWS) top = nents - ROWS;
      if (top < 0) top = 0;
      draw();
      break;
    case UI_EV_MOUSE_DOWN: {
      int r = (ev.y - (pad + head_h + 4)) / row_h;
      if (ev.y >= pad + head_h + 4 && r >= 0 && r < ROWS && top + r < nents) { clip_select(top + r); goto out; }
      break;
    }
    }
  }
out:
  ui_close(win);
  return 0;
}
