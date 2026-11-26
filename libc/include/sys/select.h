// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <stddef_extra.h>
#include <sys/syscall.h>
/* select() is built on ppoll; a process has at most 32 descriptors. */
typedef long suseconds_t;
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
#define FD_SETSIZE 32
typedef struct { unsigned long fds_bits[1]; } fd_set;
#define FD_ZERO(s) ((s)->fds_bits[0] = 0)
#define FD_SET(fd, s) ((s)->fds_bits[0] |= 1UL << (fd))
#define FD_CLR(fd, s) ((s)->fds_bits[0] &= ~(1UL << (fd)))
#define FD_ISSET(fd, s) (((s)->fds_bits[0] >> (fd)) & 1)
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#endif
