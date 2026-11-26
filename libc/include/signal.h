// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <stddef_extra.h>
#include <sys/syscall.h>
/* This system never delivers signals to a program: kill() terminates the target.
 * signal() and sigaction() only record the disposition (it is returned by the
 * next call); handlers run only through raise(). */
typedef int sig_atomic_t;
typedef unsigned long long sigset_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIGHUP 1
#ifndef SIGINT
#define SIGINT 2
#endif
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT 6
#define SIGBUS 7
#define SIGFPE 8
#ifndef SIGKILL
#define SIGKILL 9
#endif
#define SIGUSR1 10
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#ifndef SIGTERM
#define SIGTERM 15
#endif
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL 29
#define SIGPWR 30
#define SIGSYS 31
#define NSIG 65
#define _NSIG 65
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
typedef struct { int si_signo, si_errno, si_code; pid_t si_pid; uid_t si_uid; int si_status; void *si_addr; } siginfo_t;
struct sigaction {
  union { sighandler_t sa_handler; void (*sa_sigaction)(int, siginfo_t *, void *); };
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};
sighandler_t signal(int, sighandler_t);
int sigaction(int, const struct sigaction *, struct sigaction *);
int kill(pid_t, int);
int raise(int);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigpending(sigset_t *);
int sigsuspend(const sigset_t *);
#endif
