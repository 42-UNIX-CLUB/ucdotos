// SPDX-License-Identifier: GPL-2.0-or-later
/* POSIX file calls the package manager needs that libc/src/arc_compat.c does
 * not already provide: thin wrappers over Linux's generic system call
 * numbers. The kernel may not implement them yet; then they fail with ENOSYS
 * and the caller decides what to do. Links and times are in arc_compat.c;
 * modes, owners, fsync and umask in tools_compat.c (each defined once). */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

int fdatasync(int fd) { return __sysret(__syscall1(SYS_fdatasync, fd)); }
