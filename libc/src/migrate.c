// SPDX-License-Identifier: GPL-2.0-or-later
/* Giving memory back: SYS_pmigrate moves this process into a fresh memory that holds only
 * its live data, at the same addresses. A WebAssembly memory never shrinks; a new one whose
 * free parts are never written costs the browser nothing for them.
 *
 * Live data: everything from the stack pointer to the end of memory (the stack in use, data,
 * bss, the heap) except the contents of free heap blocks. The engine's call stack is not
 * carried over: the new instance starts at __migrate_resume, which calls the function given
 * to migrate_call(). So that function must find all it needs in memory (globals, heap), the
 * way a program's main loop can. Programs built with the stack transform could instead
 * resume where they called (not built). */
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#define MAXR 1024
static unsigned ranges[2 * (MAXR + 1)];
static void (*resume_fn)(void);
void (*__migrate_hook)(void); /* set by migrate() in libc/src/fork.c */

static long migrate_now(void) {
  volatile char here;
  unsigned low = ((unsigned)(unsigned long)&here - 256) & ~15u; /* the stack in use, and some margin */
  int n = __migrate_ranges(ranges, MAXR + 1, low);
  return __syscall3(SYS_pmigrate, ranges, n, 0);
}
/* Move to a fresh memory, then run fn() there (it must not return; if it does, the process
 * exits with 0). Returns -1 with errno if the move could not be made; the process goes on. */
int migrate_call(void (*fn)(void)) {
  resume_fn = fn;
  return __sysret(migrate_now());
}
__attribute__((export_name("__migrate_resume"))) void __migrate_resume(void) {
  if (__migrate_hook) __migrate_hook();
  if (resume_fn) resume_fn();
  exit(0);
}
