// SPDX-License-Identifier: GPL-2.0-or-later
/* be_fb.c - full-screen back end: draws to /dev/fb0 and reads /dev/input/event0.
 * Works without a window manager. */
#include "backend.h"
#include "keys.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

struct input_event { long long sec, usec; unsigned short type, code; int value; };
enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3, EV_MSC = 4 };

#define QN 256
typedef struct {
  int fb, ev;
  unsigned *px;
  int shift_l, shift_r, ctrl_l, ctrl_r, alt;
  int key, key_value;      /* key press waiting for its EV_SYN */
  unsigned cp;             /* character typed by that press (EV_MSC 0x10) */
  int mx, my, moved;
  tevent q[QN];
  int qh, qn;
} fbst;

static void push(fbst *s, tevent e) { if (s->qn < QN) s->q[(s->qh + s->qn++) % QN] = e; }
static int mods(fbst *s) {
  return ((s->shift_l || s->shift_r) ? KM_SHIFT : 0) | ((s->ctrl_l || s->ctrl_r) ? KM_CTRL : 0) | (s->alt ? KM_ALT : 0);
}

static int fb_geometry(backend *b) {
  fbst *s = b->priv;
  unsigned v[40] = {0};
  if (__syscall3(SYS_ioctl, s->fb, 0x4600, v) != 0 || !v[0] || !v[1]) return -1;
  unsigned *px = malloc((size_t)v[0] * v[1] * 4);
  if (!px) return -1;
  free(s->px);
  s->px = px;
  b->w = (int)v[0]; b->h = (int)v[1];
  gfx_init(&b->surf, px, b->w, b->h, b->w);
  return 0;
}

static int fb_fd(backend *b) { return ((fbst *)b->priv)->ev; }

static void handle(backend *b, struct input_event *e) {
  fbst *s = b->priv;
  tevent t;
  memset(&t, 0, sizeof t);
  if (e->type == EV_KEY) {
    int down = e->value != 0, k = e->code;
    if (k == K_LEFTSHIFT) s->shift_l = down;
    else if (k == K_RIGHTSHIFT) s->shift_r = down;
    else if (k == K_LEFTCTRL) s->ctrl_l = down;
    else if (k == K_RIGHTCTRL) s->ctrl_r = down;
    else if (k == K_LEFTALT) s->alt = down; /* AltRight is AltGr: it only changes the character */
    if (k >= K_BTN_LEFT && k <= K_BTN_MIDDLE) {
      t.type = down ? TE_MOUSE_DOWN : TE_MOUSE_UP;
      t.button = k == K_BTN_LEFT ? 0 : k == K_BTN_MIDDLE ? 1 : 2;
      t.x = s->mx; t.y = s->my; t.mods = mods(s);
      push(s, t);
      return;
    }
    if (down) { s->key = k; s->key_value = e->value; s->cp = 0; }
    else { t.type = TE_KEY_UP; t.key = k; t.mods = mods(s); push(s, t); }
  } else if (e->type == EV_MSC && e->code == 0x10) {
    s->cp = (unsigned)e->value;
  } else if (e->type == EV_MSC && e->code == 0x11) {
    if (fb_geometry(b) == 0) { t.type = TE_RESIZE; push(s, t); }
  } else if (e->type == EV_REL && e->code == 8) {
    t.type = TE_WHEEL; t.wheel = e->value; t.x = s->mx; t.y = s->my; t.mods = mods(s);
    push(s, t);
  } else if (e->type == EV_ABS) {
    if (e->code == 0) s->mx = e->value;
    if (e->code == 1) s->my = e->value;
    s->moved = 1;
  } else if (e->type == EV_SYN) {
    if (s->key) { /* a key press is complete: the key, then the character it typed */
      t.type = TE_KEY; t.key = s->key; t.mods = mods(s);
      push(s, t);
      if (s->cp) { tevent c; memset(&c, 0, sizeof c); c.type = TE_TEXT; c.cp = s->cp; c.mods = t.mods; push(s, c); }
      s->key = 0; s->cp = 0;
    }
    if (s->moved) { s->moved = 0; t.type = TE_MOUSE_MOVE; t.x = s->mx; t.y = s->my; push(s, t); }
  }
}

static int fb_next(backend *b, tevent *out) {
  fbst *s = b->priv;
  if (!s->qn) {
    struct input_event ev[32];
    long n = read(s->ev, ev, sizeof ev);
    for (long i = 0; i + 1 <= n / (long)sizeof ev[0]; i++) handle(b, &ev[i]);
  }
  if (!s->qn) return 0;
  *out = s->q[s->qh];
  s->qh = (s->qh + 1) % QN; s->qn--;
  return 1;
}

static void fb_present(backend *b, render_rect r) {
  fbst *s = b->priv;
  if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
  if (r.x0 < 0) r.x0 = 0;
  if (r.y0 < 0) r.y0 = 0;
  if (r.x1 > b->w) r.x1 = b->w;
  if (r.y1 > b->h) r.y1 = b->h;
  if ((r.x1 - r.x0) * 4 < b->w) { /* narrow: one write per scanline */
    for (int y = r.y0; y < r.y1; y++) {
      long off = ((long)y * b->w + r.x0) * 4;
      __syscall4(SYS_pwrite64, s->fb, (char *)s->px + off, (r.x1 - r.x0) * 4, off);
    }
  } else { /* whole scanlines in one write */
    long off = (long)r.y0 * b->w * 4;
    __syscall4(SYS_pwrite64, s->fb, (char *)s->px + off, (long)(r.y1 - r.y0) * b->w * 4, off);
  }
}
static void fb_title(backend *b, const char *t) { (void)b; (void)t; }
static void fb_close(backend *b) {
  fbst *s = b->priv;
  close(s->fb); close(s->ev);
  free(s->px); free(s); free(b);
}

backend *be_fb_open(void) {
  backend *b = calloc(1, sizeof *b);
  fbst *s = calloc(1, sizeof *s);
  if (!b || !s) { free(b); free(s); return NULL; }
  b->priv = s;
  s->fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  s->ev = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (s->fb < 0 || s->ev < 0 || fb_geometry(b)) {
    if (s->fb >= 0) close(s->fb);
    if (s->ev >= 0) close(s->ev);
    free(s->px); free(s); free(b);
    return NULL;
  }
  b->focused = 1;
  b->fd = fb_fd; b->next = fb_next; b->present = fb_present; b->set_title = fb_title; b->close = fb_close;
  return b;
}
