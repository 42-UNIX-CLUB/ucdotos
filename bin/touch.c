// SPDX-License-Identifier: GPL-2.0-or-later
/* touch [-c] file...: create files, or set an existing file's times to now */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#ifndef SYS_utimensat
#define SYS_utimensat 88
#endif
int main(int argc, char **argv) {
  int rc = 0, i = 1, nocreate = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) if (!strcmp(argv[i], "-c")) nocreate = 1;
  if (i >= argc) { fprintf(stderr, "usage: touch [-c] file...\n"); return 1; }
  for (; i < argc; i++) {
    if (__syscall4(SYS_utimensat, AT_FDCWD, argv[i], 0, 0) == 0) continue; /* exists: times = now */
    if (nocreate) continue;
    int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
    if (fd < 0) { fprintf(stderr, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    close(fd);
  }
  return rc;
}
