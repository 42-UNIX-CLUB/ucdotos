// SPDX-License-Identifier: GPL-2.0-or-later
/* su [-] [-l] [-c COMMAND] [USER]: run a shell (or COMMAND) as USER, root by default.
 *
 * UC.OS has no passwords. Whether this session may become root is decided when the
 * system boots: the page tells the kernel (boot parameter ucos.admin=1) whether the
 * person using it is entitled to administration. su is installed set-user-ID root; the
 * kernel honours that bit only in an entitled session. So: entitled, su runs as root and
 * switches without asking; not entitled, it runs as the caller and refuses. Root itself
 * may always switch to anyone, and anyone may "switch" to themselves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include "suid.h"

int main(int argc, char **argv) {
  int login = 0, i = 1;
  const char *cmd = NULL;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "-") || !strcmp(argv[i], "-l") || !strcmp(argv[i], "--login")) login = 1;
    else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmd = argv[++i];
    else if (argv[i][0] == '-') { fprintf(stderr, "usage: su [-] [-l] [-c command] [user]\n"); return 1; }
    else break;
  }
  const char *who = i < argc ? argv[i] : "root";
  struct passwd *pw = getpwnam(who);
  if (!pw) { fprintf(stderr, "su: user %s does not exist\n", who); return 1; }
  struct passwd target = *pw; /* the next getpw* call reuses the static entry */
  char name[64], home[256], shell[256];
  snprintf(name, sizeof name, "%s", pw->pw_name); snprintf(home, sizeof home, "%s", pw->pw_dir);
  snprintf(shell, sizeof shell, "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
  target.pw_name = name; target.pw_dir = home; target.pw_shell = shell;
  if (target.pw_uid != getuid() && geteuid() != 0) { refuse("su"); return 1; }
  if (become(&target, "su")) return 1;
  if (login) {
    clearenv_keep("TERM", "COLORTERM", "THEME", "WM_SOCKET", "XDG_RUNTIME_DIR", "LANG");
    if (chdir(home)) fprintf(stderr, "su: warning: cannot change directory to %s\n", home);
  }
  setenv("HOME", home, 1);
  setenv("SHELL", shell, 1);
  if (target.pw_uid != 0 || login) { setenv("USER", name, 1); setenv("LOGNAME", name, 1); }
  if (login || !getenv("PATH")) setenv("PATH", target.pw_uid ? "/bin" : "/bin:/sbin", 1);
  char *av[4];
  int ac = 0;
  av[ac++] = login ? "-sh" : "sh";
  if (cmd) { av[ac++] = "-c"; av[ac++] = (char *)cmd; }
  av[ac] = NULL;
  execv(shell, av);
  fprintf(stderr, "su: cannot run %s: %s\n", shell, strerror(errno));
  return 126;
}
