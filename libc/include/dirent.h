// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _DIRENT_H
#define _DIRENT_H
#include <stddef_extra.h>
#include <sys/syscall.h>
struct dirent { unsigned long long d_ino; unsigned char d_type; char d_name[256]; };
typedef struct __dir DIR;
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
/* ---- appended (libc/src/fs_posix.c) ---- */
int dirfd(DIR *);
DIR *fdopendir(int);
void rewinddir(DIR *);
#endif
