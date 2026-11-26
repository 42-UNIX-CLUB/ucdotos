// SPDX-License-Identifier: GPL-2.0-or-later
/* uidemo: the smallest complete libui program, and the window manager's test client.
 *
 * It opens a window, draws a gradient with its title and size in the theme font,
 * and prints every event it receives. Keys (for tests and for playing):
 *   n  open another uidemo             p  open a 600x400 floating latency probe
 *   c  repaint a small square only     s  ask the manager to print its counters
 *   b  ask the manager to benchmark    q  quit
 *   g  send garbage (the manager must drop us and survive)
 *   o  send an absurd PRESENT header (same)
 *   x  crash in the middle of a PRESENT message
 *   i  ignore close requests (the second Mod+Shift+q must then force it)
 * Options: --float W H   --title TEXT   --latency (measure present -> framebuffer, see wm.trace) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "ui.h"
#include "theme.h"
#include "gfx.h"
#include "uiproto.h"

static ui_window *win;
static ui_theme th;
static ttf_font font;
static ttf_cache *big, *small;
static unsigned char *font_data;
static char title[64] = "uidemo";
static int focused, square, stubborn;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static void load_theme(void) {
  ui_theme_load(&th, NULL);
  if (big) ttf_cache_free(big);
  if (small) ttf_cache_free(small);
  free(font_data);
  big = small = NULL;
  unsigned len;
  font_data = ui_read_file(th.font, &len);
  if (font_data && ttf_open(&font, font_data, len) == 0) {
    big = ttf_cache_new(&font, th.font_size * 1.6f, 128);
    small = ttf_cache_new(&font, th.font_size, 128);
  }
}
static unsigned mix(unsigned a, unsigned b, int t) { return gfx_blend_px(a, b, t < 0 ? 0 : t > 255 ? 255 : t); }

static void draw(void) {
  int w = ui_width(win), h = ui_height(win);
  gfx_surface s;
  gfx_init(&s, ui_pixels(win), w, h, w);
  /* a diagonal gradient from the theme background towards its blue and purple */
  for (int y = 0; y < h; y++) {
    unsigned *row = ui_pixels(win) + (size_t)y * w;
    unsigned left = mix(th.bg, th.color[4], y * 110 / (h ? h : 1));
    unsigned right = mix(th.bg, th.color[5], 40 + y * 90 / (h ? h : 1));
    for (int x = 0; x < w; x++) row[x] = mix(left, right, x * 255 / (w ? w : 1));
  }
  if (big && small) {
    char line[96];
    int lh = (int)(th.font_size * 1.6f) + 8;
    gfx_text(&s, big, 20, 20 + lh, title, -1, th.fg);
    snprintf(line, sizeof line, "%d x %d  %s", w, h, focused ? "[focused]" : "");
    gfx_text(&s, small, 20, 30 + lh + th.font_size + 6, line, -1, focused ? th.accent : th.dim);
    gfx_text(&s, small, 20, h - 16, "n new  p probe  c square  q quit", -1, th.dim);
  }
  ui_present(win, 0, 0, w, h);
}

/* Fill the whole window with one colour and time how long until it reaches the screen. */
static void latency_probe(void) {
  int w = ui_width(win), h = ui_height(win);
  static const unsigned cols[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff, 0xffff00ff};
  ui_wm_command(win, "trace on");
  for (int i = 0; i < 5; i++) {
    unsigned *px = ui_pixels(win);
    for (int k = 0; k < w * h; k++) px[k] = cols[i];
    double t0 = now_ms();
    ui_present(win, 0, 0, w, h);
    double t1 = now_ms();
    printf("uidemo: latency t=%.3f present %dx%d start; the write took %.3f ms\n", t0, w, h, t1 - t0);
    ui_event ev;
    double end = now_ms() + 150;
    while (now_ms() < end) if (ui_wait_event(win, &ev, (int)(end - now_ms()) + 1) < 0) return;
  }
  ui_wm_command(win, "trace off");
  draw();
}

static const char *evname(int t) {
  static const char *n[] = {"NONE", "KEY_DOWN", "KEY_UP", "TEXT", "MOUSE_MOVE", "MOUSE_DOWN", "MOUSE_UP", "WHEEL",
                            "RESIZE", "FOCUS", "BLUR", "CLOSE", "WM_STATE"};
  return t >= 0 && t <= UI_EV_WM_STATE ? n[t] : "?";
}

int main(int argc, char **argv) {
  int role = UI_NORMAL, rw = 640, rh = 480, latency = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--float") && i + 2 < argc) { role = UI_FLOATING; rw = atoi(argv[i + 1]); rh = atoi(argv[i + 2]); i += 2; }
    else if (!strcmp(argv[i], "--title") && i + 1 < argc) strlcpy(title, argv[++i], sizeof title);
    else if (!strcmp(argv[i], "--latency")) latency = 1;
  }
  if (!strcmp(title, "uidemo")) snprintf(title, sizeof title, "uidemo %d", getpid());
  load_theme();
  win = ui_open(title, "uidemo", rw, rh, role);
  if (!win) { fprintf(stderr, "uidemo: no window manager (is wm running?)\n"); return 1; }
  ui_event ev;
  int r;
  while ((r = ui_wait_event(win, &ev, -1)) >= 0) {
    if (r == 0) continue;
    switch (ev.type) {
    case UI_EV_MOUSE_MOVE: break; /* too chatty to print */
    case UI_EV_KEY_DOWN: case UI_EV_KEY_UP:
      printf("uidemo %d: %s key=%d mods=%d%s\n", getpid(), evname(ev.type), ev.key, ev.mods, ev.repeat ? " repeat" : ""); break;
    case UI_EV_TEXT: printf("uidemo %d: TEXT U+%04X\n", getpid(), ev.codepoint); break;
    case UI_EV_MOUSE_DOWN: case UI_EV_MOUSE_UP: case UI_EV_WHEEL:
      printf("uidemo %d: %s x=%d y=%d button=%d wheel=%d\n", getpid(), evname(ev.type), ev.x, ev.y, ev.button, ev.wheel); break;
    default: printf("uidemo %d: %s %dx%d\n", getpid(), evname(ev.type), ev.w, ev.h);
    }
    if (ev.type == UI_EV_RESIZE) {
      load_theme(); /* a resize to the same size means the theme may have changed */
      draw();
      if (latency) { latency = 0; latency_probe(); }
    } else if (ev.type == UI_EV_FOCUS || ev.type == UI_EV_BLUR) {
      focused = ev.type == UI_EV_FOCUS;
      draw();
    } else if (ev.type == UI_EV_CLOSE) { if (!stubborn) break; printf("uidemo %d: ignoring CLOSE\n", getpid()); }
    else if (ev.type == UI_EV_KEY_DOWN && !ev.repeat) {
      int fd = ui_fd(win);
      switch (ev.key) {
      case 16: goto out; /* q */
      case 23: stubborn = 1; break; /* i: ignore UI_EV_CLOSE from now on */
      case 49: ui_wm_command(win, "exec uidemo"); break; /* n */
      case 25: ui_wm_command(win, "exec uidemo --float 600 400 --latency --title probe"); break; /* p */
      case 31: ui_wm_command(win, "stats"); break; /* s */
      case 48: ui_wm_command(win, "bench 30"); break; /* b */
      case 46: { /* c: a 40x40 square that moves along, presented alone */
        int w = ui_width(win), x = 20 + (square * 50) % (w > 80 ? w - 60 : 1), y = ui_height(win) / 2;
        gfx_surface s;
        gfx_init(&s, ui_pixels(win), w, ui_height(win), w);
        gfx_fill_rect(&s, x, y, 40, 40, th.color[(square % 6) + 9]);
        ui_present(win, x, y, 40, 40);
        square++;
        break;
      }
      case 34: { /* g: garbage */
        static const char junk[] = "\xff\xee\x13\x37 this is not a message \x01\x02\x03";
        write(fd, junk, sizeof junk);
        break;
      }
      case 24: { /* o: a PRESENT that claims an enormous buffer */
        struct { struct uip_hdr h; struct uip_present p; } m = {{UIP_PRESENT, 0, 0x7ffffff0}, {100000, 100000, 0, 0, 100000, 100000}};
        write(fd, &m, sizeof m);
        break;
      }
      case 45: { /* x: start a PRESENT, send half of it, then crash */
        int w = ui_width(win), h = ui_height(win);
        struct { struct uip_hdr h; struct uip_present p; } m = {{UIP_PRESENT, 0, (unsigned)(24 + w * h * 4)}, {w, h, 0, 0, w, h}};
        write(fd, &m, sizeof m);
        write(fd, ui_pixels(win), (size_t)w * h * 2);
        printf("uidemo %d: crashing mid-message\n", getpid());
        __builtin_trap();
      }
      }
    }
    fflush(stdout);
  }
out:
  printf("uidemo %d: bye\n", getpid());
  ui_close(win);
  return 0;
}
