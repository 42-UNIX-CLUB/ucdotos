// SPDX-License-Identifier: GPL-2.0-or-later
/* readlink [-f] path...: print where a symbolic link points; -f: the full resolved path */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
int main(int argc, char **argv) {
  int i = 1, canon = 0, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-e") || !strcmp(argv[i], "-m")) canon = 1;
    else if (strcmp(argv[i], "-n")) { fprintf(stderr, "usage: readlink [-f] path...\n"); return 1; }
  }
  if (i >= argc) { fprintf(stderr, "usage: readlink [-f] path...\n"); return 1; }
  for (; i < argc; i++) {
    char buf[4096];
    if (canon) {
      if (!realpath(argv[i], buf)) { rc = 1; continue; }
      puts(buf);
      continue;
    }
    long n = readlink(argv[i], buf, sizeof buf - 1);
    if (n < 0) { rc = 1; continue; } /* as GNU readlink: quiet when it is not a link */
    buf[n] = 0;
    puts(buf);
  }
  return rc;
}
