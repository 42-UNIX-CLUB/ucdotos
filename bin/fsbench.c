// SPDX-License-Identifier: GPL-2.0-or-later
/* fsbench: file system scaling and memory figures.
 *
 *   fsbench [N] [DIR]
 *
 * Creates N empty files in one new directory (default 50000 in /tmp/fsb), then reports:
 * the time per create in batches, the time to look up one name (stat) at the start and at
 * the end of the directory, the time to list the directory, the kernel heap used per file,
 * per hard link and per directory, and the time to remove everything. Written so that it
 * runs on kernels without hard links too (that row then says n/a). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static long heap(void) { struct sysinfo si; return sysinfo(&si) ? -1 : (long)si.bufferram; }

static double __attribute__((noinline)) stat_many(const char *path, int n) {
  struct stat st;
  double t0 = now_ms();
  for (int i = 0; i < n; i++) stat(path, &st);
  return (now_ms() - t0) * 1e3 / n;
}

/* fsbench calls PATH: cost of write (64 bytes, rewritten in place), open + close and stat on
 * one file, best of 5 batches. On a stored file every write marks it dirty. */
static double __attribute__((noinline)) batch_write(int fd, int n) { char b[64] = {1}; double t0 = now_ms(); for (int i = 0; i < n; i++) { __syscall4(SYS_pwrite64, fd, b, 64, (i & 15) * 64); } return (now_ms() - t0) * 1e6 / n; }
static double __attribute__((noinline)) batch_open(const char *p, int n) { double t0 = now_ms(); for (int i = 0; i < n; i++) close(open(p, O_RDONLY)); return (now_ms() - t0) * 1e6 / n; }
static double __attribute__((noinline)) batch_stat(const char *p, int n) { struct stat st; double t0 = now_ms(); for (int i = 0; i < n; i++) stat(p, &st); return (now_ms() - t0) * 1e6 / n; }
static int calls(const char *p) {
  int fd = open(p, O_CREAT | O_RDWR, 0644);
  if (fd < 0) { printf("fsbench: %s: %s\n", p, strerror(errno)); return 1; }
  double w = 1e9, o = 1e9, s = 1e9;
  for (int k = 0; k < 5; k++) {
    double x = batch_write(fd, 100000); if (x < w) w = x;
    x = batch_open(p, 50000); if (x < o) o = x;
    x = batch_stat(p, 50000); if (x < s) s = x;
  }
  close(fd);
  printf("fsbench calls %s: pwrite 64 B %.0f ns, open+close %.0f ns, stat %.0f ns (best of 5)\n", p, w, o, s);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "calls")) return calls(argv[2]);
  if (argc > 3 && !strcmp(argv[1], "loop")) { /* fsbench loop open|stat PATH [LINK]: 2 million calls */
    char d[256];
    for (char *q = strchr(argv[3] + 1, '/'); q; q = strchr(q + 1, '/')) { /* make the parent directories */
      snprintf(d, sizeof d, "%.*s", (int)(q - argv[3]), argv[3]);
      mkdir(d, 0755);
    }
    int fd = open(argv[3], O_CREAT | O_RDWR, 0644); close(fd);
    const char *target = argv[3];
    if (argc > 4) { __syscall6(36 /* symlinkat */, (long)argv[3], AT_FDCWD, (long)argv[4], 0, 0, 0); target = argv[4]; }
    double t = argv[2][0] == 'o' ? batch_open(target, 2000000) : batch_stat(target, 2000000);
    printf("fsbench loop %s %s: %.0f ns\n", argv[2], target, t);
    return 0;
  }
  int n = argc > 1 ? atoi(argv[1]) : 50000;
  const char *dir = argc > 2 ? argv[2] : "/tmp/fsb";
  char p[256];
  if (mkdir(dir, 0755) && errno != EEXIST) { printf("fsbench: mkdir %s: %s\n", dir, strerror(errno)); return 1; }
  long h0 = heap();
  double t0 = now_ms(), tb = t0;
  int step = n / 10 ? n / 10 : 1;
  printf("fsbench: creating %d files in %s\n", n, dir);
  for (int i = 0; i < n; i++) {
    snprintf(p, sizeof p, "%s/file-%06d", dir, i);
    int fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { printf("fsbench: create %s: %s\n", p, strerror(errno)); return 1; }
    close(fd);
    if ((i + 1) % step == 0) {
      double t = now_ms();
      printf("  files %6d..%6d: %8.1f us per create\n", i + 1 - step, i + 1, (t - tb) * 1e3 / step);
      tb = t;
    }
  }
  double tc = now_ms() - t0;
  long h1 = heap();
  printf("  total create: %.0f ms; kernel heap %.1f bytes per file (name 11 bytes)\n", tc, (double)(h1 - h0) / n);
  {
    struct stat st;
    t0 = now_ms();
    for (int i = 0; i < n; i++) { snprintf(p, sizeof p, "%s/file-%06d", dir, i); stat(p, &st); }
    printf("  stat each name once: %.0f ms (%.2f us each)\n", now_ms() - t0, (now_ms() - t0) * 1e3 / n);
  }
  snprintf(p, sizeof p, "%s/file-%06d", dir, 0);
  printf("  stat first name: %.2f us\n", stat_many(p, 2000));
  snprintf(p, sizeof p, "%s/file-%06d", dir, n - 1);
  printf("  stat last name:  %.2f us\n", stat_many(p, 2000));
  snprintf(p, sizeof p, "%s/missing", dir);
  printf("  stat a missing name: %.2f us\n", stat_many(p, 2000));
  t0 = now_ms();
  DIR *d = opendir(dir);
  int cnt = 0;
  while (d && readdir(d)) cnt++;
  if (d) closedir(d);
  printf("  list %d entries: %.1f ms\n", cnt, now_ms() - t0);

  /* hard links, if the kernel has them: SYS_linkat = 37 */
  int links = n < 5000 ? n : 5000;
  long hl = heap();
  t0 = now_ms();
  int ok = 1;
  for (int i = 0; i < links && ok; i++) {
    char q[256];
    snprintf(p, sizeof p, "%s/file-%06d", dir, i);
    snprintf(q, sizeof q, "%s/link-%06d", dir, i);
    ok = __syscall6(37, AT_FDCWD, (long)p, AT_FDCWD, (long)q, 0, 0) == 0;
  }
  if (ok) printf("  %d hard links: %.1f us each, %.1f bytes each\n", links, (now_ms() - t0) * 1e3 / links, (double)(heap() - hl) / links);
  else printf("  hard links: n/a (linkat: %s)\n", strerror(errno));
  if (ok) for (int i = 0; i < links; i++) { snprintf(p, sizeof p, "%s/link-%06d", dir, i); unlink(p); }

  int dirs = 2000;
  long hd = heap();
  for (int i = 0; i < dirs; i++) { snprintf(p, sizeof p, "%s/dir-%06d", dir, i); mkdir(p, 0755); }
  printf("  %d directories: %.1f bytes each\n", dirs, (double)(heap() - hd) / dirs);
  for (int i = 0; i < dirs; i++) { snprintf(p, sizeof p, "%s/dir-%06d", dir, i); rmdir(p); }

  t0 = now_ms();
  for (int i = 0; i < n; i++) { snprintf(p, sizeof p, "%s/file-%06d", dir, i); unlink(p); }
  printf("  remove all: %.0f ms\n", now_ms() - t0);
  rmdir(dir);
  printf("fsbench: heap back to %+ld bytes of the start\n", heap() - h0);
  return 0;
}
