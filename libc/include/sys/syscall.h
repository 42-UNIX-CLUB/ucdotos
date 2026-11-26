// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H
#include "../../../abi/abi.h"
/* The two things a program imports from the host. `syscall` is the kernel's own
 * wasm export, so a system call is a direct wasm-to-wasm call. `block` suspends
 * this program's stack (JSPI) until the kernel wakes it. */
__attribute__((import_module("host"), import_name("syscall")))
long __host_syscall(long nr, long a, long b, long c, long d, long e, long f);
__attribute__((import_module("host"), import_name("block"))) void __host_block(void);

/* -ERESTARTSYS: wait (and run signal handlers, libc/src/syswait.c), then make the call again */
void __syscall_wait(void);
static inline long __syscall6(long nr, long a, long b, long c, long d, long e, long f) {
  long r;
  while (__builtin_expect((r = __host_syscall(nr, a, b, c, d, e, f)) == -ERESTARTSYS, 0)) __syscall_wait();
  return r;
}
#define __syscall0(n) __syscall6(n, 0, 0, 0, 0, 0, 0)
#define __syscall1(n, a) __syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define __syscall2(n, a, b) __syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define __syscall3(n, a, b, c) __syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define __syscall4(n, a, b, c, d) __syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
long __sysret(long r); /* sets errno, returns -1 on error */
#endif
