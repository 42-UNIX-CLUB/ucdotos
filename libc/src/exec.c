// SPDX-License-Identifier: GPL-2.0-or-later
/* execve and its family, and posix_spawn. execve replaces the program of this process
 * (same pid); on success it does not return. posix_spawn is built on spawn(): the file
 * actions are applied to this process's own descriptors around the spawn and undone
 * after, which is safe because the new process cannot run before we next wait. */
#include <unistd.h>
#include <spawn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <alloca.h>

int execve(const char *path, char *const argv[], char *const envp[]) {
  return __sysret(__syscall3(SYS_execve, path, argv, envp));
}
int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }

/* a file that is not a program is run as a shell script, as POSIX asks of execvp */
static int exec_script(const char *path, char *const argv[], char *const envp[]) {
  int n = 0;
  while (argv[n]) n++;
  char **av = malloc((n + 3) * sizeof *av);
  if (!av) { errno = ENOMEM; return -1; }
  av[0] = "sh"; av[1] = (char *)path;
  for (int i = 1; i <= n; i++) av[i + 1] = argv[i];
  execve("/bin/sh", av, envp);
  free(av);
  return -1;
}
int execvpe(const char *file, char *const argv[], char *const envp[]) {
  if (!*file) { errno = ENOENT; return -1; }
  if (strchr(file, '/')) {
    execve(file, argv, envp);
    if (errno == ENOEXEC) exec_script(file, argv, envp);
    return -1;
  }
  const char *path = getenv("PATH");
  if (!path) path = "/bin";
  char buf[256];
  int seen_eacces = 0;
  for (;;) {
    const char *e = strchr(path, ':');
    size_t n = e ? (size_t)(e - path) : strlen(path);
    if (n + strlen(file) + 2 <= sizeof buf) {
      if (n) { memcpy(buf, path, n); buf[n] = '/'; strcpy(buf + n + 1, file); }
      else strcpy(buf, file);
      execve(buf, argv, envp);
      if (errno == ENOEXEC) { exec_script(buf, argv, envp); return -1; }
      if (errno == EACCES) seen_eacces = 1;
      else if (errno != ENOENT && errno != ENOTDIR) return -1;
    }
    if (!e) break;
    path = e + 1;
  }
  errno = seen_eacces ? EACCES : ENOENT;
  return -1;
}
int execvp(const char *file, char *const argv[]) { return execvpe(file, argv, environ); }

/* execl, execlp, execle: the arguments end with a null pointer (execle: then envp) */
#define VEC_FROM_VA(first, ap, vec, n) do { \
    va_list c; va_copy(c, ap); n = 1; while (va_arg(c, char *)) n++; va_end(c); \
    vec = alloca((n + 1) * sizeof(char *)); vec[0] = (char *)first; \
    for (int i = 1; i <= n; i++) vec[i] = va_arg(ap, char *); } while (0)
int execl(const char *path, const char *arg, ...) {
  va_list ap; char **v; int n;
  va_start(ap, arg); VEC_FROM_VA(arg, ap, v, n); va_end(ap);
  return execve(path, v, environ);
}
int execlp(const char *file, const char *arg, ...) {
  va_list ap; char **v; int n;
  va_start(ap, arg); VEC_FROM_VA(arg, ap, v, n); va_end(ap);
  return execvp(file, v);
}
int execle(const char *path, const char *arg, ...) {
  va_list ap; char **v; int n;
  va_start(ap, arg); VEC_FROM_VA(arg, ap, v, n);
  char **envp = va_arg(ap, char **);
  va_end(ap);
  return execve(path, v, envp);
}

/* ---- posix_spawn ---- */
struct __psfa { int op, fd, newfd, oflag; mode_t mode; char *path; };
enum { FA_OPEN, FA_CLOSE, FA_DUP2 };
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) { fa->n = 0; fa->acts = 0; return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
  for (int i = 0; i < fa->n; i++) free(fa->acts[i].path);
  free(fa->acts);
  fa->n = 0; fa->acts = 0;
  return 0;
}
static int fa_add(posix_spawn_file_actions_t *fa, struct __psfa a) {
  if (a.fd < 0 || a.fd >= 64 || (a.op == FA_DUP2 && (a.newfd < 0 || a.newfd >= 64))) return EBADF;
  struct __psfa *n = realloc(fa->acts, (fa->n + 1) * sizeof *n);
  if (!n) return ENOMEM;
  fa->acts = n;
  fa->acts[fa->n++] = a;
  return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode) {
  char *p = strdup(path);
  if (!p) return ENOMEM;
  int e = fa_add(fa, (struct __psfa){FA_OPEN, fd, 0, oflag, mode, p});
  if (e) free(p);
  return e;
}
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
  return fa_add(fa, (struct __psfa){FA_CLOSE, fd, 0, 0, 0, 0});
}
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd) {
  return fa_add(fa, (struct __psfa){FA_DUP2, fd, newfd, 0, 0, 0});
}
int posix_spawnattr_init(posix_spawnattr_t *a) { memset(a, 0, sizeof *a); return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t *a) { (void)a; return 0; }
int posix_spawnattr_setflags(posix_spawnattr_t *a, short f) {
  if (f & ~(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_RESETIDS)) return EINVAL;
  a->flags = f; return 0;
}
int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *f) { *f = a->flags; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t g) { a->pgroup = g; return 0; }
int posix_spawnattr_getpgroup(const posix_spawnattr_t *a, pid_t *g) { *g = a->pgroup; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *s) { a->mask = *s; return 0; }
int posix_spawnattr_getsigmask(const posix_spawnattr_t *a, sigset_t *s) { *s = a->mask; return 0; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *s) { a->def = *s; return 0; }
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *a, sigset_t *s) { *s = a->def; return 0; }

#define MAXSAVE 32
struct saved { int n, fd[MAXSAVE], save[MAXSAVE]; };
static int remember(struct saved *sv, int fd) {
  for (int i = 0; i < sv->n; i++) if (sv->fd[i] == fd) return 0;
  if (sv->n >= MAXSAVE) return ENOMEM;
  int s = fcntl(fd, F_DUPFD, 10);
  if (s >= 0) fcntl(s, F_SETFD, FD_CLOEXEC);
  sv->fd[sv->n] = fd; sv->save[sv->n++] = s; /* -1: it was not open */
  return 0;
}
static void undo(struct saved *sv) {
  for (int i = sv->n - 1; i >= 0; i--) {
    if (sv->save[i] >= 0) { dup2(sv->save[i], sv->fd[i]); close(sv->save[i]); }
    else close(sv->fd[i]);
  }
}
static int do_spawn(pid_t *pidp, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                    char *const argv[], char *const envp[], int search) {
  struct saved sv = {0};
  sigset_t all = ~0ull, old;
  __syscall4(SYS_rt_sigprocmask, SIG_SETMASK, &all, &old, 8); /* no handler runs while our descriptors are borrowed */
  struct sigaction dfl[NSIG];
  int e = 0, nsig = 0;
  short fl = at ? at->flags : 0;
  for (int i = 0; fa && i < fa->n && !e; i++) {
    struct __psfa *a = &fa->acts[i];
    if (a->op == FA_DUP2) {
      if ((e = remember(&sv, a->newfd))) break;
      if (a->fd == a->newfd) { if (fcntl(a->fd, F_SETFD, 0) < 0) e = errno; }
      else if (dup2(a->fd, a->newfd) < 0) e = errno;
    } else if (a->op == FA_CLOSE) {
      if ((e = remember(&sv, a->fd))) break;
      close(a->fd);
    } else {
      if ((e = remember(&sv, a->fd))) break;
      int t = open(a->path, a->oflag, a->mode);
      if (t < 0) { e = errno; break; }
      if (t != a->fd) { dup2(t, a->fd); close(t); }
    }
  }
  if (!e && (fl & POSIX_SPAWN_SETSIGDEF)) /* spawn already resets handlers; ignored ones must be reset here */
    for (int s = 1; s < NSIG; s++) {
      if (s == SIGKILL || s == SIGSTOP || !((at->def >> (s - 1)) & 1)) { memset(&dfl[s], 0, sizeof dfl[s]); continue; }
      struct sigaction d;
      memset(&d, 0, sizeof d);
      sigaction(s, &d, &dfl[s]);
      nsig = s;
    }
  pid_t pid = -1;
  if (!e) {
    sigset_t m = (fl & POSIX_SPAWN_SETSIGMASK) ? at->mask : old;
    __syscall4(SYS_rt_sigprocmask, SIG_SETMASK, &m, 0, 8); /* the child inherits this mask */
    pid = search ? spawnp(path, argv, NULL) : spawn(path, argv, envp, NULL);
    __syscall4(SYS_rt_sigprocmask, SIG_SETMASK, &all, 0, 8);
    if (pid < 0) e = errno;
    else if (fl & POSIX_SPAWN_SETPGROUP) setpgid(pid, at->pgroup); /* it has not run yet */
  }
  for (int s = 1; s <= nsig; s++) if ((at->def >> (s - 1)) & 1 && s != SIGKILL && s != SIGSTOP) sigaction(s, &dfl[s], 0);
  undo(&sv);
  sigprocmask(SIG_SETMASK, &old, 0);
  if (!e && pidp) *pidp = pid;
  return e;
}
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[]) {
  return do_spawn(pid, path, fa, at, argv, envp ? envp : environ, 0);
}
/* spawnp() searches PATH with environ; set environ around it so envp is what the child gets */
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[]) {
  char **saved = environ;
  if (envp) environ = (char **)envp;
  int e = do_spawn(pid, file, fa, at, argv, environ, 1);
  environ = saved;
  return e;
}
