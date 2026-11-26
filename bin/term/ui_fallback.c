// SPDX-License-Identifier: GPL-2.0-or-later
/* ui_fallback.c - weak stand-ins for the libui window calls (libui/ui.h) used by
 * be_ui.c, so that term links before libui/ui.c exists. ui_open() returning NULL
 * makes term fall back to the framebuffer. The real libui replaces these at link
 * time (bin/term/CFLAGS links libui.a with --whole-archive so that its strong
 * definitions are pulled in even though these weak ones already satisfy the
 * references). REMOVE this file once libui/ui.c has landed. */
#include "ui.h"
#include <stddef.h>

#define WEAK __attribute__((weak))
WEAK ui_window *ui_open(const char *title, const char *app_id, int w, int h, int role) {
  (void)title; (void)app_id; (void)w; (void)h; (void)role;
  return NULL;
}
WEAK void ui_close(ui_window *w) { (void)w; }
WEAK void ui_set_title(ui_window *w, const char *t) { (void)w; (void)t; }
WEAK unsigned int *ui_pixels(ui_window *w) { (void)w; return NULL; }
WEAK int ui_width(ui_window *w) { (void)w; return 0; }
WEAK int ui_height(ui_window *w) { (void)w; return 0; }
WEAK void ui_present(ui_window *w, int x, int y, int ww, int h) { (void)w; (void)x; (void)y; (void)ww; (void)h; }
WEAK int ui_fd(ui_window *w) { (void)w; return -1; }
WEAK int ui_poll_event(ui_window *w, ui_event *e) { (void)w; (void)e; return -1; }
WEAK int ui_wait_event(ui_window *w, ui_event *e, int ms) { (void)w; (void)e; (void)ms; return -1; }
