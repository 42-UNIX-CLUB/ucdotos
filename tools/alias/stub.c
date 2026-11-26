// SPDX-License-Identifier: GPL-2.0-or-later
/* A program installed under another name (bin/NAME/ALIASES): it starts
 * TARGET (set by the Makefile, e.g. "/bin/gzip") with the same arguments,
 * argv[0] included, so the target picks its behaviour by name, and exits
 * with its status. Standard input, output and error are passed on.
 * It exists because the kernel has no hard or symbolic links yet; with links,
 * the alias names can become links to the program and this file can go. */
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv, char **envp) {
  (void)argc;
  static const int fds[3] = {0, 1, 2};
  pid_t pid = spawn(TARGET, argv, envp, fds);
  if (pid < 0) { static const char m[] = "alias: cannot start " TARGET "\n"; write(2, m, sizeof m - 1); return 127; }
  int st;
  if (waitpid(pid, &st, 0) < 0) return 127;
  return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}
