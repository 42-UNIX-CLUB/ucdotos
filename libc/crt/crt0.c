// SPDX-License-Identifier: GPL-2.0-or-later
/* Program entry. Built twice: crt0_2.o calls main(argc, argv), crt0_3.o calls
 * main(argc, argv, envp). WebAssembly checks function signatures exactly, so the
 * build links whichever one matches the program's main. */
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

/* The kernel hands over argv and the environment as one blob: argc, envc, then the strings. */
__attribute__((export_name("_start"))) void _start(void) {
  static char blob[8192];
  static char *vec[256];
  long n = __syscall2(SYS_getargs, blob, sizeof blob);
  int argc = 0, envc = 0;
  if (n >= 8) { memcpy(&argc, blob, 4); memcpy(&envc, blob + 4, 4); }
  char *p = blob + 8;
  int i = 0;
  for (; i < argc && i < 200; i++) { vec[i] = p; p += strlen(p) + 1; }
  vec[i++] = 0;
  environ = &vec[i];
  for (int e = 0; e < envc && i < 254; e++) { vec[i++] = p; p += strlen(p) + 1; }
  vec[i] = 0;
  exit(CALL_MAIN(argc, vec, environ));
}
