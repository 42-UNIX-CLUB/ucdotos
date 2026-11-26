// SPDX-License-Identifier: GPL-2.0-or-later
/* Small kernels to compare code generators: build with clang -O0, clang -O2 and cc,
 * run each, compare the best batch. Every kernel is a noinline function called many
 * times (V8 does not replace a running loop with optimised code). */
#include <stdio.h>
#include <string.h>
#include <time.h>
#define NOINLINE __attribute__((noinline))
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static char flags[100000];
NOINLINE static int sieve(void) {
  int n = 100000, count = 0;
  memset(flags, 1, sizeof flags);
  for (int i = 2; i < n; i++)
    if (flags[i]) { count++; for (int j = i + i; j < n; j += i) flags[j] = 0; }
  return count;
}

static int arr[20000];
static void qs(int *a, int lo, int hi) {
  while (lo < hi) {
    int p = a[(lo + hi) >> 1], i = lo, j = hi;
    while (i <= j) {
      while (a[i] < p) i++;
      while (a[j] > p) j--;
      if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
    }
    if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
  }
}
NOINLINE static int sort(void) {
  unsigned s = 1;
  for (int i = 0; i < 20000; i++) { s = s * 1103515245u + 12345u; arr[i] = (int)(s >> 8); }
  qs(arr, 0, 19999);
  return arr[10000] & 0xffff;
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
NOINLINE static int recurse(void) { return fib(24); }

static char text[8192];
NOINLINE static int strings(void) {
  unsigned h = 0;
  for (int r = 0; r < 20; r++) {
    char buf[64];
    for (int i = 0; i + 40 < (int)sizeof text; i += 40) {
      memcpy(buf, text + i, 40);
      buf[40] = 0;
      for (char *p = buf; *p; p++) h = h * 31 + (unsigned char)*p;
      h ^= (unsigned)strlen(buf);
    }
  }
  return (int)(h & 0xffff);
}

NOINLINE static int floats(void) {
  int inside = 0;
  for (int y = 0; y < 80; y++)
    for (int x = 0; x < 120; x++) {
      double cr = x / 40.0 - 2.0, ci = y / 40.0 - 1.0, zr = 0, zi = 0;
      int k = 0;
      while (k < 64 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; k++; }
      if (k == 64) inside++;
    }
  return inside;
}

struct particle { float x, y, vx, vy; };
static struct particle ps[2000];
NOINLINE static int structs(void) {
  for (int i = 0; i < 2000; i++) { ps[i].x = (float)i; ps[i].y = (float)(i * 2); ps[i].vx = 1.5f; ps[i].vy = -0.5f; }
  for (int step = 0; step < 50; step++)
    for (int i = 0; i < 2000; i++) {
      struct particle *p = &ps[i];
      p->x += p->vx; p->y += p->vy;
      if (p->x > 1000) p->vx = -p->vx;
      if (p->y < 0) p->vy = -p->vy;
    }
  return (int)ps[1234].x;
}

static long long mix64(void) {
  unsigned long long h = 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < 100000; i++) { h ^= h >> 31; h *= 0xbf58476d1ce4e5b9ULL; h += (unsigned long long)i; }
  return (long long)(h >> 40);
}
NOINLINE static int int64(void) { return (int)mix64(); }

typedef int (*kernel)(void);
static const struct { const char *name; kernel f; } ks[] = {
  {"sieve", sieve}, {"sort", sort}, {"recursion", recurse}, {"strings", strings}, {"floats", floats},
  {"structs", structs}, {"int64", int64},
};

int main(int argc, char **argv) {
  int reps = argc > 1 ? 0 : 15;
  for (int i = 0; i < (int)sizeof text; i++) text[i] = 'a' + (i * 7) % 26;
  printf("%-10s %10s %10s %8s\n", "kernel", "first ms", "best ms", "result");
  for (unsigned k = 0; k < sizeof ks / sizeof *ks; k++) {
    double best = 1e30, first = 0;
    int r = 0;
    for (int i = 0; i < reps; i++) {
      double t0 = now_ms();
      r = ks[k].f();
      double t = now_ms() - t0;
      if (i == 0) first = t;
      if (t < best) best = t;
    }
    printf("%-10s %10.3f %10.3f %8d\n", ks[k].name, first, best, r);
  }
  return 0;
}
