// SPDX-License-Identifier: GPL-2.0-or-later
/* sort [-rnuf] [-k N] [-t c] [-o file] [file...]: a stable merge sort */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static int rev, num, uniq, fold, key, sep;
static const char *field(const char *s) {
  if (key <= 1) return s;
  for (int k = 1; k < key && *s; k++) {
    if (sep) { const char *p = strchr(s, sep); s = p ? p + 1 : s + strlen(s); }
    else { while (*s == ' ' || *s == '\t') s++; while (*s && *s != ' ' && *s != '\t') s++; }
  }
  return s;
}
static int cmp(const char *a, const char *b) {
  const char *x = field(a), *y = field(b);
  int r = 0;
  if (num) {
    double u = strtod(x, NULL), v = strtod(y, NULL);
    r = u < v ? -1 : u > v;
  } else r = fold ? strcasecmp(x, y) : strcmp(x, y);
  if (!r && !uniq && (num || fold || key > 1)) r = strcmp(a, b); /* last resort: whole line */
  return rev ? -r : r;
}
static void msort(char **v, char **tmp, size_t n) {
  if (n < 2) return;
  size_t h = n / 2;
  msort(v, tmp, h);
  msort(v + h, tmp, n - h);
  size_t i = 0, j = h, k = 0;
  while (i < h && j < n) tmp[k++] = cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
  while (i < h) tmp[k++] = v[i++];
  while (j < n) tmp[k++] = v[j++];
  memcpy(v, tmp, n * sizeof *v);
}
static char *data;
static size_t dlen, dcap;
static int slurp(int fd) {
  long r;
  for (;;) {
    if (dlen + 65536 + 2 > dcap) data = realloc(data, dcap = (dcap + 65536) * 2);
    if ((r = read(fd, data + dlen, 65536)) <= 0) break;
    dlen += r;
  }
  if (dlen && data[dlen - 1] != '\n') data[dlen++] = '\n';
  return r < 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  const char *outfile = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'r') rev = 1; else if (*f == 'n') num = 1; else if (*f == 'u') uniq = 1; else if (*f == 'f') fold = 1;
      else if (*f == 'k' || *f == 't' || *f == 'o') {
        const char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "sort: option -%c needs an argument\n", *f); return 2; }
        if (*f == 'k') key = atoi(v); else if (*f == 't') sep = v[0]; else outfile = v;
        break;
      } else if (*f != 'b' && *f != 's') { fprintf(stderr, "usage: sort [-rnuf] [-k field] [-t char] [-o file] [file...]\n"); return 2; }
    }
  }
  if (i >= argc) slurp(0);
  for (; i < argc; i++) {
    int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
    if (fd < 0) { fprintf(stderr, "sort: cannot read: %s: %s\n", argv[i], strerror(errno)); rc = 2; continue; }
    slurp(fd);
    if (fd) close(fd);
  }
  size_t n = 0, cap = 1024;
  char **lines = malloc(cap * sizeof *lines);
  for (size_t p = 0; p < dlen;) {
    char *e = memchr(data + p, '\n', dlen - p);
    *e = 0;
    if (n == cap) lines = realloc(lines, (cap *= 2) * sizeof *lines);
    lines[n++] = data + p;
    p = e - data + 1;
  }
  char **tmp = malloc((n + 1) * sizeof *tmp);
  msort(lines, tmp, n);
  if (outfile) {
    int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "sort: %s: %s\n", outfile, strerror(errno)); return 2; }
    dup2(fd, 1);
    close(fd);
  }
  setvbuf(stdout, 0, _IOFBF, 0);
  for (size_t k = 0; k < n; k++) {
    if (uniq && k && !cmp(lines[k - 1], lines[k])) continue;
    fputs(lines[k], stdout);
    putchar('\n');
    if (ferror(stdout)) return 2;
  }
  fflush(stdout);
  return rc;
}
