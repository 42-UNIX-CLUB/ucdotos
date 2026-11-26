// SPDX-License-Identifier: GPL-2.0-or-later
// Quicksort (recursive, median of three) of 300,000 32-bit numbers: compares, swaps, calls.
#define REPS 3
#include "rt.h"
#define N 300000
static uint32_t a[N];
static void qs(uint32_t *v, int64_t lo, int64_t hi) {
  while (hi - lo > 16) {
    int64_t mid = lo + (hi - lo) / 2;
    uint32_t x = v[lo], y = v[mid], z = v[hi];
    uint32_t p = x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
    int64_t i = lo, j = hi;
    while (i <= j) {
      while (v[i] < p) i++;
      while (v[j] > p) j--;
      if (i <= j) { uint32_t t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
    }
    if (j - lo < hi - i) { qs(v, lo, j); lo = i; } else { qs(v, i, hi); hi = j; }
  }
  for (int64_t i = lo + 1; i <= hi; i++) {
    uint32_t t = v[i]; int64_t j = i - 1;
    while (j >= lo && v[j] > t) { v[j + 1] = v[j]; j--; }
    v[j + 1] = t;
  }
}
uint64_t work(void) {
  for (int i = 0; i < N; i++) a[i] = rt_rand();
  qs(a, 0, N - 1);
  uint64_t h = 0;
  for (int i = 0; i < N; i += 7) h = h * 1000003 + a[i];
  for (int i = 1; i < N; i++) if (a[i - 1] > a[i]) return 0;   // must be sorted
  return h;
}
