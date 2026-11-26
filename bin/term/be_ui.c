// SPDX-License-Identifier: GPL-2.0-or-later
/* be_ui.c - windowed back end through libui (the window manager's client API). */
#include "backend.h"
#include "keys.h"
#include "ui.h"
#include <stdlib.h>
#include <string.h>

static ui_window *W(backend *b) { return b->priv; }

static void ui_surface(backend *b) {
  b->w = ui_width(W(b)); b->h = ui_height(W(b));
  unsigned *px = ui_pixels(W(b));
  if (!px || b->w <= 0 || b->h <= 0) { b->w = b->h = 0; gfx_init(&b->surf, NULL, 0, 0, 0); return; }
  gfx_init(&b->surf, px, b->w, b->h, b->w);
}

static int ui_fd_(backend *b) { return ui_fd(W(b)); }

static int ui_next(backend *b, tevent *t) {
  ui_event e;
  int r = ui_poll_event(W(b), &e);
  if (r < 0) return -1;
  if (r == 0) return 0;
  memset(t, 0, sizeof *t);
  t->mods = ((e.mods & UI_SHIFT) ? KM_SHIFT : 0) | ((e.mods & UI_CTRL) ? KM_CTRL : 0) | ((e.mods & UI_ALT) ? KM_ALT : 0);
  t->x = e.x; t->y = e.y; t->button = e.button;
  switch (e.type) {
  case UI_EV_KEY_DOWN: t->type = TE_KEY; t->key = e.key; break;
  case UI_EV_KEY_UP: t->type = TE_KEY_UP; t->key = e.key; break;
  case UI_EV_TEXT: t->type = TE_TEXT; t->cp = e.codepoint; break;
  case UI_EV_WHEEL: t->type = TE_WHEEL; t->wheel = e.wheel; break;
  case UI_EV_MOUSE_DOWN: t->type = TE_MOUSE_DOWN; break;
  case UI_EV_MOUSE_UP: t->type = TE_MOUSE_UP; break;
  case UI_EV_MOUSE_MOVE: t->type = TE_MOUSE_MOVE; break;
  case UI_EV_RESIZE: ui_surface(b); t->type = TE_RESIZE; break;
  case UI_EV_FOCUS: b->focused = 1; t->type = TE_FOCUS; break;
  case UI_EV_BLUR: b->focused = 0; t->type = TE_BLUR; break;
  case UI_EV_CLOSE: t->type = TE_CLOSE; break;
  case UI_EV_ZOOM: t->type = TE_ZOOM; t->wheel = e.wheel; break;
  default: return ui_next(b, t); /* not for us: try the next one */
  }
  return 1;
}

static void ui_present_(backend *b, render_rect r) {
  if (r.x1 > r.x0 && r.y1 > r.y0) ui_present(W(b), r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}
static void ui_title(backend *b, const char *t) { ui_set_title(W(b), t); }
static void ui_close_(backend *b) { ui_close(W(b)); free(b); }

backend *be_ui_open(const char *title, int w, int h, int fullscreen) {
  ui_window *win = ui_open(title, "term", w, h, fullscreen ? UI_FULLSCREEN : UI_NORMAL);
  if (!win) return NULL;
  backend *b = calloc(1, sizeof *b);
  if (!b) { ui_close(win); return NULL; }
  b->priv = win;
  b->focused = 1;
  b->fd = ui_fd_; b->next = ui_next; b->present = ui_present_; b->set_title = ui_title; b->close = ui_close_;
  /* the manager decides the size and sends UI_EV_RESIZE before the first draw */
  ui_surface(b);
  for (int tries = 0; tries < 50 && (b->w <= 0 || b->h <= 0); tries++) {
    ui_event e;
    int r = ui_wait_event(win, &e, 100);
    if (r < 0) { ui_close_(b); return NULL; }
    if (r > 0 && e.type == UI_EV_RESIZE) ui_surface(b);
  }
  if (b->w <= 0 || b->h <= 0) { ui_close_(b); return NULL; }
  return b;
}
