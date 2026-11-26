// SPDX-License-Identifier: GPL-2.0-or-later
/* wm: owns the screen and the input device, listens on /run/wm.sock, and runs
 * one poll() loop over the input device, the listener and every client.
 * Nothing here blocks: client sockets are non-blocking, and when nothing
 * happens the loop sleeps in poll() with no timeout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "wm.h"

wm_config cfg;
ui_theme theme;
client *order[MAX_CLIENTS];
int nclients, cur_ws;
client *focus_of[MAX_WS];
float master_of[MAX_WS];
int ptr_x, ptr_y, ptr_shown;
int stats_frames, stats_writes;
long long stats_bytes;
double stats_compose_ms;
int trace;
int bar_h;

static int ev_fd = -1, listen_fd = -1;
static char sock_path[108];
static int mods, swallow[256], swallow_text;
static int buttons;
static client *grab;  /* the window that got the mouse button press receives motion until release */
static client *locked; /* the window the mouse is captured for: no cursor, movement as UI_EV_MOUSE_REL */
static client *drag;  /* a floating window being moved with Mod + left button */
static int drag_dx, drag_dy; /* where the pointer holds it, from its top-left corner */
static int quit;
static int state_dirty;
static int children[64], nchildren;
static char failed_exec[8][64];
static int nfailed;

double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
void state_changed(void) { state_dirty = 1; }

/* ---------- sending ---------- */
static void flush_out(client *c) {
  while (c->outlen) {
    /* MSG_NOSIGNAL: a client that went away gives EPIPE here, never SIGPIPE */
    long n = __syscall6(SYS_sendto, c->fd, (long)c->out, c->outlen, MSG_NOSIGNAL, 0, 0);
    if (n == -EAGAIN) return;
    if (n <= 0) { c->outlen = 0; return; } /* EPIPE or another error: the hangup is noticed by the next read */
    memmove(c->out, c->out + n, c->outlen - n);
    c->outlen -= n;
  }
}
static void queue_msg(client *c, int type, int flags, const void *p, unsigned len) {
  struct uip_hdr h = {(unsigned short)type, (unsigned short)flags, len};
  if (c->outlen + UIP_HDR + len > 256 * 1024) { /* the client stopped reading: drop, do not wait */
    if (!c->dropped++) fprintf(stderr, "wm: client %d (%s) is not reading its events; dropping\n", c->pid, c->app_id);
    return;
  }
  if (c->outcap < c->outlen + UIP_HDR + len) {
    unsigned cap = c->outcap ? c->outcap : 4096;
    while (cap < c->outlen + UIP_HDR + len) cap *= 2;
    unsigned char *n = realloc(c->out, cap);
    if (!n) return;
    c->out = n; c->outcap = cap;
  }
  memcpy(c->out + c->outlen, &h, UIP_HDR);
  if (len) memcpy(c->out + c->outlen + UIP_HDR, p, len);
  c->outlen += UIP_HDR + len;
  flush_out(c);
}
void send_event(client *c, int type, int key, int m, int repeat, unsigned cp, int x, int y, int button, int wheel) {
  if (!c || !c->hello) return;
  struct uip_event e = {type, key, m, repeat, cp, x, y, button, wheel};
  queue_msg(c, UIP_EVENT, 0, &e, sizeof e);
}
void send_configure(client *c) {
  rect cr;
  content_rect(c, &cr);
  if (cr.w < 1) cr.w = 1;
  if (cr.h < 1) cr.h = 1;
  c->cw = cr.w; c->ch = cr.h;
  struct uip_size s = {cr.w, cr.h};
  queue_msg(c, UIP_CONFIGURE, 0, &s, sizeof s);
}
static void fill_state(struct uip_state *s) {
  memset(s, 0, sizeof *s);
  s->workspace = cur_ws + 1;
  s->workspace_count = cfg.workspace_count;
  s->occupied_mask = occupied_mask();
  client *f = focused();
  if (f) strlcpy(s->focused_title, f->title, sizeof s->focused_title);
}
static void broadcast_state(void) {
  struct uip_state s;
  fill_state(&s);
  for (int i = 0; i < nclients; i++)
    if (order[i]->role == UI_BAR && order[i]->hello) queue_msg(order[i], UIP_STATE, 0, &s, sizeof s);
}

/* ---------- processes ---------- */
void wm_spawn(const char *cmdline) {
  char buf[256], *argv[32];
  int argc = 0;
  strlcpy(buf, cmdline, sizeof buf);
  for (char *p = buf; *p && argc < 31;) { /* split on blanks; "double" or 'single' quotes group words */
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    char q = (*p == '"' || *p == '\'') ? *p++ : 0;
    argv[argc++] = p;
    while (*p && (q ? *p != q : (*p != ' ' && *p != '\t'))) p++;
    if (*p) *p++ = 0;
  }
  argv[argc] = NULL;
  if (!argc) return;
  int pid = spawnp(argv[0], argv, NULL);
  if (pid < 0) {
    for (int i = 0; i < nfailed; i++) if (!strcmp(failed_exec[i], argv[0])) return; /* said it once already */
    fprintf(stderr, "wm: cannot start '%s': %s\n", argv[0], strerror(errno));
    if (nfailed < 8) strlcpy(failed_exec[nfailed++], argv[0], sizeof failed_exec[0]);
    return;
  }
  if (nchildren < 64) children[nchildren++] = pid;
}
static void reap(void) {
  int st, pid;
  while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
    for (int i = 0; i < nchildren; i++) if (children[i] == pid) { children[i] = children[--nchildren]; break; }
}
static int is_child(int pid) {
  for (int i = 0; i < nchildren; i++) if (children[i] == pid) return 1;
  return 0;
}

/* ---------- theme ---------- */
static int theme_names(char names[][32], int cap) {
  DIR *d = opendir("/usr/share/themes");
  int n = 0;
  for (struct dirent *e; d && (e = readdir(d)) && n < cap;) {
    int l = strlen(e->d_name);
    if (l > 6 && l < 38 && !strcmp(e->d_name + l - 6, ".theme")) {
      memcpy(names[n], e->d_name, l - 6);
      names[n][l - 6] = 0;
      n++;
    }
  }
  if (d) closedir(d);
  for (int i = 1; i < n; i++) /* sort by name so "theme next" cycles in a stable order */
    for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
      char t[32];
      strcpy(t, names[j]); strcpy(names[j], names[j - 1]); strcpy(names[j - 1], t);
    }
  return n;
}
static void theme_reload(void) {
  ui_theme_load(&theme, NULL);
  apply_geometry();
  render_wallpaper();
  relayout();
  for (int i = 0; i < nclients; i++) if (order[i]->hello) send_configure(order[i]); /* same size = redraw with the new theme */
  damage_all();
}
static int theme_set(const char *arg) {
  char names[16][32], name[32];
  int n = theme_names(names, 16);
  if (!strcmp(arg, "next") || !strcmp(arg, "prev")) {
    if (!n) return -1;
    int i = 0;
    while (i < n && strcmp(names[i], theme.name)) i++;
    i = i == n ? 0 : (i + (arg[0] == 'n' ? 1 : n - 1)) % n;
    strcpy(name, names[i]);
  } else {
    int ok = 0;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], arg)) ok = 1;
    if (!ok) return -1;
    strlcpy(name, arg, sizeof name);
  }
  const char *home = getenv("HOME");
  char path[256];
  snprintf(path, sizeof path, "%s/.config", home ? home : "/home/user");
  mkdir(path, 0755);
  strncat(path, "/ucos", sizeof path - strlen(path) - 1);
  mkdir(path, 0755);
  strncat(path, "/theme", sizeof path - strlen(path) - 1);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) { dprintf(fd, "%s\n", name); close(fd); }
  else fprintf(stderr, "wm: cannot write %s\n", path);
  theme_reload();
  fprintf(stderr, "wm: theme %s\n", theme.name);
  return 0;
}

/* ---------- commands: from key bindings and from clients ---------- */
static void print_stats(void) {
  struct sysinfo si;
  __syscall1(SYS_sysinfo, &si);
  fprintf(stderr, "wm: stats frames=%d fbwrites=%d fbbytes=%lld compose_ms=%.3f clients=%d mem_total=%lu kmem_used=%lu procs=%d wm_pages=%d\n",
          stats_frames, stats_writes, stats_bytes, stats_compose_ms, nclients, si.totalram, si.bufferram, si.procs,
          (int)__builtin_wasm_memory_size(0));
}
/* Capture the mouse while the focused window asked for it and is fullscreen; release it
 * as soon as that stops being true (another window, a workspace switch, fullscreen off,
 * the window gone). Called after everything that can change those. Costs a few
 * comparisons when nothing changes. */
static void lock_update(void) {
  client *f = focused();
  client *want = f && f->wants_lock && f->fullscreen && f->ws == cur_ws ? f : NULL;
  if (want == locked) return;
  locked = want;
  int on = locked != NULL;
  __syscall3(SYS_ioctl, ev_fd, EVIOCGRAB, &on);
  if (on) { if (ptr_shown) damage(ptr_x, ptr_y, 12, 19); ptr_shown = 0; } /* hide the cursor */
  else cursor_moved(ptr_x, ptr_y);                                        /* and bring it back */
}

int run_command(client *src, const char *cmd) {
  char verb[32] = "", arg[160] = "";
  while (*cmd == ' ') cmd++;
  int i = 0;
  while (*cmd && *cmd != ' ' && i < 31) verb[i++] = *cmd++;
  verb[i] = 0;
  while (*cmd == ' ') cmd++;
  strlcpy(arg, cmd, sizeof arg);
  if (!strcmp(verb, "exec")) { if (!*arg) return -1; wm_spawn(arg); return 0; }
  if (!strcmp(verb, "kill")) {
    client *f = focused();
    if (!f) return -1;
    if (!f->close_asked) { f->close_asked = 1; send_event(f, UI_EV_CLOSE, 0, 0, 0, 0, 0, 0, 0, 0); }
    else { /* asked before and still here: force it */
      fprintf(stderr, "wm: client %d (%s) ignored the close request; forcing it\n", f->pid, f->app_id);
      if (f->pid > 1 && is_child(f->pid)) __syscall2(SYS_kill, f->pid, SIGKILL);
      drop_client(f);
    }
    return 0;
  }
  if (!strcmp(verb, "zoom")) { /* the focused window redraws its own content bigger or smaller */
    int d = !strcmp(arg, "in") ? 1 : !strcmp(arg, "out") ? -1 : !strcmp(arg, "reset") ? 0 : 2;
    client *f = focused();
    if (d == 2 || !f) return -1;
    send_event(f, UI_EV_ZOOM, 0, 0, 0, 0, 0, 0, 0, d);
    return 0;
  }
  if (!strcmp(verb, "pointer-lock")) { /* from a window: capture the mouse while I am fullscreen */
    if (!src) return -1;
    src->wants_lock = !strcmp(arg, "on");
    lock_update();
    return 0;
  }
  if (!strcmp(verb, "theme")) return theme_set(*arg ? arg : "next");
  if (!strcmp(verb, "reload")) { config_load(0); theme_reload(); fprintf(stderr, "wm: configuration reloaded\n"); return 0; }
  if (!strcmp(verb, "exit")) { quit = 1; return 0; }
  if (!strcmp(verb, "stats")) { print_stats(); return 0; }
  if (!strcmp(verb, "trace")) { trace = !strcmp(arg, "on"); return 0; }
  if (!strcmp(verb, "animation")) { /* length of the workspace slide in ms, until the next reload; 0 = off */
    int ms = atoi(arg);
    cfg.animation_ms = ms < 0 ? 0 : ms > 5000 ? 5000 : ms;
    return 0;
  }
  if (!strcmp(verb, "bench")) {
    int n = atoi(arg) > 0 ? atoi(arg) : 20;
    double ms = bench_compose(n);
    fprintf(stderr, "wm: bench %dx%d full-screen composite + framebuffer write: %.3f ms per frame (%d frames)\n", scr_w, scr_h, ms, n);
    return 0;
  }
  (void)src;
  return layout_command(verb, arg);
}

/* ---------- clients ---------- */
static client *new_client(int fd) {
  if (nclients >= MAX_CLIENTS) return NULL;
  client *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->fd = fd;
  c->ws = cur_ws;
  order[nclients++] = c;
  return c;
}
void drop_client(client *c) {
  int i = 0;
  while (i < nclients && order[i] != c) i++;
  if (i == nclients) return;
  if (c->hello) client_removed(c);
  memmove(order + i, order + i + 1, (nclients - i - 1) * sizeof order[0]);
  nclients--;
  if (grab == c) grab = NULL;
  if (locked == c) { locked = NULL; int off = 0; __syscall3(SYS_ioctl, ev_fd, EVIOCGRAB, &off); cursor_moved(ptr_x, ptr_y); }
  if (drag == c) drag = NULL;
  close(c->fd);
  if (c->hello) relayout(); /* repaints the screen and tells the bar */
  free(c->buf); free(c->out);
  free(c);
}

static int handle_hello(client *c) {
  struct uip_hello h;
  if (c->cur.len < sizeof h) { drop_client(c); return 0; }
  memcpy(&h, c->ctl, sizeof h);
  c->pid = h.pid;
  c->role = h.role == UI_BAR ? UI_BAR : h.role == UI_FLOATING ? UI_FLOATING : UI_NORMAL;
  c->fullscreen = h.role == UI_FULLSCREEN;
  c->floating = c->role == UI_FLOATING;
  c->req_w = h.w < 1 ? 1 : h.w > UIP_MAX_DIM ? UIP_MAX_DIM : h.w;
  c->req_h = h.h < 1 ? 1 : h.h > UIP_MAX_DIM ? UIP_MAX_DIM : h.h;
  memcpy(c->app_id, h.app_id, sizeof c->app_id);
  c->app_id[sizeof c->app_id - 1] = 0;
  unsigned tl = c->cur.len - sizeof h;
  if (tl > UIP_TITLE_MAX) tl = UIP_TITLE_MAX;
  memcpy(c->title, c->ctl + sizeof h, tl);
  c->title[tl] = 0;
  c->hello = 1;
  client_added(c);
  return 1;
}

/* A complete control message. Returns 0 if the client was dropped. */
static int handle_ctl(client *c) {
  int t = c->cur.type;
  c->ctl[c->cur.len] = 0;
  if (!c->hello && t != UIP_HELLO) { drop_client(c); return 0; }
  switch (t) {
  case UIP_HELLO:
    if (c->hello) { drop_client(c); return 0; }
    return handle_hello(c);
  case UIP_TITLE:
    strlcpy(c->title, (char *)c->ctl, sizeof c->title);
    if (c == focused()) state_changed();
    return 1;
  case UIP_COMMAND: {
    int st = run_command(c, (char *)c->ctl);
    for (int i = 0; i < nclients; i++)
      if (order[i] == c) { queue_msg(c, UIP_REPLY, 0, &st, 4); return 1; }
    return 0; /* the command removed this very client */
  }
  case UIP_GETSTATE: {
    struct uip_state s;
    fill_state(&s);
    queue_msg(c, UIP_STATE, UIP_F_REPLY, &s, sizeof s);
    return 1;
  }
  }
  drop_client(c);
  return 0;
}

/* The PRESENT sub-header is in: check it and get the buffer ready. */
static int begin_present(client *c) {
  struct uip_present *p = &c->pp;
  memcpy(p, c->ctl, sizeof *p);
  if (!c->hello || p->bw < 1 || p->bh < 1 || p->bw > UIP_MAX_DIM || p->bh > UIP_MAX_DIM ||
      (long long)p->bw * p->bh > UIP_MAX_AREA || p->x < 0 || p->y < 0 || p->w < 1 || p->h < 1 ||
      p->x > p->bw - p->w || p->y > p->bh - p->h ||
      (unsigned long long)c->cur.len != sizeof *p + (unsigned long long)p->w * p->h * 4)
    return -1;
  if (p->bw != c->pw || p->bh != c->ph) {
    /* Free first: the old pixels are dropped anyway, and wasm memory never shrinks, so
     * allocating before freeing would leave both sizes claimed for good. */
    free(c->buf);
    c->buf = calloc((size_t)p->bw * p->bh, 4);
    c->pw = c->ph = 0;
    if (!c->buf) return -1;
    c->pw = p->bw; c->ph = p->bh;
  }
  c->in_pixels = 1;
  c->px_got = 0;
  c->px_total = (unsigned)p->w * p->h * 4;
  return 0;
}
static void end_present(client *c) {
  c->in_pixels = 0;
  c->hdr_got = 0;
  if (!visible(c)) return;
  rect cr;
  content_rect(c, &cr);
  int x = c->pp.x, y = c->pp.y, w = c->pp.w, h = c->pp.h;
  if (x + w > cr.w) w = cr.w - x;
  if (y + h > cr.h) h = cr.h - y;
  if (w > 0 && h > 0) {
    damage(cr.x + x, cr.y + y, w, h);
    if (trace) c->traced = 1;
  }
}

/* Feed received bytes through the state machine. Returns 0 if the client was dropped. */
static int feed(client *c, const unsigned char *p, unsigned n) {
  while (n) {
    if (c->in_pixels) { /* stream pixel bytes straight into the client's buffer */
      unsigned row = (unsigned)c->pp.w * 4;
      while (n && c->px_got < c->px_total) {
        unsigned r = c->px_got / row, col = c->px_got % row, k = row - col;
        if (k > n) k = n;
        unsigned char *dst = (unsigned char *)(c->buf + (size_t)(c->pp.y + r) * c->pw + c->pp.x) + col;
        memcpy(dst, p, k);
        p += k; n -= k; c->px_got += k;
      }
      if (c->px_got == c->px_total) end_present(c);
      continue;
    }
    if (c->hdr_got < UIP_HDR) {
      if (c->hdr_got == 0 && trace) c->t_first = now_ms();
      unsigned k = UIP_HDR - c->hdr_got;
      if (k > n) k = n;
      memcpy(c->hdr + c->hdr_got, p, k);
      c->hdr_got += k; p += k; n -= k;
      if (c->hdr_got < UIP_HDR) break;
      memcpy(&c->cur, c->hdr, UIP_HDR);
      c->ctl_got = 0;
      int t = c->cur.type;
      int ok = t == UIP_PRESENT ? c->cur.len > sizeof(struct uip_present)
               : (t >= UIP_HELLO && t <= UIP_GETSTATE && c->cur.len <= UIP_CTL_MAX);
      if (!ok) { fprintf(stderr, "wm: client %d sent a bad message (type %d, length %u); closing it\n", c->pid, t, c->cur.len); drop_client(c); return 0; }
    }
    unsigned want = c->cur.type == UIP_PRESENT ? sizeof(struct uip_present) : c->cur.len;
    unsigned k = want - c->ctl_got;
    if (k > n) k = n;
    memcpy(c->ctl + c->ctl_got, p, k);
    c->ctl_got += k; p += k; n -= k;
    if (c->ctl_got < want) break;
    if (c->cur.type == UIP_PRESENT) {
      if (begin_present(c) < 0) { fprintf(stderr, "wm: client %d sent a bad PRESENT; closing it\n", c->pid); drop_client(c); return 0; }
      continue;
    }
    c->hdr_got = 0;
    if (!handle_ctl(c)) return 0;
  }
  return 1;
}

static void client_readable(client *c) {
  static unsigned char buf[65536];
  long total = 0;
  while (total < 4 * 1024 * 1024) { /* bounded, so one busy client cannot starve the others */
    long n = __syscall3(SYS_read, c->fd, buf, sizeof buf);
    if (n == -EAGAIN) return;
    if (n <= 0) { drop_client(c); return; } /* hangup (or error): the window goes away */
    total += n;
    if (!feed(c, buf, n)) return;
  }
}

/* ---------- input ---------- */
struct input_event { long long sec, usec; unsigned short type, code; int value; };

static int modbit(int code) {
  switch (code) {
  case 42: case 54: return UI_SHIFT;
  case 29: case 97: return UI_CTRL;
  case 56: case 100: return UI_ALT;
  case 125: case 126: return UI_SUPER;
  }
  return 0;
}
static void to_window(client *c, int *x, int *y) {
  rect cr;
  content_rect(c, &cr);
  *x = ptr_x - cr.x; *y = ptr_y - cr.y;
}
static void handle_key(int code, int value) {
  int mb = modbit(code);
  if (mb) {
    if (value) mods |= mb; else mods &= ~mb;
    /* keep the bit if the other side's key is still held: simplification, both sides share it */
  }
  if (code < 256 && !mb && value == 1) {
    for (int i = 0; i < cfg.nbinds; i++)
      if (cfg.binds[i].key == code && cfg.binds[i].mods == mods) {
        swallow[code] = 1;
        swallow_text = 1;
        run_command(NULL, cfg.binds[i].cmd);
        return;
      }
  }
  if (code < 256 && swallow[code]) { /* the repeat or release of a bound key */
    if (value == 0) swallow[code] = 0;
    swallow_text = 1;
    return;
  }
  client *f = focused();
  send_event(f, value ? UI_EV_KEY_DOWN : UI_EV_KEY_UP, code, mods, value == 2, 0, 0, 0, 0, 0);
}
/* Move the window being dragged so that its top-left corner is at x, y, kept on the screen. */
static void drag_to(int x, int y) {
  client *c = drag;
  if (!c->floating || !visible(c)) { drag = NULL; return; } /* it stopped floating or left the screen */
  if (x > scr_w - c->r.w) x = scr_w - c->r.w;
  if (y > scr_h - c->r.h) y = scr_h - c->r.h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x == c->r.x && y == c->r.y) return;
  damage(c->r.x, c->r.y, c->r.w, c->r.h);
  c->r.x = x; c->r.y = y;
  c->placed = 1;
  damage(c->r.x, c->r.y, c->r.w, c->r.h);
}
static void handle_button(int code, int value) {
  int b = code == 0x110 ? 0 : code == 0x112 ? 1 : code == 0x111 ? 2 : -1;
  if (b < 0) return;
  if (drag) { /* the window being moved gets no clicks; the left button's release ends the move */
    if (value) buttons |= 1 << b; else buttons &= ~(1 << b);
    if (!value && b == 0) drag = NULL;
    return;
  }
  client *c = value ? client_at(ptr_x, ptr_y) : grab;
  if (value && b == 0 && (mods & cfg.mod) && c && c->floating && !buttons) {
    set_focus(c); /* also raises it */
    buttons |= 1;
    drag = c;
    drag_dx = ptr_x - c->r.x; drag_dy = ptr_y - c->r.y;
    return;
  }
  if (value) {
    buttons |= 1 << b;
    grab = c;
    if (c && c->role != UI_BAR && c != focused()) set_focus(c); /* focus follows click */
  } else {
    buttons &= ~(1 << b);
    if (!buttons) grab = NULL;
  }
  if (c) {
    int x, y;
    to_window(c, &x, &y);
    send_event(c, value ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP, 0, mods, 0, 0, x, y, b, 0);
  }
}
static void input_readable(void) {
  static struct input_event evs[64];
  static int nx = -1, ny = -1, rdx, rdy, resized;
  for (;;) {
    long n = __syscall3(SYS_read, ev_fd, evs, sizeof evs);
    if (n <= 0) break;
    for (int i = 0; i < n / (long)sizeof evs[0]; i++) {
      struct input_event *e = &evs[i];
      switch (e->type) {
      case 1: /* EV_KEY */
        if (e->code >= 0x110 && e->code <= 0x112) handle_button(e->code, e->value);
        else if (e->code < 256) handle_key(e->code, e->value);
        break;
      case 2: /* EV_REL */
        if (e->code == 0) rdx += e->value;         /* movement while the mouse is captured */
        else if (e->code == 1) rdy += e->value;
        else if (e->code == 8) {
          client *c = client_at(ptr_x, ptr_y);
          if (c) { int x, y; to_window(c, &x, &y); send_event(c, UI_EV_WHEEL, 0, mods, 0, 0, x, y, 0, e->value > 0 ? 1 : -1); }
        }
        break;
      case 3: /* EV_ABS */
        if (e->code == 0) nx = e->value;
        else if (e->code == 1) ny = e->value;
        break;
      case 4: /* EV_MSC */
        if (e->code == 0x10) { if (!swallow_text) send_event(focused(), UI_EV_TEXT, 0, mods, 0, (unsigned)e->value, 0, 0, 0, 0); }
        else if (e->code == 0x11) resized = 1;
        break;
      case 0: /* EV_SYN: the end of one batch */
        swallow_text = 0;
        if (rdx || rdy) {
          if (locked) send_event(locked, UI_EV_MOUSE_REL, 0, mods, 0, 0, rdx, rdy, 0, 0);
          rdx = rdy = 0;
        }
        if (locked) nx = ny = -1;                  /* captured: the cursor stays hidden and still */
        if (nx >= 0 || ny >= 0) {
          int ox = ptr_x, oy = ptr_y;
          if (nx >= 0) ptr_x = nx;
          if (ny >= 0) ptr_y = ny;
          nx = ny = -1;
          if (ptr_x < 0) ptr_x = 0;
          if (ptr_y < 0) ptr_y = 0;
          if (ptr_x >= scr_w) ptr_x = scr_w - 1;
          if (ptr_y >= scr_h) ptr_y = scr_h - 1;
          if (ox != ptr_x || oy != ptr_y || !ptr_shown) {
            cursor_moved(ox, oy);
            client *c = drag ? NULL : grab ? grab : client_at(ptr_x, ptr_y);
            if (drag) drag_to(ptr_x - drag_dx, ptr_y - drag_dy);
            if (c) { int x, y; to_window(c, &x, &y); send_event(c, UI_EV_MOUSE_MOVE, 0, mods, 0, 0, x, y, 0, 0); }
          }
        }
        if (resized) {
          resized = 0;
          fb_setup(-1); /* new size: reallocate the back buffer and the wallpaper */
          render_wallpaper();
          relayout();
          damage_all();
        }
        break;
      }
    }
  }
}

static void setup_env(void) {
  /* started as pid 1 there is no environment at all: give children a sane one */
  if (!getenv("PATH")) setenv("PATH", "/bin", 1);
  if (!getenv("HOME")) setenv("HOME", "/home/user", 1);
  if (!getenv("USER")) setenv("USER", "user", 1);
  if (!getenv("TERM")) setenv("TERM", "xterm-256color", 1);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  setup_env();
  int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (fb < 0 || fb_setup(fb) < 0) { fprintf(stderr, "wm: no framebuffer\n"); return 1; }
  ev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  /* the socket goes in the user's runtime directory (init makes /run/user/UID, as logind
   * does); /run/wm.sock when there is none (started by hand, or as root) */
  struct { unsigned short fam; char path[108]; } addr = {1, "/run/wm.sock"};
  const char *rt = getenv("XDG_RUNTIME_DIR");
  struct stat rst;
  if (rt && *rt && !stat(rt, &rst) && S_ISDIR(rst.st_mode) && strlen(rt) + 9 < sizeof addr.path) snprintf(addr.path, sizeof addr.path, "%s/wm.sock", rt);
  else mkdir("/run", 0755);
  strlcpy(sock_path, addr.path, sizeof sock_path);
  unlink(sock_path);
  listen_fd = __syscall3(SYS_socket, 1, 1 | O_NONBLOCK | O_CLOEXEC, 0);
  if (listen_fd < 0 || __syscall3(SYS_bind, listen_fd, &addr, sizeof addr) < 0 || __syscall2(SYS_listen, listen_fd, 16) < 0) {
    fprintf(stderr, "wm: cannot listen on %s\n", sock_path);
    return 1;
  }
  setenv("WM_SOCKET", sock_path, 1);
  /* a client that dies while the manager writes to it must not take the manager with it:
   * writes to clients use MSG_NOSIGNAL (flush_out), so SIGPIPE stays at its default here and
   * in every program the manager starts */
  ui_theme_load(&theme, NULL);
  config_load(1);
  for (int i = 0; i < MAX_WS; i++) master_of[i] = cfg.master;
  render_wallpaper();
  ptr_x = scr_w / 2; ptr_y = scr_h / 2;
  damage_all();
  compose();
  fprintf(stderr, "wm: %dx%d, theme %s, listening on %s\n", scr_w, scr_h, theme.name, sock_path);
  for (int i = 0; i < cfg.nauto; i++) wm_spawn(cfg.autostart[i]);

  double next_frame = 0;
  while (!quit) {
    reap();
    struct pollfd pf[3 + MAX_CLIENTS];
    client *who[3 + MAX_CLIENTS];
    int n = 0;
    pf[n].fd = ev_fd; pf[n].events = POLLIN; who[n++] = NULL;
    pf[n].fd = listen_fd; pf[n].events = POLLIN; who[n++] = NULL;
    for (int i = 0; i < nclients; i++) {
      pf[n].fd = order[i]->fd;
      pf[n].events = POLLIN | (order[i]->outlen ? POLLOUT : 0);
      who[n++] = order[i];
    }
    int timeout = -1;
    if (damage_pending()) {
      double left = next_frame - now_ms();
      timeout = left <= 0 ? 0 : (int)left + 1;
    }
    int r = poll(pf, n, timeout);
    if (r > 0) {
      if (pf[0].revents & POLLIN) input_readable();
      if (pf[1].revents & POLLIN) {
        for (;;) {
          int fd = __syscall1(SYS_accept, listen_fd);
          if (fd < 0) break;
          fcntl(fd, F_SETFD, FD_CLOEXEC);
          fcntl(fd, F_SETFL, O_NONBLOCK);
          if (!new_client(fd)) { fprintf(stderr, "wm: too many clients (%d); refusing one\n", MAX_CLIENTS); close(fd); }
        }
      }
      for (int i = 2; i < n; i++) {
        client *c = who[i];
        int alive = 0;
        for (int j = 0; j < nclients; j++) if (order[j] == c) alive = 1;
        if (!alive || !pf[i].revents) continue; /* removed by an earlier command in this round */
        if (pf[i].revents & POLLOUT) flush_out(c);
        if (pf[i].revents & (POLLIN | POLLHUP | POLLERR | 0x20)) client_readable(c);
      }
    }
    lock_update();
    if (state_dirty) { state_dirty = 0; broadcast_state(); }
    if (damage_pending() && now_ms() >= next_frame) {
      compose();
      next_frame = now_ms() + 15;
      if (trace) {
        double t = now_ms();
        for (int i = 0; i < nclients; i++)
          if (order[i]->traced) {
            order[i]->traced = 0;
            fprintf(stderr, "wm: trace t=%.3f present %dx%d from pid %d: first byte -> framebuffer %.3f ms\n",
                    t, order[i]->pp.w, order[i]->pp.h, order[i]->pid, t - order[i]->t_first);
          }
      }
    }
  }
  fprintf(stderr, "wm: exit\n");
  for (int i = 0; i < nclients; i++) close(order[i]->fd);
  unlink(sock_path);
  return 0;
}
