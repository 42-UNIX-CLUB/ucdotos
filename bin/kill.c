// SPDX-License-Identifier: GPL-2.0-or-later
/* kill [-s SIG | -SIG] pid...  and  kill -l. This system delivers no signals:
 * kill terminates the target, and its parent sees it as killed by SIG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/syscall.h>
static const char *names[] = {0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM"};
static int signum(const char *s) {
  if (isdigit((unsigned char)*s)) return atoi(s);
  if (!strncmp(s, "SIG", 3)) s += 3;
  for (int i = 1; i < 16; i++) if (!strcasecmp(s, names[i])) return i;
  return -1;
}
int main(int argc, char **argv) {
  int sig = 15, i = 1, rc = 0;
  if (i < argc && !strcmp(argv[i], "-l")) {
    for (int k = 1; k < 16; k++) printf("%2d) SIG%-6s%s", k, names[k], k % 5 == 0 ? "\n" : " ");
    return 0;
  }
  if (i < argc && !strcmp(argv[i], "-s") && i + 1 < argc) { sig = signum(argv[i + 1]); i += 2; }
  else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) { sig = signum(argv[i] + 1); i++; }
  if (i < argc && !strcmp(argv[i], "--")) i++;
  if (sig < 0) { fprintf(stderr, "kill: invalid signal\n"); return 1; }
  if (i >= argc) { fprintf(stderr, "usage: kill [-s SIG | -SIG] pid...\n       kill -l\n"); return 1; }
  for (; i < argc; i++) {
    char *e;
    long pid = strtol(argv[i], &e, 10);
    if (*e || e == argv[i]) { fprintf(stderr, "kill: %s: arguments must be process ids\n", argv[i]); rc = 1; continue; }
    if (__sysret(__syscall2(SYS_kill, pid, sig)) < 0) { fprintf(stderr, "kill: (%ld) - %s\n", pid, strerror(errno)); rc = 1; }
  }
  return rc;
}
