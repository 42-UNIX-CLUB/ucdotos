// SPDX-License-Identifier: GPL-2.0-or-later
/* init: pid 1. Starts the sound server (audiod) and the router (routerd, the machine's
 * way out to the network), then the desktop, and keeps the system alive.
 * It tries, in order: the window manager, a full-screen terminal on the bare
 * framebuffer, and finally a shell on the kernel console. If the thing it
 * started exits, it starts it again, a few times, then falls to the next.
 *
 * init runs as root (the kernel starts the first process with uid 0). It starts the
 * session as the desktop user (`user` in /etc/passwd), the way a display manager does
 * on Linux: it makes the user's runtime directory /run/user/UID, then each child sets
 * its groups, group and user before it runs the program (fork, set ids, exec; here
 * fork_call, which needs no stack transform).
 *
 * init PROGRAM ARGS... runs one program as the desktop user, for tests;
 * init --root PROGRAM ARGS... runs it as root. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SESSION_USER "user"

static char *env[] = {
  "PATH=/bin", "HOME=/home/user", "USER=user", "SHELL=/bin/sh", "LOGNAME=user",
  "TERM=xterm-256color", "COLORTERM=truecolor", "LANG=C.UTF-8", "XDG_RUNTIME_DIR=/run/user/1000", 0,
  0, 0, 0, 0, 0, 0, 0, 0, /* room for the values from /etc/passwd */
};
static char *root_env[] = {
  "PATH=/bin:/sbin", "HOME=/root", "USER=root", "SHELL=/bin/sh", "LOGNAME=root",
  "TERM=xterm-256color", "COLORTERM=truecolor", "LANG=C.UTF-8", 0,
};
static struct passwd user; /* the session user; uid 0 means run as root */
static char uname_[64], uhome[256], ushell[256];

static void session_user(void) {
  struct passwd *pw = getpwnam(SESSION_USER);
  if (!pw) { /* no user database: the numbers the image is built with */
    fprintf(stderr, "init: no user '%s' in /etc/passwd; using uid and gid 1000\n", SESSION_USER);
    user.pw_uid = user.pw_gid = 1000;
    strcpy(uname_, SESSION_USER); strcpy(uhome, "/home/user"); strcpy(ushell, "/bin/sh");
  } else {
    user = *pw;
    snprintf(uname_, sizeof uname_, "%s", pw->pw_name);
    snprintf(uhome, sizeof uhome, "%s", pw->pw_dir);
    snprintf(ushell, sizeof ushell, "%s", pw->pw_shell[0] ? pw->pw_shell : "/bin/sh");
  }
  user.pw_name = uname_; user.pw_dir = uhome; user.pw_shell = ushell;
  static char e[5][300];
  snprintf(e[0], sizeof e[0], "HOME=%s", uhome);
  snprintf(e[1], sizeof e[1], "USER=%s", uname_);
  snprintf(e[2], sizeof e[2], "LOGNAME=%s", uname_);
  snprintf(e[3], sizeof e[3], "SHELL=%s", ushell);
  snprintf(e[4], sizeof e[4], "XDG_RUNTIME_DIR=/run/user/%u", user.pw_uid);
  env[1] = e[0]; env[2] = e[1]; env[4] = e[2]; env[3] = e[3]; env[8] = e[4];
  /* the runtime directory: owned by the user, private, as logind makes it */
  char run[64];
  snprintf(run, sizeof run, "/run/user/%u", user.pw_uid);
  mkdir("/run/user", 0755);
  if (mkdir(run, 0700) && errno != EEXIST) fprintf(stderr, "init: cannot make %s: %s\n", run, strerror(errno));
  if (chown(run, user.pw_uid, user.pw_gid)) fprintf(stderr, "init: cannot give %s to %s: %s\n", run, uname_, strerror(errno));
  chmod(run, 0700);
}

struct job { char *const *argv; int as_root; };
/* the child of fork_call: drop to the user, then become the program */
static int child(void *arg) {
  struct job *j = arg;
  if (!j->as_root) {
    if (initgroups(user.pw_name, user.pw_gid) || setresgid(user.pw_gid, user.pw_gid, user.pw_gid) ||
        setresuid(user.pw_uid, user.pw_uid, user.pw_uid)) {
      fprintf(stderr, "init: cannot switch to user %s: %s\n", user.pw_name, strerror(errno));
      return 126;
    }
    if (chdir(user.pw_dir)) chdir("/");
  }
  execve(j->argv[0], j->argv, j->as_root ? root_env : env);
  fprintf(stderr, "init: cannot run %s: %s\n", j->argv[0], strerror(errno));
  return 127;
}
/* start a program in the user's session and do not wait for it (run() reaps it if it ends) */
static void start(char *const argv[]) {
  struct stat st;
  if (stat(argv[0], &st) < 0) return;
  static struct job j;
  j.argv = argv; j.as_root = 0;
  if (fork_call(child, &j) < 0) fprintf(stderr, "init: cannot start %s: %s\n", argv[0], strerror(errno));
}
static int run(char *const argv[], int as_root) {
  struct stat st;
  if (stat(argv[0], &st) < 0) return -1;
  struct job j = {argv, as_root};
  pid_t pid = fork_call(child, &j);
  if (pid < 0) { fprintf(stderr, "init: cannot start %s: %s\n", argv[0], strerror(errno)); return -1; }
  int status = 0;
  for (;;) { /* also reaps orphans handed to pid 1 */
    pid_t w = waitpid(-1, &status, 0);
    if (w == pid) return status;
    if (w < 0) return -1;
  }
}
int main(int argc, char **argv) {
  chdir("/");
  session_user();
  if (argc > 1) { /* init [--root] PROGRAM ARGS...: run one program, for tests */
    int as_root = !strcmp(argv[1], "--root");
    if (as_root && argc < 3) return 127;
    int st = run(argv + 1 + as_root, as_root);
    return st < 0 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  }
  /* The sound server (audiod) runs beside everything else, in the desktop user's session as
   * on Linux (PipeWire and PulseAudio are per-user services there): it opens /dev/dsp
   * through the user's audio group and listens in $XDG_RUNTIME_DIR. Idle, it sleeps in
   * poll() and holds no sound stream. init does not wait for it. */
  char *sound[] = {"/bin/audiod", 0};
  start(sound);
  /* The router runs beside everything else; init does not wait for it. If it is not in
   * the image, or exits, the machine simply has no way out beyond itself. It is a system
   * service: started here as root, before any stage drops to the desktop user. */
  char *router[] = {"/bin/routerd", 0};
  struct stat rst;
  if (stat(router[0], &rst) == 0) spawn(router[0], router, root_env, 0);
  char *wm[] = {"/bin/wm", 0};
  char *term[] = {"/bin/term", "--fb", 0};
  char *sh[] = {"/bin/sh", 0};
  char **stages[] = {wm, term, sh};
  for (int i = 0; i < 3; i++) {
    for (int tries = 0; tries < 3; tries++) {
      int st = run(stages[i], 0);
      if (st < 0) break; /* not installed */
      if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0; /* a clean exit ends the session */
      fprintf(stderr, "init: %s ended (status %d), restarting\n", stages[i][0], st);
    }
  }
  fprintf(stderr, "init: nothing left to run\n");
  return 1;
}
