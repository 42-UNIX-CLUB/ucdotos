// SPDX-License-Identifier: GPL-2.0-or-later
/* Theme loader: ~/.config/ucos/theme names the theme, /usr/share/themes/NAME.theme describes it. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "theme.h"
#include "gfx.h"

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

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static int parse_color(const char *v, unsigned *out) {
  if (*v == '#') v++;
  unsigned rgb = 0;
  for (int i = 0; i < 6; i++) {
    int h = hexval(v[i]);
    if (h < 0) return -1;
    rgb = rgb << 4 | h;
  }
  *out = GFX_HEX(rgb);
  return 0;
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

int ui_theme_load(ui_theme *t, const char *name) {
  char nm[32], path[96];
  defaults(t);
  if (name && *name && !strchr(name, '/')) strlcpy(nm, name, sizeof nm);
  else active_name(nm, sizeof nm);
  strlcpy(t->name, nm, sizeof t->name);
  strcpy(path, "/usr/share/themes/");
  strncat(path, nm, sizeof path - strlen(path) - 8);
  strcat(path, ".theme");
  char *s = (char *)ui_read_file(path, NULL);
  if (!s) return -1;
  int have_desktop = 0, have_panel = 0, have_float = 0;
  for (char *line = s, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *hash = strchr(line, '#');
    /* a '#' starts a comment unless it begins a colour value (key = #rrggbb) */
    char *eq = strchr(line, '=');
    if (hash && (!eq || hash < eq)) *hash = 0;
    if (!eq || (hash && hash < eq)) continue;
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (*k == ' ' || *k == '\t') k++;
    for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;
    while (*v == ' ' || *v == '\t') v++;
    for (char *c = v + 1; *c; c++) /* a '#' after a blank starts a trailing comment */
      if (*c == '#' && (c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
    for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;
    if (!*k || !*v) continue;
    struct { const char *key; unsigned *dst; } colors[] = {
      {"bg", &t->bg}, {"fg", &t->fg}, {"accent", &t->accent}, {"border_focus", &t->border_focus},
      {"border_normal", &t->border_normal}, {"bar_bg", &t->bar_bg}, {"bar_fg", &t->bar_fg}, {"dim", &t->dim},
      {"desktop", &t->desktop}, {"border_float", &t->border_float}, {"panel", &t->panel},
    };
    int done = 0;
    for (unsigned i = 0; i < sizeof colors / sizeof colors[0]; i++)
      if (!strcmp(k, colors[i].key)) {
        parse_color(v, colors[i].dst);
        if (colors[i].dst == &t->desktop) have_desktop = 1;
        if (colors[i].dst == &t->panel) have_panel = 1;
        if (colors[i].dst == &t->border_float) have_float = 1;
        done = 1;
      }
    if (done) continue;
    if (!strncmp(k, "color", 5) && k[5] >= '0' && k[5] <= '9') {
      int i = atoi(k + 5);
      if (i >= 0 && i < 16) parse_color(v, &t->color[i]);
    } else if (!strcmp(k, "font")) parse_font(v, t->font);
    else if (!strcmp(k, "font_bold")) parse_font(v, t->font_bold);
    else if (!strcmp(k, "font_size")) { int n = atoi(v); if (n >= 6 && n <= 72) t->font_size = n; }
    else if (!strcmp(k, "gap")) { int n = atoi(v); if (n >= 0 && n <= 100) t->gap = n; }
    else if (!strcmp(k, "border")) { int n = atoi(v); if (n >= 0 && n <= 20) t->border = n; }
    else if (!strcmp(k, "radius")) { int n = atoi(v); if (n >= 0 && n <= 40) t->radius = n; }
  }
  free(s);
  /* optional colours not given: derive them from the required ones */
  if (!have_desktop) t->desktop = gfx_blend_px(t->bg, 0, 40);
  if (!have_panel) t->panel = t->bar_bg;
  if (!have_float) t->border_float = t->color[11];
  return 0;
}
