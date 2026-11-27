// SPDX-License-Identifier: GPL-2.0-or-later
/* Signals. Nothing can interrupt running WebAssembly, so a signal reaches a program only
 * when it enters the kernel:
 *  - Default actions (terminate, stop, continue, ignore) happen in the kernel, at once,
 *    when the signal is sent, unless the signal is blocked.
 *  - A signal with a handler is made pending and the target is "kicked": its system call
 *    counter is moved so that its next call takes the out-of-line slice check, which
 *    sees the kick (sig_kicked). The hot path of syscall_pid is unchanged.
 *  - Delivery: the kicked call (or a blocking call, from block()) returns -ERESTARTSYS
 *    with "do not wait". The libc's wrapper then asks for the signal with SYS_sigtake,
 *    calls the handler on the program's own stack, restores the mask and makes the call
 *    again. A call that was blocked when the signal came then fails with EINTR, unless
 *    every handler that ran had SA_RESTART (and the call is not a sleep). */
#include "kernel.h"

#define BIT(s) (1ull << ((s) - 1))
#define UNBLOCKABLE (BIT(SIGKILL) | BIT(SIGSTOP))
#define STOPSIGS (BIT(SIGSTOP) | BIT(SIGTSTP) | BIT(SIGTTIN) | BIT(SIGTTOU))
/* default action "ignore": SIGCHLD, SIGCONT, SIGURG (23), SIGWINCH */
#define DFL_IGNORE (BIT(SIGCHLD) | BIT(SIGCONT) | BIT(23) | BIT(SIGWINCH))

static u64 handled(struct proc *p) { /* signals that have a handler */
  u64 m = 0;
  for (int s = 1; s < 65; s++) if (p->sa[s].handler > 1) m |= BIT(s);
  return m;
}
static u64 deliverable(struct proc *p) { return p->sig_pending & ~p->sig_blocked & handled(p); }

void sig_kick(struct proc *p) {
  if (p->sig_kick) return;
  p->sig_kick = 1;
  u32 add = 255 - (u32)(p->nsys & 255); /* the next ++nsys has low byte 0: the slow check runs */
  p->nsys += add;
  p->kick_skew += add;
}

static void notify_parent(struct proc *c, int code, int status) {
  struct proc *par = c->ppid > 0 ? &procs[c->ppid] : NULL;
  if (!par || par->state != P_RUN) return;
  wq_wake(&par->child_wq);
  if (!(par->sa[SIGCHLD].flags & SA_NOCLDSTOP)) sig_send(par, SIGCHLD, code, c->pid, status);
}
static void stop(struct proc *p, int sig) {
  if (p->stopped) return;
  p->stopped = 1; p->stop_sig = sig; p->wait_report = 1;
  sig_kick(p);              /* it parks at its next system call */
  host_wake(p->pid);        /* a parked one retries its call and parks again, stopped */
  notify_parent(p, 5 /* CLD_STOPPED */, sig);
}
static void cont(struct proc *p) {
  p->sig_pending &= ~STOPSIGS;
  if (!p->stopped) return;
  p->stopped = 0; p->wait_report = 2;
  host_wake(p->pid);
  notify_parent(p, 6 /* CLD_CONTINUED */, SIGCONT);
}
/* the default action of an unblocked signal with no handler */
static void apply_default(struct proc *p, int sig) {
  p->sig_pending &= ~BIT(sig);
  if (BIT(sig) & DFL_IGNORE) return;
  if (BIT(sig) & STOPSIGS) { stop(p, sig); return; }
  proc_exit(p, 256 + sig);
  host_wake(p->pid); /* let its suspended stack unwind */
}

/* code, from_pid and status go to siginfo: code is Linux's si_code (0 kill, 0x80 kernel, CLD_* for SIGCHLD) */
void sig_send(struct proc *p, int sig, int code, int from_pid, int status) {
  if (sig <= 0 || sig > 64 || p->state != P_RUN) return;
  if (sig == SIGKILL) { proc_exit(p, 256 + SIGKILL); host_wake(p->pid); return; }
  if (sig == SIGCONT) cont(p);
  if (BIT(sig) & STOPSIGS) p->sig_pending &= ~BIT(SIGCONT);
  u32 h = p->sa[sig].handler;
  if (h == 1 && sig != SIGSTOP) return;                     /* ignored */
  if (h == 0 && (BIT(sig) & DFL_IGNORE)) return;            /* ignored by default */
  p->sig_pid[sig] = from_pid; p->sig_code[sig] = code; p->sig_status[sig] = status;
  if ((p->sig_blocked & BIT(sig)) && !(BIT(sig) & UNBLOCKABLE)) { p->sig_pending |= BIT(sig); return; }
  if (h == 0 || sig == SIGSTOP) { apply_default(p, sig); return; }
  p->sig_pending |= BIT(sig);
  if (p->sleeping && !p->stopped) { /* it is parked in a blocking call: that call is interrupted */
    p->blk_signal = 1 | (p->blk_norestart << 1);
    host_wake(p->pid);
  }
  sig_kick(p);
}

/* At a system call entry, when the process was kicked. Returns 1 when the call must not
 * run now and syscall_pid returns cur->kick_ret instead. */
int sig_kicked(long nr) {
  struct proc *p = cur;
  p->sig_kick = 0;
  p->nsys -= p->kick_skew;
  p->kick_skew = 0;
  /* the libc's own delivery call is never intercepted: the kick waits for the next call */
  if (nr == SYS_sigtake) { sig_kick(p); return 0; }
  if (p->eintr_next) { /* this is the retry of a call a handler interrupted */
    p->eintr_next = 0;
    p->sleep_until = 0;
    if (p->in_sigsuspend) { p->sig_blocked = p->sig_saved_mask; p->in_sigsuspend = 0; }
    p->kick_ret = -EINTR;
    return 1;
  }
  if (p->stopped) {
    sig_kick(p); /* check again when it is woken */
    p->nsys--;
    block_raw(-1);
    p->kick_ret = -ERESTARTSYS;
    return 1;
  }
  if (deliverable(p)) {
    p->nsys--; /* the call is made again after the handlers */
    if (p->blk_signal) { p->in_delivery = p->blk_signal; p->blk_signal = 0; }
    p->block_ms = -2; /* the host returns at once: no waiting */
    p->kick_ret = -ERESTARTSYS;
    return 1;
  }
  return 0;
}

/* From block(), block_nr(): the calling process is about to wait. A signal with a handler
 * that can be delivered interrupts the wait instead. */
long sig_interrupt(int norestart) {
  struct proc *p = cur;
  if (p->alarm_at) alarm_check(host_now());
  if (p->state != P_RUN) return -ERESTARTSYS; /* the alarm killed it */
  p->blk_norestart = (u8)norestart;
  if (!deliverable(p)) return 0;
  p->in_delivery = 1 | (norestart << 1);
  p->blk_signal = 0;
  p->block_ms = -2;
  p->sleeping = 0;
  return -ERESTARTSYS;
}

/* SYS_sigtake: the next signal to run a handler for. Returns 0 when none is left; then a
 * call that was interrupted is told to fail with EINTR on its retry, if it must. */
static long sys_sigtake(u32 uout) {
  struct proc *p = cur;
  /* woken from a blocking call by the signal, the libc takes it before retrying that call */
  if (p->blk_signal) { p->in_delivery = p->blk_signal; p->blk_signal = 0; }
  if (p->in_sigsuspend) p->in_delivery = 3; /* any handler ends a sigsuspend */
  u64 d = deliverable(p);
  if (!d) {
    if (p->in_delivery) {
      if (p->want_eintr) { p->eintr_next = 1; sig_kick(p); }
      p->in_delivery = 0; p->want_eintr = 0;
    }
    return 0;
  }
  int sig = __builtin_ctzll(d) + 1;
  struct k_sigaction *a = &p->sa[sig];
  struct sigtake t;
  memset(&t, 0, sizeof t);
  t.handler = a->handler; t.flags = a->flags; t.oldmask = p->sig_blocked;
  t.signo = sig; t.code = p->sig_code[sig]; t.pid = p->sig_pid[sig]; t.status = p->sig_status[sig];
  if (copy_to_user(uout, &t, sizeof t)) return -EFAULT;
  p->sig_pending &= ~BIT(sig);
  if (p->in_delivery && (!(a->flags & SA_RESTART) || (p->in_delivery & 2))) p->want_eintr = 1;
  p->sig_blocked |= a->mask & ~UNBLOCKABLE;
  if (!(a->flags & SA_NODEFER)) p->sig_blocked |= BIT(sig);
  if (a->flags & SA_RESETHAND) { a->handler = 0; a->flags &= ~SA_SIGINFO; }
  return sig;
}

/* May the caller signal q? Linux's rule: root may signal anyone; otherwise the caller's real
 * or effective uid must be the target's real or saved uid; SIGCONT within one session is
 * always allowed. Only with permission enforcement on (vfs_enforce 2). */
static int may_signal(struct proc *q, int sig) {
  if (vfs_enforce < 2) return 1;
  struct fscred *me = &fscred[cur->pid], *t = &fscred[q->pid];
  if (me->euid == 0) return 1;
  if (me->euid == t->uid || me->euid == t->suid || me->uid == t->uid || me->uid == t->suid) return 1;
  return sig == SIGCONT && q->sid == cur->sid;
}
/* kill(2): pid > 0 one process, 0 the caller's group, -1 everyone but init and the caller,
 * < -1 the group -pid. */
long sys_kill2(int pid, int sig) {
  if (sig < 0 || sig > 64) return -EINVAL;
  int me = cur->pid, found = 0, allowed = 0;
  if (pid > 0) {
    if (pid >= NPROC || procs[pid].state != P_RUN) return -ESRCH;
    if (!may_signal(&procs[pid], sig)) return -EPERM;
    if (sig) sig_send(&procs[pid], sig, 0, me, 0);
    return cur->state == P_RUN ? 0 : -ERESTARTSYS; /* it killed itself */
  }
  int grp = pid == 0 ? cur->pgid : -pid;
  for (int i = 1; i < NPROC; i++) {
    struct proc *q = &procs[i];
    if (q->state != P_RUN) continue;
    if (pid == -1 ? (i == 1 || i == me) : q->pgid != grp) continue;
    found = 1;
    if (!may_signal(q, sig)) continue;
    allowed = 1;
    if (sig && i != me) sig_send(q, sig, 0, me, 0);
  }
  if (sig && pid != -1 && cur->state == P_RUN && cur->pgid == grp) sig_send(cur, sig, 0, me, 0); /* the caller last */
  if (!found) return -ESRCH;
  if (!allowed && !(pid != -1 && cur->pgid == grp)) return -EPERM;
  return cur->state == P_RUN ? 0 : -ERESTARTSYS;
}

long sig_pipe(void) {
  sig_send(cur, SIGPIPE, 0x80, cur->pid, 0);
  return cur->state == P_RUN ? -EPIPE : -ERESTARTSYS;
}

/* A child became a zombie: SIGCHLD to its parent (called by proc_exit). */
void sig_child_exited(struct proc *c) {
  struct proc *par = c->ppid > 0 ? &procs[c->ppid] : NULL;
  if (!par || par->state != P_RUN) return;
  int code = c->exit_code >= 256 ? 2 /* CLD_KILLED */ : 1 /* CLD_EXITED */;
  int st = c->exit_code >= 256 ? c->exit_code - 256 : c->exit_code & 0xff;
  sig_send(par, SIGCHLD, code, c->pid, st);
}

/* execve: handlers go back to the default; ignored signals stay ignored; mask and pending stay */
void sig_exec_reset(struct proc *p) {
  for (int s = 1; s < 65; s++)
    if (p->sa[s].handler > 1) { p->sa[s].handler = 0; p->sa[s].flags = 0; p->sa[s].mask = 0; }
  p->in_delivery = p->want_eintr = p->eintr_next = p->blk_signal = 0;
}
/* fork: everything is copied except pending signals and timers */
void sig_fork_copy(struct proc *c, struct proc *p) {
  memcpy(c->sa, p->sa, sizeof c->sa);
  c->sig_blocked = p->sig_blocked;
}

/* ITIMER_REAL. Checked when the process blocks (its wait is cut short to the deadline) and
 * at the time-slice check of its system calls. A pure compute loop never sees it. */
void alarm_check(double now) {
  struct proc *p = cur;
  if (!p->alarm_at || now < p->alarm_at) return;
  if (p->alarm_ival > 0) { while (p->alarm_at <= now) p->alarm_at += p->alarm_ival; }
  else p->alarm_at = 0;
  sig_send(p, SIGALRM, 0x80, 0, 0);
}
static void ms_to_tv(double ms, s64 *sec, long *usec) {
  if (ms < 0) ms = 0;
  *sec = (s64)(ms / 1000.0);
  *usec = (long)((ms - (double)*sec * 1000.0) * 1000.0);
}
static long sys_getitimer(int which, u32 uval) {
  if (which != 0) return -EINVAL; /* only ITIMER_REAL */
  struct k_itimerval v;
  memset(&v, 0, sizeof v);
  if (cur->alarm_at) ms_to_tv(cur->alarm_at - host_now(), &v.value_sec, &v.value_usec);
  ms_to_tv(cur->alarm_ival, &v.interval_sec, &v.interval_usec);
  return copy_to_user(uval, &v, sizeof v);
}
static long sys_setitimer(int which, u32 unew, u32 uold) {
  if (which != 0) return -EINVAL;
  struct k_itimerval v;
  if (copy_from_user(&v, unew, sizeof v)) return -EFAULT;
  if (v.value_sec < 0 || v.value_usec < 0 || v.value_usec >= 1000000 || v.interval_sec < 0 || v.interval_usec < 0 || v.interval_usec >= 1000000) return -EINVAL;
  if (uold) { long e = sys_getitimer(0, uold); if (e) return e; }
  double val = (double)v.value_sec * 1000.0 + v.value_usec / 1000.0;
  cur->alarm_ival = (double)v.interval_sec * 1000.0 + v.interval_usec / 1000.0;
  cur->alarm_at = val > 0 ? host_now() + val : 0;
  if (!cur->alarm_at) cur->alarm_ival = 0;
  return 0;
}

static long sys_rt_sigaction(int sig, u32 uact, u32 uold, u32 size) {
  if (size != 8) return -EINVAL;
  if (sig <= 0 || sig > 64) return -EINVAL;
  if (uact && (BIT(sig) & UNBLOCKABLE)) return -EINVAL;
  struct k_sigaction a;
  if (uact && copy_from_user(&a, uact, sizeof a)) return -EFAULT;
  if (uold && copy_to_user(uold, &cur->sa[sig], sizeof a)) return -EFAULT;
  if (!uact) return 0;
  a.mask &= ~UNBLOCKABLE;
  cur->sa[sig] = a;
  /* now ignored: a pending one is dropped */
  if (a.handler == 1 || (a.handler == 0 && (BIT(sig) & DFL_IGNORE))) cur->sig_pending &= ~BIT(sig);
  return 0;
}
/* After the mask changed: unblocked signals take their default action now; handlers are
 * run by the libc right after this call (it checks), or at the next call boundary. */
static void after_unblock(struct proc *p) {
  u64 ready = p->sig_pending & ~p->sig_blocked;
  for (int s = 1; ready && s < 65; s++) {
    if (!(ready & BIT(s))) continue;
    ready &= ~BIT(s);
    if (p->sa[s].handler == 0) { apply_default(p, s); if (p->state != P_RUN) return; }
  }
  if (deliverable(p)) sig_kick(p);
}
static long sys_rt_sigprocmask(int how, u32 uset, u32 uold, u32 size) {
  if (size != 8) return -EINVAL;
  u64 set = 0, old = cur->sig_blocked;
  if (uset && copy_from_user(&set, uset, 8)) return -EFAULT;
  if (uset) {
    if (how == 0) cur->sig_blocked |= set;          /* SIG_BLOCK */
    else if (how == 1) cur->sig_blocked &= ~set;    /* SIG_UNBLOCK */
    else if (how == 2) cur->sig_blocked = set;      /* SIG_SETMASK */
    else return -EINVAL;
    cur->sig_blocked &= ~UNBLOCKABLE;
  }
  if (uold && copy_to_user(uold, &old, 8)) { cur->sig_blocked = old; return -EFAULT; }
  after_unblock(cur);
  return cur->state == P_RUN ? 0 : -ERESTARTSYS;
}
/* Waits with the given mask until a handler has run; always fails with EINTR (from the
 * kick at the retry), and the old mask comes back then. */
static long sys_rt_sigsuspend(u32 uset, u32 size) {
  if (size != 8) return -EINVAL;
  if (!cur->in_sigsuspend) {
    u64 set;
    if (copy_from_user(&set, uset, 8)) return -EFAULT;
    cur->sig_saved_mask = cur->sig_blocked;
    cur->sig_blocked = set & ~UNBLOCKABLE;
    cur->in_sigsuspend = 1;
    after_unblock(cur);
    if (cur->state != P_RUN) return -ERESTARTSYS;
  }
  return block_nr(-1);
}

long sys_signal_call(long nr, long a, long b, long c, long d) {
  switch (nr) {
  case SYS_rt_sigaction: return sys_rt_sigaction(a, b, c, d);
  case SYS_rt_sigprocmask: return sys_rt_sigprocmask(a, b, c, d);
  case SYS_rt_sigpending: {
    if (b != 8) return -EINVAL;
    u64 m = cur->sig_pending & cur->sig_blocked;
    return copy_to_user(a, &m, 8);
  }
  case SYS_rt_sigsuspend: return sys_rt_sigsuspend(a, b);
  case SYS_tgkill: return (a <= 0 || b <= 0) ? -EINVAL : a != b ? -ESRCH : sys_kill2(b, c); /* one thread per process */
  case SYS_sigtake: return sys_sigtake(a);
  case SYS_setitimer: return sys_setitimer(a, b, c);
  case SYS_getitimer: return sys_getitimer(a, b);
  }
  return -ENOSYS;
}
