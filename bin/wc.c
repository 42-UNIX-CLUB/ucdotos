// SPDX-License-Identifier: GPL-2.0-or-later
/* wc [-lwc] [file...] */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static int fl, fw, fc;
static void show(long l, long w, long c, const char *name) {
  int many = fl + fw + fc > 1 || name;
  const char *fmt = many ? "%7ld" : "%ld";
  int first = 1;
  if (fl) { printf(fmt, l); first = 0; }
  if (fw) { if (!first) putchar(' '); printf(fmt, w); first = 0; }
  if (fc) { if (!first) putchar(' '); printf(fmt, c); }
  if (name) printf(" %s", name);
  putchar('\n');
}
static int count(int fd, long *l, long *w, long *c) {
  static char buf[65536];
  long n;
  int inword = 0;
  *l = *w = *c = 0;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    *c += n;
    for (long i = 0; i < n; i++) {
      char ch = buf[i];
      if (ch == '\n') (*l)++;
      int sp = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f';
      if (!sp && !inword) (*w)++;
      inword = !sp;
    }
  }
  return n < 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'l') fl = 1; else if (*f == 'w') fw = 1; else if (*f == 'c' || *f == 'm') fc = 1;
      else { fprintf(stderr, "usage: wc [-lwc] [file...]\n"); return 1; }
    }
  if (!fl && !fw && !fc) fl = fw = fc = 1;
  long l, w, c, tl = 0, tw = 0, tc = 0;
  if (i >= argc) { count(0, &l, &w, &c); show(l, w, c, NULL); return 0; }
  int nfiles = argc - i;
  for (; i < argc; i++) {
    int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
    if (fd < 0) { fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    count(fd, &l, &w, &c);
    if (fd) close(fd);
    show(l, w, c, argv[i]);
    tl += l; tw += w; tc += c;
  }
  if (nfiles > 1) show(tl, tw, tc, "total");
  return rc;
}
