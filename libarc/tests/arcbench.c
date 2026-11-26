// SPDX-License-Identifier: GPL-2.0-or-later
/* Decompression throughput, timed inside the process.
 *   arcbench FILE [RUNS]
 * Reads FILE into memory, decompresses it RUNS times (default 5) from memory
 * into a sink that drops the bytes, and prints the first (cold) run and the
 * best run in MB/s of decompressed output (MB = 10^6 bytes).
 * Builds natively (cc -O2 libarc/tests/arcbench.c libarc/*.c) and for UC.OS
 * (see docs/PERFORMANCE.md for the command), so both numbers come from the
 * same code and the same timer. */
#include "../arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

struct mem { const unsigned char *p; size_t n, off; };
static long msrc(void *ctx, void *buf, unsigned long n) {
  struct mem *m = ctx;
  size_t k = m->n - m->off;
  if (k > n) k = n;
  memcpy(buf, m->p + m->off, k);
  m->off += k;
  return (long)k;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned char out[1 << 16];

__attribute__((noinline)) static long long run(const unsigned char *p, size_t n, char *err, size_t errn) {
  struct mem m = {p, n, 0};
  arc_stream *s = arc_open(ARC_AUTO, msrc, &m);
  if (!s) return -1;
  long long total = 0;
  long r;
  while ((r = arc_read(s, out, sizeof out)) > 0) total += r;
  if (r < 0) { snprintf(err, errn, "%s", arc_error(s)); total = -1; }
  arc_close(s);
  return total;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: arcbench FILE [RUNS]\n"); return 2; }
  int runs = argc > 2 ? atoi(argv[2]) : 5;
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { perror(argv[1]); return 1; }
  size_t cap = 1 << 20, n = 0;
  unsigned char *p = malloc(cap);
  long r;
  while (p && (r = read(fd, p + n, cap - n)) > 0) {
    n += r;
    if (n == cap) { cap *= 2; p = realloc(p, cap); }
  }
  close(fd);
  if (!p) { fprintf(stderr, "arcbench: out of memory\n"); return 1; }
  double first = 0, best = 1e30;
  long long size = 0;
  char err[200] = "";
  for (int i = 0; i < runs; i++) {
    double t0 = now();
    size = run(p, n, err, sizeof err);
    double t = now() - t0;
    if (size < 0) { fprintf(stderr, "arcbench: %s: %s\n", argv[1], err); return 1; }
    if (i == 0) first = t;
    if (t < best) best = t;
  }
  printf("%s: %lld bytes out, first %.1f MB/s, best %.1f MB/s (%d runs)\n", argv[1], size, size / first / 1e6,
         size / best / 1e6, runs);
  return 0;
}
