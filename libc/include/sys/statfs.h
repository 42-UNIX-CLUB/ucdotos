// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
#include <sys/syscall.h>
/* struct statfs is Linux's statfs64 layout (abi/abi.h) */
int statfs(const char *, struct statfs *);
int fstatfs(int, struct statfs *);
#endif
