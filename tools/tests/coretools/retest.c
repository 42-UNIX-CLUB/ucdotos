// SPDX-License-Identifier: GPL-2.0-or-later
/* Regular expression test driver. Reads lines "FLAGS<TAB>PATTERN<TAB>SUBJECT"
 * and prints the result of regcomp + regexec for each. Compiled natively twice
 * by run.mjs: against libc/src/regex.c (the engine under test) and against the
 * host's C library (the reference for regenerating expectations).
 * FLAGS letters: E extended, I icase, N newline, b notbol, e noteol, - none.
 * PATTERN and SUBJECT may use \n \t \\ \xHH. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

static int unescape(const char *s, char *out) {
  int n = 0;
  while (*s) {
    if (*s == '\\' && s[1]) {
      s++;
      if (*s == 'n') out[n++] = '\n';
      else if (*s == 't') out[n++] = '\t';
      else if (*s == '\\') out[n++] = '\\';
      else if (*s == 'x' && s[1] && s[2]) { char h[3] = {s[1], s[2], 0}; out[n++] = (char)strtol(h, 0, 16); s += 2; }
      else { out[n++] = '\\'; out[n++] = *s; }
      s++;
    } else out[n++] = *s++;
  }
  out[n] = 0;
  return n;
}

int main(int argc, char **argv) {
  FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
  if (!f) { perror(argv[1]); return 2; }
  char *line = 0;
  size_t cap = 0;
  long len;
  while ((len = getline(&line, &cap, f)) > 0) {
    if (line[len - 1] == '\n') line[--len] = 0;
    if (!len || line[0] == '#') continue;
    char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : 0;
    if (!t2) { printf("bad line: %s\n", line); continue; }
    *t1 = *t2 = 0;
    int cf = 0, ef = 0;
    for (char *c = line; *c; c++) {
      if (*c == 'E') cf |= REG_EXTENDED;
      else if (*c == 'I') cf |= REG_ICASE;
      else if (*c == 'N') cf |= REG_NEWLINE;
      else if (*c == 'b') ef |= REG_NOTBOL;
      else if (*c == 'e') ef |= REG_NOTEOL;
    }
    char *pat = malloc(strlen(t1 + 1) + 1), *sub = malloc(strlen(t2 + 1) + 1);
    unescape(t1 + 1, pat);
    unescape(t2 + 1, sub);
    regex_t re;
    int rc = regcomp(&re, pat, cf);
    if (rc) { printf("compile-error\n"); free(pat); free(sub); continue; }
    regmatch_t m[10];
    rc = regexec(&re, sub, 10, m, ef);
    if (rc == REG_NOMATCH) printf("nomatch\n");
    else if (rc) printf("exec-error %d\n", rc);
    else {
      printf("match");
      size_t n = re.re_nsub < 9 ? re.re_nsub : 9;
      for (size_t i = 0; i <= n; i++) printf(" %d,%d", (int)m[i].rm_so, (int)m[i].rm_eo);
      printf("\n");
    }
    regfree(&re);
    free(pat);
    free(sub);
  }
  free(line);
  if (f != stdin) fclose(f);
  return 0;
}
