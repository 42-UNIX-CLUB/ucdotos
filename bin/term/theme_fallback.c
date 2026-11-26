// SPDX-License-Identifier: GPL-2.0-or-later
/* theme_fallback.c - weak stand-ins for ui_theme_load() and ui_read_file()
 * (libui/theme.h). They implement the documented file format with the
 * built-in themes `deck` and `brand`, so the terminal is themed before
 * libui/theme.c exists. The real libui definitions replace these at link time.
 * REMOVE this file once libui/theme.c has landed. */
#include "theme.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WEAK __attribute__((weak))
#define RGB(h) (0xff000000u | (((h) >> 16) & 255u) | ((h) & 0xff00u) | (((h) & 255u) << 16)) /* libgfx order */

static const struct { const char *name; unsigned bg, fg, accent, dim; unsigned c[16]; const char *font, *bold; } builtin[] = {
  {"deck", 0x282828, 0xebdbb2, 0xfe8019, 0x928374,
   {0x282828, 0xcc241d, 0x98971a, 0xd79921, 0x458588, 0xb16286, 0x689d6a, 0xa89984,
    0x928374, 0xfb4934, 0xb8bb26, 0xfabd2f, 0x83a598, 0xd3869b, 0x8ec07c, 0xebdbb2},
   "/usr/share/fonts/FiraCode-Regular.ttf", "/usr/share/fonts/FiraCode-Bold.ttf"},
  {"brand", 0x0e362e, 0xede6c5, 0xf2c14e, 0x6f8f80,
   {0x0a2621, 0xe06c5a, 0x8fbf7f, 0xe6c36a, 0x6fa8c7, 0xc792b5, 0x6fc2b0, 0xd9d2b4,
    0x4f7a6c, 0xf08a76, 0xa8d494, 0xf2d58a, 0x8cc2de, 0xdbaccc, 0x8adbc8, 0xede6c5},
   "/usr/share/fonts/JetBrainsMonoNerdFontMono-Regular.ttf", "/usr/share/fonts/JetBrainsMonoNerdFontMono-Bold.ttf"},
};

static void set_builtin(ui_theme *t, int i) {
  memset(t, 0, sizeof *t);
  strncpy(t->name, builtin[i].name, sizeof t->name - 1);
  t->bg = RGB(builtin[i].bg); t->fg = RGB(builtin[i].fg); t->accent = RGB(builtin[i].accent);
  t->dim = RGB(builtin[i].dim);
  t->border_focus = t->accent; t->border_normal = t->dim; t->bar_bg = t->bg; t->bar_fg = t->fg;
  for (int k = 0; k < 16; k++) t->color[k] = RGB(builtin[i].c[k]);
  strncpy(t->font, builtin[i].font, sizeof t->font - 1);
  strncpy(t->font_bold, builtin[i].bold, sizeof t->font_bold - 1);
  t->font_size = 16; t->gap = 8; t->border = 2; t->radius = 8;
}

WEAK unsigned char *ui_read_file(const char *path, unsigned *len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  size_t cap = 65536, n = 0;
  unsigned char *b = malloc(cap);
  for (;;) {
    if (!b) { close(fd); return NULL; }
    if (n == cap) { unsigned char *nb = realloc(b, cap *= 2); if (!nb) { free(b); close(fd); return NULL; } b = nb; }
    long r = read(fd, b + n, cap - n);
    if (r < 0) { free(b); close(fd); return NULL; }
    if (r == 0) break;
    n += (size_t)r;
  }
  close(fd);
  if (len) *len = (unsigned)n;
  return b;
}

static int hexval(const char *s, unsigned *out) {
  unsigned v = 0; int n = 0;
  if (*s == '#') s++;
  for (; n < 6; n++, s++) {
    int c = *s, d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return -1;
    v = v * 16 + (unsigned)d;
  }
  *out = RGB(v);
  return 0;
}

/* Parse `key = value` lines over t (keys not present keep their value). */
static void parse(ui_theme *t, char *text) {
  for (char *line = text; line && *line;) {
    char *next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *hash = strchr(line, '#');
    char *eq = strchr(line, '=');
    if (hash && (!eq || hash < eq)) { line = next; continue; } /* comment line */
    if (eq) {
      *eq = 0;
      char *k = line, *v = eq + 1;
      while (*k == ' ' || *k == '\t') k++;
      for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
      while (*v == ' ' || *v == '\t') v++;
      for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;
      if (*v != '#') { char *h = strchr(v, '#'); if (h) { *h = 0; for (char *e = h; e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0; } }
      unsigned *col = NULL;
      if (!strcmp(k, "bg")) col = &t->bg;
      else if (!strcmp(k, "fg")) col = &t->fg;
      else if (!strcmp(k, "accent")) col = &t->accent;
      else if (!strcmp(k, "border_focus")) col = &t->border_focus;
      else if (!strcmp(k, "border_normal")) col = &t->border_normal;
      else if (!strcmp(k, "bar_bg")) col = &t->bar_bg;
      else if (!strcmp(k, "bar_fg")) col = &t->bar_fg;
      else if (!strcmp(k, "dim")) col = &t->dim;
      else if (!strncmp(k, "color", 5) && k[5] >= '0' && k[5] <= '9') {
        int i = atoi(k + 5);
        if (i >= 0 && i < 16) col = &t->color[i];
      }
      if (col) hexval(v, col);
      else if (!strcmp(k, "font")) { strncpy(t->font, v, sizeof t->font - 1); t->font[sizeof t->font - 1] = 0; }
      else if (!strcmp(k, "font_bold")) { strncpy(t->font_bold, v, sizeof t->font_bold - 1); t->font_bold[sizeof t->font_bold - 1] = 0; }
      else if (!strcmp(k, "font_size")) t->font_size = atoi(v);
      else if (!strcmp(k, "gap")) t->gap = atoi(v);
      else if (!strcmp(k, "border")) t->border = atoi(v);
      else if (!strcmp(k, "radius")) t->radius = atoi(v);
    }
    line = next;
  }
}

WEAK int ui_theme_load(ui_theme *t, const char *name) {
  char nm[32] = "deck";
  if (!name) { /* active theme: first word of ~/.config/ucos/theme */
    const char *home = getenv("HOME");
    char path[256];
    strncpy(path, home ? home : "/home/user", 200);
    path[200] = 0;
    strcat(path, "/.config/ucos/theme");
    unsigned len;
    char *s = (char *)ui_read_file(path, &len);
    if (s) {
      unsigned i = 0, n = 0;
      while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
      while (i < len && n < sizeof nm - 1 && s[i] > ' ') nm[n++] = s[i++];
      if (n) nm[n] = 0;
      free(s);
    }
    name = nm;
  }
  set_builtin(t, !strcmp(name, "brand") ? 1 : 0);
  size_t nl = strlen(name) < sizeof t->name - 1 ? strlen(name) : sizeof t->name - 1;
  memcpy(t->name, name, nl);
  t->name[nl] = 0;
  char path[160];
  strcpy(path, "/usr/share/themes/");
  strncat(path, name, 100);
  strcat(path, ".theme");
  unsigned len;
  unsigned char *s = ui_read_file(path, &len);
  if (!s) return -1;
  char *text = malloc(len + 1);
  if (!text) { free(s); return -1; }
  memcpy(text, s, len);
  text[len] = 0;
  free(s);
  parse(t, text);
  free(text);
  return 0;
}
