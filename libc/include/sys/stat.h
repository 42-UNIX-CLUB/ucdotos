// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <stddef_extra.h>
#include <sys/syscall.h>
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m)&S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m)&S_IFMT) == S_IFIFO)
int stat(const char *, struct stat *);
int lstat(const char *, struct stat *);
int fstat(int, struct stat *);
int mkdir(const char *, mode_t);
/* modes (libc/src/tools_compat.c; the kernel may answer ENOSYS) */
int chmod(const char *, mode_t);
int fchmod(int, mode_t);
mode_t umask(mode_t);
/* permissions, times and special files (libc/src/arc_compat.c; the kernel may answer ENOSYS) */
#ifndef _DEV_T_DEFINED
#define _DEV_T_DEFINED
typedef unsigned long long dev_t;
#endif
#define S_ISLNK(m) (((m)&S_IFMT) == S_IFLNK)
#define S_ISBLK(m) (((m)&S_IFMT) == S_IFBLK)
#define S_ISSOCK(m) (((m)&S_IFMT) == S_IFSOCK)
int fchmodat(int, const char *, mode_t, int);
int utimensat(int, const char *, const struct timespec[2], int);
int futimens(int, const struct timespec[2]);
int mknod(const char *, mode_t, dev_t);
int mknodat(int, const char *, mode_t, dev_t);
int mkfifo(const char *, mode_t);
#endif
