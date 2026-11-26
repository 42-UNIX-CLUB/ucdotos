// SPDX-License-Identifier: GPL-2.0-or-later
/* tee [-a] file...: copy stdin to stdout and to files */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv) {
  int app = 0, i = 1, fds[30], n = 0, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) if (!strcmp(argv[i], "-a")) app = 1;
  for (; i < argc && n < 30; i++) {
    int fd = open(argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) { fprintf(stderr, "tee: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    fds[n++] = fd;
  }
  static char buf[16384];
  long r;
  int out_ok = 1;
  while ((r = read(0, buf, sizeof buf)) > 0) {
    if (out_ok && write(1, buf, r) < 0) out_ok = 0;
    for (int k = 0; k < n; k++) write(fds[k], buf, r);
  }
  for (int k = 0; k < n; k++) close(fds[k]);
  return rc;
}
