// SPDX-License-Identifier: GPL-2.0-or-later
/* paste [-s] [-d LIST] [-z] file...: merge lines. LIST is used in turn and
 * takes \n \t \\ and \0 (no delimiter). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static char *dl;
static int ndl, eol = '\n';

static void parse_delims(const char *s) {
  dl = malloc(strlen(s) + 1);
  ndl = 0;
  for (; *s; s++) {
    int c = *s;
    if (c == '\\' && s[1]) {
      s++;
      c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == '0' ? 0 : *s == '\\' ? '\\' : *s == 'b' ? '\b' : *s == 'f' ? '\f' : *s == 'r' ? '\r' : *s == 'v' ? '\v' : *s;
      if (*s == '0') c = -1;
    }
    dl[ndl++] = c;
  }
}
static void put_delim(int k) { int c = (signed char)dl[k % ndl]; if (c != -1) putchar(c); }
/* one line without its terminator; returns length or -1 at end */
static long get_line(FILE *f, char **buf, size_t *cap) { long n = getdelim(buf, cap, eol, f); if (n > 0 && (*buf)[n - 1] == eol) n--; return n; }

int main(int argc, char **argv) {
  int i = 1, serial = 0, rc = 0;
  parse_delims("\\t");
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--serial")) { serial = 1; continue; }
    if (!strcmp(a, "--zero-terminated")) { eol = 0; continue; }
    if (!strncmp(a, "--delimiters=", 13)) { parse_delims(a + 13); continue; }
    for (char *f = a + 1; *f; f++) {
      if (*f == 's') serial = 1;
      else if (*f == 'z') eol = 0;
      else if (*f == 'd') {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "paste: option requires an argument -- 'd'\n"); return 1; }
        parse_delims(v);
        break;
      } else { fprintf(stderr, "paste: invalid option -- '%c'\nusage: paste [-s] [-d list] file...\n", *f); return 1; }
    }
  }
  if (ndl == 0) { dl = realloc(dl, 1); dl[0] = -1; ndl = 1; } /* -d '' */
  static char *dash[] = {"-"};
  char **names = i < argc ? argv + i : dash;
  int n = i < argc ? argc - i : 1;
  FILE **fs = calloc(n, sizeof *fs);
  for (int k = 0; k < n; k++) {
    fs[k] = strcmp(names[k], "-") ? fopen(names[k], "r") : stdin;
    if (!fs[k]) { fprintf(stderr, "paste: %s: %s\n", names[k], strerror(errno)); return 1; }
  }
  setvbuf(stdout, NULL, _IOFBF, 65536);
  char *buf = NULL;
  size_t cap = 0;
  if (serial) {
    for (int k = 0; k < n; k++) {
      long len, cnt = 0;
      while ((len = get_line(fs[k], &buf, &cap)) >= 0) {
        if (cnt) put_delim(cnt - 1);
        fwrite(buf, 1, len, stdout);
        cnt++;
      }
      putchar(eol);
      if (fs[k] != stdin) fclose(fs[k]);
    }
  } else {
    /* each row: every file's next line (empty once it has ended), with the
       delimiters between them; a row is written while any file has lines */
    int *done = calloc(n, sizeof *done), left = n;
    char *row = NULL;
    size_t rcap = 0, rlen;
    while (left) {
      int got = 0;
      rlen = 0;
      for (int k = 0; k < n; k++) {
        long len = -1;
        if (!done[k]) {
          len = get_line(fs[k], &buf, &cap);
          if (len < 0) { done[k] = 1; left--; }
        }
        if (rlen + (len > 0 ? len : 0) + 2 > rcap) row = realloc(row, rcap = (rlen + len + 2) * 2);
        if (len >= 0) { memcpy(row + rlen, buf, len); rlen += len; got = 1; }
        if (k < n - 1) { int c = (signed char)dl[k % ndl]; if (c != -1) row[rlen++] = c; }
      }
      if (got) { fwrite(row, 1, rlen, stdout); putchar(eol); }
    }
  }
  if (fflush(stdout)) { perror("paste: write error"); rc = 1; }
  return rc;
}
