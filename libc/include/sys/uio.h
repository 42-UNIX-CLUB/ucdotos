// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#include <stddef_extra.h>
#include <sys/syscall.h>
/* struct iovec is in abi/abi.h */
ssize_t readv(int, const struct iovec *, int);
ssize_t writev(int, const struct iovec *, int);
ssize_t preadv(int, const struct iovec *, int, off_t);
ssize_t pwritev(int, const struct iovec *, int, off_t);
#endif
