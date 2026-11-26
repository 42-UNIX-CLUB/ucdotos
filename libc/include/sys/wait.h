// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#include <stddef_extra.h>
#include <sys/syscall.h>
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s)&0x7f)
#define WIFEXITED(s) (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7f)
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
/* stopped and continued children (waitpid with WUNTRACED, WCONTINUED) */
#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s) WEXITSTATUS(s)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WCOREDUMP(s) 0
#endif
