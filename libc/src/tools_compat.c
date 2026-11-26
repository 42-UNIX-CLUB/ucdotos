// SPDX-License-Identifier: GPL-2.0-or-later
/* C library calls the core tools need: modes and owners, umask, sync, random
 * bytes, temporary files and ttyname. Most pass straight to the Linux generic
 * system calls; the kernel may not implement some yet, and then they fail with
 * errno ENOSYS and the caller decides what to do.
 * Defined weak, as in arc_compat.c, so that a strong definition elsewhere in
 * the C library takes over without a duplicate-symbol error. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/syscall.h>

#define WEAK __attribute__((weak))
#ifndef SYS_fchmod
#define SYS_fchmod 52
#endif
#ifndef SYS_fchmodat
#define SYS_fchmodat 53
#endif
#ifndef SYS_fchownat
#define SYS_fchownat 54
#endif
#ifndef SYS_fchown
#define SYS_fchown 55
#endif
#ifndef SYS_sync
#define SYS_sync 81
#endif
#ifndef SYS_fsync
#define SYS_fsync 82
#endif
#ifndef SYS_umask
#define SYS_umask 166
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

WEAK int chmod(const char *path, mode_t mode) { return __sysret(__syscall4(SYS_fchmodat, AT_FDCWD, path, mode, 0)); }
WEAK int fchmod(int fd, mode_t mode) { return __sysret(__syscall2(SYS_fchmod, fd, mode)); }
WEAK int chown(const char *path, uid_t u, gid_t g) { return __sysret(__syscall6(SYS_fchownat, AT_FDCWD, (long)path, u, g, 0, 0)); }
WEAK int lchown(const char *path, uid_t u, gid_t g) { return __sysret(__syscall6(SYS_fchownat, AT_FDCWD, (long)path, u, g, AT_SYMLINK_NOFOLLOW, 0)); }
WEAK int fchown(int fd, uid_t u, gid_t g) { return __sysret(__syscall3(SYS_fchown, fd, u, g)); }
WEAK int fsync(int fd) { return __sysret(__syscall1(SYS_fsync, fd)); }
WEAK void sync(void) { __syscall0(SYS_sync); }
/* umask: without kernel support, report the usual 022 and change nothing */
WEAK mode_t umask(mode_t m) { long r = __syscall1(SYS_umask, m); return r < 0 ? 022 : (mode_t)r; }

WEAK ssize_t getrandom(void *buf, size_t n, unsigned flags) { return __sysret(__syscall3(SYS_getrandom, buf, n, flags)); }

/* replace the trailing XXXXXX of a template with random letters and digits */
static int fill_template(char *t, int suffixlen) {
  static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  size_t len = strlen(t);
  if (suffixlen < 0 || len < (size_t)suffixlen + 6) { errno = EINVAL; return -1; }
  char *x = t + len - suffixlen - 6;
  if (memcmp(x, "XXXXXX", 6)) { errno = EINVAL; return -1; }
  unsigned char r[6];
  if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) return -1;
  for (int i = 0; i < 6; i++) x[i] = set[r[i] % (sizeof set - 1)];
  return 0;
}
WEAK int mkstemps(char *t, int suffixlen) {
  char *x = t + strlen(t) - suffixlen - 6;
  for (int tries = 0; tries < 100; tries++) {
    if (tries) memcpy(x, "XXXXXX", 6);
    if (fill_template(t, suffixlen) < 0) return -1;
    int fd = open(t, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) return fd;
  }
  return -1;
}
WEAK int mkstemp(char *t) { return mkstemps(t, 0); }
WEAK char *mkdtemp(char *t) {
  char *x = t + strlen(t) - 6;
  for (int tries = 0; tries < 100; tries++) {
    if (tries) memcpy(x, "XXXXXX", 6);
    if (fill_template(t, 0) < 0) return NULL;
    if (mkdir(t, 0700) == 0) return t;
    if (errno != EEXIST) return NULL;
  }
  return NULL;
}

/* ttyname: find the device node with the same identity as the descriptor */
static int same_file(const char *path, const struct stat *st) {
  struct stat s;
  return stat(path, &s) == 0 && s.st_dev == st->st_dev && s.st_ino == st->st_ino;
}
WEAK char *ttyname(int fd) {
  static char name[64];
  struct stat st;
  if (!isatty(fd)) return NULL; /* isatty sets errno */
  if (fstat(fd, &st) < 0) return NULL;
  static const char *const dirs[] = {"/dev/pts", "/dev"};
  for (int d = 0; d < 2; d++) {
    DIR *dir = opendir(dirs[d]);
    if (!dir) continue;
    struct dirent *e;
    while ((e = readdir(dir))) {
      if (e->d_name[0] == '.' || strlen(dirs[d]) + strlen(e->d_name) + 2 > sizeof name) continue;
      strcpy(name, dirs[d]);
      strcat(name, "/");
      strcat(name, e->d_name);
      if (same_file(name, &st)) { closedir(dir); return name; }
    }
    closedir(dir);
  }
  errno = ENOTTY;
  return NULL;
}
