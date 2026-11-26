// SPDX-License-Identifier: GPL-2.0-or-later
/* POSIX file system calls: links, modes, owners, times, locks, statx, statfs and
 * statvfs, vectored I/O, credentials, and the *at family. Thin wrappers; the semantics
 * live in the kernel (kernel/fs_sys.c, docs/VFS-NOTES.md). */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/time.h>

#define S(nr, ...) __sysret(__syscall6(nr, __VA_ARGS__))
#define A(x) ((long)(x))

/* ---- links ---- */
int symlink(const char *target, const char *path) { return S(SYS_symlinkat, A(target), AT_FDCWD, A(path), 0, 0, 0); }
int symlinkat(const char *target, int dirfd, const char *path) { return S(SYS_symlinkat, A(target), dirfd, A(path), 0, 0, 0); }
ssize_t readlink(const char *path, char *buf, size_t n) { return S(SYS_readlinkat, AT_FDCWD, A(path), A(buf), (long)n, 0, 0); }
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t n) { return S(SYS_readlinkat, dirfd, A(path), A(buf), (long)n, 0, 0); }
int link(const char *a, const char *b) { return S(SYS_linkat, AT_FDCWD, A(a), AT_FDCWD, A(b), 0, 0); }
int linkat(int fa, const char *a, int fb, const char *b, int flags) { return S(SYS_linkat, fa, A(a), fb, A(b), flags, 0); }
int unlinkat(int dirfd, const char *path, int flags) { return S(SYS_unlinkat, dirfd, A(path), flags, 0, 0, 0); }
int renameat(int fa, const char *a, int fb, const char *b) { return S(SYS_renameat, fa, A(a), fb, A(b), 0, 0); }
int renameat2(int fa, const char *a, int fb, const char *b, unsigned flags) { return S(SYS_renameat2, fa, A(a), fb, A(b), (long)flags, 0); }

/* ---- directories and paths ---- */
int fchdir(int fd) { return S(SYS_fchdir, fd, 0, 0, 0, 0, 0); }
int chroot(const char *path) { return S(SYS_chroot, A(path), 0, 0, 0, 0, 0); }
int mkdirat(int dirfd, const char *path, mode_t m) { return S(SYS_mkdirat, dirfd, A(path), (long)m, 0, 0, 0); }
int mknodat(int dirfd, const char *path, mode_t m, dev_t dev) { return S(SYS_mknodat, dirfd, A(path), (long)m, (long)dev, 0, 0); }
int mknod(const char *path, mode_t m, dev_t dev) { return mknodat(AT_FDCWD, path, m, dev); }
int mkfifo(const char *path, mode_t m) { return mknodat(AT_FDCWD, path, S_IFIFO | (m & 07777), 0); }
int mkfifoat(int dirfd, const char *path, mode_t m) { return mknodat(dirfd, path, S_IFIFO | (m & 07777), 0); }
int creat(const char *path, mode_t m) { return open(path, O_CREAT | O_WRONLY | O_TRUNC, m); }

/* ---- stat ---- */
int fstatat(int dirfd, const char *path, struct stat *st, int flags) { return S(SYS_newfstatat, dirfd, A(path), A(st), flags, 0, 0); }
int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *sx) { return S(SYS_statx, dirfd, A(path), flags, (long)mask, A(sx), 0); }
int statfs(const char *path, struct statfs *s) { return S(SYS_statfs, A(path), A(s), 0, 0, 0, 0); }
int fstatfs(int fd, struct statfs *s) { return S(SYS_fstatfs, fd, A(s), 0, 0, 0, 0); }
static void to_statvfs(const struct statfs *a, struct statvfs *b) {
  memset(b, 0, sizeof *b);
  b->f_bsize = a->f_bsize; b->f_frsize = a->f_frsize;
  b->f_blocks = a->f_blocks; b->f_bfree = a->f_bfree; b->f_bavail = a->f_bavail;
  b->f_files = a->f_files; b->f_ffree = a->f_ffree; b->f_favail = a->f_ffree;
  b->f_fsid = (unsigned long)a->f_fsid[0]; b->f_namemax = a->f_namelen;
  b->f_flag = a->f_flags & 0x400 ? ST_RELATIME : 0;
}
int statvfs(const char *path, struct statvfs *v) { struct statfs s; if (statfs(path, &s)) return -1; to_statvfs(&s, v); return 0; }
int fstatvfs(int fd, struct statvfs *v) { struct statfs s; if (fstatfs(fd, &s)) return -1; to_statvfs(&s, v); return 0; }

/* ---- modes, owners, times ---- */
int chmod(const char *path, mode_t m) { return S(SYS_fchmodat, AT_FDCWD, A(path), (long)m, 0, 0, 0); }
int fchmod(int fd, mode_t m) { return S(SYS_fchmod, fd, (long)m, 0, 0, 0, 0); }
int fchmodat(int dirfd, const char *path, mode_t m, int flags) {
  if (!flags) return S(SYS_fchmodat, dirfd, A(path), (long)m, 0, 0, 0);
  return S(SYS_fchmodat2, dirfd, A(path), (long)m, flags, 0, 0);
}
int chown(const char *path, uid_t u, gid_t g) { return S(SYS_fchownat, AT_FDCWD, A(path), (long)u, (long)g, 0, 0); }
int lchown(const char *path, uid_t u, gid_t g) { return S(SYS_fchownat, AT_FDCWD, A(path), (long)u, (long)g, AT_SYMLINK_NOFOLLOW, 0); }
int fchown(int fd, uid_t u, gid_t g) { return S(SYS_fchown, fd, (long)u, (long)g, 0, 0, 0); }
int fchownat(int dirfd, const char *path, uid_t u, gid_t g, int flags) { return S(SYS_fchownat, dirfd, A(path), (long)u, (long)g, flags, 0); }
mode_t umask(mode_t m) { return (mode_t)__syscall1(SYS_umask, m); }
int utimensat(int dirfd, const char *path, const struct timespec t[2], int flags) { return S(SYS_utimensat, dirfd, A(path), A(t), flags, 0, 0); }
int futimens(int fd, const struct timespec t[2]) { return S(SYS_utimensat, fd, 0, A(t), 0, 0, 0); }
static const struct timespec *tv_to_ts(const struct timeval *tv, struct timespec *ts) {
  if (!tv) return 0;
  for (int k = 0; k < 2; k++) {
    if (tv[k].tv_usec < 0 || tv[k].tv_usec >= 1000000) { errno = EINVAL; return (const struct timespec *)-1; }
    ts[k].tv_sec = tv[k].tv_sec; ts[k].tv_nsec = tv[k].tv_usec * 1000;
  }
  return ts;
}
static int utimes_at(int dirfd, const char *path, const struct timeval *tv, int flags) {
  struct timespec ts[2];
  const struct timespec *p = tv_to_ts(tv, ts);
  if (p == (const struct timespec *)-1) return -1;
  return utimensat(dirfd, path, p, flags);
}
int utimes(const char *path, const struct timeval tv[2]) { return utimes_at(AT_FDCWD, path, tv, 0); }
int lutimes(const char *path, const struct timeval tv[2]) { return utimes_at(AT_FDCWD, path, tv, AT_SYMLINK_NOFOLLOW); }
int futimes(int fd, const struct timeval tv[2]) {
  struct timespec ts[2];
  const struct timespec *p = tv_to_ts(tv, ts);
  if (p == (const struct timespec *)-1) return -1;
  return futimens(fd, p);
}
int utime(const char *path, const struct utimbuf *u) {
  if (!u) return utimensat(AT_FDCWD, path, 0, 0);
  struct timespec ts[2] = {{u->actime, 0}, {u->modtime, 0}};
  return utimensat(AT_FDCWD, path, ts, 0);
}
int faccessat(int dirfd, const char *path, int mode, int flags) {
  if (!flags) return S(SYS_faccessat, dirfd, A(path), mode, 0, 0, 0);
  return S(SYS_faccessat2, dirfd, A(path), mode, flags, 0, 0);
}
int eaccess(const char *path, int mode) { return faccessat(AT_FDCWD, path, mode, AT_EACCESS); }
int euidaccess(const char *path, int mode) { return eaccess(path, mode); }

/* ---- sizes, syncing, locks ---- */
int truncate(const char *path, off_t n) { return S(SYS_truncate, A(path), (long)n, 0, 0, 0, 0); }
int fallocate(int fd, int mode, off_t off, off_t len) { return S(SYS_fallocate, fd, mode, (long)off, (long)len, 0, 0); }
int posix_fallocate(int fd, off_t off, off_t len) { long r = __syscall6(SYS_fallocate, fd, 0, (long)off, (long)len, 0, 0); return r < 0 ? (int)-r : 0; }
int fsync(int fd) { return S(SYS_fsync, fd, 0, 0, 0, 0, 0); }
int fdatasync(int fd) { return S(SYS_fdatasync, fd, 0, 0, 0, 0, 0); }
int syncfs(int fd) { return S(SYS_syncfs, fd, 0, 0, 0, 0, 0); }
void sync(void) { __syscall0(SYS_sync); }
int flock(int fd, int op) { return S(SYS_flock, fd, op, 0, 0, 0, 0); }
int lockf(int fd, int cmd, off_t len) {
  struct flock fl = {F_WRLCK, SEEK_CUR, 0, len, 0};
  switch (cmd) {
  case F_ULOCK: fl.l_type = F_UNLCK; return fcntl(fd, F_SETLK, &fl);
  case F_LOCK: return fcntl(fd, F_SETLKW, &fl);
  case F_TLOCK: return fcntl(fd, F_SETLK, &fl);
  case F_TEST:
    fl.l_type = F_WRLCK;
    if (fcntl(fd, F_GETLK, &fl) < 0) return -1;
    if (fl.l_type == F_UNLCK || fl.l_pid == getpid()) return 0;
    errno = EACCES;
    return -1;
  }
  errno = EINVAL;
  return -1;
}

/* ---- positioned and vectored I/O ---- */
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return S(SYS_pread64, fd, A(buf), (long)n, (long)off, 0, 0); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return S(SYS_pwrite64, fd, A(buf), (long)n, (long)off, 0, 0); }
ssize_t readv(int fd, const struct iovec *v, int n) { return S(SYS_readv, fd, A(v), n, 0, 0, 0); }
ssize_t writev(int fd, const struct iovec *v, int n) { return S(SYS_writev, fd, A(v), n, 0, 0, 0); }
ssize_t preadv(int fd, const struct iovec *v, int n, off_t off) { return S(SYS_preadv, fd, A(v), n, (long)off, 0, 0); }
ssize_t pwritev(int fd, const struct iovec *v, int n, off_t off) { return S(SYS_pwritev, fd, A(v), n, (long)off, 0, 0); }

/* ---- credentials ---- */
int setuid(uid_t u) { return S(SYS_setuid, (long)u, 0, 0, 0, 0, 0); }
int setgid(gid_t g) { return S(SYS_setgid, (long)g, 0, 0, 0, 0, 0); }
int seteuid(uid_t u) { return S(SYS_setresuid, -1, (long)u, -1, 0, 0, 0); }
int setegid(gid_t g) { return S(SYS_setresgid, -1, (long)g, -1, 0, 0, 0); }
int setreuid(uid_t r, uid_t e) { return S(SYS_setreuid, (long)r, (long)e, 0, 0, 0, 0); }
int setregid(gid_t r, gid_t e) { return S(SYS_setregid, (long)r, (long)e, 0, 0, 0, 0); }
int setresuid(uid_t r, uid_t e, uid_t s) { return S(SYS_setresuid, (long)r, (long)e, (long)s, 0, 0, 0); }
int setresgid(gid_t r, gid_t e, gid_t s) { return S(SYS_setresgid, (long)r, (long)e, (long)s, 0, 0, 0); }
int getresuid(uid_t *r, uid_t *e, uid_t *s) { return S(SYS_getresuid, A(r), A(e), A(s), 0, 0, 0); }
int getresgid(gid_t *r, gid_t *e, gid_t *s) { return S(SYS_getresgid, A(r), A(e), A(s), 0, 0, 0); }
int getgroups(int n, gid_t *list) { return S(SYS_getgroups, n, A(list), 0, 0, 0, 0); }
int setgroups(size_t n, const gid_t *list) { return S(SYS_setgroups, (long)n, A(list), 0, 0, 0, 0); }

/* ---- directory streams on descriptors. The layout must match struct __dir in unistd.c. ---- */
struct __dir { int fd; long len, pos; struct dirent ent; char buf[2048]; };
int dirfd(DIR *d) { return d->fd; }
DIR *fdopendir(int fd) {
  struct stat st;
  if (fstat(fd, &st) < 0) return 0;
  if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return 0; }
  DIR *d = calloc(1, sizeof *d);
  if (!d) return 0;
  d->fd = fd;
  return d;
}
void rewinddir(DIR *d) { lseek(d->fd, 0, SEEK_SET); d->len = d->pos = 0; }
