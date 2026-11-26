// SPDX-License-Identifier: GPL-2.0-or-later
/* tail [-n N | -n +N | -N] [-c N] [file...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static long count = 10;
static int bytes, from_start;
static int tail(int fd) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  long r;
  while ((r = read(fd, buf + len, cap - len)) > 0) {
    len += r;
    if (len == cap) buf = realloc(buf, cap *= 2);
  }
  size_t start;
  if (bytes) start = from_start ? (count > 0 ? (size_t)count - 1 : 0) : (len > (size_t)count ? len - count : 0);
  else if (from_start) {
    long line = 1;
    start = 0;
    while (start < len && line < count) if (buf[start++] == '\n') line++;
  } else {
    long n = count;
    start = len;
    if (start && buf[start - 1] == '\n') start--; /* the last line's newline */
    while (start > 0) { if (buf[start - 1] == '\n' && --n == 0) break; start--; }
    if (count == 0) start = len;
  }
  if (start > len) start = len;
  int rc = write(1, buf + start, len - start) < 0;
  free(buf);
  return rc;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *v = NULL;
    if (!strcmp(argv[i], "--")) { i++; break; }
    if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-c")) && i + 1 < argc) { bytes = argv[i][1] == 'c'; v = argv[++i]; }
    else if (!strncmp(argv[i], "-n", 2) || !strncmp(argv[i], "-c", 2)) { bytes = argv[i][1] == 'c'; v = argv[i] + 2; }
    else if (argv[i][1] >= '0' && argv[i][1] <= '9') v = argv[i] + 1;
    else { fprintf(stderr, "usage: tail [-n [+]lines | -c [+]bytes] [file...]\n"); return 1; }
    from_start = *v == '+';
    count = labs(atol(v + (*v == '+' || *v == '-')));
  }
  int nfiles = argc - i;
  if (!nfiles) return tail(0);
  for (int k = i; k < argc; k++) {
    int fd = strcmp(argv[k], "-") ? open(argv[k], O_RDONLY) : 0;
    if (fd < 0) { fprintf(stderr, "tail: cannot open '%s' for reading: %s\n", argv[k], strerror(errno)); rc = 1; continue; }
    if (nfiles > 1) { char hdr[600]; int n = snprintf(hdr, sizeof hdr, "%s==> %s <==\n", k > i ? "\n" : "", argv[k]); write(1, hdr, n); }
    if (tail(fd)) return 1;
    if (fd) close(fd);
  }
  return rc;
}
