// SPDX-License-Identifier: GPL-2.0-or-later
/* hexdump [-C] [file...]: -C is the canonical hex + ASCII display */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static int canon;
static unsigned long off;
static unsigned char line[16], prev[16];
static int have_prev, starred;
static void emit(int n) {
  if (have_prev && n == 16 && !memcmp(line, prev, 16)) { if (!starred) puts("*"); starred = 1; off += 16; return; }
  starred = 0;
  if (canon) {
    printf("%08lx ", off);
    for (int i = 0; i < 16; i++) { if (i == 8) putchar(' '); if (i < n) printf(" %02x", line[i]); else printf("   "); }
    printf("  |");
    for (int i = 0; i < n; i++) putchar(line[i] >= 32 && line[i] < 127 ? line[i] : '.');
    puts("|");
  } else {
    printf("%07lx", off);
    for (int i = 0; i < n; i += 2) printf(" %04x", line[i] | (i + 1 < n ? line[i + 1] << 8 : 0));
    putchar('\n');
  }
  memcpy(prev, line, 16);
  have_prev = n == 16;
  off += n;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0, fill = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-C")) canon = 1;
    else { fprintf(stderr, "usage: hexdump [-C] [file...]\n"); return 1; }
  }
  setvbuf(stdout, 0, _IOFBF, 0);
  int nfiles = argc - i;
  for (int k = 0; k < (nfiles ? nfiles : 1); k++) {
    int fd = nfiles ? open(argv[i + k], O_RDONLY) : 0;
    if (fd < 0) { fprintf(stderr, "hexdump: %s: %s\n", argv[i + k], strerror(errno)); rc = 1; continue; }
    static unsigned char buf[16384];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
      for (long j = 0; j < n; j++) { line[fill++] = buf[j]; if (fill == 16) { emit(16); fill = 0; } }
    if (fd) close(fd);
  }
  if (fill) emit(fill);
  if (off || canon) printf(canon ? "%08lx\n" : "%07lx\n", off);
  fflush(stdout);
  return rc;
}
