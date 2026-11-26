// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H
#include <sys/types.h>
typedef unsigned long long fsblkcnt_t;
typedef unsigned long long fsfilcnt_t;
struct statvfs {
  unsigned long f_bsize, f_frsize;
  fsblkcnt_t f_blocks, f_bfree, f_bavail;
  fsfilcnt_t f_files, f_ffree, f_favail;
  unsigned long f_fsid, f_flag, f_namemax;
};
#define ST_RDONLY 1
#define ST_NOSUID 2
#define ST_RELATIME 4096
int statvfs(const char *, struct statvfs *);
int fstatvfs(int, struct statvfs *);
#endif
