// SPDX-License-Identifier: GPL-2.0-or-later
/* term - xterm-compatible terminal emulator for UC.OS.
 *
 *   term [--fb] [-T title] [--font-size N] [--hold] [--stats] [-e cmd args...]
 *   term --demo NAME ...   built-in test programs (see demo.c)
 *
 * Started as pid 1 (headless tests) it behaves as `term --fb --stats`. */
#include "vt.h"
#include "render.h"
#include "backend.h"
#include "keys.h"
#include "theme.h"
#include "clip.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

int demo_main(int argc, char **argv);

#define FALLBACK_FONT "/usr/share/fonts/JetBrainsMonoNerdFontMono-Regular.ttf"
#define FRAME_MS 16.0

static struct {
  backend *be;
  vt *t;
  render r;
  ui_theme th;
  unsigned char *fdata[3];
  ttf_font font[3];
  char fpath[3][128];
  int font_size;          /* --font-size, 0 = theme */
  int zoom;               /* zoom steps away from that size (see font_px) */
  int ptm, child, dead, hold, exit_status;
  int pad, swallow_text, title_fixed, mouse_buttons;
  int stats;
  /* statistics */
  double st_t0, st_render_ms, st_render_max, st_parse_ms;
  long st_frames, st_bytes;
} T;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void die(const char *msg) {
  fprintf(stderr, "term: %s\n", msg);
  exit(1);
}

/* ---------- fonts and theme ---------- */

static int load_font(int i, const char *path) {
  if (T.fdata[i] && !strcmp(T.fpath[i], path)) return 0;
  free(T.fdata[i]);
  T.fdata[i] = NULL;
  T.fpath[i][0] = 0;
  unsigned len;
  unsigned char *d = ui_read_file(path, &len);
  if (!d) return -1;
  if (ttf_open(&T.font[i], d, len)) { free(d); return -1; }
  T.fdata[i] = d;
  strncpy(T.fpath[i], path, sizeof T.fpath[i] - 1);
  return 0;
}

/* Zoom (UI_EV_ZOOM from the window manager) moves the text size in steps of ZOOM_STEP pixels. */
#define ZOOM_STEP 2
#define ZOOM_MIN_PX 8
#define ZOOM_MAX_PX 72
static int base_px(void) { return T.font_size > 0 ? T.font_size : T.th.font_size > 0 ? T.th.font_size : 16; }
static int font_px(void) {
  int px = base_px() + T.zoom * ZOOM_STEP;
  return px < ZOOM_MIN_PX ? ZOOM_MIN_PX : px > ZOOM_MAX_PX ? ZOOM_MAX_PX : px;
}

static void setup_fonts(void) {
  if (load_font(0, T.th.font) && load_font(0, "/usr/share/fonts/FiraCode-Regular.ttf")) die("cannot load a font");
  int bold = load_font(1, T.th.font_bold) == 0;
  /* a Nerd Font supplies powerline and icon glyphs that Fira Code lacks */
  int fb = !strstr(T.fpath[0], "Nerd") && load_font(2, FALLBACK_FONT) == 0;
  int px = font_px();
  if (render_init(&T.r, &T.font[0], bold ? &T.font[1] : NULL, fb ? &T.font[2] : NULL, (float)px)) die("out of memory");
  render_colors(&T.r, T.th.fg, T.th.bg, T.th.accent, T.th.color);
  T.pad = 8; /* padding around the grid */
}

/* ---------- pty ---------- */

static void pty_write(const char *p, size_t n) {
  while (n > 0) {
    long w = write(T.ptm, p, n);
    if (w <= 0) break;
    p += w; n -= (size_t)w;
  }
}

/* Pasted text goes to the pty PASTE_CHUNK bytes at a time, PASTE_MS apart. The pty keeps
 * only 4 KiB of input for the program and drops what does not fit, so a big paste written
 * at once would lose its middle. Keys typed while a paste is going out wait behind it. */
#define PASTE_CHUNK 1024
#define PASTE_MS 4
static char *pq;
static size_t pq_off, pq_len, pq_cap;
static double pq_last;
static void pq_add(const void *s, size_t n) {
  if (pq_len + n > pq_cap) {
    size_t cap = pq_cap ? pq_cap : 4096;
    while (cap < pq_len + n) cap *= 2;
    char *b = realloc(pq, cap);
    if (!b) return; /* no memory: this piece is lost, the rest still goes */
    pq = b; pq_cap = cap;
  }
  memcpy(pq + pq_len, s, n);
  pq_len += n;
}
static int pq_busy(void) { return pq_off < pq_len; }
static void pq_pump(double now) {
  if (!pq_busy() || now - pq_last < PASTE_MS || T.dead) return;
  size_t n = pq_len - pq_off < PASTE_CHUNK ? pq_len - pq_off : PASTE_CHUNK;
  pty_write(pq + pq_off, n);
  pq_off += n;
  pq_last = now;
  if (pq_off == pq_len) pq_off = pq_len = 0;
}

static void send(const void *s, int n) {
  if (T.dead || n <= 0) return;
  vt_view_bottom(T.t);
  if (pq_busy()) pq_add(s, (size_t)n); /* keep the order: after the paste */
  else pty_write(s, (size_t)n);
}

static void apply_size(void) {
  int cols, rows;
  render_grid_size(&T.r, T.be->w, T.be->h, T.pad, &cols, &rows);
  if (!T.t) {
    T.t = vt_new(cols, rows, 5000);
    if (!T.t) die("out of memory");
  } else vt_resize(T.t, cols, rows);
  render_layout(&T.r, T.pad, T.pad);
  if (T.ptm > 0) {
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, (unsigned short)(cols * T.r.cw), (unsigned short)(rows * T.r.ch)};
    __syscall3(SYS_ioctl, T.ptm, TIOCSWINSZ, &ws);
  }
}

static void reload_theme(void) {
  ui_theme old = T.th;
  ui_theme_load(&T.th, NULL);
  if (strcmp(old.font, T.th.font) || strcmp(old.font_bold, T.th.font_bold) || old.font_size != T.th.font_size || old.gap != T.th.gap)
    setup_fonts();
  else
    render_colors(&T.r, T.th.fg, T.th.bg, T.th.accent, T.th.color);
}

static char *find_exe(const char *name, const char *path, char *buf, size_t cap) {
  if (strchr(name, '/')) return (char *)name;
  for (const char *p = path; p && *p;) {
    const char *e = strchr(p, ':');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n + strlen(name) + 2 < cap) {
      memcpy(buf, p, n); buf[n] = '/'; strcpy(buf + n + 1, name);
      struct stat st;
      if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)) return buf;
    }
    p += n + (e ? 1 : 0);
  }
  return NULL;
}

/* environment for the child: ours, with the terminal variables set */
static char **child_env(void) {
  static char *env[128];
  static char theme[64];
  extern char **environ;
  static const char *defaults[] = {"PATH=/bin", "HOME=/home/user", "USER=user", "SHELL=/bin/sh"};
  int n = 0;
  snprintf(theme, sizeof theme, "THEME=%s", T.th.name[0] ? T.th.name : "deck");
  env[n++] = "TERM=xterm-256color";
  env[n++] = "COLORTERM=truecolor";
  env[n++] = theme;
  for (char **e = environ; e && *e && n < 100; e++)
    if (strncmp(*e, "TERM=", 5) && strncmp(*e, "COLORTERM=", 10) && strncmp(*e, "THEME=", 6) &&
        strncmp(*e, "COLUMNS=", 8) && strncmp(*e, "LINES=", 6)) env[n++] = *e;
  for (int d = 0; d < 4; d++) {
    size_t k = strchr(defaults[d], '=') - defaults[d] + 1;
    int have = 0;
    for (int i = 0; i < n; i++) if (!strncmp(env[i], defaults[d], k)) have = 1;
    if (!have) env[n++] = (char *)defaults[d];
  }
  env[n] = NULL;
  return env;
}

static void start_child(char **argv) {
  T.ptm = open("/dev/ptmx", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (T.ptm < 0) die("cannot open /dev/ptmx");
  int n = -1;
  if (__syscall3(SYS_ioctl, T.ptm, TIOCGPTN, &n) != 0) die("TIOCGPTN failed");
  apply_size(); /* sends TIOCSWINSZ */
  char path[32];
  snprintf(path, sizeof path, "/dev/pts/%d", n);
  int pts = open(path, O_RDWR);
  if (pts < 0) die("cannot open the pty slave");
  char **env = child_env();
  const char *p = NULL;
  for (char **e = env; *e; e++) if (!strncmp(*e, "PATH=", 5)) p = *e + 5;
  char buf[256];
  char *exe = find_exe(argv[0], p, buf, sizeof buf);
  int map[3] = {pts, pts, pts};
  T.child = exe ? spawn(exe, argv, env, map) : -1;
  if (T.child < 0) {
    char msg[300];
    int len = snprintf(msg, sizeof msg, "term: cannot run %s\r\n", argv[0]);
    vt_feed(T.t, msg, (size_t)len);
    fprintf(stderr, "term: cannot run %s\n", argv[0]);
    T.dead = 1; T.hold = 1;
  } else {
    /* own process group in the foreground, so that Ctrl-C reaches it (a shell
     * becomes a session leader and takes over from here) */
    __syscall2(SYS_setpgid, T.child, T.child);
    __syscall3(SYS_ioctl, T.ptm, TIOCSPGRP, &T.child);
  }
  close(pts);
}

static void child_gone(void) {
  if (T.dead) return;
  T.dead = 1;
  int status = 0;
  if (T.child > 0 && waitpid(T.child, &status, 0) == T.child) T.exit_status = status;
  if (!T.hold) return;
  char msg[96];
  int n = snprintf(msg, sizeof msg, "\r\n\x1b[0;7m[process exited with status %d: press a key to close]\x1b[0m",
                   WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
  vt_feed(T.t, msg, (size_t)n);
}

/* ---------- input ---------- */

static int cell_at(int px, int py, int *cx, int *cy) {
  *cx = (px - T.pad) / T.r.cw;
  *cy = (py - T.pad) / T.r.ch;
  if (*cx < 0) *cx = 0;
  if (*cy < 0) *cy = 0;
  if (*cx >= T.t->cols) *cx = T.t->cols - 1;
  if (*cy >= T.t->rows) *cy = T.t->rows - 1;
  return 0;
}

/* ---------- selection, copy and paste ---------- */
/* Drag with the left button to select; a double click selects a word, a triple click a
 * line. When the program in the terminal asked for the mouse (htop, vi), Shift+drag
 * selects. Ctrl+Shift+C copies the selection to the clipboard (clipd), Ctrl+Shift+V pastes
 * its newest entry. Positions are absolute lines (0 = the oldest scrollback line) so that
 * scrolling the view does not move the selection. Output that changes a selected line, or
 * scrolls the screen, ends the selection. */
static struct {
  int on, drag, mode;          /* mode: 0 characters, 1 words, 2 lines */
  int al, ac, el, ec;          /* anchor and moving end: absolute line, column */
  int sb_len, sb_head, alt;    /* the scrollback and screen it was made on */
  int clicks, cl, cc;          /* multi-click detection */
  double click_ms;
} S;

static int abs_line(int y) { return T.t->sb_len - T.t->view + y; }
static const vt_cell *line_cells(int l, int *w) { return vt_view_row(T.t, l - T.t->sb_len + T.t->view, w); }
static int is_word(uint32_t c) { return c != ' ' && !(c < 128 && strchr("\t\"'`()[]{}<>|;,", (int)c)); }

/* The selection in reading order, words and lines expanded. */
static void sel_bounds(int *l0, int *c0, int *l1, int *c1) {
  int al = S.al, ac = S.ac, el = S.el, ec = S.ec, w;
  if (el < al || (el == al && ec < ac)) { int t = al; al = el; el = t; t = ac; ac = ec; ec = t; }
  if (S.mode == 2) { ac = 0; ec = T.t->cols - 1; }
  else if (S.mode == 1) {
    const vt_cell *r = line_cells(al, &w);
    if (r && ac < w && is_word(r[ac].ch)) while (ac > 0 && is_word(r[ac - 1].ch)) ac--;
    r = line_cells(el, &w);
    if (r && ec < w && is_word(r[ec].ch)) while (ec + 1 < w && is_word(r[ec + 1].ch)) ec++;
  }
  *l0 = al; *c0 = ac; *l1 = el; *c1 = ec;
}
/* Tell the renderer, in window rows. */
static void sel_sync(void) {
  render *r = &T.r;
  r->sel_on = S.on;
  if (!S.on) return;
  int l0, c0, l1, c1, base = abs_line(0);
  sel_bounds(&l0, &c0, &l1, &c1);
  r->sel_y0 = l0 - base; r->sel_x0 = c0; r->sel_y1 = l1 - base; r->sel_x1 = c1;
}
static void sel_clear(void) {
  if (S.on) T.t->all_dirty = 1;
  S.on = S.drag = 0;
}
static void sel_press(int px, int py) {
  vt *t = T.t;
  int cx, cy;
  cell_at(px, py, &cx, &cy);
  int l = abs_line(cy);
  double now = now_ms();
  S.clicks = now - S.click_ms < 400 && l == S.cl && cx == S.cc ? S.clicks % 3 + 1 : 1;
  S.click_ms = now; S.cl = l; S.cc = cx;
  sel_clear();
  S.mode = S.clicks - 1;
  S.al = S.el = l; S.ac = S.ec = cx;
  S.drag = 1;
  S.sb_len = t->sb_len; S.sb_head = t->sb_head; S.alt = t->alt_active;
  if (S.mode) { S.on = 1; t->all_dirty = 1; } /* a word or a line is selected at once */
}
static void sel_move(int px, int py) {
  int cx, cy;
  cell_at(px, py, &cx, &cy);
  int l = abs_line(cy);
  if (l == S.el && cx == S.ec && S.on) return;
  if (l == S.al && cx == S.ac && !S.on) return; /* not moved off the first cell yet */
  S.el = l; S.ec = cx;
  S.on = 1;
  T.t->all_dirty = 1;
}
/* After output: drop the selection if what it covers changed. pre = the rows' dirty
 * flags before the output, was_all = all_dirty before it. */
static void sel_after_output(const uint8_t *pre, int was_all) {
  vt *t = T.t;
  if (!S.on && !S.drag) return;
  if (t->sb_len != S.sb_len || t->sb_head != S.sb_head || t->alt_active != S.alt || (!was_all && t->all_dirty)) { sel_clear(); return; }
  if (!S.on) return;
  int l0, c0, l1, c1, base = abs_line(0);
  sel_bounds(&l0, &c0, &l1, &c1);
  for (int y = l0 - base < 0 ? 0 : l0 - base; y <= l1 - base && y < t->rows; y++)
    if (t->dirty[y] && !pre[y]) { sel_clear(); return; }
}
/* The selected text: UTF-8, lines joined with \n, blanks at the end of each line dropped. */
static char *sel_text(size_t *len) {
  int l0, c0, l1, c1;
  sel_bounds(&l0, &c0, &l1, &c1);
  size_t cap = (size_t)(l1 - l0 + 1) * ((size_t)T.t->cols * 4 + 1) + 1, n = 0;
  char *o = malloc(cap);
  if (!o) return NULL;
  for (int l = l0; l <= l1; l++) {
    int w;
    const vt_cell *r = line_cells(l, &w);
    int from = l == l0 ? c0 : 0, to = l == l1 ? c1 : w - 1;
    if (to >= w) to = w - 1;
    size_t end = n;
    for (int x = from; r && x <= to; x++) {
      n += (size_t)vt_utf8_encode(r[x].ch, o + n);
      if (r[x].ch != ' ') end = n;
    }
    n = end;
    if (l < l1) o[n++] = '\n';
  }
  o[n] = 0;
  *len = n;
  return o;
}
static void copy_selection(void) {
  if (!S.on) return;
  size_t n;
  char *text = sel_text(&n);
  if (!text) return;
  int r = clip_put(text, n);
  if (r) fprintf(stderr, "term: copy failed (%d)\n", r);
  free(text);
}
/* The newest clipboard entry, typed into the pty: line feeds become carriage returns (what
 * the Enter key sends), and inside a bracketed paste ESC bytes are left out, so that the
 * text cannot end the bracket early. */
static void paste(void) {
  char *d;
  size_t n;
  int r = clip_get(0, &d, &n);
  if (r) { if (r != -ENOENT) fprintf(stderr, "term: paste failed (%d)\n", r); return; }
  int br = T.t->bracketed_paste;
  vt_view_bottom(T.t);
  if (br) pq_add("\x1b[200~", 6);
  size_t a = 0;
  for (size_t i = 0; i <= n; i++) { /* copy runs of plain bytes, fix up the rest */
    unsigned char ch = i < n ? (unsigned char)d[i] : 0;
    if (i < n && ch != '\n' && ch != '\r' && !(br && ch == 0x1b)) continue;
    pq_add(d + a, i - a);
    a = i + 1;
    if (i == n) break;
    if (ch == '\r' && i + 1 < n && d[i + 1] == '\n') { i++; a = i + 1; } /* CR LF: one Enter */
    if (ch != 0x1b) pq_add("\r", 1);
  }
  if (br) pq_add("\x1b[201~", 6);
  free(d);
  pq_last = 0;
  pq_pump(now_ms()); /* the first piece at once */
}

/* xterm mouse report: b = button code (0..2, 3 release, 64/65 wheel, +32 motion) */
static void mouse_report(int b, int px, int py, int release, int mods) {
  int cx, cy;
  cell_at(px, py, &cx, &cy);
  if (mods & KM_SHIFT) b |= 4;
  if (mods & KM_ALT) b |= 8;
  if (mods & KM_CTRL) b |= 16;
  char s[48];
  int n;
  if (T.t->mouse_sgr) n = snprintf(s, sizeof s, "\x1b[<%d;%d;%d%c", b, cx + 1, cy + 1, release ? 'm' : 'M');
  else {
    if (release) b = (b & ~3) | 3;
    n = snprintf(s, sizeof s, "\x1b[M%c%c%c", 32 + b, 32 + (cx + 1 > 222 ? 222 : cx + 1), 32 + (cy + 1 > 222 ? 222 : cy + 1));
  }
  send(s, n);
}

/* test hook (Ctrl+Alt+Shift+F12): the visible text to the console, for test scripts */
static void dump_screen(void) {
  static char line[4096];
  fprintf(stderr, "[screen-begin] %dx%d cursor %d,%d view %d alt %d cell %dx%d pad %d\n", T.t->cols, T.t->rows, T.t->x, T.t->y,
          T.t->view, T.t->alt_active, T.r.cw, T.r.ch, T.pad);
  for (int y = 0; y < T.t->rows; y++) { vt_row_text(T.t, y, line, sizeof line); fprintf(stderr, "[screen] %s\n", line); }
  fprintf(stderr, "[screen-end]\n");
}

static void on_event(tevent *e) {
  vt *t = T.t;
  switch (e->type) {
  case TE_KEY: {
    if (key_is_modifier(e->key)) return;
    T.swallow_text = 1;
    if (e->key == K_F12 && e->mods == (KM_SHIFT | KM_CTRL | KM_ALT)) { dump_screen(); return; }
    if (T.dead) { if (T.hold) exit(0); return; }
    int m = e->mods;
    if (m == (KM_CTRL | KM_SHIFT) && (e->key == 46 || e->key == 47)) { /* C, V */
      if (e->key == 46) copy_selection(); else paste();
      return;
    }
    if ((m & KM_SHIFT) && !(m & (KM_CTRL | KM_ALT)) && !t->alt_active && (e->key == K_PAGEUP || e->key == K_PAGEDOWN)) {
      vt_scroll_view(t, (e->key == K_PAGEUP ? 1 : -1) * (t->rows > 2 ? t->rows - 1 : 1));
      return;
    }
    char buf[32];
    int n = key_encode(e->key, m, t->appcursor, buf);
    if (n > 0) send(buf, n);
    else T.swallow_text = 0; /* a text key: the character follows */
    break;
  }
  case TE_TEXT: {
    if (T.swallow_text) { T.swallow_text = 0; return; }
    if (T.dead) { if (T.hold) exit(0); return; }
    char b[4];
    send(b, vt_utf8_encode(e->cp, b));
    break;
  }
  case TE_WHEEL:
    if (t->mouse) mouse_report(e->wheel > 0 ? 64 : 65, e->x, e->y, 0, e->mods);
    else if (t->alt_active) { /* alternate scroll: arrows for pagers and editors */
      const char *s = e->wheel > 0 ? (t->appcursor ? "\x1bOA" : "\x1b[A") : (t->appcursor ? "\x1bOB" : "\x1b[B");
      for (int i = 0; i < 3; i++) send(s, 3);
    } else vt_scroll_view(t, 3 * e->wheel);
    break;
  case TE_MOUSE_DOWN:
    T.mouse_buttons |= 1 << e->button;
    if (t->mouse && !(e->mods & KM_SHIFT)) mouse_report(e->button, e->x, e->y, 0, e->mods);
    else if (e->button == 0) sel_press(e->x, e->y);
    break;
  case TE_MOUSE_UP:
    T.mouse_buttons &= ~(1 << e->button);
    if (S.drag && e->button == 0) { sel_move(e->x, e->y); S.drag = 0; }
    else if (t->mouse && t->mouse != VT_MOUSE_X10) mouse_report(e->button, e->x, e->y, 1, e->mods);
    break;
  case TE_MOUSE_MOVE:
    if (S.drag) sel_move(e->x, e->y);
    else if (t->mouse == VT_MOUSE_ANY || (t->mouse == VT_MOUSE_DRAG && T.mouse_buttons)) {
      int b = T.mouse_buttons & 1 ? 0 : T.mouse_buttons & 2 ? 1 : T.mouse_buttons & 4 ? 2 : 3;
      mouse_report(32 + b, e->x, e->y, 0, e->mods);
    }
    break;
  case TE_RESIZE: /* also the theme-change notification */
    sel_clear();
    reload_theme();
    apply_size();
    render_invalidate(&T.r);
    break;
  case TE_ZOOM: {
    int old = font_px(), was = T.zoom;
    T.zoom = e->wheel ? T.zoom + e->wheel : 0;
    if (font_px() == old) { /* nothing to redraw */
      /* at a limit, keep the count where the size stops changing, so the other key works at once */
      if (e->wheel) T.zoom = was;
      return;
    }
    sel_clear();
    setup_fonts();
    apply_size();
    render_invalidate(&T.r);
    break;
  }
  case TE_FOCUS: case TE_BLUR:
    if (t->focus_events) send(e->type == TE_FOCUS ? "\x1b[I" : "\x1b[O", 3);
    t->dirty[t->y] = 1;
    break;
  case TE_CLOSE:
    exit(0);
  }
}

/* ---------- main loop ---------- */

static void draw(void) {
  double t0 = now_ms();
  render_rect d;
  sel_sync();
  if (render_draw(&T.r, T.t, &T.be->surf, T.be->focused, &d)) T.be->present(T.be, d);
  if (T.t->title_changed && !T.title_fixed) { T.be->set_title(T.be, T.t->title); }
  T.t->title_changed = 0;
  double dt = now_ms() - t0;
  T.st_frames++;
  T.st_render_ms += dt;
  if (dt > T.st_render_max) T.st_render_max = dt;
}

static void stats_tick(double now) {
  if (now - T.st_t0 < 2000) return;
  if (T.stats && T.st_frames >= 10)
    fprintf(stderr, "[term] %ld frames in %.0f ms: render %.2f ms avg, %.2f ms max; parse %.2f ms per frame, %.2f MB/s of output\n",
            T.st_frames, now - T.st_t0, T.st_render_ms / T.st_frames, T.st_render_max, T.st_parse_ms / T.st_frames,
            T.st_bytes / (now - T.st_t0) / 1e3);
  T.st_t0 = now; T.st_frames = 0; T.st_render_ms = T.st_render_max = T.st_parse_ms = 0; T.st_bytes = 0;
}

static void usage(void) {
  fprintf(stderr, "usage: term [--fb] [--fullscreen] [-T title] [--font-size N] [--hold] [--stats] [-e command args...]\n");
  exit(2);
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--demo")) return demo_main(argc - 1, argv + 1);
  int fb = 0, full = 0, pid1 = getpid() == 1;
  const char *title = "term";
  char **cmd = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--fb")) fb = 1;
    else if (!strcmp(argv[i], "--fullscreen")) full = 1; /* ask the window manager for the whole screen */
    else if (!strcmp(argv[i], "--hold")) T.hold = 1;
    else if (!strcmp(argv[i], "--stats")) T.stats = 1;
    else if (!strcmp(argv[i], "-T") && i + 1 < argc) { title = argv[++i]; T.title_fixed = 1; }
    else if (!strcmp(argv[i], "--font-size") && i + 1 < argc) T.font_size = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-e") && i + 1 < argc) { cmd = &argv[i + 1]; break; }
    else usage();
  }
  if (pid1) {
    fb = 1; T.stats = 1;
    if (!getenv("PATH")) setenv("PATH", "/bin", 1);
    if (!getenv("HOME")) setenv("HOME", "/home/user", 1);
  }
  ui_theme_load(&T.th, NULL);
  setup_fonts();
  if (!fb) T.be = be_ui_open(title, 80 * T.r.cw + 2 * T.pad, 25 * T.r.ch + 2 * T.pad, full);
  if (!T.be) T.be = be_fb_open();
  if (!T.be) die("no window manager and no framebuffer");
  T.be->set_title(T.be, title);

  static char *shell[] = {"/bin/sh", NULL};
  static char *demo_shell[] = {"/bin/term", "--demo", "shell", NULL};
  struct stat st;
  if (!cmd) cmd = stat("/bin/sh", &st) == 0 ? shell : demo_shell;
  start_child(cmd);

  static char buf[65536];
  double next_frame = 0;
  int need = 1;
  T.st_t0 = now_ms();
  for (;;) {
    double now = now_ms();
    int timeout = -1;
    if (need) timeout = now >= next_frame ? 0 : (int)(next_frame - now) + 1;
    else if (T.stats) timeout = 2000;
    if (pq_busy() && (timeout < 0 || timeout > PASTE_MS)) timeout = PASTE_MS;
    struct pollfd pf[2] = {{T.dead ? -1 : T.ptm, POLLIN, 0}, {T.be->fd(T.be), POLLIN, 0}};
    poll(pf, 2, timeout);

    tevent e; /* input first, so that a flood of output never starves it */
    int r;
    while ((r = T.be->next(T.be, &e)) > 0) { on_event(&e); need = 1; }
    if (r < 0) exit(0); /* window manager gone */

    if (!T.dead && (pf[0].revents & (POLLIN | POLLHUP))) {
      double t0 = now_ms();
      for (;;) {
        long n = read(T.ptm, buf, sizeof buf);
        if (n > 0) {
          static uint8_t pre[4096]; /* rows' dirty flags before this output */
          int was_all = T.t->all_dirty, rows = T.t->rows < 4096 ? T.t->rows : 4096;
          if (S.on || S.drag) memcpy(pre, T.t->dirty, (size_t)rows);
          vt_feed(T.t, buf, (size_t)n);
          sel_after_output(pre, was_all);
          T.st_bytes += n;
          if (T.t->reply_len) { int k = T.t->reply_len; T.t->reply_len = 0; send(T.t->reply, k); }
          vt_view_bottom(T.t);
          need = 1;
          if (now_ms() - t0 > 8) break; /* let input and frames through */
          continue;
        }
        if (n == 0) { child_gone(); need = 1; } /* every slave closed */
        break;
      }
      T.st_parse_ms += now_ms() - t0;
      if (T.dead && !T.hold) { draw(); break; }
    }
    now = now_ms();
    pq_pump(now);
    if (need && now >= next_frame) {
      draw();
      need = 0;
      next_frame = now + FRAME_MS;
    }
    stats_tick(now);
  }
  T.be->close(T.be);
  return WIFEXITED(T.exit_status) ? WEXITSTATUS(T.exit_status) : 1;
}
