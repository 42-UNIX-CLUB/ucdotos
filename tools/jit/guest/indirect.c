// SPDX-License-Identifier: GPL-2.0-or-later
// Calls through a table of function pointers chosen at random: the indirect-call worst case.
#define REPS 2
#include "rt.h"
typedef uint64_t (*fn)(uint64_t, uint64_t);
__attribute__((noinline)) static uint64_t f0(uint64_t a, uint64_t b) { return a + b; }
__attribute__((noinline)) static uint64_t f1(uint64_t a, uint64_t b) { return a ^ (b << 1); }
__attribute__((noinline)) static uint64_t f2(uint64_t a, uint64_t b) { return a * 3 + b; }
__attribute__((noinline)) static uint64_t f3(uint64_t a, uint64_t b) { return (a >> 3) | b; }
__attribute__((noinline)) static uint64_t f4(uint64_t a, uint64_t b) { return a - (b & 255); }
__attribute__((noinline)) static uint64_t f5(uint64_t a, uint64_t b) { return a + (b >> 2) + 1; }
__attribute__((noinline)) static uint64_t f6(uint64_t a, uint64_t b) { return (a << 1) ^ b; }
__attribute__((noinline)) static uint64_t f7(uint64_t a, uint64_t b) { return a + b * 5; }
static fn table[8] = { f0, f1, f2, f3, f4, f5, f6, f7 };
uint64_t work(void) {
  uint64_t acc = 1;
  for (int i = 0; i < 3000000; i++) acc = table[rt_rand() & 7](acc, (uint64_t)i);
  return acc;
}
