// SPDX-License-Identifier: GPL-2.0-or-later
/* test: the shell's test builtin as a program, for callers that cannot reach a
 * builtin (find -exec test, xargs test, env test). The code is the builtin's
 * own, in bin/sh/testexpr.c; this file only gives it a way to print errors.
 * Called as "[" (by that name), the last argument must be "]". */
#include "sh/testexpr.c"

void outs(int fd, const char *s) { write(fd, s, strlen(s)); }
void outf(int fd, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
  if (n > 0) write(fd, buf, n);
}
int main(int argc, char **argv) { return sh_test(argc, argv); }
