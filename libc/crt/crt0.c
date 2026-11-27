// SPDX-License-Identifier: GPL-2.0-or-later
/* Program entry. Built twice: crt0_2.o calls main(argc, argv), crt0_3.o calls
 * main(argc, argv, envp). WebAssembly checks function signatures exactly, so the
 * build links whichever one matches the program's main. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#ifdef MAIN3
int main(int, char **, char **);
#define CALL_MAIN(c, v, e) main(c, v, e)
#else
int main(int, char **);
#define CALL_MAIN(c, v, e) main(c, v)
#endif
extern char **environ;
/* Present when the program links fork (libc/src/fork.c): main then runs under its catcher. */
extern int __fork_main(int (*)(void)) __attribute__((weak));
static int main_argc;
static char **main_argv;
static int main_thunk(void) { return CALL_MAIN(main_argc, main_argv, environ); }

/* The kernel hands over argv and the environment as one blob: argc, envc, then the
 * strings. The kernel caps the blob at 64 KiB. Almost every blob fits in 4 KiB, so only a
 * bigger one (the kernel answers -ERANGE) is given the full 64 KiB, from the heap: a static
 * 64 KiB buffer would cost every program a page of memory it never uses. */
__attribute__((export_name("_start"))) void _start(void) {
  static char small[4096];
  static char *none[2];
  char *blob = small;
  long n = __syscall2(SYS_getargs, blob, sizeof small);
  if (n == -ERANGE && (blob = malloc(65536))) n = __syscall2(SYS_getargs, blob, 65536);
  if (!blob || n < 0) { blob = small; n = 0; }
  int argc = 0, envc = 0;
  if (n >= 8) { memcpy(&argc, blob, 4); memcpy(&envc, blob + 4, 4); }
  char **vec = malloc(((unsigned)argc + (unsigned)envc + 2) * sizeof *vec);
  if (!vec) { vec = none; argc = envc = 0; }
  char *p = blob + 8, *end = blob + (n > 8 ? n : 8);
  int i = 0, got = 0;
  for (; got < argc && p < end; got++) { vec[i++] = p; p += strlen(p) + 1; }
  argc = got;
  vec[i++] = 0;
  environ = &vec[i];
  for (int e = 0; e < envc && p < end; e++) { vec[i++] = p; p += strlen(p) + 1; }
  vec[i] = 0;
  if (__fork_main) { main_argc = argc; main_argv = vec; exit(__fork_main(main_thunk)); }
  exit(CALL_MAIN(argc, vec, environ));
}
