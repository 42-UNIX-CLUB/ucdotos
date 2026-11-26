// SPDX-License-Identifier: GPL-2.0-or-later
/* Thin wrappers for the POSIX calls tar needs to restore files: links,
 * permissions, owners, times and special files. They pass straight to the
 * Linux generic system calls. The kernel may not implement them yet; then
 * they fail with errno ENOSYS and the caller decides what to do.
 * Defined weak so that a later strong definition elsewhere in the C library
 * takes over without a duplicate-symbol error. chmod, fchmod, chown, lchown
 * and fchown are in tools_compat.c (the core tools added them first); each
 * function is defined once in the C library. */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define WEAK __attribute__((weak))

WEAK int symlinkat(const char *target, int dirfd, const char *path) { return __sysret(__syscall3(SYS_symlinkat, target, dirfd, path)); }
WEAK int symlink(const char *target, const char *path) { return symlinkat(target, AT_FDCWD, path); }
WEAK int linkat(int olddir, const char *old, int newdir, const char *new, int flags) {
  return __sysret(__syscall6(SYS_linkat, olddir, (long)old, newdir, (long)new, flags, 0));
}
WEAK int link(const char *old, const char *new) { return linkat(AT_FDCWD, old, AT_FDCWD, new, 0); }
WEAK ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t n) { return __sysret(__syscall4(SYS_readlinkat, dirfd, path, buf, n)); }
WEAK ssize_t readlink(const char *path, char *buf, size_t n) { return readlinkat(AT_FDCWD, path, buf, n); }
WEAK int fchmodat(int dirfd, const char *path, mode_t mode, int flags) { return __sysret(__syscall4(SYS_fchmodat, dirfd, path, mode, flags)); }
WEAK int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) {
  return __sysret(__syscall6(SYS_fchownat, dirfd, (long)path, uid, gid, flags, 0));
}
WEAK int utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags) {
  return __sysret(__syscall4(SYS_utimensat, dirfd, path, ts, flags));
}
WEAK int futimens(int fd, const struct timespec ts[2]) { return utimensat(fd, 0, ts, 0); }
WEAK int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) { return __sysret(__syscall4(SYS_mknodat, dirfd, path, mode, (long)dev)); }
WEAK int mknod(const char *path, mode_t mode, dev_t dev) { return mknodat(AT_FDCWD, path, mode, dev); }
WEAK int mkfifo(const char *path, mode_t mode) { return mknod(path, (mode & 07777) | S_IFIFO, 0); }
