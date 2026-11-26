// SPDX-License-Identifier: GPL-2.0-or-later
/* progbench: wall time of real programs on fixed workloads, to compare two builds of the
 * same programs (for example with and without the fork stack transform). Each workload is
 * started with spawn and waited for, `runs` times; the first run and the best run are
 * reported, as JSON for tools/bench/ab.mjs with --json.
 *   progbench [--json] [--runs N] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void put_file(const char *path, const char *s) { int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); write(fd, s, strlen(s)); close(fd); }

struct work { const char *name, *in; char *argv[8]; };
static struct work works[] = {
  {"sh-loop", 0, {"/bin/sh", "-c", "i=0; while [ $i -lt 4000 ]; do i=$((i+1)); done", 0}},
  {"grep", 0, {"/bin/grep", "-c", "9.*7", "/tmp/pb.lines", 0}},
  {"sort", 0, {"/bin/sort", "/tmp/pb.sort", 0}},
  {"bc", "/tmp/pb.bc", {"/bin/bc", 0}},
};

int main(int argc, char **argv) {
  int json = 0, runs = 7;
  for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--json")) json = 1; else if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]); }
  /* inputs */
  static char big[4 << 20]; /* 100,000 lines of at most 24 bytes */
  size_t n = 0;
  for (int i = 1; i <= 100000; i++) n += snprintf(big + n, sizeof big - n, "%d line %d\n", i, i * 7);
  int fd = open("/tmp/pb.lines", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(fd, big, n); close(fd);
  n = 0;
  unsigned x = 12345;
  for (int i = 0; i < 40000; i++) { x = x * 1103515245 + 12345; n += snprintf(big + n, sizeof big - n, "%u %x\n", x >> 8, x); }
  fd = open("/tmp/pb.sort", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(fd, big, n); close(fd);
  put_file("/tmp/pb.bc", "2^30000\n");
  int devnull = open("/dev/null", O_WRONLY);
  if (json) printf("{\"results\":[");
  int first = 1;
  for (unsigned w = 0; w < sizeof works / sizeof *works; w++) {
    double best = 1e18, t1 = 0;
    for (int r = 0; r < runs; r++) {
      int in = works[w].in ? open(works[w].in, O_RDONLY) : -1;
      int map[3] = {in, devnull, -1};
      double t0 = now_ms();
      pid_t p = spawn(works[w].argv[0], works[w].argv, 0, map);
      int st = -1;
      if (p > 0) waitpid(p, &st, 0);
      double t = now_ms() - t0;
      if (in >= 0) close(in);
      if (p < 0 || !WIFEXITED(st) || WEXITSTATUS(st) > 1) { fprintf(stderr, "progbench: %s failed (pid %d, status %d, errno %d)\n", works[w].name, p, st, errno); return 1; }
      if (r == 0) t1 = t;
      if (t < best) best = t;
    }
    if (json) printf("%s{\"name\":\"%s first\",\"unit\":\"ms\",\"value\":%.3f},{\"name\":\"%s best\",\"unit\":\"ms\",\"value\":%.3f}", first ? "" : ",", works[w].name, t1, works[w].name, best);
    else printf("%-8s first %8.2f ms   best of %d %8.2f ms\n", works[w].name, t1, runs, best);
    first = 0;
  }
  if (json) printf("]}\n");
  return 0;
}
