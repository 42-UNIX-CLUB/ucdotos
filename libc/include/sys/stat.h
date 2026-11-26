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
/* ---- appended: links, modes, the *at calls, statx (libc/src/fs_posix.c) ---- */
#include <sys/types.h>
#define S_ISLNK(m) (((m)&S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m)&S_IFMT) == S_IFSOCK)
#define S_ISBLK(m) (((m)&S_IFMT) == S_IFBLK)
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01
#define S_IREAD S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC S_IXUSR
#define ACCESSPERMS 0777
#define ALLPERMS 07777
#define DEFFILEMODE 0666
int chmod(const char *, mode_t);
int fchmod(int, mode_t);
int fchmodat(int, const char *, mode_t, int);
int mkdirat(int, const char *, mode_t);
int mknod(const char *, mode_t, dev_t);
int mknodat(int, const char *, mode_t, dev_t);
int mkfifo(const char *, mode_t);
int mkfifoat(int, const char *, mode_t);
mode_t umask(mode_t);
int fstatat(int, const char *, struct stat *, int);
int utimensat(int, const char *, const struct timespec[2], int);
int futimens(int, const struct timespec[2]);
int statx(int, const char *, int, unsigned, struct statx *);
#endif
