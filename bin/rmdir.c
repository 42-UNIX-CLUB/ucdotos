// SPDX-License-Identifier: GPL-2.0-or-later
/* rmdir [-p] dir... */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
int main(int argc, char **argv) {
  int parents = 0, rc = 0, i = 1;
  if (i < argc && !strcmp(argv[i], "-p")) parents = 1, i++;
  if (i >= argc) { fprintf(stderr, "usage: rmdir [-p] dir...\n"); return 1; }
  for (; i < argc; i++) {
    if (rmdir(argv[i]) != 0) { fprintf(stderr, "rmdir: failed to remove '%s': %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    if (!parents) continue;
    char *p = argv[i], *s;
    while ((s = strrchr(p, '/')) && s != p) {
      *s = 0;
      if (rmdir(p) != 0) { fprintf(stderr, "rmdir: failed to remove '%s': %s\n", p, strerror(errno)); rc = 1; break; }
    }
  }
  return rc;
}
