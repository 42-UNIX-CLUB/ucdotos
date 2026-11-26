// SPDX-License-Identifier: GPL-2.0-or-later
/* make: the system layer. Everything make asks of the operating system goes
 * through here: starting and waiting for processes, time stamps, directories.
 * Two implementations: UC.OS (spawn, no fork, no signals) and POSIX (fork,
 * exec, waitpid), so the same make logic is tested natively. */
#ifndef __wasm__
#define _GNU_SOURCE
#endif
#include "make.h"
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

int sys_close(int fd) { return close(fd); }
int sys_read(int fd, char *b, int n) {
  for (;;) {
    int r = (int)read(fd, b, n);
    if (r < 0 && errno == EINTR) continue;
    return r;
  }
}
int sys_chdir(const char *d) { return chdir(d); }
char *sys_getcwd(void) {
  char buf[4096];
  if (!getcwd(buf, sizeof buf)) return xstrdup(".");
  return xstrdup(buf);
}
int sys_unlink(const char *path) { return unlink(path); }
int sys_isdir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
void sys_list_dir(const char *dir, strlist *out) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    sl_add(out, xstrdup(e->d_name));
  }
  closedir(d);
}
int sys_wait_status(int status, int *exited, int *code, int *sig) {
  *exited = WIFEXITED(status);
  *code = *exited ? WEXITSTATUS(status) : 0;
  *sig = *exited ? 0 : WTERMSIG(status);
  return 0;
}
int sys_wait_any(int *status, int block) {
  for (;;) {
    int r = waitpid(-1, status, block ? 0 : WNOHANG);
    if (r < 0 && errno == EINTR) continue;
    return r < 0 ? -1 : r;
  }
}
int sys_waitpid(int pid, int *status) {
  for (;;) {
    int r = waitpid(pid, status, 0);
    if (r < 0 && errno == EINTR) continue;
    return r;
  }
}
int sys_kill(int pid, int sig) { return kill(pid, sig); }

/* find NAME in the colon-separated PATH; NULL if absent */
static char *path_find(const char *name, const char *path) {
  if (strchr(name, '/')) return xstrdup(name);
  if (!path) path = "/bin:/usr/bin";
  const char *p = path;
  for (;;) {
    const char *e = strchr(p, ':');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    char *cand;
    if (n == 0) cand = xstrdup(name);
    else { char *dir = xstrndup(p, n); cand = xconcat(dir, "/", name); free(dir); }
    struct stat st;
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)
#ifndef __wasm__
        && access(cand, X_OK) == 0
#endif
    ) return cand;
    free(cand);
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

#ifdef __wasm__
/* ---------------- UC.OS ---------------- */
#include <sys/syscall.h>

int sys_stat(const char *path, sysstat *s) {
  struct stat st;
  memset(s, 0, sizeof *s);
  if (stat(path, &st) != 0) return 0;
  s->exists = 1;
  s->is_dir = S_ISDIR(st.st_mode);
  /* the kernel keeps whole seconds only */
  s->mtime = (mtime_t)st.st_mtime * 1000000000LL;
  return 0;
}
int sys_pipe(int fds[2]) { return pipe2(fds, O_CLOEXEC); }
int sys_spawn(char **argv, char **envp, int fd_out) {
  int map[3] = {-1, fd_out, -1};
  char *p = path_find(argv[0], getenv("PATH"));
  if (!p) return -ENOENT;
  int pid = spawn(p, argv, envp, fd_out >= 0 ? map : 0);
  free(p);
  return pid < 0 ? -errno : pid;
}
int sys_spawn_path(char **argv, char **envp, const char *path) {
  char *p = path_find(argv[0], path);
  if (!p) return -ENOENT;
  int pid = spawn(p, argv, envp, 0);
  free(p);
  return pid < 0 ? -errno : pid;
}
int sys_touch(const char *path) {
  if (__syscall4(SYS_utimensat, AT_FDCWD, path, 0, 0) == 0) return 0;
  int fd = open(path, O_WRONLY | O_CREAT, 0666);
  if (fd < 0) return -1;
  close(fd);
  return 0;
}
void sys_install_signals(void) {}
int sys_interrupted(void) { return 0; }
void sys_reexec(char **argv) {
  fflush(stdout);
  fflush(stderr);
  char *p = path_find(argv[0], getenv("PATH"));
  int pid = p ? spawn(p, argv, environ, 0) : -1;
  if (pid < 0) { fprintf(stderr, "%s: %s: %s\n", progname, argv[0], strerror(errno)); exit(2); }
  int st;
  sys_waitpid(pid, &st);
  int ex, code, sig;
  sys_wait_status(st, &ex, &code, &sig);
  exit(ex ? code : 2);
}
#else
/* ---------------- POSIX (native testing) ---------------- */
int sys_stat(const char *path, sysstat *s) {
  struct stat st;
  memset(s, 0, sizeof *s);
  if (stat(path, &st) != 0) return 0;
  s->exists = 1;
  s->is_dir = S_ISDIR(st.st_mode);
  s->mtime = (mtime_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
  return 0;
}
int sys_pipe(int fds[2]) { return pipe2(fds, O_CLOEXEC); }

/* fork and exec; the exec error comes back through a close-on-exec pipe */
static int fork_exec(const char *file, char **argv, char **envp, int fd_out) {
  int ep[2];
  if (pipe2(ep, O_CLOEXEC) < 0) return -errno;
  pid_t pid = fork();
  if (pid < 0) { int e = errno; close(ep[0]); close(ep[1]); return -e; }
  if (pid == 0) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    if (fd_out >= 0) dup2(fd_out, 1);
    execve(file, argv, envp);
    int e = errno;
    if (write(ep[1], &e, sizeof e) < 0) {}
    _exit(127);
  }
  close(ep[1]);
  int e = 0;
  ssize_t r;
  do r = read(ep[0], &e, sizeof e); while (r < 0 && errno == EINTR);
  close(ep[0]);
  if (r == (ssize_t)sizeof e) {
    int st;
    waitpid(pid, &st, 0);
    return -e;
  }
  return pid;
}
static const char *env_path(char **envp) {
  for (char **e = envp; e && *e; e++) if (!strncmp(*e, "PATH=", 5)) return *e + 5;
  return getenv("PATH");
}
int sys_spawn(char **argv, char **envp, int fd_out) {
  char *p = path_find(argv[0], env_path(envp));
  if (!p) return -ENOENT;
  int r = fork_exec(p, argv, envp, fd_out);
  free(p);
  return r;
}
int sys_spawn_path(char **argv, char **envp, const char *path) {
  char *p = path_find(argv[0], path ? path : env_path(envp));
  if (!p) return -ENOENT;
  int r = fork_exec(p, argv, envp, -1);
  free(p);
  return r;
}
int sys_touch(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_NOCTTY, 0666);
  if (fd < 0) return -1;
  close(fd);
  return utimensat(AT_FDCWD, path, 0, 0);
}
static volatile sig_atomic_t got_sig;
static void on_signal(int sig) {
  got_sig = sig;
  handle_interrupt(sig);
  signal(sig, SIG_DFL);
  raise(sig);
}
void sys_install_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, 0);
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGHUP, &sa, 0);
}
int sys_interrupted(void) { return got_sig != 0; }
void sys_reexec(char **argv) {
  fflush(stdout);
  fflush(stderr);
  execvp(argv[0], argv);
  fprintf(stderr, "%s: %s: %s\n", progname, argv[0], strerror(errno));
  exit(2);
}
#endif
