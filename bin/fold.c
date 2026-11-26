// SPDX-License-Identifier: GPL-2.0-or-later
/* fold [-bs] [-w WIDTH] [file...]: wrap lines at WIDTH columns (default 80).
 * Columns: a tab goes to the next multiple of 8, backspace goes back one,
 * carriage return goes to 0; with -b every byte is one column. -s breaks
 * after the last blank that fits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int bytes, spaces;
static long width = 80;

static long advance(long col, int c) {
  if (bytes) return col + 1;
  if (c == '\t') return col + 8 - col % 8;
  if (c == '\b') return col > 0 ? col - 1 : 0;
  if (c == '\r') return 0;
  return col + 1;
}
static void fold(FILE *f) {
  char *line = NULL;
  size_t cap = 0, lcap = 0;
  long len;
  char *buf = NULL; /* the part of the current output line not yet written */
  while ((len = getline(&line, &cap, f)) > 0) {
    int nl = line[len - 1] == '\n';
    if (nl) len--;
    long col = 0, n = 0;
    if ((size_t)len + 1 > lcap) buf = realloc(buf, lcap = len + 1);
    for (long i = 0; i < len; i++) {
      int c = (unsigned char)line[i];
      long nc = advance(col, c);
      if (nc > width && n > 0 && (bytes || !(c == '\b' || c == '\r'))) {
        long cut = n;
        if (spaces) {
          long k = n;
          while (k > 0 && buf[k - 1] != ' ' && buf[k - 1] != '\t') k--;
          if (k > 0) cut = k;
        }
        fwrite(buf, 1, cut, stdout);
        putchar('\n');
        memmove(buf, buf + cut, n - cut);
        n -= cut;
        col = 0;
        for (long k = 0; k < n; k++) col = advance(col, (unsigned char)buf[k]);
        nc = advance(col, c);
      }
      buf[n++] = c;
      col = nc;
    }
    fwrite(buf, 1, n, stdout);
    if (nl) putchar('\n');
  }
  free(line);
  free(buf);
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--bytes")) { bytes = 1; continue; }
    if (!strcmp(a, "--spaces")) { spaces = 1; continue; }
    if (!strncmp(a, "--width=", 8)) { width = atol(a + 8); if (width < 1) { fprintf(stderr, "fold: invalid number of columns: '%s'\n", a + 8); return 1; } continue; }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'b') bytes = 1;
      else if (*f == 's') spaces = 1;
      else if (*f == 'w' || (*f >= '0' && *f <= '9')) {
        char *v = *f == 'w' ? (f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL)) : f;
        char *e;
        if (!v) { fprintf(stderr, "fold: option requires an argument -- 'w'\n"); return 1; }
        width = strtol(v, &e, 10);
        if (*e || width < 1) { fprintf(stderr, "fold: invalid number of columns: '%s'\n", v); return 1; }
        break;
      } else { fprintf(stderr, "fold: invalid option -- '%c'\nusage: fold [-bs] [-w width] [file...]\n", *f); return 1; }
    }
  }
  setvbuf(stdout, NULL, _IOFBF, 65536);
  if (i >= argc) fold(stdin);
  for (; i < argc; i++) {
    FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
    if (!f) { fprintf(stderr, "fold: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    fold(f);
    if (f != stdin) fclose(f);
  }
  if (fflush(stdout)) rc = 1;
  return rc;
}
