// SPDX-License-Identifier: GPL-2.0-or-later
// jtime: run a command and print its wall time in milliseconds to stderr.
//   jtime COMMAND [ARGS...]
// For the binary translator's measurements (docs/BINARY-TRANSLATION.md); UC.OS has no time(1).
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: jtime COMMAND [ARGS...]\n"); return 2; }
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  int fdmap[3] = { -1, -1, -1 };
  int pid = spawnp(argv[1], argv + 1, fdmap);
  if (pid < 0) { perror(argv[1]); return 127; }
  int st = 0;
  waitpid(pid, &st, 0);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
  fprintf(stderr, "jtime %.1f ms exit %d\n", ms, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (st & 0x7f));
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
