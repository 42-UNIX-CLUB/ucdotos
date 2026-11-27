// SPDX-License-Identifier: GPL-2.0-or-later
/* term.h: the small terminal layer shared (as identical copies) by vi, htop and fetch.
 * Everything OS-specific sits behind #ifdef __wasm__ so the same source builds with the
 * host cc and runs in a real Linux terminal: raw mode, window size, system info.
 * Header-only (static functions); include it from exactly one .c file per program. */
#ifndef TERM_H
#define TERM_H
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#ifdef __wasm__
#include <sys/syscall.h>
typedef struct termios tty_mode;
static int tty_get(int fd, tty_mode *t) { return __syscall3(SYS_ioctl, fd, TCGETS, (long)t) == 0 ? 0 : -1; }
static int tty_set(int fd, const tty_mode *t) { return __syscall3(SYS_ioctl, fd, TCSETSW, (long)t) == 0 ? 0 : -1; }
static int tty_size(int fd, int *rows, int *cols) {
  struct winsize w;
  if (__syscall3(SYS_ioctl, fd, TIOCGWINSZ, (long)&w) != 0 || !w.ws_row || !w.ws_col) return -1;
  *rows = w.ws_row; *cols = w.ws_col;
  return 0;
}
#define TTY_IXON 0x400
#else
#include <termios.h>
#include <sys/ioctl.h>
typedef struct termios tty_mode;
static int tty_get(int fd, tty_mode *t) { return tcgetattr(fd, t); }
static int tty_set(int fd, const tty_mode *t) { return tcsetattr(fd, TCSADRAIN, t); }
static int tty_size(int fd, int *rows, int *cols) {
  struct winsize w;
  if (ioctl(fd, TIOCGWINSZ, &w) != 0 || !w.ws_row || !w.ws_col) return -1;
  *rows = w.ws_row; *cols = w.ws_col;
  return 0;
}
#define TTY_IXON IXON
#endif

/* raw: no echo, no line editing, no signals (Ctrl-C arrives as a byte). keep_opost keeps \n -> \r\n. */
static tty_mode tty_saved;
static int tty_is_raw;
static int tty_raw(int keep_opost) {
  tty_mode t;
  if (tty_get(0, &t)) return -1;
  tty_saved = t;
  t.c_iflag &= ~(ICRNL | TTY_IXON);
  if (!keep_opost) t.c_oflag &= ~OPOST;
  t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  if (tty_set(0, &t)) return -1;
  tty_is_raw = 1;
  return 0;
}
static void tty_restore(void) { if (tty_is_raw) { tty_set(0, &tty_saved); tty_is_raw = 0; } }
static int tty_check(int fd) { tty_mode t; return tty_get(fd, &t) == 0; }

static double clock_sim_ms; /* testing: round readings down to this step, like a coarse browser clock */
static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double t = (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
  if (clock_sim_ms > 0) t = (double)(long long)(t / clock_sim_ms) * clock_sim_ms;
  return t;
}

/* --- output buffer: build a whole frame, then write it in one go --- */
static char *ob;
static size_t ob_len, ob_cap;
static void ob_put(const char *s, size_t n) {
  if (ob_len + n > ob_cap) {
    size_t c = ob_cap ? ob_cap : 16384;
    while (c < ob_len + n) c *= 2;
    char *p = realloc(ob, c);
    if (!p) return;
    ob = p; ob_cap = c;
  }
  memcpy(ob + ob_len, s, n);
  ob_len += n;
}
static void ob_puts(const char *s) { ob_put(s, strlen(s)); }
static void ob_printf(const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) ob_put(tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
static void ob_flush(void) {
  size_t off = 0;
  while (off < ob_len) {
    ssize_t r = write(1, ob + off, ob_len - off);
    if (r <= 0) break;
    off += r;
  }
  ob_len = 0;
}
static void ob_fg(unsigned rgb) { ob_printf("\033[38;2;%u;%u;%um", rgb >> 16, (rgb >> 8) & 255, rgb & 255); }
static void ob_bg(unsigned rgb) { ob_printf("\033[48;2;%u;%u;%um", rgb >> 16, (rgb >> 8) & 255, rgb & 255); }
static void ob_goto(int row, int col) { ob_printf("\033[%d;%dH", row + 1, col + 1); }

/* --- themes: the active theme's /usr/share/themes/NAME.uconf, read with libuconf ---
 * The colours come from the theme file, so every theme works here too:
 *   bg colors.bg, bg1 colors.bar.bg, bg2 colors.surface (else colors.border.normal),
 *   fg colors.fg, grey colors.dim, orange colors.accent, and red yellow green aqua blue
 *   purple from the theme's bright terminal colours. desc is [theme] description.
 * deck and brand are also built in, for when no theme file can be read. */
#include "uconf.h"
struct theme {
  const char *name, *desc;
  unsigned bg, bg1, bg2, fg, grey, orange, red, yellow, green, aqua, blue, purple;
};
static const struct theme theme_deck = {"deck", "gruvbox", 0x282828, 0x3c3836, 0x504945, 0xebdbb2, 0x928374,
                                        0xfe8019, 0xfb4934, 0xfabd2f, 0xb8bb26, 0x8ec07c, 0x83a598, 0xd3869b};
/* brand has no blue or purple: sage stands in for aqua/blue, orange for the accent slots */
static const struct theme theme_brand = {"brand", "42 brand", 0x0e362e, 0x174a3f, 0x23604f, 0xede6c5, 0x86a596,
                                         0xfe7700, 0xfc413a, 0xffc72e, 0xa9b200, 0xadc787, 0xadc787, 0xfe7700};
static struct theme theme_cur;
static char theme_cur_name[32], theme_cur_desc[48];
static const struct theme *th = &theme_deck;
/* Fill theme_cur from /usr/share/themes/NAME.uconf; 0 if the file was read. */
static int theme_from_file(const char *name) {
  char path[96];
  snprintf(path, sizeof path, "/usr/share/themes/%s.uconf", name);
  uconf c;
  if (uconf_load(&c, path) < 0) { uconf_free(&c); return -1; }
  const struct theme *d = !strcmp(name, "brand") ? &theme_brand : &theme_deck; /* for keys a theme leaves out */
  struct theme t;
  snprintf(theme_cur_name, sizeof theme_cur_name, "%s", name);
  snprintf(theme_cur_desc, sizeof theme_cur_desc, "%s", uconf_str(&c, "theme.description", name));
  t.name = theme_cur_name; t.desc = theme_cur_desc;
  t.bg = uconf_color(&c, "colors.bg", d->bg);
  t.bg1 = uconf_color(&c, "colors.bar.bg", d->bg1);
  t.bg2 = uconf_color(&c, "colors.surface", uconf_color(&c, "colors.border.normal", d->bg2));
  t.fg = uconf_color(&c, "colors.fg", d->fg);
  t.grey = uconf_color(&c, "colors.dim", d->grey);
  t.orange = uconf_color(&c, "colors.accent", d->orange);
  t.red = uconf_color(&c, "terminal.bright.red", d->red);
  t.yellow = uconf_color(&c, "terminal.bright.yellow", d->yellow);
  t.green = uconf_color(&c, "terminal.bright.green", d->green);
  t.aqua = uconf_color(&c, "terminal.bright.cyan", d->aqua);
  t.blue = uconf_color(&c, "terminal.bright.blue", d->blue);
  t.purple = uconf_color(&c, "terminal.bright.magenta", d->purple);
  uconf_free(&c);
  theme_cur = t;
  return 0;
}
/* The active theme: the first word of ~/.config/ucos/theme (the desktop's switch, Alt+t)
 * wins over $THEME, which is fixed when the terminal starts. Returns 1 if it changed. */
static int theme_reload(void) {
  char name[32] = "", path[300];
  const char *home = getenv("HOME");
  snprintf(path, sizeof path, "%s/.config/ucos/theme", home && *home ? home : "/home/user");
  FILE *f = fopen(path, "r");
  if (f) {
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    size_t i = 0, k = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
    while (i < n && buf[i] > ' ' && k < sizeof name - 1) name[k++] = buf[i++];
    name[k] = 0;
  }
  if (!name[0]) { const char *t = getenv("THEME"); snprintf(name, sizeof name, "%s", t ? t : "deck"); }
  struct theme before = *th;
  char was[32];
  snprintf(was, sizeof was, "%s", th->name); /* th->name may be the buffer theme_from_file rewrites */
  if (theme_from_file(name) == 0) th = &theme_cur;
  else th = !strcmp(name, "brand") ? &theme_brand : &theme_deck;
  return strcmp(was, th->name) || memcmp(&before.bg, &th->bg, sizeof(unsigned) * 12) != 0;
}
static void theme_init(void) { theme_reload(); }

/* Clock resolution: browsers coarsen performance.now (0.1 ms in Chrome, 1 ms in Firefox).
 * The smallest non-zero step between consecutive readings, sampled for about ms milliseconds. */
static double clock_res_ms(double (*clk)(void), double ms) {
  double start = clk(), prev = start, best = 1e9;
  long spins = 0;
  for (;;) {
    double t = clk();
    if (t != prev) { if (t - prev < best) best = t - prev; prev = t; }
    if (t - start >= ms && best < 1e9) break;
    if (++spins > 50000000L) break;
  }
  return best;
}

/* --- keys: xterm sequences decoded to codes >= 256 --- */
enum { K_ESC = 27, K_BS = 127, K_UP = 256, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS,
       K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_UNKNOWN };
static unsigned char kin[512];
static int kin_pos, kin_len, kin_eof;
/* wait up to ms (-1 forever) for input; returns 1 if a byte is available */
static int key_wait(int ms) {
  if (kin_pos < kin_len) return 1;
  if (kin_eof) return 1;
  struct pollfd p = {0, POLLIN, 0};
  return poll(&p, 1, ms) > 0;
}
static int key_byte(int ms) { /* -1 on timeout, -2 on EOF */
  if (kin_pos >= kin_len) {
    if (kin_eof) return -2;
    if (ms >= 0 && !key_wait(ms)) return -1;
    ssize_t n = read(0, kin, sizeof kin);
    if (n <= 0) { kin_eof = 1; return -2; }
    kin_pos = 0; kin_len = n;
  }
  return kin[kin_pos++];
}
static void key_unget(void) { if (kin_pos > 0) kin_pos--; }
/* read one key; ms is the timeout for the first byte (-1 = block). Returns -1 on timeout, -2 on EOF. */
static int key_read(int ms) {
  int c = key_byte(ms);
  if (c != 27) return c;
  int c2 = key_byte(25); /* a lone Escape is followed by silence */
  if (c2 < 0) return K_ESC;
  if (c2 != '[' && c2 != 'O') { key_unget(); return K_ESC; }
  int c3 = key_byte(25);
  if (c3 < 0) { key_unget(); return K_ESC; }
  if (c2 == 'O') {
    switch (c3) {
    case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME; case 'F': return K_END;
    case 'P': return K_F1; case 'Q': return K_F2; case 'R': return K_F3; case 'S': return K_F4;
    }
    key_unget(); key_unget(); /* "Escape, O, x": the user typed them; vi opens a line above */
    return K_ESC;
  }
  int num = 0;
  while ((c3 >= '0' && c3 <= '9') || c3 == ';') {
    if (c3 != ';') num = num * 10 + c3 - '0'; else num = 0;
    if ((c3 = key_byte(25)) < 0) return K_UNKNOWN;
  }
  switch (c3) {
  case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
  case 'H': return K_HOME; case 'F': return K_END;
  case '~':
    switch (num) {
    case 1: case 7: return K_HOME; case 4: case 8: return K_END; case 2: return K_INS; case 3: return K_DEL;
    case 5: return K_PGUP; case 6: return K_PGDN; case 11: return K_F1; case 12: return K_F2; case 13: return K_F3;
    case 14: return K_F4; case 15: return K_F5; case 17: return K_F6; case 18: return K_F7; case 19: return K_F8;
    case 20: return K_F9; case 21: return K_F10;
    }
  }
  return K_UNKNOWN;
}

/* --- UTF-8 helpers --- */
static int u8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
static int u8_cont(unsigned char c) { return (c & 0xc0) == 0x80; }
/* display width of a UTF-8 string (one column per code point) */
static int u8_width(const char *s) { int w = 0; for (; *s; s++) if (!u8_cont((unsigned char)*s)) w++; return w; }
/* emit s padded or cut to exactly w columns */
static void ob_padded(const char *s, int w, int right) {
  int sw = u8_width(s);
  if (sw > w) {
    int cols = 0; const char *p = s;
    while (*p && cols < w) { int l = u8_len((unsigned char)*p); p += l; cols++; }
    ob_put(s, p - s);
    return;
  }
  if (right) for (int i = sw; i < w; i++) ob_put(" ", 1);
  ob_puts(s);
  if (!right) for (int i = sw; i < w; i++) ob_put(" ", 1);
}
#endif
