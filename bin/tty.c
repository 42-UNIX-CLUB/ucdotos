// SPDX-License-Identifier: GPL-2.0-or-later
/* tty [-s]: print the name of the terminal on standard input.
 * Exit status: 0 a terminal, 1 not a terminal, 2 usage error. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int silent = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--silent") || !strcmp(argv[i], "--quiet")) silent = 1;
    else { fprintf(stderr, "tty: extra operand '%s'\nusage: tty [-s]\n", argv[i]); return 2; }
  }
  if (!isatty(0)) { if (!silent) puts("not a tty"); return 1; }
  if (silent) return 0;
  const char *n = ttyname(0);
  if (!n) { puts("not a tty"); return 1; }
  puts(n);
  return 0;
}
