// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime for the translator's test programs. One source builds three ways (see Makefile.jit):
//   riscv64 (the guest):   clang --target=riscv64-unknown-elf -march=rv64im -O2 -nostdlib -static, ld.lld
//   wasm32 (the ceiling):  clang --target=wasm32 -O2 -nostdlib; the host calls run() repeatedly
//   native (reference):    cc -O2
// Each program defines `uint64_t work(void)`: one pass of the benchmark, returning a checksum.
// The guest and wasm builds run `REPS` passes; the native build times them itself.
#ifndef RT_H
#define RT_H
#include <stdint.h>
#include <stddef.h>

#ifndef REPS
#define REPS 1
#endif

uint64_t work(void);

#if defined(__riscv)
static inline long rt_sys3(long n, long a, long b, long c) {
  register long a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c, a7 __asm__("a7") = n;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
  return a0;
}
static void rt_write(const char *p, long n) { rt_sys3(64, 1, (long)p, n); }
static void rt_exit(int c) { rt_sys3(93, c, 0, 0); for (;;) {} }
#elif defined(__wasm__)
__attribute__((import_module("env"), import_name("print"))) void env_print(const char *p, size_t n);
static void rt_write(const char *p, long n) { env_print(p, (size_t)n); }
// C's fmin/fmax (a NaN operand loses), which wasm's min/max are not
float fminf(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b == a ? (__builtin_signbit(a) ? a : b) : b; }
float fmaxf(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b == a ? (__builtin_signbit(a) ? b : a) : b; }
#else
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
static void rt_write(const char *p, long n) { if (write(1, p, (size_t)n) < 0) {} }
#endif

#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__ || defined(__riscv) || defined(__wasm__)
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
#endif

static void rt_puts(const char *s) { long n = 0; while (s[n]) n++; rt_write(s, n); }
static void rt_putx(uint64_t v) {
  char b[17];
  for (int i = 15; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
  b[16] = 0;
  rt_puts(b);
}
static void rt_putu(uint64_t v) {
  char b[21]; int i = 20; b[i] = 0;
  do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
  rt_puts(b + i);
}

static uint64_t rt_rng = 0x9e3779b97f4a7c15ull;
static inline uint32_t rt_rand(void) {
  rt_rng ^= rt_rng << 13; rt_rng ^= rt_rng >> 7; rt_rng ^= rt_rng << 17;
  return (uint32_t)(rt_rng >> 16);
}

static uint64_t rt_run(void) {
  uint64_t sum = 0;
  for (int i = 0; i < REPS; i++) { rt_rng = 0x9e3779b97f4a7c15ull; sum = sum * 31 + work(); }
  return sum;
}

#if defined(__riscv)
__attribute__((noinline)) void rt_main(void) {
  uint64_t s = rt_run();
  rt_puts("checksum "); rt_putx(s); rt_puts("\n");
  rt_exit(0);
}
__attribute__((naked, section(".text.start"))) void _start(void) {
  __asm__ volatile("j rt_main\n");   // the loader set up the stack
}
#elif defined(__wasm__)
__attribute__((export_name("run"))) uint64_t run(void) { return rt_run(); }
__attribute__((export_name("report"))) void report(void) { uint64_t s = rt_run(); rt_puts("checksum "); rt_putx(s); rt_puts("\n"); }
#else
int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 5;
  uint64_t s = 0;
  double best = 1e30, first = 0;
  for (int i = 0; i < n; i++) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    s = rt_run();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    if (!i) first = ms;
    if (ms < best) best = ms;
  }
  rt_puts("checksum "); rt_putx(s); rt_puts("\n");
  char buf[80]; int len = 0;
  (void)buf; (void)len;
  rt_puts("@@NATIVE best_ms="); rt_putu((uint64_t)(best * 1000)); rt_puts(" first_ms="); rt_putu((uint64_t)(first * 1000)); rt_puts(" (microseconds)\n");
  return 0;
}
#endif
#endif
