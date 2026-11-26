// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal runtime for guest test programs: no libc, Linux system calls only. The same
 * source also builds natively (with the host libc) so that outputs can be compared. */
#ifndef LX_TEST_RT_H
#define LX_TEST_RT_H
#include <stdint.h>
#include <stddef.h>

#if defined(__riscv)
static inline long rt_sys(long n, long a, long b, long c, long d, long e, long f) {
  register long a0 __asm__("a0") = a, a1 __asm__("a1") = b, a2 __asm__("a2") = c;
  register long a3 __asm__("a3") = d, a4 __asm__("a4") = e, a5 __asm__("a5") = f;
  register long a7 __asm__("a7") = n;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
  return a0;
}
#define RT_WRITE 64
#define RT_EXIT 93
static inline long rt_write(int fd, const void *p, size_t n) { return rt_sys(RT_WRITE, fd, (long)p, (long)n, 0, 0, 0); }
static inline void rt_exit(int c) { rt_sys(RT_EXIT, c, 0, 0, 0, 0, 0); for (;;) {} }
int main(void);
static int rt_argc;
static char **rt_argv;
__attribute__((naked, noreturn)) void _start(void) {
  __asm__ volatile(".option push\n.option norelax\nla gp, __global_pointer$\n.option pop\n"
                   "mv a0, sp\ncall rt_start\n");
}
__attribute__((used)) void rt_start(long *sp) { rt_argc = (int)sp[0]; rt_argv = (char **)(sp + 1); rt_exit(main()); }
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
#else
#include <unistd.h>
#include <stdlib.h>
static inline long rt_write(int fd, const void *p, size_t n) { return write(fd, p, n); }
static inline void rt_exit(int c) { exit(c); }
#endif

static inline size_t rt_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline void rt_puts(const char *s) { rt_write(1, s, rt_strlen(s)); }
static inline void rt_hex(uint64_t v) {
  char b[17];
  for (int i = 15; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
  b[16] = 0;
  rt_puts(b);
}
static inline void rt_dec(int64_t v) {
  char b[24]; int i = 23; b[i] = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) b[--i] = '-';
  rt_puts(b + i);
}
#endif
