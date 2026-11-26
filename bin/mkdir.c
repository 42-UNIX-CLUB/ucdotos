// SPDX-License-Identifier: GPL-2.0-or-later
/* mkdir [-p] dir... */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
static int mkpath(char *p) {
  struct stat st;
  for (char *s = p + 1; *s; s++) {
    if (*s != '/') continue;
    *s = 0;
    if (stat(p, &st) != 0 && mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
    *s = '/';
  }
  if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
  return mkdir(p, 0755);
}
int main(int argc, char **argv) {
  int parents = 0, rc = 0, i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "-p")) parents = 1;
    else if (!strcmp(argv[i], "-m") && i + 1 < argc) i++;
    else if (!strcmp(argv[i], "--")) { i++; break; }
    else { fprintf(stderr, "mkdir: invalid option %s\nusage: mkdir [-p] dir...\n", argv[i]); return 1; }
  }
  if (i >= argc) { fprintf(stderr, "usage: mkdir [-p] dir...\n"); return 1; }
  for (; i < argc; i++)
    if ((parents ? mkpath(argv[i]) : mkdir(argv[i], 0755)) != 0) {
      fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", argv[i], strerror(errno));
      rc = 1;
    }
  return rc;
}
