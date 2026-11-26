// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _LIBC_LIMITS_H
#define _LIBC_LIMITS_H
/* clang's limits.h has the C limits; add the POSIX ones this system uses. */
#include_next <limits.h>
#define PATH_MAX 4096 /* buffer size; the kernel currently accepts paths up to 255 bytes */
#define NAME_MAX 255
#define PIPE_BUF 4096
#define ARG_MAX 8192 /* argv + environment blob handed to a new program */
#define OPEN_MAX 32  /* file descriptors per process */
#define HOST_NAME_MAX 64
#define LOGIN_NAME_MAX 32
#define LINE_MAX 2048
#define SSIZE_MAX LONG_MAX
#define IOV_MAX 1024
#endif
