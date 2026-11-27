// SPDX-License-Identifier: GPL-2.0-or-later
/* Themes: one text file decides how the whole desktop looks.
 *
 * The active theme name is the first word of ~/.config/ucos/theme (default "deck").
 * A theme is /usr/share/themes/NAME.uconf, in the .uconf format (libuconf/uconf.h):
 *   [colors]    bg fg accent dim, bar { bg fg }, border { focus normal }      colours, #rrggbb
 *               desktop panel, border { float }                               (optional)
 *   [terminal]  black red green yellow blue magenta cyan white, bright { the same eight }
 *   [font]      regular bold (a file in /usr/share/fonts, or a path), size
 *   [geometry]  gap border radius                                             pixels
 * Every graphical program loads it with ui_theme_load(), so changing the file
 * and reloading re-rices everything. */
#ifndef UI_THEME_H
#define UI_THEME_H

typedef struct {
  char name[32];
  unsigned bg, fg, accent, border_focus, border_normal, bar_bg, bar_fg, dim; /* libgfx colours (R,G,B,A bytes) */
  unsigned color[16];
  char font[128], font_bold[128];
  int font_size, gap, border, radius;
  /* added after the first version (append-only):
   *   desktop       the wallpaper colour behind the windows
   *   border_float  border of floating windows (launcher, dialogs)
   *   panel         a raised surface: launcher background, selection rows */
  unsigned desktop, border_float, panel;
} ui_theme;

/* Load the active theme (name == NULL) or a named one. Missing keys keep built-in defaults,
 * so this never fails; returns 0 if the file was found, -1 if defaults were used. */
int ui_theme_load(ui_theme *t, const char *name);
/* Read a whole file into malloc'ed memory (for fonts). Returns NULL on error. */
unsigned char *ui_read_file(const char *path, unsigned *len);

#endif
