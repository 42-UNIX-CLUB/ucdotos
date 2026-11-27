// SPDX-License-Identifier: GPL-2.0-or-later
/* themes: the theme picker. A strip of cards along the bottom of the screen, one per
 * /usr/share/themes/NAME.uconf, each drawn in that theme's own colours. Left and Right (or
 * h and l, Tab) preview a theme on the whole desktop as you move; Enter keeps it, Escape
 * goes back to the theme you started with, a click picks a card. It closes when it loses
 * the focus, keeping what is shown.
 *
 * Memory: every table is static and sized for MAXT themes; each theme file is read once,
 * at start. The picker keeps the fonts it started with while you preview, so moving through
 * themes allocates nothing here. Previews wait until the keys pause (PREVIEW_MS), so holding
 * an arrow down does not make every program on the desktop redraw for every repeat. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"

#define MAXT 64
#define VISIBLE 6       /* cards shown at once */
#define CARD_W 172
#define CARD_H 86
#define GAP 10
#define PREVIEW_MS 140

static char names[MAXT][32];
static ui_theme themes[MAXT];
static int n, sel, first, start_sel, shown_sel = -1;
static ui_window *win;
static ui_theme cur;          /* the picker's own colours: the theme active when it opened */
static ttf_font font, bold;
static unsigned char *font_data, *bold_data;
static ttf_cache *reg, *bld;
static ttf_metrics met;
static int pad = 14, head_h;

static int by_name(const void *a, const void *b) { return strcmp(a, b); }
static void list_themes(void) {
  DIR *d = opendir("/usr/share/themes");
  for (struct dirent *e; d && (e = readdir(d)) && n < MAXT;) {
    size_t l = strlen(e->d_name);
    if (l <= 6 || l >= sizeof names[0] + 6 || strcmp(e->d_name + l - 6, ".uconf")) continue;
    memcpy(names[n], e->d_name, l - 6);
    names[n][l - 6] = 0;
    n++;
  }
  if (d) closedir(d);
  qsort(names, (size_t)n, sizeof names[0], by_name);
  for (int i = 0; i < n; i++) ui_theme_load(&themes[i], names[i]);
}

static void load_fonts(void) {
  unsigned len;
  font_data = ui_read_file(cur.font, &len);
  if (font_data && ttf_open(&font, font_data, len) == 0) { reg = ttf_cache_new(&font, cur.font_size, 128); ttf_metrics_px(&font, cur.font_size, &met); }
  bold_data = ui_read_file(cur.font_bold, &len);
  if (bold_data && ttf_open(&bold, bold_data, len) == 0) bld = ttf_cache_new(&bold, cur.font_size, 96);
  if (!bld && reg) bld = ttf_cache_new(&font, cur.font_size, 96);
  head_h = (reg ? met.line_height : cur.font_size) + 14;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* The longest start of s that fits in w pixels (never half a character). */
static int fit(ttf_cache *c, const char *s, int w) {
  int len = (int)strlen(s);
  while (len > 0 && gfx_text_width(c, s, len) > w) {
    len--;
    while (len > 0 && (s[len] & 0xc0) == 0x80) len--;
  }
  return len;
}

static void draw_card(gfx_surface *s, int i, int x, int y) {
  const ui_theme *t = &themes[i];
  int on = i == sel;
  if (on) gfx_round_rect_fill(s, x - 3, y - 3, CARD_W + 6, CARD_H + 6, 10, cur.accent);
  gfx_round_rect_fill(s, x, y, CARD_W, CARD_H, 8, t->bg);
  /* a little window: title strip in the theme's bar colours, then its focus border */
  gfx_round_rect_fill(s, x + 8, y + 8, CARD_W - 16, 14, 4, t->bar_bg);
  gfx_fill_rect(s, x + 12, y + 13, 8, 4, t->accent);
  gfx_round_rect_stroke(s, x + 8, y + 26, CARD_W - 16, 22, 4, 2, t->border_focus);
  /* the name, in the theme's text colour */
  if (bld) gfx_text(s, bld, x + 10, y + 26 + 16, names[i], fit(bld, names[i], CARD_W - 24), t->fg);
  /* the eight bright terminal colours */
  int sw = (CARD_W - 16) / 8;
  for (int k = 0; k < 8; k++) gfx_fill_rect(s, x + 8 + k * sw, y + CARD_H - 26, sw, 16, t->color[8 + k]);
}

static void draw(void) {
  int w = ui_width(win), h = ui_height(win);
  gfx_surface s;
  gfx_init(&s, ui_pixels(win), w, h, w);
  gfx_clear(&s, cur.panel);
  if (!reg) { ui_present(win, 0, 0, w, h); return; }
  int base = pad + (head_h + met.ascent - met.descent) / 2 - 6;
  int x = gfx_text(&s, bld, pad, base, "\xe2\x80\xba", -1, cur.border_float) + met.advance;
  x = gfx_text(&s, bld, x, base, "themes", -1, cur.fg) + met.advance * 2;
  gfx_text(&s, reg, x, base, "\xe2\x86\x90 \xe2\x86\x92 preview   Enter keep   Esc back", -1, cur.dim);
  char cnt[16];
  snprintf(cnt, sizeof cnt, "%d/%d", n ? sel + 1 : 0, n);
  gfx_text(&s, reg, w - pad - gfx_text_width(reg, cnt, -1), base, cnt, -1, cur.dim);
  int y = pad + head_h;
  for (int k = 0; k < VISIBLE && first + k < n; k++) draw_card(&s, first + k, pad + 3 + k * (CARD_W + GAP), y + 3);
  if (!n) gfx_text(&s, reg, pad, y + CARD_H / 2, "no themes in /usr/share/themes", -1, cur.dim);
  ui_present(win, 0, 0, w, h);
}

/* Ask the window manager to switch the whole desktop to theme i. */
static void apply(int i) {
  if (i < 0 || i >= n || i == shown_sel) return;
  char cmd[48];
  snprintf(cmd, sizeof cmd, "theme %s", names[i]);
  ui_wm_command(win, cmd);
  shown_sel = i;
}
static void select_card(int i) {
  if (!n) return;
  sel = (i % n + n) % n;
  if (sel < first) first = sel;
  if (sel >= first + VISIBLE) first = sel - VISIBLE + 1;
}

int main(void) {
  ui_theme_load(&cur, NULL);
  list_themes();
  for (int i = 0; i < n; i++) if (!strcmp(names[i], cur.name)) sel = start_sel = shown_sel = i;
  select_card(sel);
  load_fonts();
  int vis = n < VISIBLE ? (n ? n : 1) : VISIBLE;
  int w = 2 * pad + 6 + vis * (CARD_W + GAP) - GAP, h = 2 * pad + head_h + CARD_H + 6;
  if (w < 420) w = 420;
  win = ui_open("themes", "themes", w, h, UI_FLOATING_BOTTOM);
  if (!win) { fprintf(stderr, "themes: no window manager\n"); return 1; }
  ui_event ev;
  int had_focus = 0;
  double pending = 0; /* when the last move happened; 0 = nothing to preview */
  for (;;) {
    int wait = -1;
    if (pending) { double left = pending + PREVIEW_MS - now_ms(); wait = left > 0 ? (int)left + 1 : 0; }
    int r = ui_wait_event(win, &ev, wait);
    if (r < 0) break;
    if (r == 0) { if (pending) { apply(sel); pending = 0; } continue; }
    switch (ev.type) {
    case UI_EV_RESIZE: { /* also a theme change: take its colours, keep the fonts already loaded */
      ui_theme t;
      ui_theme_load(&t, NULL);
      memcpy(t.font, cur.font, sizeof t.font);
      memcpy(t.font_bold, cur.font_bold, sizeof t.font_bold);
      t.font_size = cur.font_size;
      cur = t;
      draw();
      break;
    }
    case UI_EV_FOCUS: had_focus = 1; break;
    case UI_EV_BLUR: if (had_focus) goto out; break; /* clicked elsewhere: keep what is shown */
    case UI_EV_CLOSE: goto out;
    case UI_EV_KEY_DOWN: {
      int k = ev.key;
      if (k == 1) { apply(start_sel); goto out; }                   /* Escape: back */
      if (k == 28 || k == 96) { apply(sel); goto out; }              /* Enter: keep */
      if (k == 105 || k == 35 || (k == 15 && (ev.mods & UI_SHIFT))) select_card(sel - 1); /* Left, h, Shift+Tab */
      else if (k == 106 || k == 38 || k == 15) select_card(sel + 1);                      /* Right, l, Tab */
      else if (k == 102) select_card(0);                                                  /* Home */
      else if (k == 107) select_card(n - 1);                                              /* End */
      else break;
      pending = now_ms();
      draw();
      break;
    }
    case UI_EV_WHEEL: select_card(sel - ev.wheel); pending = now_ms(); draw(); break;
    case UI_EV_MOUSE_DOWN: {
      int y = pad + head_h + 3, k = (ev.x - pad - 3) / (CARD_W + GAP);
      if (ev.y >= y && ev.y < y + CARD_H && ev.x >= pad && k >= 0 && k < VISIBLE && first + k < n) { sel = first + k; apply(sel); goto out; }
      break;
    }
    }
  }
out:
  ui_close(win);
  return 0;
}
