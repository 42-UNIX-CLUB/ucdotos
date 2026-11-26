// SPDX-License-Identifier: GPL-2.0-or-later
/* timeit CMD ARGS...: run a command and print how long it took (used by the in-OS
 * self-hosting test; the shell has no `time`). */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc, char **argv) {
  if (argc < 2) { fputs("usage: timeit cmd args...\n", stderr); return 2; }
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  int pid = spawnp(argv[1], argv + 1, 0), st = 0;
  if (pid < 0) { perror(argv[1]); return 127; }
  waitpid(pid, &st, 0);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
  fprintf(stderr, "timeit: %s: %.0f ms, exit %d\n", argv[1], ms, WEXITSTATUS(st));
  return WEXITSTATUS(st);
}
