// SPDX-License-Identifier: GPL-2.0-or-later
/* touch file...: create files; for an existing file, rewrite its first byte so
 * the modification time moves (the kernel has no utimensat yet). */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv) {
  int rc = 0, i = 1, nocreate = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) if (!strcmp(argv[i], "-c")) nocreate = 1;
  if (i >= argc) { fprintf(stderr, "usage: touch [-c] file...\n"); return 1; }
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDWR | (nocreate ? 0 : O_CREAT), 0644);
    if (fd < 0) {
      if (nocreate && errno == ENOENT) continue;
      fprintf(stderr, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    char c;
    if (read(fd, &c, 1) == 1) { lseek(fd, 0, SEEK_SET); write(fd, &c, 1); }
    close(fd);
  }
  return rc;
}
