// SPDX-License-Identifier: GPL-2.0-or-later
/* See os.h. Two halves: UC.OS (raw system calls where the libc has no wrapper) and a
 * POSIX host (for the native tests). */
#include "os.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#ifdef __wasm__
#include <sys/syscall.h>
struct sun { unsigned short fam; char path[108]; };
#define SYS_symlinkat_ 36
#define SYS_readlinkat_ 78
#define SYS_rt_sigprocmask_ 135
#define SYS_dup3_ 24

int svc_listen_unix(const char *path, int nonblock) {
  struct sun a = {1, {0}};
  if (strlen(path) >= sizeof a.path) return -ENAMETOOLONG;
  strcpy(a.path, path);
  unlink(path);
  int fd = __syscall3(SYS_socket, 1, 1 | O_CLOEXEC | (nonblock ? O_NONBLOCK : 0), 0);
  if (fd < 0) return fd;
  long r = __syscall3(SYS_bind, fd, &a, sizeof a);
  if (r >= 0) r = __syscall2(SYS_listen, fd, 16);
  if (r < 0) { close(fd); return r; }
  return fd;
}
int svc_connect_unix(const char *path) {
  struct sun a = {1, {0}};
  if (strlen(path) >= sizeof a.path) return -ENAMETOOLONG;
  strcpy(a.path, path);
  int fd = __syscall3(SYS_socket, 1, 1 | O_CLOEXEC, 0);
  if (fd < 0) return fd;
  long r = __syscall3(SYS_connect, fd, &a, sizeof a);
  if (r < 0) { close(fd); return r; }
  return fd;
}
int svc_accept(int lfd, int nonblock) {
  int fd = __syscall1(SYS_accept, lfd);
  if (fd < 0) return fd;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (nonblock) fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}
int svc_symlink(const char *target, const char *path) {
  long r = __syscall3(SYS_symlinkat_, target, AT_FDCWD, path);
  return r < 0 ? (int)r : 0;
}
int svc_readlink_raw(const char *path, char *buf, int len) {
  long r = __syscall4(SYS_readlinkat_, AT_FDCWD, path, buf, len);
  return (int)r;
}
int svc_is_symlink(const char *path) {
  char b[8];
  return svc_readlink_raw(path, b, sizeof b) >= 0;
}
int svc_have_signals(void) {
  unsigned long long old = 0;
  return __syscall4(SYS_rt_sigprocmask_, 0, 0, &old, 8) != -ENOSYS;
}
int64_t svc_now_ms(void) {
  struct timespec ts;
  clock_gettime(1, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t svc_epoch_ms(void) {
  struct timespec ts;
  clock_gettime(0, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int svc_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3],
              const int *extra, int nextra, int pgid) {
  /* The kernel's spawn passes every descriptor that is not close-on-exec, at the same
   * number. The manager keeps all its own descriptors close-on-exec and parks
   * /dev/null on 3, 4, ... (svc_reserve_fds), so it can put a listening socket on 3
   * for the length of the call and put the placeholder back afterwards. */
  for (int i = 0; i < nextra; i++) __syscall3(SYS_dup3_, extra[i], 3 + i, 0);
  int pid = spawn(path, argv, envp, fdmap);
  for (int i = 0; i < nextra; i++) {
    int z = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (z >= 0) { __syscall3(SYS_dup3_, z, 3 + i, O_CLOEXEC); close(z); }
  }
  if (pid > 0 && pgid >= 0) setpgid(pid, pgid ? pgid : pid);
  return pid < 0 ? -errno : pid;
}
int svc_procs(struct svc_pinfo *out, int max) {
  int n = 0;
  struct procinfo pi;
  for (int i = 0; n < max && __syscall2(SYS_procinfo, i, &pi) == 0; i++) {
    out[n].pid = pi.pid; out[n].ppid = pi.ppid; out[n].state = pi.state; out[n].pages = pi.mem_pages;
    long g = __syscall1(SYS_getpgid, pi.pid);
    out[n].pgid = g < 0 ? -1 : (int)g;
    memcpy(out[n].name, pi.name, sizeof out[n].name);
    out[n].name[31] = 0;
    n++;
  }
  return n;
}
#else /* POSIX host */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
int svc_listen_unix(const char *path, int nonblock) {
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof a.sun_path) return -ENAMETOOLONG;
  strcpy(a.sun_path, path);
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0);
  if (fd < 0) return -errno;
  if (bind(fd, (void *)&a, sizeof a) < 0 || listen(fd, 16) < 0) { int e = errno; close(fd); return -e; }
  return fd;
}
int svc_connect_unix(const char *path) {
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof a.sun_path) return -ENAMETOOLONG;
  strcpy(a.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  if (connect(fd, (void *)&a, sizeof a) < 0) { int e = errno; close(fd); return -e; }
  return fd;
}
int svc_accept(int lfd, int nonblock) {
  int fd = accept4(lfd, 0, 0, SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0));
  return fd < 0 ? -errno : fd;
}
int svc_symlink(const char *target, const char *path) { return symlink(target, path) < 0 ? -errno : 0; }
int svc_readlink_raw(const char *path, char *buf, int len) {
  ssize_t r = readlink(path, buf, len);
  return r < 0 ? -errno : (int)r;
}
int svc_is_symlink(const char *path) { struct stat st; return lstat(path, &st) == 0 && S_ISLNK(st.st_mode); }
int svc_have_signals(void) { return 1; }
int64_t svc_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
int64_t svc_epoch_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
int svc_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3],
              const int *extra, int nextra, int pgid) {
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) return -errno;
  pid_t pid = fork();
  if (pid < 0) { int e = errno; close(p[0]); close(p[1]); return -e; }
  if (pid == 0) {
    if (pgid >= 0 && setpgid(0, pgid) < 0) setpgid(0, 0);
    int hi[3 + 8];
    int n = 0;
    for (int i = 0; i < 3; i++) hi[n++] = fdmap && fdmap[i] >= 0 ? fcntl(fdmap[i], F_DUPFD_CLOEXEC, 64) : -1;
    for (int i = 0; i < nextra && i < 8; i++) hi[n++] = fcntl(extra[i], F_DUPFD_CLOEXEC, 64);
    for (int i = 0; i < n; i++) if (hi[i] >= 0) dup2(hi[i], i);
    signal(SIGPIPE, SIG_DFL);
    execve(path, argv, envp);
    int e = errno;
    if (write(p[1], &e, sizeof e) < 0) {}
    _exit(127);
  }
  close(p[1]);
  int e = 0;
  ssize_t r = read(p[0], &e, sizeof e);
  close(p[0]);
  if (pgid >= 0 && setpgid(pid, pgid ? pgid : pid) < 0 && pgid) setpgid(pid, pid);
  if (r == sizeof e) { waitpid(pid, 0, 0); return -e; }
  return pid;
}
int svc_procs(struct svc_pinfo *out, int max) {
  DIR *d = opendir("/proc");
  if (!d) return 0;
  struct dirent *de;
  int n = 0;
  while (n < max && (de = readdir(d))) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    char p[64], buf[512];
    snprintf(p, sizeof p, "/proc/%s/stat", de->d_name);
    FILE *f = fopen(p, "r");
    if (!f) continue;
    size_t l = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[l] = 0;
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp) continue;
    struct svc_pinfo *o = &out[n];
    memset(o, 0, sizeof *o);
    o->pid = atoi(buf);
    size_t nl = rp - lp - 1;
    if (nl > 31) nl = 31;
    memcpy(o->name, lp + 1, nl);
    char st;
    if (sscanf(rp + 2, "%c %d %d", &st, &o->ppid, &o->pgid) != 3) continue;
    o->state = st == 'Z' ? 2 : 1;
    n++;
  }
  closedir(d);
  return n;
}
#endif

void svc_reserve_fds(int n) {
  for (int i = 0; i < n; i++) {
    int z = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (z < 0) return;
    if (z != 3 + i) { dup2(z, 3 + i); close(z); }
    fcntl(3 + i, F_SETFD, FD_CLOEXEC);
  }
}
int svc_set_cloexec(int fd, int on) { return fcntl(fd, F_SETFD, on ? FD_CLOEXEC : 0); }
int svc_set_nonblock(int fd, int on) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return fl;
  return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
