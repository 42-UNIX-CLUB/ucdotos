// SPDX-License-Identifier: GPL-2.0-or-later
/* keys: the keybinding cheat sheet. Two small windows, both app_id "keys":
 *   - the sheet, in the top-left corner: every `bind` line of ~/.config/wm/wm.uconf, shown
 *     as it is written there ($mod SHIFT, q    kill)
 *   - the hint, at the bottom: the key that toggles them ($mod, slash)
 * The window manager's `toggle keys` command (bound to $mod / by default) hides and shows
 * both together. Neither takes the keyboard focus, and both stay on every workspace. A click
 * on either hides them. Both re-read wm.uconf and the theme whenever the window manager asks
 * them to redraw: on a reload, a theme change, and each time they are shown again.
 *
 * Memory: the bindings live in static tables (MAXB lines); fonts and the parsed file are
 * freed before they are read again, so a reload reuses the same memory. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"
#include "uconf.h"

#define MAXB 128
#define ROWS 22         /* rows per column of the sheet */
#define FLAGS (UI_STICKY | UI_NOFOCUS)

typedef struct { char keys[48], action[96], match[48]; } bind;
static bind b[MAXB];
static int nb;
static char modval[24], toggle_keys[48], where[64];

static ui_window *sheet, *hint;
static ui_theme th;
static ttf_font font, bold;
static unsigned char *font_data, *bold_data;
static ttf_cache *reg, *bld;
static ttf_metrics met;
static int pad = 12, row_h, key_w, act_w, cols;

/* "ALT SHIFT , Q" -> "alt shift,q": what makes two lines bind the same keys */
static void normalize(const char *mods, const char *key, char *out, size_t cap) {
  size_t n = 0;
  for (const char *s = mods; *s && n + 1 < cap; s++) {
    char ch = *s == '_' || *s == '+' || *s == '\t' ? ' ' : *s;
    if (ch == ' ' && (!n || out[n - 1] == ' ')) continue;
    out[n++] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
  }
  while (n && out[n - 1] == ' ') n--;
  if (n + 1 < cap) out[n++] = ',';
  for (const char *s = key; *s && n + 1 < cap; s++) out[n++] = (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
  out[n] = 0;
}

/* Read the bind lines of wm.uconf, in order: a line for keys already bound replaces the
 * earlier one, and unbind removes it, as in the window manager. */
static void load_binds(void) {
  const char *home = getenv("HOME");
  snprintf(where, sizeof where, "%s/.config/wm/wm.uconf", home && *home ? home : "/home/user");
  nb = 0;
  modval[0] = toggle_keys[0] = 0;
  uconf c;
  uconf_load(&c, where);
  const char *m = uconf_var(&c, "mod");
  snprintf(modval, sizeof modval, "%s", m ? m : "ALT");
  for (int i = 0; i < c.n; i++) {
    int unbind = !strcmp(c.e[i].key, "binds.unbind");
    if (!unbind && strcmp(c.e[i].key, "binds.bind")) continue;
    char val[160], raw[160], *f[3], *r[3];
    snprintf(val, sizeof val, "%s", c.e[i].value);
    snprintf(raw, sizeof raw, "%s", c.e[i].raw);
    int nf = uconf_split(val, f, 3), nr = uconf_split(raw, r, 3);
    if (nf < 2 || nr < 2 || (!unbind && (nf < 3 || nr < 3))) continue; /* the window manager reports these */
    char match[48];
    normalize(f[0], f[1], match, sizeof match);
    for (int k = 0; k < nb; k++)
      if (!strcmp(b[k].match, match)) { memmove(b + k, b + k + 1, (size_t)(nb - k - 1) * sizeof b[0]); nb--; k--; }
    if (unbind || nb == MAXB) continue;
    bind *e = &b[nb++];
    snprintf(e->keys, sizeof e->keys, "%s, %s", r[0], r[1]); /* as written: "$mod SHIFT, q" or ", F1" */
    snprintf(e->action, sizeof e->action, "%s", r[2]);
    snprintf(e->match, sizeof e->match, "%s", match);
    if (!strncasecmp(f[2], "toggle", 6) && strstr(f[2], "keys")) snprintf(toggle_keys, sizeof toggle_keys, "%s", e->keys);
  }
  uconf_free(&c);
  static int said = -1;
  if (nb != said) fprintf(stderr, "keys: %d bindings from %s\n", nb, where); /* only when the count changes */
  said = nb;
}

static void load_theme(void) {
  ui_theme old = th;
  ui_theme_load(&th, NULL);
  if (reg && !strcmp(old.font, th.font) && !strcmp(old.font_bold, th.font_bold) && old.font_size == th.font_size)
    return; /* same fonts: only the colours changed, and they are already in th */
  if (reg) ttf_cache_free(reg);
  if (bld) ttf_cache_free(bld);
  free(font_data); free(bold_data); /* first: memory never shrinks */
  reg = bld = NULL;
  font_data = bold_data = NULL;
  int px = th.font_size > 7 ? th.font_size - 1 : th.font_size;
  unsigned len;
  if ((font_data = ui_read_file(th.font, &len)) && ttf_open(&font, font_data, len) == 0) {
    reg = ttf_cache_new(&font, px, 128);
    ttf_metrics_px(&font, px, &met);
  }
  if ((bold_data = ui_read_file(th.font_bold, &len)) && ttf_open(&bold, bold_data, len) == 0) bld = ttf_cache_new(&bold, px, 96);
  if (!bld && reg) bld = ttf_cache_new(&font, px, 96);
  row_h = (reg ? met.line_height : px) + 4;
}

/* The sizes the two windows need for what there is to show now. */
static void measure(int *sw, int *sh, int *hw, int *hh) {
  key_w = act_w = 0;
  for (int i = 0; reg && i < nb; i++) {
    int k = gfx_text_width(bld, b[i].keys, -1), a = gfx_text_width(reg, b[i].action, -1);
    if (k > key_w) key_w = k;
    if (a > act_w) act_w = a;
  }
  if (act_w > 260) act_w = 260; /* long exec lines are cut to fit */
  cols = nb ? (nb + ROWS - 1) / ROWS : 1;
  int rows = nb < ROWS ? (nb ? nb : 1) : ROWS;
  int cw = key_w + 18 + act_w;
  *sw = 2 * pad + cols * cw + (cols - 1) * 28;
  if (*sw < 300) *sw = 300;
  *sh = 2 * pad + row_h * (rows + 3);
  char line[128];
  snprintf(line, sizeof line, "%s    keybindings", toggle_keys[0] ? toggle_keys : "no key toggles this");
  *hw = 2 * pad + (reg ? gfx_text_width(bld, line, -1) : 200) + 8;
  *hh = row_h + 2 * 8;
}

static int fit(ttf_cache *c, const char *s, int w) {
  int len = (int)strlen(s);
  while (len > 0 && gfx_text_width(c, s, len) > w) {
    len--;
    while (len > 0 && (s[len] & 0xc0) == 0x80) len--;
  }
  return len;
}

static void draw_sheet(void) {
  if (!sheet) return;
  int w = ui_width(sheet), h = ui_height(sheet);
  gfx_surface s;
  gfx_init(&s, ui_pixels(sheet), w, h, w);
  gfx_clear(&s, th.panel);
  if (!reg) { ui_present(sheet, 0, 0, w, h); return; }
  int base = pad + met.ascent;
  int x = gfx_text(&s, bld, pad, base, "keybindings", -1, th.fg) + met.advance * 2;
  char mod[48];
  snprintf(mod, sizeof mod, "$mod = %s", modval);
  gfx_text(&s, reg, x, base, mod, -1, th.dim);
  gfx_fill_rect(&s, pad, pad + row_h + 2, w - 2 * pad, 1, gfx_blend_px(th.panel, th.dim, 90));
  int y0 = pad + row_h + 8, cw = key_w + 18 + act_w;
  for (int i = 0; i < nb; i++) {
    int col = i / ROWS, row = i % ROWS;
    int cx = pad + col * (cw + 28), y = y0 + row * row_h + met.ascent;
    gfx_text(&s, bld, cx, y, b[i].keys, -1, th.accent);
    gfx_text(&s, reg, cx + key_w + 18, y, b[i].action, fit(reg, b[i].action, act_w), th.fg);
  }
  if (!nb) gfx_text(&s, reg, pad, y0 + met.ascent, "no bind lines in wm.uconf: the built-in ones apply", -1, th.dim);
  gfx_text(&s, reg, pad, h - pad - met.descent, where, fit(reg, where, w - 2 * pad), th.dim);
  ui_present(sheet, 0, 0, w, h);
}
static void draw_hint(void) {
  if (!hint) return;
  int w = ui_width(hint), h = ui_height(hint);
  gfx_surface s;
  gfx_init(&s, ui_pixels(hint), w, h, w);
  gfx_clear(&s, th.panel);
  if (reg) {
    int base = (h + met.ascent - met.descent) / 2;
    int x = gfx_text(&s, bld, pad, base, toggle_keys[0] ? toggle_keys : "no key toggles this", -1, th.accent);
    gfx_text(&s, reg, x + met.advance * 4, base, "keybindings", -1, th.fg);
  }
  ui_present(hint, 0, 0, w, h);
}

/* (Re)open the windows when they are missing or what they show needs another size. The size
 * compared is the one last asked for, not the one given: the window manager shrinks a window
 * that does not fit the screen, and reopening it at the same request would loop forever. */
static int asked_sw, asked_sh, asked_hw, asked_hh;
static void open_windows(void) {
  int sw, sh, hw, hh;
  measure(&sw, &sh, &hw, &hh);
  if (sheet && (sw != asked_sw || sh != asked_sh)) { ui_close(sheet); sheet = NULL; }
  if (hint && (hw != asked_hw || hh != asked_hh)) { ui_close(hint); hint = NULL; }
  if (!sheet) { sheet = ui_open("keybindings", "keys", sw, sh, UI_FLOATING_TOPLEFT | FLAGS); asked_sw = sw; asked_sh = sh; }
  if (!hint) { hint = ui_open("keybindings hint", "keys", hw, hh, UI_FLOATING_BOTTOM | FLAGS); asked_hw = hw; asked_hh = hh; }
}

static void refresh(void) {
  load_binds();
  load_theme();
  open_windows();
  draw_sheet();
  draw_hint();
}

static int handle(ui_window *w, int *again) {
  ui_event ev;
  int r;
  while ((r = ui_poll_event(w, &ev)) > 0) {
    if (ev.type == UI_EV_RESIZE) *again = 1;
    else if (ev.type == UI_EV_CLOSE) return -1;
    else if (ev.type == UI_EV_MOUSE_DOWN) ui_wm_command(w, "toggle keys");
  }
  return r < 0 ? -1 : 0;
}

int main(void) {
  load_binds();
  load_theme();
  open_windows();
  if (!sheet || !hint) { fprintf(stderr, "keys: no window manager\n"); return 1; }
  for (;;) {
    /* events first: libui may already hold some (the first RESIZE waits in its queue) */
    int again = 0;
    if (handle(sheet, &again) < 0 || handle(hint, &again) < 0) break;
    if (again) refresh(); /* a reload, a theme change, or shown again: read everything anew */
    if (!sheet || !hint) break;
    if (again) continue;  /* refresh may have reopened a window: its first events are queued */
    struct pollfd pf[2] = {{ui_fd(sheet), POLLIN, 0}, {ui_fd(hint), POLLIN, 0}};
    poll(pf, 2, -1);
  }
  if (sheet) ui_close(sheet);
  if (hint) ui_close(hint);
  return 0;
}
