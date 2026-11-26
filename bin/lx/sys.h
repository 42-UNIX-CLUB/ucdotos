// SPDX-License-Identifier: GPL-2.0-or-later
/* The Linux system call layer: state shared by sys.c (files, memory, time, identity),
 * sys_sig.c (signals) and sys_proc.c (clone, execve, wait). */
#ifndef LX_SYS_H
#define LX_SYS_H
#include "lx.h"
#include "arch.h"
#include "abi_lx.h"

#define LX_NFD 1024

/* What the dispatcher should do with the CPU after a call. */
enum { SR_RET = 0, SR_NORET = 1, SR_RESTART = 2 };

/* Entry point: the CPU stopped at a system call instruction. */
void lx_syscall(void);
/* The CPU stopped for another reason (fault, illegal instruction, breakpoint). */
void lx_trap(int stop, const struct lx_fault *f);
/* Deliver pending signals, if any. Called after every stop. */
void lx_signals(void);
extern volatile uint64_t lx_sig_pending; /* bit (sig-1) */

/* per-descriptor guest path (for *at calls in rooted mode, /proc/self/fd, fchdir) */
const char *fd_path_get(int fd);
void fd_path_set(int fd, const char *gpath);
void fd_path_dup(int from, int to);
void fd_close_cloexec(void);

/* signals (sys_sig.c) */
int64_t sys_rt_sigaction(uint64_t a[6]);
int64_t sys_rt_sigprocmask(uint64_t a[6]);
int64_t sys_sigaltstack(uint64_t a[6]);
int64_t sys_rt_sigreturn(void);
int64_t sys_kill(uint64_t a[6], int nr);
int64_t sys_rt_sigsuspend(uint64_t a[6]); /* LX_SIG_DELIVERED: a handler was entered */
#define LX_SIG_DELIVERED INT64_MIN
int64_t sys_rt_sigpending(uint64_t a[6]);
void sig_raise(int sig);              /* make a signal pending for this process */
void sig_exec_reset(void);            /* handlers back to default at execve */
int sig_restart_wanted(void);         /* a pending signal's handler has SA_RESTART */
int sig_has_pending(void);
uint64_t sig_get_mask(void);
void sig_set_mask(uint64_t m);
void lx_exit_signal(int sig); /* returns only in an emulated fork child */

/* processes (sys_proc.c) */
int64_t sys_clone(uint64_t a[6]);
int64_t sys_clone3(uint64_t a[6]);
int64_t sys_execve(uint64_t a[6], int *noret);
int64_t sys_wait4(uint64_t a[6]);
int64_t sys_waitid(uint64_t a[6]);
/* Start the guest program at host path with host argv/envp: used by main and execve. */
int64_t lx_exec(const char *gpath, char *const argv[], char *const envp[]);
int lx_in_emulated_child(void);       /* running a fork child in the parent's process */
extern char lx_exe_path[4096];        /* guest path of the running program (/proc/self/exe) */
extern uint64_t lx_clear_tid;         /* set_tid_address */

/* green threads (sys_thread.c) */
int64_t lx_thread_create(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid);
int lx_thread_exit(void);            /* 1: other threads remain, one of them now runs */
void lx_thread_yield(void);
void lx_threads_reset(void);
int lx_thread_count(void);
int64_t lx_gettid(void);
int64_t lx_futex(uint64_t a[6], int *noret);

/* statistics and tracing */
extern uint64_t lx_sys_count[LX_NSYS];
const char *lx_sysname(int nr);
void lx_print_stats(void);

#endif
