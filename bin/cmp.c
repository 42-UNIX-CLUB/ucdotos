// SPDX-License-Identifier: GPL-2.0-or-later
/* cmp [-l | -s] [-b] [-n LIMIT] [-i SKIP1[:SKIP2]] file1 [file2 [skip1 [skip2]]]
 * Exit status: 0 the same, 1 different, 2 trouble. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static long long num(const char *s) {
  char *e;
  long long v = strtoll(s, &e, 0);
  if (*e == 'k' || *e == 'K') v *= 1024, e++;
  else if (*e == 'M') v *= 1024 * 1024, e++;
  if (*e || v < 0) { fprintf(stderr, "cmp: invalid number '%s'\n", s); exit(2); }
  return v;
}
static FILE *open_in(const char *name) {
  FILE *f = strcmp(name, "-") ? fopen(name, "rb") : stdin;
  if (!f) { fprintf(stderr, "cmp: %s: %s\n", name, strerror(errno)); exit(2); }
  return f;
}
static void skip(FILE *f, long long n) { while (n-- > 0 && getc(f) != EOF) {} }
static void show_byte(int c) {
  if (c >= 128) { fputs("M-", stdout); c -= 128; }
  if (c < 32) printf("^%c", c + 64);
  else if (c == 127) fputs("^?", stdout);
  else putchar(c);
}

int main(int argc, char **argv) {
  int i = 1, list = 0, silent = 0, bytes = 0;
  long long limit = -1, sk[2] = {0, 0};
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--verbose")) { list = 1; continue; }
    if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) { silent = 1; continue; }
    if (!strcmp(a, "--print-bytes")) { bytes = 1; continue; }
    if (!strncmp(a, "--bytes=", 8)) { limit = num(a + 8); continue; }
    if (!strncmp(a, "--ignore-initial=", 17)) { char *c = strchr(a + 17, ':'); if (c) { *c = 0; sk[1] = num(c + 1); } else sk[1] = num(a + 17); sk[0] = num(a + 17); continue; }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'l') list = 1;
      else if (*f == 's') silent = 1;
      else if (*f == 'b') bytes = 1;
      else if (*f == 'n' || *f == 'i') {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "cmp: option requires an argument -- '%c'\n", *f); return 2; }
        if (*f == 'n') limit = num(v);
        else { char *c = strchr(v, ':'); if (c) { *c = 0; sk[1] = num(c + 1); } else sk[1] = num(v); sk[0] = num(v); }
        break;
      } else { fprintf(stderr, "cmp: invalid option -- '%c'\nusage: cmp [-l | -s] file1 [file2 [skip1 [skip2]]]\n", *f); return 2; }
    }
  }
  if (list && silent) { fprintf(stderr, "cmp: options -l and -s are incompatible\n"); return 2; }
  int n = argc - i;
  if (n < 1) { fprintf(stderr, "cmp: missing operand\n"); return 2; }
  if (n > 4) { fprintf(stderr, "cmp: extra operand '%s'\n", argv[i + 4]); return 2; }
  const char *n1 = argv[i], *n2 = n > 1 ? argv[i + 1] : "-";
  if (n > 2) sk[0] = num(argv[i + 2]);
  if (n > 3) sk[1] = num(argv[i + 3]);
  if (!strcmp(n1, "-") && !strcmp(n2, "-")) return 0;
  FILE *f1 = open_in(n1), *f2 = open_in(n2);
  skip(f1, sk[0]);
  skip(f2, sk[1]);
  long long pos = 0, line = 1;
  int differ = 0;
  for (;;) {
    if (limit >= 0 && pos >= limit) break;
    int a = getc(f1), b = getc(f2);
    if (a == EOF || b == EOF) {
      if (a == b) break;
      if (!silent) {
        fflush(stdout);
        const char *shorter = a == EOF ? n1 : n2;
        if (pos == 0) fprintf(stderr, "cmp: EOF on %s which is empty\n", shorter);
        else if (list) fprintf(stderr, "cmp: EOF on %s after byte %lld\n", shorter, pos);
        else fprintf(stderr, "cmp: EOF on %s after byte %lld, in line %lld\n", shorter, pos, line);
      }
      return 1;
    }
    pos++;
    if (a != b) {
      differ = 1;
      if (silent) return 1;
      if (!list) {
        printf("%s %s differ: %s %lld, line %lld", n1, n2, bytes ? "byte" : "char", pos, line);
        if (bytes) { printf(" is %3o ", a); show_byte(a); printf(" %3o ", b); show_byte(b); }
        putchar('\n');
        return 1;
      }
      if (bytes) { printf("%lld %3o ", pos, a); show_byte(a); printf(" %3o ", b); show_byte(b); putchar('\n'); }
      else printf("%lld %3o %3o\n", pos, a, b);
    }
    if (a == '\n') line++;
  }
  return differ;
}
