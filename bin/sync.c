// SPDX-License-Identifier: GPL-2.0-or-later
/* sync [-d] [-f] [file...]: write buffered data out. With files, each one is
 * synced; without, everything. The memory file system has nothing to write
 * today, so this matters only once files are stored somewhere. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int i = 1, rc = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (strcmp(argv[i], "-d") && strcmp(argv[i], "-f") && strcmp(argv[i], "--data") && strcmp(argv[i], "--file-system")) {
      fprintf(stderr, "sync: invalid option '%s'\nusage: sync [-d] [-f] [file...]\n", argv[i]);
      return 1;
    }
  }
  if (i >= argc) { sync(); return 0; }
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "sync: error opening '%s': %s\n", argv[i], strerror(errno)); rc = 1; continue; }
    if (fsync(fd) != 0 && errno != ENOSYS && errno != EINVAL) { fprintf(stderr, "sync: error syncing '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
    close(fd);
  }
  return rc;
}
