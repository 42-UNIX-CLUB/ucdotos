// SPDX-License-Identifier: GPL-2.0-or-later
/* kbench: how long does one system call take? Reports the first batch (code still
 * in the engine's baseline tier) and the best batch (after the engine has optimised it). */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static double now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e9 + ts.tv_nsec; }
__attribute__((noinline)) static long batch(int n) {
  long s = 0;
  for (int i = 0; i < n; i++) s += __syscall0(SYS_getppid);
  return s;
}
int main(void) {
  const int N = 200000;
  double first = 0, best = 1e18;
  long sink = 0;
  for (int round = 0; round < 200; round++) {
    double t0 = now_ns();
    sink += batch(N);
    double dt = (now_ns() - t0) / N;
    if (round == 0) first = dt;
    if (dt < best) best = dt;
  }
  printf("getppid: first batch %.1f ns per call, best batch %.1f ns per call (%ld)\n", first, best, sink ? 1L : 0L);
  return 0;
}
