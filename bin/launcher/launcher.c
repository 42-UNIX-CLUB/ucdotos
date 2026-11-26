// SPDX-License-Identifier: GPL-2.0-or-later
/* launcher: a rofi/dmenu-style popup. Type to filter, Up/Down to choose, Enter to run,
 * Escape to cancel. Entries come from the .app files in /usr/share/applications, then every other
 * program in /bin. The window manager starts the chosen command ("exec ..."), so it
 * owns and reaps the new process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"

#define MAXE 256
#define ROWS 8
typedef struct { char name[48], exec[96], comment[64]; } entry;
static entry ents[MAXE];
static int nents, shown[MAXE], nshown, sel, top;
static char query[64];
static ui_window *win;
static ui_theme th;
static ttf_font font, bold;
static unsigned char *font_data, *bold_data;
static ttf_cache *reg, *bld;
static ttf_metrics met;
static int pad = 14, row_h, prompt_h;

static int by_name(const void *a, const void *b) { return strcasecmp(((const entry *)a)->name, ((const entry *)b)->name); }

static void load_apps(void) {
  DIR *d = opendir("/usr/share/applications");
  for (struct dirent *e; d && (e = readdir(d)) && nents < MAXE;) {
    int l = strlen(e->d_name);
    if (l < 5 || strcmp(e->d_name + l - 4, ".app")) continue;
    char path[320];
    snprintf(path, sizeof path, "/usr/share/applications/%s", e->d_name);
    char *s = (char *)ui_read_file(path, NULL);
    if (!s) continue;
    entry *en = &ents[nents];
    memset(en, 0, sizeof *en);
    for (char *line = s, *next; line && *line; line = next) {
      next = strchr(line, '\n');
      if (next) *next++ = 0;
      if (!strncmp(line, "name=", 5)) strlcpy(en->name, line + 5, sizeof en->name);
      else if (!strncmp(line, "exec=", 5)) strlcpy(en->exec, line + 5, sizeof en->exec);
      else if (!strncmp(line, "comment=", 8)) strlcpy(en->comment, line + 8, sizeof en->comment);
    }
    free(s);
    if (en->name[0] && en->exec[0]) nents++;
  }
  if (d) closedir(d);
  qsort(ents, nents, sizeof ents[0], by_name);
  /* then every program in /bin that no .app file already names */
  int napps = nents;
  d = opendir("/bin");
  for (struct dirent *e; d && (e = readdir(d)) && nents < MAXE;) {
    if (e->d_name[0] == '.' || !strcmp(e->d_name, "launcher") || !strcmp(e->d_name, "init")) continue;
    int dup = 0;
    for (int i = 0; i < napps && !dup; i++) {
      int l = strcspn(ents[i].exec, " ");
      dup = (int)strlen(e->d_name) == l && !strncmp(ents[i].exec, e->d_name, l) && !strchr(ents[i].exec, ' ');
    }
    if (dup) continue;
    entry *en = &ents[nents++];
    strlcpy(en->name, e->d_name, sizeof en->name);
    strlcpy(en->exec, e->d_name, sizeof en->exec);
    snprintf(en->comment, sizeof en->comment, "/bin/%s", e->d_name);
  }
  if (d) closedir(d);
  qsort(ents + napps, nents - napps, sizeof ents[0], by_name);
}

static const char *find_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return hay;
  return n ? NULL : hay;
}
/* Matches whose name starts with the query come first, then names containing it, then commands containing it. */
static void filter(void) {
  nshown = 0;
  for (int pass = 0; pass < 3; pass++)
    for (int i = 0; i < nents; i++) {
      const char *m = find_ci(ents[i].name, query);
      int hit = pass == 0 ? m == ents[i].name : pass == 1 ? m && m != ents[i].name : !m && find_ci(ents[i].exec, query);
      if (hit) shown[nshown++] = i;
    }
  if (sel >= nshown) sel = nshown ? nshown - 1 : 0;
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;
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
  prompt_h = row_h + 10;
}

static void draw(void) {
  int w = ui_width(win), h = ui_height(win);
  gfx_surface s;
  gfx_init(&s, ui_pixels(win), w, h, w);
  gfx_clear(&s, th.panel);
  if (!reg) { ui_present(win, 0, 0, w, h); return; }
  int base = pad + (prompt_h + met.ascent - met.descent) / 2 - 4;
  /* prompt line: "› query" and a block cursor */
  int x = gfx_text(&s, bld, pad, base, "\xe2\x80\xba", -1, th.border_float) + met.advance;
  x = gfx_text(&s, reg, x, base, query, -1, th.fg);
  gfx_fill_rect(&s, x + 1, base - met.ascent + 2, met.advance - 2, met.ascent + met.descent - 2, th.fg);
  char cnt[16];
  snprintf(cnt, sizeof cnt, "%d/%d", nshown, nents);
  gfx_text(&s, reg, w - pad - gfx_text_width(reg, cnt, -1), base, cnt, -1, th.dim);
  gfx_fill_rect(&s, pad, pad + prompt_h - 4, w - 2 * pad, 1, gfx_blend_px(th.panel, th.dim, 90));
  /* the list */
  int y0 = pad + prompt_h + 4;
  for (int r = 0; r < ROWS && top + r < nshown; r++) {
    entry *e = &ents[shown[top + r]];
    int y = y0 + r * row_h, b = y + (row_h + met.ascent - met.descent) / 2;
    int tx = pad + met.advance * 2;
    if (top + r == sel) {
      int tw = gfx_text_width(bld, e->name, -1);
      gfx_text(&s, bld, pad, b, "\xe2\x80\xba", -1, th.border_float);
      gfx_fill_rect(&s, tx - 6, y + 2, tw + 12, row_h - 4, th.border_float);
      gfx_text(&s, bld, tx, b, e->name, -1, th.panel);
    } else gfx_text(&s, reg, tx, b, e->name, -1, th.fg);
    int cx = pad + met.advance * 20;
    if (e->comment[0] && cx < w - pad) {
      char c[64];
      strlcpy(c, e->comment, sizeof c);
      int len = strlen(c);
      while (len > 0 && cx + gfx_text_width(reg, c, len) > w - pad) len--;
      gfx_text(&s, reg, cx, b, c, len, th.dim);
    }
  }
  if (!nshown) gfx_text(&s, reg, pad + met.advance * 2, y0 + (row_h + met.ascent) / 2, "no match", -1, th.dim);
  ui_present(win, 0, 0, w, h);
}

static void launch(int i) {
  if (i < 0 || i >= nshown) return;
  char cmd[128];
  snprintf(cmd, sizeof cmd, "exec %s", ents[shown[i]].exec);
  ui_wm_command(win, cmd);
}

static void add_text(unsigned cp) {
  char u[4];
  int n = 0;
  if (cp < 32 || cp == 127) return;
  if (cp < 0x80) u[n++] = cp;
  else if (cp < 0x800) { u[n++] = 0xc0 | cp >> 6; u[n++] = 0x80 | (cp & 63); }
  else if (cp < 0x10000) { u[n++] = 0xe0 | cp >> 12; u[n++] = 0x80 | (cp >> 6 & 63); u[n++] = 0x80 | (cp & 63); }
  else { u[n++] = 0xf0 | cp >> 18; u[n++] = 0x80 | (cp >> 12 & 63); u[n++] = 0x80 | (cp >> 6 & 63); u[n++] = 0x80 | (cp & 63); }
  int l = strlen(query);
  if (l + n >= (int)sizeof query) return;
  memcpy(query + l, u, n);
  query[l + n] = 0;
  sel = top = 0;
  filter();
}

int main(void) {
  load_theme();
  load_apps();
  filter();
  int w = 600, h = 2 * pad + prompt_h + 4 + ROWS * row_h;
  win = ui_open("launcher", "launcher", w, h, UI_FLOATING);
  if (!win) { fprintf(stderr, "launcher: no window manager\n"); return 1; }
  ui_event ev;
  int had_focus = 0;
  while (ui_wait_event(win, &ev, -1) > 0) {
    switch (ev.type) {
    case UI_EV_RESIZE: load_theme(); draw(); break;
    case UI_EV_FOCUS: had_focus = 1; break;
    case UI_EV_BLUR: if (had_focus) goto out; break; /* clicked elsewhere: go away, like rofi */
    case UI_EV_CLOSE: goto out;
    case UI_EV_TEXT: add_text(ev.codepoint); draw(); break;
    case UI_EV_KEY_DOWN:
      if (ev.key == 1) goto out;                                  /* Escape */
      if (ev.key == 28 || ev.key == 96) { launch(sel); goto out; } /* Enter */
      if (ev.key == 103 || (ev.key == 15 && (ev.mods & UI_SHIFT))) { if (sel > 0) sel--; }          /* Up, Shift+Tab */
      else if (ev.key == 108 || ev.key == 15) { if (sel + 1 < nshown) sel++; }                     /* Down, Tab */
      else if (ev.key == 14) { int l = strlen(query); while (l > 0 && (query[l - 1] & 0xc0) == 0x80) l--; if (l > 0) l--; query[l] = 0; sel = top = 0; filter(); }
      else if (ev.key == 104) { sel = sel > ROWS ? sel - ROWS : 0; }                                 /* PageUp */
      else if (ev.key == 109) { sel = sel + ROWS < nshown ? sel + ROWS : (nshown ? nshown - 1 : 0); } /* PageDown */
      else break;
      if (sel < top) top = sel;
      if (sel >= top + ROWS) top = sel - ROWS + 1;
      draw();
      break;
    case UI_EV_WHEEL:
      top -= ev.wheel;
      if (top > nshown - ROWS) top = nshown - ROWS;
      if (top < 0) top = 0;
      draw();
      break;
    case UI_EV_MOUSE_DOWN: {
      int r = (ev.y - (pad + prompt_h + 4)) / row_h;
      if (ev.y >= pad + prompt_h + 4 && r >= 0 && r < ROWS && top + r < nshown) { launch(top + r); goto out; }
      break;
    }
    }
  }
out:
  ui_close(win);
  return 0;
}
