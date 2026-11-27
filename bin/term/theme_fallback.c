// SPDX-License-Identifier: GPL-2.0-or-later
/* theme_fallback.c - weak stand-ins for ui_theme_load() and ui_read_file()
 * (libui/theme.h), with the built-in themes `deck` and `brand`. They serve the
 * native tests (bin/term/tests), which build the renderer without libui and have
 * no /usr/share/themes. In the system, libui's real definitions replace these at
 * link time and read the .uconf theme files. */
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
  return 0;
}
