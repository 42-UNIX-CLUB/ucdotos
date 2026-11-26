// SPDX-License-Identifier: GPL-2.0-or-later
/* Signals. The kernel keeps dispositions, the mask and pending signals. A handler runs at
 * a system call boundary: the kernel answers -ERESTARTSYS without waiting, the wrapper's
 * slow path (libc/src/syswait.c) calls __sig_hook, which is deliver() below once the
 * program has installed a handler. A loop that makes no system call never sees a signal. */
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

extern void (*__sig_hook)(void);

/* take every signal the kernel has for us and run its handler */
static void deliver(void) {
  struct sigtake t;
  long sig;
  while ((sig = __syscall1(SYS_sigtake, &t)) > 0) {
    if (t.flags & SA_SIGINFO) {
      siginfo_t si;
      memset(&si, 0, sizeof si);
      si.si_signo = sig; si.si_code = t.code; si.si_pid = t.pid; si.si_uid = 1000; si.si_status = t.status;
      ((void (*)(int, siginfo_t *, void *))(unsigned long)t.handler)(sig, &si, 0);
    } else ((void (*)(int))(unsigned long)t.handler)(sig);
    __syscall4(SYS_rt_sigprocmask, SIG_SETMASK, &t.oldmask, 0, 8);
  }
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  struct k_sigaction k, ko;
  if (act) {
    memset(&k, 0, sizeof k);
    k.handler = (u32)(unsigned long)act->sa_handler;
    k.flags = act->sa_flags;
    k.mask = act->sa_mask;
    if (k.handler > 1) __sig_hook = deliver;
  }
  long r = __syscall4(SYS_rt_sigaction, sig, act ? &k : 0, old ? &ko : 0, 8);
  if (r < 0) return __sysret(r);
  if (old) {
    memset(old, 0, sizeof *old);
    old->sa_handler = (sighandler_t)(unsigned long)ko.handler;
    old->sa_flags = ko.flags;
    old->sa_mask = ko.mask;
  }
  return 0;
}
/* BSD semantics, as glibc: the handler stays installed and interrupted calls restart */
sighandler_t signal(int sig, sighandler_t h) {
  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = h;
  sa.sa_flags = SA_RESTART;
  if (sigaction(sig, &sa, &old) < 0) return SIG_ERR;
  return old.sa_handler;
}
int kill(pid_t pid, int sig) {
  long r = __syscall2(SYS_kill, pid, sig);
  if (__sig_hook) deliver(); /* a signal sent to ourselves runs before kill returns */
  return __sysret(r);
}
int killpg(pid_t pg, int sig) {
  if (pg <= 1) { errno = EINVAL; return -1; }
  return kill(-pg, sig);
}
int raise(int sig) { return kill(getpid(), sig); }
int sigemptyset(sigset_t *s) { *s = 0; return 0; }
int sigfillset(sigset_t *s) { *s = ~0ull; return 0; }
static int bad_sig(int n) { if (n <= 0 || n >= NSIG) { errno = EINVAL; return 1; } return 0; }
int sigaddset(sigset_t *s, int n) { if (bad_sig(n)) return -1; *s |= 1ull << (n - 1); return 0; }
int sigdelset(sigset_t *s, int n) { if (bad_sig(n)) return -1; *s &= ~(1ull << (n - 1)); return 0; }
int sigismember(const sigset_t *s, int n) { if (bad_sig(n)) return -1; return (*s >> (n - 1)) & 1; }
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
  long r = __syscall4(SYS_rt_sigprocmask, how, set, old, 8);
  if (r < 0) return __sysret(r);
  if (__sig_hook) deliver(); /* a signal just unblocked runs before this returns */
  return 0;
}
int sigpending(sigset_t *s) { return __sysret(__syscall2(SYS_rt_sigpending, s, 8)); }
int sigsuspend(const sigset_t *s) { return __sysret(__syscall2(SYS_rt_sigsuspend, s, 8)); }
int pause(void) {
  sigset_t cur;
  if (__syscall4(SYS_rt_sigprocmask, SIG_BLOCK, 0, &cur, 8) < 0) return -1;
  return sigsuspend(&cur);
}
int siginterrupt(int sig, int flag) {
  struct sigaction sa;
  if (sigaction(sig, 0, &sa) < 0) return -1;
  if (flag) sa.sa_flags &= ~SA_RESTART; else sa.sa_flags |= SA_RESTART;
  return sigaction(sig, &sa, 0);
}

/* ITIMER_REAL only */
int getitimer(int which, struct itimerval *v) {
  struct k_itimerval k;
  long r = __syscall2(SYS_getitimer, which, &k);
  if (r < 0) return __sysret(r);
  v->it_interval.tv_sec = k.interval_sec; v->it_interval.tv_usec = k.interval_usec;
  v->it_value.tv_sec = k.value_sec; v->it_value.tv_usec = k.value_usec;
  return 0;
}
int setitimer(int which, const struct itimerval *v, struct itimerval *old) {
  struct k_itimerval k, ko;
  k.interval_sec = v->it_interval.tv_sec; k.interval_usec = v->it_interval.tv_usec;
  k.value_sec = v->it_value.tv_sec; k.value_usec = v->it_value.tv_usec;
  long r = __syscall3(SYS_setitimer, which, &k, old ? &ko : 0);
  if (r < 0) return __sysret(r);
  if (old) {
    old->it_interval.tv_sec = ko.interval_sec; old->it_interval.tv_usec = ko.interval_usec;
    old->it_value.tv_sec = ko.value_sec; old->it_value.tv_usec = ko.value_usec;
  }
  return 0;
}
unsigned alarm(unsigned s) {
  struct itimerval v = {{0, 0}, {(long)s, 0}}, old;
  if (setitimer(ITIMER_REAL, &v, &old) < 0) return 0;
  return old.it_value.tv_sec + (old.it_value.tv_usec >= 500000 || (!old.it_value.tv_sec && old.it_value.tv_usec));
}
