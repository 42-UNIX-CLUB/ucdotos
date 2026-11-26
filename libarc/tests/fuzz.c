// SPDX-License-Identifier: GPL-2.0-or-later
/* Mutation fuzzer for the libarc decoders. Build it with the sanitizers:
 *   cc -O1 -g -fsanitize=address,undefined -o fuzz libarc/tests/fuzz.c libarc/*.c
 *   ./fuzz SECONDS SEED_FILE...
 * Each round takes a seed (a valid .gz, .xz, .zst, .tar or .deb), mutates it
 * (bit flips, random bytes, cuts, repeats, inserts, little-endian field
 * tweaks), and runs it through arc_read with ARC_AUTO and the matching
 * format, through tar_next/tar_read, and through ar_next. Any memory error
 * stops the run with a sanitizer report; the mutated input is written to
 * fuzz-crash.bin first so it can be replayed with: ./fuzz 0 fuzz-crash.bin */
#include "../arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct mem { const unsigned char *p; size_t n, off, chunk; };
static long msrc(void *ctx, void *buf, unsigned long n) {
  struct mem *m = ctx;
  size_t k = m->n - m->off;
  if (k > n) k = n;
  if (m->chunk && k > m->chunk) k = m->chunk;
  memcpy(buf, m->p + m->off, k);
  m->off += k;
  return (long)k;
}

static unsigned long long rng = 88172645463325252ull;
static unsigned long long rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static unsigned char sink[1 << 16];

static void run_stream(const unsigned char *p, size_t n, int fmt, size_t chunk) {
  struct mem m = {p, n, 0, chunk};
  arc_stream *s = arc_open(fmt, msrc, &m);
  if (!s) return;
  arc_set_memlimit(s, 64ull << 20);
  unsigned long long total = 0;
  long r;
  while ((r = arc_read(s, sink, sizeof sink)) > 0) {
    total += r;
    if (total > (256ull << 20)) break; /* a valid stream may expand a lot; stop wasting time */
  }
  arc_close(s);
}

static void run_tar(const unsigned char *p, size_t n) {
  struct mem m = {p, n, 0, 0};
  arc_stream *s = arc_open(ARC_AUTO, msrc, &m);
  if (!s) return;
  arc_set_memlimit(s, 64ull << 20);
  static tar_entry e;
  int k = 0;
  while (tar_next(s, &e) == 1 && k++ < 100000) {
    if (rnd() & 1) { long r; while ((r = tar_read(s, sink, sizeof sink)) > 0) {} if (r < 0) break; }
  }
  arc_close(s);
}

static void run_ar(const unsigned char *p, size_t n) {
  struct mem m = {p, n, 0, 0};
  if (ar_begin(msrc, &m) < 0) return;
  ar_member a;
  int k = 0;
  while (ar_next(msrc, &m, &a) == 1 && k++ < 100000) {
    unsigned long long left = a.size64;
    if (left > m.n - m.off) left = m.n - m.off;
    m.off += left;
    if (ar_skip_pad(msrc, &m, &a) < 0) break;
    if (rnd() % 3 == 0) {
      /* treat the member as a compressed tar, as a package manager would */
      run_tar(m.p + m.off - left, left);
    }
  }
}

static size_t mutate(unsigned char *d, size_t n, size_t cap) {
  int rounds = 1 + rnd() % 8;
  for (int i = 0; i < rounds && n; i++) {
    size_t at = rnd() % n;
    switch (rnd() % 9) {
    case 0: case 1: d[at] ^= 1u << (rnd() % 8); break;
    case 2: d[at] = rnd(); break;
    case 3: n = at + 1; break;                                    /* cut */
    case 4: { size_t k = 1 + rnd() % 16; if (at + k <= n) memset(d + at, rnd() & 1 ? 0 : 0xff, k); break; }
    case 5: if (n < cap) { memmove(d + at + 1, d + at, n - at); d[at] = rnd(); n++; } break; /* insert */
    case 6: if (n > 1) { memmove(d + at, d + at + 1, n - at - 1); n--; } break;               /* delete */
    case 7: { size_t k = 1 + rnd() % 64, src = rnd() % n; if (n + k <= cap && src + k <= n) { unsigned char tmp[64]; memcpy(tmp, d + src, k); memmove(d + at + k, d + at, n - at); memcpy(d + at, tmp, k); n += k; } break; }
    case 8: if (at + 4 <= n) { unsigned v = d[at] | d[at + 1] << 8; v += (rnd() % 64) - 32; d[at] = v; d[at + 1] = v >> 8; } break;
    }
  }
  return n;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: fuzz SECONDS SEED...\n"); return 2; }
  double secs = atof(argv[1]);
  int ns = argc - 2;
  unsigned char **seed = calloc(ns, sizeof *seed);
  size_t *slen = calloc(ns, sizeof *slen), maxn = 0;
  for (int i = 0; i < ns; i++) {
    FILE *f = fopen(argv[2 + i], "rb");
    if (!f) { perror(argv[2 + i]); return 2; }
    fseek(f, 0, SEEK_END);
    slen[i] = ftell(f);
    fseek(f, 0, SEEK_SET);
    seed[i] = malloc(slen[i] + 1);
    if (fread(seed[i], 1, slen[i], f) != slen[i]) return 2;
    fclose(f);
    if (slen[i] > maxn) maxn = slen[i];
  }
  rng ^= (unsigned long long)time(0);
  size_t cap = maxn + 4096;
  unsigned char *buf = malloc(cap);
  if (secs == 0) { /* replay */
    for (int i = 0; i < ns; i++) {
      for (int f = ARC_AUTO; f <= ARC_DEFLATE; f++) run_stream(seed[i], slen[i], f, 0);
      run_tar(seed[i], slen[i]);
      run_ar(seed[i], slen[i]);
    }
    printf("replayed %d input(s)\n", ns);
    return 0;
  }
  clock_t start = clock();
  unsigned long long iters = 0;
  while ((double)(clock() - start) / CLOCKS_PER_SEC < secs) {
    int i = rnd() % ns;
    memcpy(buf, seed[i], slen[i]);
    size_t n = mutate(buf, slen[i], cap);
    FILE *f = fopen("fuzz-crash.bin", "wb");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
    run_stream(buf, n, ARC_AUTO, (rnd() & 3) ? 0 : 1 + rnd() % 300);
    int fmts[] = {ARC_GZIP, ARC_XZ, ARC_ZSTD, ARC_ZLIB, ARC_DEFLATE};
    run_stream(buf, n, fmts[rnd() % 5], 0);
    if (rnd() % 4 == 0) run_tar(buf, n);
    if (rnd() % 8 == 0) run_ar(buf, n);
    iters++;
  }
  remove("fuzz-crash.bin");
  printf("fuzz: %llu inputs in %.0f s, no memory errors\n", iters, secs);
  for (int i = 0; i < ns; i++) free(seed[i]);
  free(seed); free(slen); free(buf);
  return 0;
}
