// SPDX-License-Identifier: GPL-2.0-or-later
/* uniq [-c] [-d] [-u] [-i] [input [output]] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static int fc, fd_, fu, fi;
static FILE *out;
static void flush_line(const char *l, long n) {
  if (!l || (fd_ && n < 2) || (fu && n > 1)) return;
  if (fc) fprintf(out, "%7ld %s", n, l); else fputs(l, out);
}
static char *getline_(FILE *f) {
  size_t cap = 256, n = 0;
  char *s = malloc(cap);
  int c;
  while ((c = fgetc(f)) != EOF) { if (n + 2 >= cap) s = realloc(s, cap *= 2); s[n++] = c; if (c == '\n') break; }
  if (!n) { free(s); return NULL; }
  if (s[n - 1] != '\n') s[n++] = '\n';
  s[n] = 0;
  return s;
}
int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'c') fc = 1; else if (*f == 'd') fd_ = 1; else if (*f == 'u') fu = 1; else if (*f == 'i') fi = 1;
      else { fprintf(stderr, "usage: uniq [-cdui] [input [output]]\n"); return 1; }
    }
  FILE *in = stdin;
  out = stdout;
  if (i < argc && strcmp(argv[i], "-") && !(in = fopen(argv[i], "r"))) { fprintf(stderr, "uniq: %s: %s\n", argv[i], strerror(errno)); return 1; }
  if (i + 1 < argc && !(out = fopen(argv[i + 1], "w"))) { fprintf(stderr, "uniq: %s: %s\n", argv[i + 1], strerror(errno)); return 1; }
  if (out == stdout) setvbuf(stdout, 0, _IOFBF, 0);
  char *prev = NULL, *l;
  long n = 0;
  while ((l = getline_(in))) {
    if (prev && !(fi ? strcasecmp(prev, l) : strcmp(prev, l))) { n++; free(l); continue; }
    flush_line(prev, n);
    free(prev);
    prev = l;
    n = 1;
    if (ferror(out)) return 1;
  }
  flush_line(prev, n);
  fflush(out);
  return 0;
}
