// SPDX-License-Identifier: GPL-2.0-or-later
/* env [-i] [-u NAME] [NAME=value...] [command [arg...]] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
extern char **environ;
int main(int argc, char **argv) {
  static char *env[256];
  int n = 0, i = 1, clear = 0;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "-")) clear = 1;
    else if (!strcmp(argv[i], "-u") && i + 1 < argc) { unsetenv(argv[++i]); }
    else if (!strcmp(argv[i], "--")) { i++; break; }
    else { fprintf(stderr, "usage: env [-i] [-u NAME] [NAME=value...] [command [arg...]]\n"); return 125; }
  }
  if (!clear) for (char **e = environ; e && *e && n < 250; e++) env[n++] = *e;
  for (; i < argc && strchr(argv[i], '='); i++) {
    size_t k = strchr(argv[i], '=') - argv[i] + 1;
    int j;
    for (j = 0; j < n; j++) if (!strncmp(env[j], argv[i], k)) break;
    if (j < 250) { env[j] = argv[i]; if (j == n) n++; }
  }
  env[n] = 0;
  if (i >= argc) { for (int j = 0; j < n; j++) puts(env[j]); return 0; }
  environ = env;
  pid_t pid = spawnp(argv[i], argv + i, 0);
  if (pid < 0) { fprintf(stderr, "env: '%s': %s\n", argv[i], strerror(errno)); return errno == ENOENT ? 127 : 126; }
  int st;
  waitpid(pid, &st, 0);
  return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}
