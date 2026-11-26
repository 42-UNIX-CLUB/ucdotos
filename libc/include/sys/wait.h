// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <stddef_extra.h>
#include <sys/syscall.h>
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s)&0x7f)
#define WIFEXITED(s) (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0)
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
#endif
