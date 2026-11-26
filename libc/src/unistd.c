// SPDX-License-Identifier: GPL-2.0-or-later
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

ssize_t read(int fd, void *buf, size_t n) { return __sysret(__syscall3(SYS_read, fd, buf, n)); }
ssize_t write(int fd, const void *buf, size_t n) {
  /* the kernel may accept only part of a write (a full pipe); finish it here */
  size_t done = 0;
  while (done < n) {
    long r = __syscall3(SYS_write, fd, (const char *)buf + done, n - done);
    if (r < 0) return done ? (ssize_t)done : __sysret(r);
    if (r == 0) break;
    done += r;
  }
  return done;
}
int open(const char *path, int flags, ...) {
  va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
  return __sysret(__syscall4(SYS_openat, AT_FDCWD, path, flags, mode));
}
int openat(int dirfd, const char *path, int flags, ...) {
  va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
  return __sysret(__syscall4(SYS_openat, dirfd, path, flags, mode));
}
int close(int fd) { return __sysret(__syscall1(SYS_close, fd)); }
off_t lseek(int fd, off_t off, int whence) { return __sysret(__syscall3(SYS_lseek, fd, off, whence)); }
int dup(int fd) { return __sysret(__syscall1(SYS_dup, fd)); }
int dup2(int a, int b) { return a == b ? b : __sysret(__syscall3(SYS_dup3, a, b, 0)); }
int pipe2(int fds[2], int flags) { return __sysret(__syscall2(SYS_pipe2, fds, flags)); }
int pipe(int fds[2]) { return pipe2(fds, 0); }
int fcntl(int fd, int cmd, ...) {
  va_list ap; va_start(ap, cmd); long arg = va_arg(ap, long); va_end(ap);
  return __sysret(__syscall3(SYS_fcntl, fd, cmd, arg));
}
int chdir(const char *p) { return __sysret(__syscall1(SYS_chdir, p)); }
char *getcwd(char *buf, size_t n) { return __sysret(__syscall2(SYS_getcwd, buf, n)) < 0 ? 0 : buf; }
int unlink(const char *p) { return __sysret(__syscall3(SYS_unlinkat, AT_FDCWD, p, 0)); }
int rmdir(const char *p) { return __sysret(__syscall3(SYS_unlinkat, AT_FDCWD, p, AT_REMOVEDIR)); }
int remove(const char *p) { return unlink(p) == 0 ? 0 : rmdir(p); }
int mkdir(const char *p, mode_t m) { return __sysret(__syscall3(SYS_mkdirat, AT_FDCWD, p, m)); }
int stat(const char *p, struct stat *st) { return __sysret(__syscall3(SYS_newfstatat, AT_FDCWD, p, st)); }
int lstat(const char *p, struct stat *st) { return __sysret(__syscall4(SYS_newfstatat, AT_FDCWD, p, st, AT_SYMLINK_NOFOLLOW)); }
int fstat(int fd, struct stat *st) { return __sysret(__syscall2(SYS_fstat, fd, st)); }
int access(const char *p, int mode) { return __sysret(__syscall3(SYS_faccessat, AT_FDCWD, p, mode)); }
int ftruncate(int fd, off_t n) { return __sysret(__syscall2(SYS_ftruncate, fd, n)); }
int isatty(int fd) { struct stat st; return fstat(fd, &st) == 0 && S_ISCHR(st.st_mode); }
pid_t getpid(void) { return __syscall0(SYS_getpid); }
pid_t getppid(void) { return __syscall0(SYS_getppid); }
uid_t getuid(void) { return __syscall0(SYS_getuid); }
pid_t waitpid(pid_t pid, int *status, int options) { return __sysret(__syscall4(SYS_wait4, pid, status, options, 0)); }
pid_t wait(int *status) { return waitpid(-1, status, 0); }
int nanosleep(const struct timespec *req, struct timespec *rem) { (void)rem; return __sysret(__syscall2(SYS_nanosleep, req, 0)); }
int usleep(unsigned us) { struct timespec ts = {us / 1000000, (us % 1000000) * 1000}; return nanosleep(&ts, 0); }
unsigned sleep(unsigned s) { struct timespec ts = {s, 0}; nanosleep(&ts, 0); return 0; }
int clock_gettime(int clk, struct timespec *ts) { return __sysret(__syscall2(SYS_clock_gettime, clk, ts)); }
time_t time(time_t *t) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); if (t) *t = ts.tv_sec; return ts.tv_sec; }
clock_t clock(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (clock_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000); }
int poll(struct pollfd *fds, unsigned long n, int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  return __sysret(__syscall3(SYS_ppoll, fds, n, ms < 0 ? 0 : (long)&ts));
}
pid_t __elf_spawn(const char *, char *const[], char *const[], const int[3]); /* libc/src/elfspawn.c (prototype) */
pid_t spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3]) {
  long r = __syscall4(SYS_spawn, path, argv, envp ? envp : environ, fdmap);
  if (r == -ENOEXEC) r = __elf_spawn(path, argv, envp ? envp : environ, fdmap); /* a Linux program: the binary engine */
  return __sysret(r);
}
pid_t spawnp(const char *file, char *const argv[], const int fdmap[3]) {
  if (strchr(file, '/')) return spawn(file, argv, environ, fdmap);
  const char *path = getenv("PATH");
  if (!path) path = "/bin";
  char buf[256];
  while (*path) {
    const char *e = strchr(path, ':');
    size_t n = e ? (size_t)(e - path) : strlen(path);
    if (n + strlen(file) + 2 < sizeof buf) {
      memcpy(buf, path, n); buf[n] = '/'; strcpy(buf + n + 1, file);
      pid_t p = spawn(buf, argv, environ, fdmap);
      if (p >= 0 || errno != ENOENT) return p;
    }
    path += n + (e ? 1 : 0);
  }
  errno = ENOENT;
  return -1;
}

struct __dir { int fd; long len, pos; struct dirent ent; char buf[2048]; };
DIR *opendir(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return 0;
  DIR *d = calloc(1, sizeof *d);
  d->fd = fd;
  return d;
}
struct dirent *readdir(DIR *d) {
  if (d->pos >= d->len) {
    d->len = __syscall3(SYS_getdents64, d->fd, d->buf, sizeof d->buf);
    d->pos = 0;
    if (d->len <= 0) return 0;
  }
  struct dirent64 *e = (struct dirent64 *)(d->buf + d->pos);
  d->pos += e->d_reclen;
  d->ent.d_ino = e->d_ino; d->ent.d_type = e->d_type;
  strlcpy(d->ent.d_name, e->d_name, sizeof d->ent.d_name);
  return &d->ent;
}
int closedir(DIR *d) { close(d->fd); free(d); return 0; }
