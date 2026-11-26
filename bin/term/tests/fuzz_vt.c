// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L
/* Fuzz and benchmark the parser.
 *   cc -std=c11 -g -O1 -fsanitize=address,undefined -I.. fuzz_vt.c ../vt.c -o fuzz && ./fuzz fuzz 8000000
 *   cc -std=c11 -O2 -I.. fuzz_vt.c ../vt.c -o bench && ./bench bench */
#include "../vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }
static unsigned rn(unsigned n) { return n ? rnd() % n : 0; }

static char buf[1 << 16];
static int blen;
static void put(const char *s) { while (*s && blen < (int)sizeof buf) buf[blen++] = *s++; }
static void putc_(int c) { if (blen < (int)sizeof buf) buf[blen++] = (char)c; }
static void putn(int n) { char b[16]; snprintf(b, sizeof b, "%d", n); put(b); }

/* random but plausible sequences */
static void gen_seq(void) {
  static const char finals[] = "@ABCDEFGHIJKLMPSTXZ`abcdefghlmnqrstu";
  static const char escs[] = "DEHM78c=>NOZ\\";
  switch (rn(12)) {
  case 0: case 1: case 2: { /* CSI */
    put("\x1b[");
    if (rn(4) == 0) putc_("?>=<"[rn(4)]);
    int np = (int)rn(6);
    for (int i = 0; i < np; i++) {
      if (i) putc_(rn(8) ? ';' : ':');
      if (rn(5)) putn((int)(rn(10) ? rn(30) : rn(100000)));
    }
    if (rn(10) == 0) putc_(" !\"$'"[rn(5)]);
    putc_(finals[rn(sizeof finals - 1)]);
    break;
  }
  case 3: { /* SGR with colours */
    put("\x1b[");
    int n = (int)rn(5);
    for (int i = 0; i < n; i++) {
      if (i) putc_(';');
      switch (rn(4)) {
      case 0: putn((int)rn(110)); break;
      case 1: put(rn(2) ? "38;5;" : "48;5;"); putn((int)rn(300)); break;
      case 2: put(rn(2) ? "38;2;" : "48;2;"); putn((int)rn(256)); putc_(';'); putn((int)rn(256)); putc_(';'); putn((int)rn(256)); break;
      case 3: put("38:2::"); putn((int)rn(256)); putc_(':'); putn((int)rn(256)); putc_(':'); putn((int)rn(256)); break;
      }
    }
    putc_('m');
    break;
  }
  case 4: { /* DEC modes */
    static const int modes[] = {1, 6, 7, 12, 25, 47, 1047, 1048, 1049, 2004, 1000, 1002, 1006, 4, 20};
    put("\x1b[?"); putn(modes[rn(15)]); putc_(rn(2) ? 'h' : 'l');
    break;
  }
  case 5: putc_(27); putc_(escs[rn(sizeof escs - 1)]); break;
  case 6: put("\x1b"); putc_("()*+"[rn(4)]); putc_(rn(2) ? '0' : 'B'); break;
  case 7: { /* OSC */
    put("\x1b]"); putn((int)rn(12)); putc_(';');
    int n = (int)rn(40);
    for (int i = 0; i < n; i++) putc_(32 + (int)rn(200));
    put(rn(2) ? "\x07" : "\x1b\\");
    break;
  }
  case 8: put(rn(2) ? "\x1bPq#0;1;2~~--" : "\x1b_Gabc"); if (rn(2)) put("\x1b\\"); break;
  case 9: { static const char *u[] = {"\xc3\xa9", "\xe2\x94\x80", "\xf0\x9f\x98\x80", "\xe2\x96\x80", "\xcc\x81", "\xed\xa0\x80", "\xc0\xaf", "\xf4\x90\x80\x80"}; put(u[rn(8)]); break; }
  case 10: putc_("\r\n\t\b\x07\x0e\x0f\x18\x1a\x0b\x0c"[rn(11)]); break;
  case 11: { int n = (int)rn(100); for (int i = 0; i < n; i++) putc_(32 + (int)rn(95)); break; }
  }
}

static void check_invariants(vt *t) {
  if (t->x < 0 || t->x >= t->cols || t->y < 0 || t->y >= t->rows || t->top < 0 || t->bot >= t->rows || t->top >= t->bot + (t->rows == 1)) {
    printf("invariant broken: cursor %d,%d size %dx%d region %d-%d\n", t->x, t->y, t->cols, t->rows, t->top, t->bot);
    abort();
  }
  if (t->pending && t->x != t->cols - 1) { printf("pending wrap away from the last column\n"); abort(); }
  for (int y = 0; y < t->rows; y++) { int w; const vt_cell *r = vt_view_row(t, y, &w); if (w && !r) abort(); }
  char line[4096];
  vt_row_text(t, rn(t->rows), line, sizeof line);
}

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static int bench(void) {
  /* three workloads: plain text, coloured ls-like output, truecolor half-block frames */
  static char text[1 << 20], color[1 << 20], tc[1 << 20];
  int nt = 0, nc = 0, nh = 0;
  while (nt < (int)sizeof text - 200) { int n = 20 + (int)rn(60); for (int i = 0; i < n; i++) text[nt++] = 'a' + (char)rn(26); text[nt++] = '\r'; text[nt++] = '\n'; }
  while (nc < (int)sizeof color - 200) nc += snprintf(color + nc, 200, "\x1b[01;3%dmfile%u\x1b[0m  -rw-r--r-- 1 user user %u Nov 26 file.txt\r\n", (int)rn(8), rn(1000), rn(100000));
  while (nh < (int)sizeof tc - 200) {
    if (rn(160) == 0) nh += snprintf(tc + nh, 200, "\x1b[H");
    nh += snprintf(tc + nh, 200, "\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um\xe2\x96\x80", rn(256), rn(256), rn(256), rn(256), rn(256), rn(256));
  }
  const char *names[3] = {"plain text", "SGR-coloured lines", "truecolor half blocks"};
  char *bufs[3] = {text, color, tc};
  int lens[3] = {nt, nc, nh};
  for (int k = 0; k < 3; k++) {
    vt *t = vt_new(160, 45, 2000);
    double t0 = now();
    long total = 0;
    while (now() - t0 < 1.0) {
      for (int off = 0; off < lens[k]; off += 65536) {
        int n = lens[k] - off < 65536 ? lens[k] - off : 65536;
        vt_feed(t, bufs[k] + off, (size_t)n);
      }
      total += lens[k];
    }
    double dt = now() - t0;
    printf("parser throughput, %-22s %7.1f MB/s\n", names[k], total / dt / 1e6);
    vt_free(t);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "bench")) return bench();
  long target = argc > 2 ? atol(argv[2]) : 4000000;
  vt *t = vt_new(80, 24, 300);
  long total = 0;
  int round = 0;
  while (total < target) {
    blen = 0;
    int mode = round++ % 3;
    if (mode == 0) { int n = 1 + (int)rn(4096); for (int i = 0; i < n; i++) putc_((int)rn(256)); } /* raw random bytes */
    else if (mode == 1) { int n = 1 + (int)rn(200); for (int i = 0; i < n; i++) gen_seq(); }
    else { int n = 1 + (int)rn(400); for (int i = 0; i < n; i++) if (rn(3)) gen_seq(); else putc_((int)rn(256)); }
    /* feed in random pieces */
    for (int off = 0; off < blen;) {
      int n = 1 + (int)rn(blen - off < 300 ? (unsigned)(blen - off) : 300u);
      vt_feed(t, buf + off, (size_t)n);
      off += n;
      t->reply_len = 0;
      check_invariants(t);
    }
    total += blen;
    switch (rn(40)) {
    case 0: vt_resize(t, 1 + (int)rn(200), 1 + (int)rn(70)); break;
    case 1: vt_scroll_view(t, (int)rn(50) - 25); break;
    case 2: vt_view_bottom(t); break;
    case 3: vt_resize(t, 80, 24); break;
    }
    check_invariants(t);
  }
  vt_free(t);
  printf("fuzz: %ld bytes in %d rounds, no faults\n", total, round);
  return 0;
}
