// SPDX-License-Identifier: GPL-2.0-or-later
// Recursive Fibonacci: nothing but calls and returns.
#define REPS 2
#include "rt.h"
__attribute__((noinline)) static uint64_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
uint64_t work(void) { return fib(32); }
