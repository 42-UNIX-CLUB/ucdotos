// SPDX-License-Identifier: GPL-2.0-or-later
/* sudo [-u USER] [-i | -s] [COMMAND [ARG...]]  /  sudo -l  /  sudo -v
 * Run COMMAND as root (or USER). As su (see bin/su.c for the rule): it works without a
 * password when this session is entitled to administration, and refuses otherwise.
 * -s runs the shell, -i a login shell; -l says whether this session may use sudo; -v
 * succeeds or fails the same way without running anything. */
#include "suid.h"

int main(int argc, char **argv) {
  const char *who = "root";
  int shell = 0, login = 0, list = 0, check = 0, i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strcmp(argv[i], "-u") && i + 1 < argc) who = argv[++i];
    else if (!strcmp(argv[i], "-s")) shell = 1;
    else if (!strcmp(argv[i], "-i")) login = shell = 1;
    else if (!strcmp(argv[i], "-l")) list = 1;
    else if (!strcmp(argv[i], "-v")) check = 1;
    else if (!strcmp(argv[i], "-n")) ; /* never asks anyway */
    else { fprintf(stderr, "usage: sudo [-u user] [-i | -s] [command [arg...]]\n"); return 1; }
  }
  int able = geteuid() == 0;
  if (list) {
    if (able) printf("This session may run any command as any user.\n");
    else refuse("sudo");
    return able ? 0 : 1;
  }
  if (!able) { refuse("sudo"); return 1; }
  if (check) return 0;
  if (i >= argc && !shell) { fprintf(stderr, "usage: sudo [-u user] [-i | -s] [command [arg...]]\n"); return 1; }
  struct passwd *pw = getpwnam(who);
  if (!pw) { fprintf(stderr, "sudo: unknown user %s\n", who); return 1; }
  struct passwd t = *pw;
  char name[64], home[256], sh[256];
  snprintf(name, sizeof name, "%s", pw->pw_name); snprintf(home, sizeof home, "%s", pw->pw_dir);
  snprintf(sh, sizeof sh, "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
  t.pw_name = name; t.pw_dir = home; t.pw_shell = sh;
  char caller[32];
  snprintf(caller, sizeof caller, "%u", getuid());
  if (become(&t, "sudo")) return 1;
  if (login) {
    clearenv_keep("TERM", "COLORTERM", "THEME", "WM_SOCKET", "XDG_RUNTIME_DIR", "LANG");
    if (chdir(home)) fprintf(stderr, "sudo: warning: cannot change directory to %s\n", home);
    setenv("PATH", t.pw_uid ? "/bin" : "/bin:/sbin", 1);
  }
  setenv("HOME", home, 1); setenv("SHELL", sh, 1); setenv("USER", name, 1); setenv("LOGNAME", name, 1);
  setenv("SUDO_UID", caller, 1);
  if (shell) {
    char *av[] = {login ? "-sh" : "sh", NULL, NULL, NULL};
    if (i < argc) { /* sudo -s CMD: the shell runs CMD */
      av[1] = "-c";
      size_t n = 1;
      for (int k = i; k < argc; k++) n += strlen(argv[k]) + 1;
      char *line = malloc(n);
      line[0] = 0;
      for (int k = i; k < argc; k++) { if (k > i) strcat(line, " "); strcat(line, argv[k]); }
      av[2] = line;
    }
    execv(sh, av);
    fprintf(stderr, "sudo: cannot run %s: %s\n", sh, strerror(errno));
    return 126;
  }
  execvp(argv[i], argv + i);
  fprintf(stderr, "sudo: %s: %s\n", argv[i], errno == ENOENT ? "command not found" : strerror(errno));
  return errno == ENOENT ? 127 : 126;
}
