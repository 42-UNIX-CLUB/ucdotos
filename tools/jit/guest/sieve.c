// SPDX-License-Identifier: GPL-2.0-or-later
// Sieve of Eratosthenes over 2 million bytes: byte loads and stores, nested loops.
#define REPS 6
#include "rt.h"
#define N 2000000
static uint8_t comp[N + 1];
uint64_t work(void) {
  for (uint32_t i = 0; i <= N; i++) comp[i] = 0;
  uint64_t count = 0;
  for (uint32_t i = 2; i <= N; i++) {
    if (comp[i]) continue;
    count += i;
    for (uint32_t j = i + i; j <= N; j += i) comp[j] = 1;
  }
  return count;
}
