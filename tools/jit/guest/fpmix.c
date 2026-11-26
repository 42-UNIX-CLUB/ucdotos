// SPDX-License-Identifier: GPL-2.0-or-later
// Single and double precision mix: float arithmetic, conversions both ways, min/max,
// square roots, comparisons. Checksums the raw bits, so any rounding difference shows.
#define REPS 2
#include "rt.h"
uint64_t work(void) {
  float f = 1.5f, g = -0.25f;
  double d = 3.0, e = 0.0;
  uint64_t h = 0;
  for (int i = 0; i < 400000; i++) {
    float x = (float)(int32_t)(rt_rand() & 0xffff) * 0.001f - 30.0f;
    f = f * 0.999f + x * 0.001f;
    g = __builtin_fmaxf(g, x) * 0.5f + __builtin_fminf(f, x) * 0.25f;
    d = d * 0.9999 + (double)f * 1e-4;
    e += __builtin_sqrt(__builtin_fabs(d)) - (double)__builtin_sqrtf(__builtin_fabsf(g));
    int64_t k = (int64_t)(d * 1000.0);
    int32_t w = (int32_t)(g * 100.0f);
    uint32_t u = (uint32_t)__builtin_fabsf(x * 1000.0f);
    h = h * 1000003 + (uint64_t)k + (uint64_t)(int64_t)w + u + (f < g) + (d >= e);
  }
  uint32_t fb, gb; uint64_t db, eb;
  __builtin_memcpy(&fb, &f, 4); __builtin_memcpy(&gb, &g, 4); __builtin_memcpy(&db, &d, 8); __builtin_memcpy(&eb, &e, 8);
  return h ^ fb ^ ((uint64_t)gb << 32) ^ db ^ (eb >> 3);
}
