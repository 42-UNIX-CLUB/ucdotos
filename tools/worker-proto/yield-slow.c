// SPDX-License-Identifier: GPL-2.0-or-later
/* Worker-process prototype, approach C (yield points): the slow path. The inserted check
 * calls this once every YIELD_EVERY loop iterations (counted across all loops).
 * It reads the clock (one system call). When this process has run for more than SLICE_MS
 * since the last yield, it makes 256 getppid calls: the kernel looks at the clock on every
 * 256th call and parks a process whose slice is over (kernel/syscall.c), so one of them
 * yields, and a killed process unwinds there. No kernel change is needed; a kernel that let
 * sched_yield really yield would make this one call. Not instrumented itself. */
#include <sys/syscall.h>
#include <time.h>

#ifndef YIELD_EVERY
#define YIELD_EVERY 65536
#endif
#define SLICE_MS 4.0
void __ucos_yield_set(int);
static double last;
void __ucos_yield_slow(void) {
  __ucos_yield_set(YIELD_EVERY);
  struct timespec ts;
  __syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts);
  double now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
  if (now - last < SLICE_MS) return;
  for (int i = 0; i < 256; i++) __syscall0(SYS_getppid);
  last = now;
}

/* -fsanitize-coverage=trace-pc-guard variant (yield-cc.sh with YIELD_MODE=sancov): the
 * compiler calls this on every edge of the control-flow graph. */
static int cov_ctr;
void __sanitizer_cov_trace_pc_guard(unsigned *guard) {
  (void)guard;
  if (--cov_ctr < 0) { cov_ctr = YIELD_EVERY; __ucos_yield_slow(); }
}
void __sanitizer_cov_trace_pc_guard_init(unsigned *start, unsigned *stop) { (void)start; (void)stop; }
