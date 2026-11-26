// SPDX-License-Identifier: GPL-2.0-or-later
// 160 x 160 integer matrix multiply: 32-bit multiply-adds, strided loads.
#define REPS 3
#include "rt.h"
#define N 160
static uint32_t A[N][N], Bm[N][N], C[N][N];
uint64_t work(void) {
  for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { A[i][j] = rt_rand() & 0xffff; Bm[i][j] = rt_rand() & 0xffff; }
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      uint32_t s = 0;
      for (int k = 0; k < N; k++) s += A[i][k] * Bm[k][j];
      C[i][j] = s;
    }
  uint64_t h = 0;
  for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) h = h * 31 + C[i][j];
  return h;
}
