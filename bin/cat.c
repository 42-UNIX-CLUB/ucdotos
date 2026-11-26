// SPDX-License-Identifier: GPL-2.0-or-later
/* cat [-n] [-b] [-s] [file...]: "-" is standard input */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static int number, nonblank, squeeze, line = 1, at_bol = 1, blanks;
static int dump(int fd) {
  static char buf[65536];
  long n;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    if (!number && !nonblank && !squeeze) { if (write(1, buf, n) < 0) return 2; continue; }
    for (long i = 0; i < n; i++) {
      char c = buf[i];
      if (at_bol) {
        if (c == '\n') { if (squeeze && ++blanks > 1) continue; }
        else blanks = 0;
        if (number && !(nonblank && c == '\n')) printf("%6d\t", line++);
      }
      putchar(c);
      at_bol = c == '\n';
    }
    if (ferror(stdout)) return 2;
  }
  return n < 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0, files = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'n') number = 1;
      else if (*f == 'b') number = nonblank = 1;
      else if (*f == 's') squeeze = 1;
      else if (*f != 'u') { fprintf(stderr, "cat: invalid option -- '%c'\nusage: cat [-nbs] [file...]\n", *f); return 1; }
    }
  }
  for (; i < argc; i++) {
    files++;
    int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
    if (fd < 0) { fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    int r = dump(fd);
    if (r == 1) { fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
    if (fd) close(fd);
    if (r == 2) return 1;
  }
  if (!files) rc = dump(0) != 0;
  fflush(stdout);
  return rc;
}
