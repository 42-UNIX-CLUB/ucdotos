// SPDX-License-Identifier: GPL-2.0-or-later
/* fnmatch test driver: lines "FLAGS<TAB>PATTERN<TAB>STRING", prints match or
 * nomatch. FLAGS letters: P pathname, D period, N noescape, C casefold,
 * L leading dir, - none. Built against libc/src/fnmatch.c and against the
 * host's C library by run.mjs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>

int main(int argc, char **argv) {
  FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
  if (!f) { perror(argv[1]); return 2; }
  char *line = NULL;
  size_t cap = 0;
  long len;
  while ((len = getline(&line, &cap, f)) > 0) {
    if (line[len - 1] == '\n') line[--len] = 0;
    if (!len || line[0] == '#') continue;
    char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (!t2) { printf("bad line\n"); continue; }
    *t1 = *t2 = 0;
    int fl = 0;
    for (char *c = line; *c; c++)
      fl |= *c == 'P' ? FNM_PATHNAME : *c == 'D' ? FNM_PERIOD : *c == 'N' ? FNM_NOESCAPE : *c == 'C' ? FNM_CASEFOLD : *c == 'L' ? FNM_LEADING_DIR : 0;
    int r = fnmatch(t1 + 1, t2 + 1, fl);
    puts(r == 0 ? "match" : r == FNM_NOMATCH ? "nomatch" : "error");
  }
  free(line);
  if (f != stdin) fclose(f);
  return 0;
}
