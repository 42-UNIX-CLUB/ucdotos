// SPDX-License-Identifier: GPL-2.0-or-later
/* Runtime support that every program needs: errno, exit, atexit.
 * The entry point itself (_start) is in libc/crt/crt0.c. */
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>
char **environ;
int errno;
void __stdio_flush_all(void);
long __sysret(long r) {
  if ((unsigned long)r > -4096UL) { errno = -r; return -1; }
  return r;
}
static void (*atexit_fns[32])(void);
static int atexit_n;
int atexit(void (*fn)(void)) { if (atexit_n >= 32) return -1; atexit_fns[atexit_n++] = fn; return 0; }
void _exit(int code) { for (;;) __syscall1(SYS_exit, code); }
void exit(int code) {
  while (atexit_n) atexit_fns[--atexit_n]();
  __stdio_flush_all();
  _exit(code);
}
void abort(void) { __stdio_flush_all(); __builtin_trap(); }
