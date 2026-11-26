// SPDX-License-Identifier: GPL-2.0-or-later
/* libui client side: one socket to the window manager, one pixel buffer, an event queue.
 * The wire format is described in uiproto.h. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/syscall.h>
#include "ui.h"
#include "uiproto.h"

#define QCAP 256
struct ui_window {
  int fd, w, h, role, dead;
  unsigned *px;
  unsigned char *rx;     /* bytes received but not yet parsed */
  unsigned rxlen, rxcap;
  ui_event q[QCAP];      /* parsed events not yet delivered */
  int qhead, qlen;
  ui_wm_info info;
  int state_replies, replies, reply_status;
  unsigned char *tx;     /* staging buffer for PRESENT rows */
  unsigned txcap;
};

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int send_all(ui_window *win, const void *buf, unsigned len) {
  if (win->dead) return -1;
  /* MSG_NOSIGNAL: a library must not let the manager's death kill its program with
   * SIGPIPE; the program sees the window die (EPIPE) and decides */
  while (len) {
    long n = __syscall6(SYS_sendto, win->fd, (long)buf, len, MSG_NOSIGNAL, 0, 0);
    if (n == -EINTR) continue;
    if (n <= 0) { win->dead = 1; return -1; }
    buf = (const char *)buf + n; len -= n;
  }
  return 0;
}
static int send_msg(ui_window *win, int type, const void *payload, unsigned len) {
  struct uip_hdr h = {(unsigned short)type, 0, len};
  unsigned char buf[UIP_HDR + UIP_CTL_MAX];
  if (len > UIP_CTL_MAX) return -1;
  memcpy(buf, &h, UIP_HDR);
  if (len) memcpy(buf + UIP_HDR, payload, len);
  return send_all(win, buf, UIP_HDR + len);
}

static void push(ui_window *win, const ui_event *ev) {
  if (win->qlen == QCAP) { /* full: drop the oldest pointer motion if any, else the oldest event */
    int drop = 0;
    for (int i = 0; i < QCAP; i++)
      if (win->q[(win->qhead + i) % QCAP].type == UI_EV_MOUSE_MOVE) { drop = i; break; }
    for (int i = drop; i > 0; i--) win->q[(win->qhead + i) % QCAP] = win->q[(win->qhead + i - 1) % QCAP];
    win->qhead = (win->qhead + 1) % QCAP;
    win->qlen--;
  }
  win->q[(win->qhead + win->qlen++) % QCAP] = *ev;
}

/* Handle one complete message from the manager. */
static void handle(ui_window *win, const struct uip_hdr *h, const unsigned char *p) {
  ui_event ev;
  memset(&ev, 0, sizeof ev);
  switch (h->type) {
  case UIP_CONFIGURE: {
    struct uip_size s;
    if (h->len < sizeof s) return;
    memcpy(&s, p, sizeof s);
    if (s.w < 1 || s.h < 1 || s.w > UIP_MAX_DIM || s.h > UIP_MAX_DIM || (long long)s.w * s.h > UIP_MAX_AREA) return;
    ev.type = UI_EV_RESIZE; ev.w = s.w; ev.h = s.h;
    push(win, &ev); /* the buffer is reallocated when this event is delivered */
    break;
  }
  case UIP_EVENT: {
    struct uip_event e;
    if (h->len < sizeof e) return;
    memcpy(&e, p, sizeof e);
    ev.type = e.type; ev.key = e.key; ev.mods = e.mods; ev.repeat = e.repeat; ev.codepoint = e.codepoint;
    ev.x = e.x; ev.y = e.y; ev.button = e.button; ev.wheel = e.wheel;
    if (ev.type > UI_EV_NONE && ev.type <= UI_EV_MOUSE_REL && ev.type != UI_EV_RESIZE) push(win, &ev);
    break;
  }
  case UIP_STATE: {
    struct uip_state s;
    if (h->len < sizeof s) return;
    memcpy(&s, p, sizeof s);
    win->info.workspace = s.workspace;
    win->info.workspace_count = s.workspace_count;
    win->info.occupied_mask = s.occupied_mask;
    memcpy(win->info.focused_title, s.focused_title, sizeof s.focused_title);
    win->info.focused_title[sizeof win->info.focused_title - 1] = 0;
    if (h->flags & UIP_F_REPLY) win->state_replies++;
    else { ev.type = UI_EV_WM_STATE; push(win, &ev); }
    break;
  }
  case UIP_REPLY:
    if (h->len >= 4) memcpy(&win->reply_status, p, 4);
    win->replies++;
    break;
  }
}

/* Read what the socket has (waiting up to timeout_ms for the first byte) and parse
 * every complete message. Returns -1 once the manager is gone, else 0. */
static int pump(ui_window *win, int timeout_ms) {
  if (win->dead) return -1;
  struct pollfd pf = {win->fd, POLLIN, 0};
  int r = poll(&pf, 1, timeout_ms);
  if (r <= 0) return r < 0 && errno != EINTR ? (win->dead = 1, -1) : 0;
  if (win->rxcap - win->rxlen < 4096) {
    unsigned cap = win->rxcap ? win->rxcap * 2 : 16384;
    unsigned char *n = realloc(win->rx, cap);
    if (!n) { win->dead = 1; return -1; }
    win->rx = n; win->rxcap = cap;
  }
  long n = read(win->fd, win->rx + win->rxlen, win->rxcap - win->rxlen);
  if (n <= 0) { if (n < 0 && errno == EAGAIN) return 0; win->dead = 1; return -1; }
  win->rxlen += n;
  unsigned off = 0;
  while (win->rxlen - off >= UIP_HDR) {
    struct uip_hdr h;
    memcpy(&h, win->rx + off, UIP_HDR);
    if (h.len > UIP_CTL_MAX) { win->dead = 1; return -1; } /* not a manager we understand */
    if (win->rxlen - off < UIP_HDR + h.len) break;
    handle(win, &h, win->rx + off + UIP_HDR);
    off += UIP_HDR + h.len;
  }
  memmove(win->rx, win->rx + off, win->rxlen - off);
  win->rxlen -= off;
  return 0;
}

static int resize_buffer(ui_window *win, int w, int h) {
  if (w == win->w && h == win->h && win->px) return 0;
  unsigned *n = calloc((size_t)w * h, 4);
  if (!n) return -1;
  free(win->px);
  win->px = n; win->w = w; win->h = h;
  return 0;
}

ui_window *ui_open(const char *title, const char *app_id, int w, int h, int role) {
  struct { unsigned short fam; char path[108]; } addr = {1, "/run/wm.sock"};
  const char *env = getenv("WM_SOCKET");
  if (env && *env) strlcpy(addr.path, env, sizeof addr.path);
  int fd = __syscall3(SYS_socket, 1, 1 | O_CLOEXEC, 0);
  if (fd < 0) return NULL;
  if (__syscall3(SYS_connect, fd, &addr, sizeof addr) < 0) { close(fd); return NULL; }
  ui_window *win = calloc(1, sizeof *win);
  if (!win) { close(fd); return NULL; }
  win->fd = fd; win->role = role;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  struct { struct uip_hello hello; char title[UIP_TITLE_MAX + 1]; } m;
  memset(&m, 0, sizeof m);
  m.hello.version = UIP_VERSION; m.hello.pid = getpid(); m.hello.role = role; m.hello.w = w; m.hello.h = h;
  strlcpy(m.hello.app_id, app_id ? app_id : "", sizeof m.hello.app_id);
  unsigned tl = title ? strnlen(title, UIP_TITLE_MAX) : 0;
  if (tl) memcpy(m.title, title, tl);
  if (send_msg(win, UIP_HELLO, &m, sizeof m.hello + tl) < 0) { ui_close(win); return NULL; }
  /* Wait for the first CONFIGURE so ui_width/ui_height/ui_pixels are right at once.
   * The UI_EV_RESIZE stays queued so the program's event loop draws the first frame. */
  double end = now_ms() + 3000;
  while (!win->dead) {
    for (int i = 0; i < win->qlen; i++) {
      ui_event *e = &win->q[(win->qhead + i) % QCAP];
      if (e->type == UI_EV_RESIZE) {
        if (resize_buffer(win, e->w, e->h) < 0) { ui_close(win); return NULL; }
        return win;
      }
    }
    int left = (int)(end - now_ms());
    if (left <= 0) break;
    pump(win, left);
  }
  ui_close(win);
  return NULL;
}

void ui_close(ui_window *win) {
  if (!win) return;
  close(win->fd);
  free(win->px); free(win->rx); free(win->tx);
  free(win);
}

void ui_set_title(ui_window *win, const char *title) {
  if (!title) title = "";
  send_msg(win, UIP_TITLE, title, strnlen(title, UIP_TITLE_MAX));
}

unsigned int *ui_pixels(ui_window *win) { return win->px; }
int ui_width(ui_window *win) { return win->w; }
int ui_height(ui_window *win) { return win->h; }
int ui_fd(ui_window *win) { return win->fd; }

void ui_present(ui_window *win, int x, int y, int w, int h) {
  if (!win || win->dead || !win->px) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w > win->w - x) w = win->w - x;
  if (h > win->h - y) h = win->h - y;
  if (w <= 0 || h <= 0) return;
  struct { struct uip_hdr h; struct uip_present p; } m = {
    {UIP_PRESENT, 0, (unsigned)(sizeof(struct uip_present) + (size_t)w * h * 4)}, {win->w, win->h, x, y, w, h}};
  if (send_all(win, &m, sizeof m) < 0) return;
  if (w == win->w) { /* whole rows: the rectangle is contiguous in memory */
    send_all(win, win->px + (size_t)y * win->w, (unsigned)w * h * 4);
    return;
  }
  /* Partial rows: gather them into a staging buffer of up to 256 KiB per write. */
  unsigned row = (unsigned)w * 4, rows = (256 * 1024) / row;
  if (!rows) rows = 1;
  if (rows > (unsigned)h) rows = h;
  if (win->txcap < rows * row) {
    free(win->tx);
    win->tx = malloc(rows * row);
    win->txcap = win->tx ? rows * row : 0;
    if (!win->tx) { win->dead = 1; return; }
  }
  for (int j = 0; j < h; ) {
    unsigned n = 0;
    for (; n < rows && j < h; n++, j++) memcpy(win->tx + n * row, win->px + (size_t)(y + j) * win->w + x, row);
    if (send_all(win, win->tx, n * row) < 0) return;
  }
}

/* Deliver the oldest queued event; a resize reallocates the buffer at this point. */
static int deliver(ui_window *win, ui_event *ev) {
  if (!win->qlen) return 0;
  *ev = win->q[win->qhead];
  win->qhead = (win->qhead + 1) % QCAP;
  win->qlen--;
  if (ev->type == UI_EV_RESIZE && resize_buffer(win, ev->w, ev->h) < 0) win->dead = 1;
  return 1;
}

int ui_poll_event(ui_window *win, ui_event *ev) {
  if (!win->qlen) pump(win, 0);
  return deliver(win, ev);
}

int ui_wait_event(ui_window *win, ui_event *ev, int timeout_ms) {
  double end = now_ms() + timeout_ms;
  for (;;) {
    if (deliver(win, ev)) return 1;
    if (win->dead) return -1;
    int left = timeout_ms < 0 ? -1 : (int)(end - now_ms() + 0.999);
    if (timeout_ms >= 0 && left <= 0) left = 0;
    if (pump(win, left) < 0) return -1;
    if (!win->qlen && timeout_ms >= 0 && now_ms() >= end) return 0;
  }
}

int ui_wm_state(ui_window *win, ui_wm_info *out) {
  int want = win->state_replies + 1;
  if (send_msg(win, UIP_GETSTATE, NULL, 0) < 0) return -1;
  double end = now_ms() + 2000;
  while (win->state_replies < want) {
    int left = (int)(end - now_ms());
    if (left <= 0 || pump(win, left) < 0) return -1;
  }
  if (out) *out = win->info;
  return 0;
}

int ui_wm_command(ui_window *win, const char *command) {
  int want = win->replies + 1;
  if (!command || send_msg(win, UIP_COMMAND, command, strnlen(command, UIP_CTL_MAX)) < 0) return -1;
  double end = now_ms() + 2000;
  while (win->replies < want) {
    int left = (int)(end - now_ms());
    if (left <= 0 || pump(win, left) < 0) return -1;
  }
  return win->reply_status;
}

int ui_pointer_lock(ui_window *win, int on) { return ui_wm_command(win, on ? "pointer-lock on" : "pointer-lock off"); }
