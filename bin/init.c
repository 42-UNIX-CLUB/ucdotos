// SPDX-License-Identifier: GPL-2.0-or-later
/* init: pid 1. Starts the desktop and keeps the system alive.
 * It tries, in order: the window manager, a full-screen terminal on the bare
 * framebuffer, and finally a shell on the kernel console. If the thing it
 * started exits, it starts it again, a few times, then falls to the next. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char *env[] = {
  "PATH=/bin", "HOME=/home/user", "USER=user", "SHELL=/bin/sh", "LOGNAME=user",
  "TERM=xterm-256color", "COLORTERM=truecolor", "LANG=C.UTF-8", 0,
};
static int run(char *const argv[]) {
  struct stat st;
  if (stat(argv[0], &st) < 0) return -1;
  pid_t pid = spawn(argv[0], argv, env, 0);
  if (pid < 0) return -1;
  int status = 0;
  for (;;) { /* also reaps orphans handed to pid 1 */
    pid_t w = waitpid(-1, &status, 0);
    if (w == pid) return status;
    if (w < 0) return -1;
  }
}
int main(int argc, char **argv) {
  chdir("/home/user");
  if (argc > 1) { /* init PROGRAM ARGS...: run one program, for tests */
    int st = run(argv + 1);
    return st < 0 ? 127 : WEXITSTATUS(st);
  }
  char *wm[] = {"/bin/wm", 0};
  char *term[] = {"/bin/term", "--fb", 0};
  char *sh[] = {"/bin/sh", 0};
  char **stages[] = {wm, term, sh};
  for (int i = 0; i < 3; i++) {
    for (int tries = 0; tries < 3; tries++) {
      int st = run(stages[i]);
      if (st < 0) break; /* not installed */
      if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0; /* a clean exit ends the session */
      fprintf(stderr, "init: %s ended (status %d), restarting\n", stages[i][0], st);
    }
  }
  fprintf(stderr, "init: nothing left to run\n");
  return 1;
}
