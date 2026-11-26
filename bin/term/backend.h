// SPDX-License-Identifier: GPL-2.0-or-later
/* backend.h - where the terminal's pixels go and its input comes from:
 * a libui window (be_ui.c) or the raw framebuffer and evdev device (be_fb.c). */
#ifndef BACKEND_H
#define BACKEND_H
#include "gfx.h"
#include "render.h"

enum { TE_KEY, TE_KEY_UP, TE_TEXT, TE_WHEEL, TE_RESIZE, TE_FOCUS, TE_BLUR, TE_CLOSE,
       TE_MOUSE_DOWN, TE_MOUSE_UP, TE_MOUSE_MOVE };
typedef struct {
  int type;
  int key, mods;     /* TE_KEY: Linux key code, KM_* modifiers */
  unsigned cp;       /* TE_TEXT: Unicode character */
  int x, y, button;  /* mouse, window pixels; button 0 left, 1 middle, 2 right */
  int wheel;         /* +1 up, -1 down */
} tevent;

typedef struct backend {
  gfx_surface surf;  /* the pixels to draw into (re-initialised on TE_RESIZE) */
  int w, h, focused;
  int (*fd)(struct backend *);                  /* descriptor to poll for input */
  int (*next)(struct backend *, tevent *);      /* non-blocking: 1 if an event was returned, -1 if gone */
  void (*present)(struct backend *, render_rect);
  void (*set_title)(struct backend *, const char *);
  void (*close)(struct backend *);
  void *priv;
} backend;

backend *be_fb_open(void);
backend *be_ui_open(const char *title, int w, int h, int fullscreen);

#endif
