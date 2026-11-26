// SPDX-License-Identifier: GPL-2.0-or-later
/* ubench: how fast is user code? CPU-bound kernels that make no system call while they are
 * timed. Each kernel runs in a noinline function that is called several times: a wasm engine
 * swaps in optimised code between calls, never in the middle of a running loop, so the first
 * run shows the cold (baseline tier) cost and the best run the optimised cost.
 * Times come from the process CPU clock, so time spent parked by the scheduler is not counted.
 * Usage: ubench [--json] [--runs N]
 * Builds for the OS and natively on Linux (cc -O2 bin/ubench.c -o ubench -lm) to compare. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}
#define K __attribute__((noinline))
static volatile unsigned sink;
static volatile unsigned vsize; /* sizes read through a volatile so the compiler cannot specialise */

/* 1. sieve of Eratosthenes up to 4,000,000 (byte array) */
#define SIEVE_N 4000000
static unsigned char sieve_buf[SIEVE_N + 1];
K static unsigned k_sieve(void) {
  unsigned n = SIEVE_N, count = 0;
  memset(sieve_buf, 1, n + 1);
  sieve_buf[0] = sieve_buf[1] = 0;
  for (unsigned i = 2; i * i <= n; i++)
    if (sieve_buf[i]) for (unsigned j = i * i; j <= n; j += i) sieve_buf[j] = 0;
  for (unsigned i = 0; i <= n; i++) count += sieve_buf[i];
  return count;
}

/* 2. sort one million 32-bit integers: our own quicksort (same code on both sides), then libc qsort */
#define SORT_N 1000000
static unsigned sort_src[SORT_N], sort_buf[SORT_N];
static void sort_fill(void) { unsigned x = 12345; for (int i = 0; i < SORT_N; i++) { x = x * 1103515245u + 12345u; sort_src[i] = x; } }
static void isort(unsigned *a, int n) {
  for (int i = 1; i < n; i++) { unsigned v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
}
static void qsort_u(unsigned *a, int n) {
  while (n > 24) {
    unsigned p = a[n / 2], t;
    int i = 0, j = n - 1;
    for (;;) { while (a[i] < p) i++; while (a[j] > p) j--; if (i >= j) break; t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
    if (j + 1 < n - j - 1) { qsort_u(a, j + 1); a += j + 1; n -= j + 1; } else { qsort_u(a + j + 1, n - j - 1); n = j + 1; }
  }
  isort(a, n);
}
K static unsigned k_sort(void) { qsort_u(sort_buf, SORT_N); return sort_buf[SORT_N / 2]; }
static int cmp_u(const void *a, const void *b) { unsigned x = *(const unsigned *)a, y = *(const unsigned *)b; return x < y ? -1 : x > y; }
K static unsigned k_qsort(void) { qsort(sort_buf, SORT_N, sizeof sort_buf[0], cmp_u); return sort_buf[SORT_N / 2]; }

/* 3. memcpy and memset sweeps through the libc, at three sizes; 64 MiB moved per run */
#define MEM_BIG (1u << 20)
static unsigned char mem_a[MEM_BIG + 64], mem_b[MEM_BIG + 64];
static unsigned mem_sweep(int set, unsigned size) {
  unsigned total = 64u << 20, n = total / size, off = 0;
  for (unsigned i = 0; i < n; i++) {
    if (set) memset(mem_a + off, (int)i, size); else memcpy(mem_a + off, mem_b + (off ^ 32), size);
    off = (off + size) & (MEM_BIG - 1);
    if (off + size > MEM_BIG) off = 0;
  }
  return mem_a[size / 2];
}
K static unsigned k_memcpy16(void) { return mem_sweep(0, vsize); }
K static unsigned k_memcpy4k(void) { return mem_sweep(0, vsize << 8); }
K static unsigned k_memcpy1m(void) { return mem_sweep(0, vsize << 16); }
K static unsigned k_memset16(void) { return mem_sweep(1, vsize); }
K static unsigned k_memset4k(void) { return mem_sweep(1, vsize << 8); }

/* 4. floating point: Mandelbrot set, 600 x 400, up to 200 iterations, plus sqrt */
K static unsigned k_mandel(void) {
  unsigned inside = 0;
  double acc = 0;
  for (int y = 0; y < 400; y++)
    for (int x = 0; x < 600; x++) {
      double cr = -2.2 + x * (3.2 / 600), ci = -1.2 + y * (2.4 / 400), zr = 0, zi = 0;
      int i = 0;
      while (i < 200 && zr * zr + zi * zi < 4) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; i++; }
      if (i == 200) inside++;
      acc += sqrt(zr * zr + zi * zi);
    }
  return inside + (unsigned)acc;
}

/* 5. function calls: naive recursive Fibonacci of 30 (1.6 million calls) */
K static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
K static unsigned k_fib(void) { return fib(vsize + 14); }

/* 6. strings through the libc: snprintf, strlen, strcmp, strchr, strtol, memcmp */
K static unsigned k_string(void) {
  char buf[128], prev[128] = "";
  unsigned acc = 0;
  for (int i = 0; i < 100000; i++) {
    int n = snprintf(buf, sizeof buf, "item %d: %s/%x %5.2f", i, (i & 1) ? "odd" : "even", (unsigned)i * 2654435761u, i * 0.125);
    acc += n + strlen(buf) + (strcmp(buf, prev) > 0);
    char *c = strchr(buf, ':');
    acc += (unsigned)strtol(buf + 5, NULL, 10) + (c ? (unsigned)(c - buf) : 0);
    acc += memcmp(buf, prev, 8) != 0;
    memcpy(prev, buf, (size_t)n + 1);
  }
  return acc;
}

/* 7. malloc and free churn: 200,000 blocks of 16 to 1040 bytes, a window of 1,000 live */
K static unsigned k_malloc(void) {
  static void *live[1000];
  unsigned x = 7, acc = 0;
  for (int i = 0; i < 200000; i++) {
    x = x * 1103515245u + 12345u;
    int slot = (x >> 8) % 1000;
    free(live[slot]);
    size_t sz = 16 + ((x >> 16) & 1023);
    unsigned char *p = malloc(sz);
    if (p) { p[0] = (unsigned char)i; p[sz - 1] = 1; acc += p[0]; }
    live[slot] = p;
  }
  for (int i = 0; i < 1000; i++) { free(live[i]); live[i] = 0; }
  return acc;
}

/* 8. integer hashing: CRC-32 (bitwise table) over 8 MiB */
static unsigned crc_table[256];
K static unsigned k_crc(void) {
  unsigned c = 0xffffffffu;
  for (int r = 0; r < 8; r++)
    for (unsigned i = 0; i < MEM_BIG; i++) c = crc_table[(c ^ mem_b[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

struct kern { const char *name, *what; unsigned (*fn)(void); int prep; double best, first, med; };
static struct kern ks[] = {
  {"sieve", "sieve of Eratosthenes to 4 million", k_sieve, 0, 0, 0, 0},
  {"sort", "quicksort 1M ints (own code)", k_sort, 1, 0, 0, 0},
  {"qsort", "libc qsort 1M ints", k_qsort, 1, 0, 0, 0},
  {"memcpy16", "memcpy 16 B x 4M", k_memcpy16, 0, 0, 0, 0},
  {"memcpy4k", "memcpy 4 KiB x 16K", k_memcpy4k, 0, 0, 0, 0},
  {"memcpy1m", "memcpy 1 MiB x 64", k_memcpy1m, 0, 0, 0, 0},
  {"memset16", "memset 16 B x 4M", k_memset16, 0, 0, 0, 0},
  {"memset4k", "memset 4 KiB x 16K", k_memset4k, 0, 0, 0, 0},
  {"mandel", "Mandelbrot 600x400, doubles", k_mandel, 0, 0, 0, 0},
  {"fib", "recursive fib(30)", k_fib, 0, 0, 0, 0},
  {"string", "snprintf/strlen/strtol x 100K", k_string, 0, 0, 0, 0},
  {"malloc", "malloc/free 200K blocks", k_malloc, 0, 0, 0, 0},
  {"crc32", "CRC-32 over 8 MiB", k_crc, 0, 0, 0, 0},
};
#define NK (int)(sizeof ks / sizeof ks[0])

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
  int json = 0, runs = 9;
  const char *only = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) json = 1;
    else if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    else { fprintf(stderr, "usage: ubench [--json] [--runs N] [--only NAME,NAME]\n"); return 2; }
  }
  if (runs < 2) runs = 2;
  if (runs > 64) runs = 64;
  vsize = 16;
  sort_fill();
  for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
  for (unsigned i = 0; i < sizeof mem_b; i++) mem_b[i] = (unsigned char)(i * 131 + 7);
  for (int k = 0; k < NK; k++) {
    struct kern *e = &ks[k];
    if (only && !strstr(only, e->name)) { e->best = -1; continue; }
    double t[64];
    for (int r = 0; r < runs; r++) {
      if (e->prep) memcpy(sort_buf, sort_src, sizeof sort_buf);
      double t0 = now_ms();
      sink += e->fn();
      t[r] = now_ms() - t0;
    }
    e->first = t[0];
    qsort(t, runs, sizeof t[0], cmpd);
    e->best = t[0];
    e->med = t[runs / 2];
  }
#ifdef __wasm__
  const char *where = "UC.OS (wasm32)";
#else
  const char *where = "native";
#endif
  if (json) {
    printf("{\n  \"system\": \"%s\",\n  \"method\": \"best of %d runs on the process CPU clock; first = cold first run\",\n  \"results\": [\n", where, runs);
    int first = 1;
    for (int k = 0; k < NK; k++) {
      if (ks[k].best < 0) continue;
      printf("%s    {\"name\": \"%s\", \"unit\": \"ms\", \"value\": %.3f, \"median\": %.3f, \"first_batch\": %.3f}", first ? "" : ",\n", ks[k].name, ks[k].best, ks[k].med, ks[k].first);
      first = 0;
    }
    printf("\n  ]\n}\n");
  } else {
    printf("ubench on %s: milliseconds per run, best and median of %d, and the cold first run\n", where, runs);
    printf("  %-10s %-32s %9s %9s %9s\n", "kernel", "what", "best", "median", "first");
    for (int k = 0; k < NK; k++)
      if (ks[k].best >= 0) printf("  %-10s %-32s %9.2f %9.2f %9.2f\n", ks[k].name, ks[k].what, ks[k].best, ks[k].med, ks[k].first);
  }
  return 0;
}
