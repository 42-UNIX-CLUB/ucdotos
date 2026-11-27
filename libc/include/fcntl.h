// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FCNTL_H
#define _FCNTL_H
#include <stddef_extra.h>
#include <sys/syscall.h>
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int open(const char *, int, ...);
int openat(int, const char *, int, ...);
int fcntl(int, int, ...);
/* ---- appended (libc/src/fs_posix.c) ---- */
#include <sys/types.h>
int creat(const char *, mode_t);
int fallocate(int, int, off_t, off_t);
int posix_fallocate(int, off_t, off_t);
#endif
