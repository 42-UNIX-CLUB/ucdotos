// SPDX-License-Identifier: GPL-2.0-or-later
/* nproc [--all] [--ignore=N]: the number of processors available */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int all = 0;
  long ignore = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--all")) all = 1;
    else if (!strncmp(argv[i], "--ignore=", 9)) ignore = atol(argv[i] + 9);
    else if (!strcmp(argv[i], "--ignore") && i + 1 < argc) ignore = atol(argv[++i]);
    else { fprintf(stderr, "nproc: extra operand '%s'\nusage: nproc [--all] [--ignore=N]\n", argv[i]); return 1; }
  }
  long n = sysconf(all ? _SC_NPROCESSORS_CONF : _SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  n -= ignore;
  if (n < 1) n = 1;
  printf("%ld\n", n);
  return 0;
}
