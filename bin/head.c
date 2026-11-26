// SPDX-License-Identifier: GPL-2.0-or-later
/* head [-n N | -N] [-c N] [file...] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static long count = 10;
static int bytes;
static int head(int fd) {
  static char buf[16384];
  long left = count, n;
  if (left <= 0) return 0;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    long k = 0;
    if (bytes) k = n < left ? n : left, left -= k;
    else { while (k < n && left) if (buf[k++] == '\n') left--; }
    if (write(1, buf, k) < 0) return 1;
    if (!left) break;
  }
  return 0;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-c")) && i + 1 < argc) { bytes = argv[i][1] == 'c'; count = atol(argv[++i]); }
    else if (!strncmp(argv[i], "-n", 2) || !strncmp(argv[i], "-c", 2)) { bytes = argv[i][1] == 'c'; count = atol(argv[i] + 2); }
    else if (argv[i][1] >= '0' && argv[i][1] <= '9') count = atol(argv[i] + 1);
    else { fprintf(stderr, "usage: head [-n lines | -c bytes] [file...]\n"); return 1; }
  }
  int nfiles = argc - i;
  if (!nfiles) return head(0);
  for (int k = i; k < argc; k++) {
    int fd = strcmp(argv[k], "-") ? open(argv[k], O_RDONLY) : 0;
    if (fd < 0) { fprintf(stderr, "head: cannot open '%s' for reading: %s\n", argv[k], strerror(errno)); rc = 1; continue; }
    if (nfiles > 1) { char hdr[600]; int n = snprintf(hdr, sizeof hdr, "%s==> %s <==\n", k > i ? "\n" : "", argv[k]); write(1, hdr, n); }
    if (head(fd)) return 1;
    if (fd) close(fd);
  }
  return rc;
}
