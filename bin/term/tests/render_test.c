// SPDX-License-Identifier: GPL-2.0-or-later
/* Native renderer test: feed byte streams to a vt, render it with render.c and
 * write a PPM image. Also measures the cost of a full redraw.
 *   --render-each N: draw after every N bytes (used with random input under ASan).
 *   render_test [--theme deck|brand] [--cols N] [--rows N] [--font-size N] [--unfocused]
 *               [--fontdir DIR] [--out file.ppm] [--bench] input-files...
 * Build: see run_tests.sh */
#define _POSIX_C_SOURCE 200809L
#include "../vt.h"
#include "../render.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *fontdir = "rootfs/usr/share/fonts";

static unsigned char *load_font(const char *path, unsigned *len) {
  unsigned char *d = ui_read_file(path, len);
  if (d) return d;
  const char *base = strrchr(path, '/');
  char p[512];
  snprintf(p, sizeof p, "%s/%s", fontdir, base ? base + 1 : path);
  return ui_read_file(p, len);
}
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

int main(int argc, char **argv) {
  const char *theme = "deck", *out = "out.ppm";
  int cols = 80, rows = 24, size = 0, focused = 1, bench = 0, each = 0;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
    if (!strcmp(argv[i], "--theme")) theme = argv[++i];
    else if (!strcmp(argv[i], "--cols")) cols = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--rows")) rows = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--font-size")) size = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--unfocused")) focused = 0;
    else if (!strcmp(argv[i], "--fontdir")) fontdir = argv[++i];
    else if (!strcmp(argv[i], "--out")) out = argv[++i];
    else if (!strcmp(argv[i], "--bench")) bench = 1;
    else if (!strcmp(argv[i], "--render-each")) each = atoi(argv[++i]); /* render after every N bytes (fuzzing) */
  }
  ui_theme th;
  ui_theme_load(&th, theme);
  if (size) th.font_size = size;
  unsigned l1, l2, l3;
  unsigned char *d1 = load_font(th.font, &l1), *d2 = load_font(th.font_bold, &l2);
  unsigned char *d3 = load_font("/usr/share/fonts/JetBrainsMonoNerdFontMono-Regular.ttf", &l3);
  ttf_font f1, f2, f3;
  if (!d1 || ttf_open(&f1, d1, l1)) { fprintf(stderr, "cannot load %s\n", th.font); return 1; }
  int have_bold = d2 && !ttf_open(&f2, d2, l2), have_fb = d3 && !ttf_open(&f3, d3, l3);
  render r = {0};
  if (render_init(&r, &f1, have_bold ? &f2 : NULL, have_fb && strstr(th.font, "Fira") ? &f3 : NULL, (float)th.font_size)) return 1;
  render_colors(&r, th.fg, th.bg, th.accent, th.color);
  int pad = th.gap;
  int W = cols * r.cw + 2 * pad, H = rows * r.ch + 2 * pad;
  unsigned *px = calloc((size_t)W * H, 4);
  gfx_surface s;
  gfx_init(&s, px, W, H, W);
  render_layout(&r, pad, pad);
  vt *t = vt_new(cols, rows, 2000);
  for (; i < argc; i++) {
    unsigned len;
    unsigned char *data = ui_read_file(argv[i], &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
    if (each > 0) {
      render_rect dm;
      for (unsigned off = 0; off < len; off += (unsigned)each) {
        vt_feed(t, data + off, len - off < (unsigned)each ? len - off : (unsigned)each);
        if ((off / each) % 7 == 3) vt_scroll_view(t, 5);
        if ((off / each) % 11 == 5) vt_view_bottom(t);
        render_draw(&r, t, &s, (off / each) & 1, &dm);
      }
    } else vt_feed(t, data, len);
    free(data);
  }
  render_rect dmg;
  render_draw(&r, t, &s, focused, &dmg);
  if (bench) {
    int n = 60;
    double t0 = now();
    for (int k = 0; k < n; k++) { render_invalidate(&r); render_draw(&r, t, &s, focused, &dmg); }
    double full = (now() - t0) / n;
    /* every cell changes (new content), no forced repaint: the realistic animation case */
    char *frame[2];
    size_t flen[2] = {0, 0};
    for (int k = 0; k < 2; k++) { /* two truecolor half-block frames, pre-generated */
      frame[k] = malloc((size_t)cols * rows * 48 + 16);
      flen[k] = (size_t)sprintf(frame[k], "\x1b[H");
      for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
          flen[k] += (size_t)sprintf(frame[k] + flen[k], "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80", (x * 3 + k * 50) & 255, (y * 5) & 255, k * 90, (x + y + k * 30) & 255, 100, 200);
    }
    double tp = 0, td = 0;
    for (int k = 0; k < n; k++) {
      t0 = now();
      vt_feed(t, frame[k & 1], flen[k & 1]);
      double t1 = now();
      render_draw(&r, t, &s, focused, &dmg);
      tp += t1 - t0; td += now() - t1;
    }
    double anim = (tp + td) / n;
    printf("half-block frame of %zu bytes: parse %.2f ms + draw %.2f ms\n", flen[0], tp / n, td / n);
    t0 = now();
    for (int k = 0; k < n; k++) { vt_feed(t, k & 1 ? "a" : "\b", 1); render_draw(&r, t, &s, focused, &dmg); }
    double small = (now() - t0) / n;
    printf("%dx%d grid, cell %dx%d px, surface %dx%d: full repaint %.2f ms, every cell changed (parse + draw) %.2f ms, one cell %.3f ms\n",
           cols, rows, r.cw, r.ch, W, H, full, anim, small);
  }
  FILE *f = fopen(out, "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int k = 0; k < W * H; k++) { unsigned p = px[k]; fputc(p & 255, f); fputc((p >> 8) & 255, f); fputc((p >> 16) & 255, f); }
  fclose(f);
  vt_free(t);
  render_free(&r);
  free(px); free(d1); free(d2); free(d3);
  return 0;
}
