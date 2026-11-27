// SPDX-License-Identifier: GPL-2.0-or-later
/* Theme loader: ~/.config/ucos/theme names the theme, /usr/share/themes/NAME.uconf describes it. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "theme.h"
#include "gfx.h"
#include "uconf.h"
#include <stddef.h>
#include <stdio.h>

unsigned char *ui_read_file(const char *path, unsigned *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < 0 || st.st_size > (64 << 20)) { close(fd); return NULL; }
  unsigned n = (unsigned)st.st_size, got = 0;
  unsigned char *buf = malloc(n + 1);
  if (!buf) { close(fd); return NULL; }
  while (got < n) {
    long r = read(fd, buf + got, n - got);
    if (r <= 0) break;
    got += r;
  }
  close(fd);
  buf[got] = 0; /* text files can be used as C strings */
  if (len) *len = got;
  return buf;
}

/* Built-in defaults: the "deck" theme (Gruvbox, as in the club's slides). */
static const unsigned deck16[16] = {0x282828, 0xcc241d, 0x98971a, 0xd79921, 0x458588, 0xb16286, 0x689d6a, 0xa89984,
                                    0x928374, 0xfb4934, 0xb8bb26, 0xfabd2f, 0x83a598, 0xd3869b, 0x8ec07c, 0xebdbb2};
static void defaults(ui_theme *t) {
  memset(t, 0, sizeof *t);
  strcpy(t->name, "deck");
  t->bg = GFX_HEX(0x282828); t->fg = GFX_HEX(0xebdbb2); t->accent = GFX_HEX(0xfe8019);
  t->border_focus = GFX_HEX(0xfe8019); t->border_normal = GFX_HEX(0x665c54);
  t->bar_bg = GFX_HEX(0x3c3836); t->bar_fg = GFX_HEX(0xebdbb2); t->dim = GFX_HEX(0x928374);
  t->desktop = GFX_HEX(0x1d2021); t->border_float = GFX_HEX(0xfabd2f); t->panel = GFX_HEX(0x3c3836);
  for (int i = 0; i < 16; i++) t->color[i] = GFX_HEX(deck16[i]);
  strcpy(t->font, "/usr/share/fonts/FiraCode-Regular.ttf");
  strcpy(t->font_bold, "/usr/share/fonts/FiraCode-Bold.ttf");
  t->font_size = 15; t->gap = 12; t->border = 2; t->radius = 8;
}

static void parse_font(const char *v, char *out) {
  if (*v == '/') strlcpy(out, v, 128);
  else { strcpy(out, "/usr/share/fonts/"); strncat(out, v, 127 - strlen(out)); }
}

/* The theme name: first word of ~/.config/ucos/theme; only [A-Za-z0-9_-] so it cannot escape the directory. */
static void active_name(char *name, int cap) {
  const char *home = getenv("HOME");
  char path[256];
  strlcpy(path, home && *home ? home : "/home/user", sizeof path);
  strncat(path, "/.config/ucos/theme", sizeof path - strlen(path) - 1);
  unsigned char *s = ui_read_file(path, NULL);
  int n = 0;
  if (s) {
    const unsigned char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    while (n < cap - 1 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
      name[n++] = *p++;
    free(s);
  }
  name[n] = 0;
  if (!n) strlcpy(name, "deck", cap);
}

/* A theme is /usr/share/themes/NAME.uconf (the format: libuconf/uconf.h):
 *   [colors]    bg fg accent dim desktop panel, bar { bg fg }, border { focus normal float }
 *   [terminal]  black red green yellow blue magenta cyan white, bright { the same eight }
 *   [font]      regular bold (a path, or a file in /usr/share/fonts) size
 *   [geometry]  gap border radius */
static const char *const ansi[8] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
static void color(uconf *c, const char *key, unsigned *dst) {
  unsigned def = ((*dst & 0xff) << 16) | (*dst & 0xff00) | ((*dst >> 16) & 0xff); /* back to 0xRRGGBB */
  *dst = GFX_HEX(uconf_color(c, key, def));
}
int ui_theme_load(ui_theme *t, const char *name) {
  char nm[32], path[96];
  defaults(t);
  if (name && *name && !strchr(name, '/')) strlcpy(nm, name, sizeof nm);
  else active_name(nm, sizeof nm);
  strlcpy(t->name, nm, sizeof t->name);
  strcpy(path, "/usr/share/themes/");
  strncat(path, nm, sizeof path - strlen(path) - 7);
  strcat(path, ".uconf");
  uconf c;
  if (uconf_load(&c, path) < 0) { uconf_free(&c); return -1; }
  static const struct { const char *key; size_t off; } colors[] = {
    {"colors.bg", offsetof(ui_theme, bg)}, {"colors.fg", offsetof(ui_theme, fg)}, {"colors.accent", offsetof(ui_theme, accent)},
    {"colors.dim", offsetof(ui_theme, dim)}, {"colors.bar.bg", offsetof(ui_theme, bar_bg)}, {"colors.bar.fg", offsetof(ui_theme, bar_fg)},
    {"colors.border.focus", offsetof(ui_theme, border_focus)}, {"colors.border.normal", offsetof(ui_theme, border_normal)},
  };
  for (unsigned i = 0; i < sizeof colors / sizeof colors[0]; i++) color(&c, colors[i].key, (unsigned *)((char *)t + colors[i].off));
  char key[48];
  for (int i = 0; i < 16; i++) {
    snprintf(key, sizeof key, i < 8 ? "terminal.%s" : "terminal.bright.%s", ansi[i & 7]);
    color(&c, key, &t->color[i]);
  }
  /* optional colours not given: derive them from the others */
  if (uconf_str(&c, "colors.desktop", NULL)) color(&c, "colors.desktop", &t->desktop); else t->desktop = gfx_blend_px(t->bg, 0, 40);
  if (uconf_str(&c, "colors.panel", NULL)) color(&c, "colors.panel", &t->panel); else t->panel = t->bar_bg;
  if (uconf_str(&c, "colors.border.float", NULL)) color(&c, "colors.border.float", &t->border_float); else t->border_float = t->color[11];
  const char *f = uconf_str(&c, "font.regular", NULL);
  if (f) parse_font(f, t->font);
  if ((f = uconf_str(&c, "font.bold", NULL))) parse_font(f, t->font_bold);
  t->font_size = uconf_int(&c, "font.size", t->font_size, 6, 72);
  t->gap = uconf_int(&c, "geometry.gap", t->gap, 0, 100);
  t->border = uconf_int(&c, "geometry.border", t->border, 0, 20);
  t->radius = uconf_int(&c, "geometry.radius", t->radius, 0, 40);
  uconf_free(&c);
  return 0;
}
