// SPDX-License-Identifier: GPL-2.0-or-later
/* The Linux system call layer, part 2: signals.
 *
 * The engine owns the guest's signal state: handlers, the mask, the alternate stack and
 * the pending set. Signals are delivered between guest instructions, at the points where
 * the CPU returns to the engine (every system call, every fault, and every time the
 * instruction budget runs out), by building the architecture's signal frame on the guest
 * stack (arch->sig_setup). Sources today: the guest itself (raise, abort, kill of its own
 * pid, a fault, a write to a broken pipe) and, natively, the host's signals. UC.OS has no
 * signals yet; lx_sig_pending is the entry point an asynchronous source will use. */
#include "sys.h"
#include "host.h"

volatile uint64_t lx_sig_pending;
static struct g_sigaction sact[LX_NSIG + 1];
static uint64_t sigmask;
uint64_t lx_altstack_sp, lx_altstack_size;
int lx_altstack_flags = LX_SS_DISABLE;

#define BIT(s) (1ull << ((s) - 1))
#define UNBLOCKABLE (BIT(LX_SIGKILL) | BIT(LX_SIGSTOP))

uint64_t sig_get_mask(void) { return sigmask; }
void sig_set_mask(uint64_t m) { sigmask = m & ~UNBLOCKABLE; }
void sig_raise(int sig) { if (sig >= 1 && sig <= LX_NSIG) lx_sig_pending |= BIT(sig); }

void sig_exec_reset(void) {
  for (int s = 1; s <= LX_NSIG; s++)
    if (sact[s].handler > 1) { sact[s].handler = 0; sact[s].flags = 0; sact[s].mask = 0; }
  lx_altstack_sp = lx_altstack_size = 0;
  lx_altstack_flags = LX_SS_DISABLE;
}

static int default_ignored(int sig) {
  return sig == LX_SIGCHLD || sig == LX_SIGCONT || sig == 23 /* SIGURG */ || sig == LX_SIGWINCH ||
         sig == LX_SIGSTOP || sig == LX_SIGTSTP || sig == LX_SIGTTIN || sig == LX_SIGTTOU; /* no job control: stops are ignored */
}

int lx_child_exit(int code, int sig);
/* Returns only when this is an emulated fork child: the parent has been restored. */
void lx_exit_signal(int sig) {
  if (lx_child_exit(0, sig)) return;
  mem_reset();
  if (lx_cfg.stats) lx_print_stats();
  hb_die(sig);
}

static void merge_host(void) {
  if (hb_pending_signal) {
    int s = hb_pending_signal;
    hb_pending_signal = 0;
    for (int i = 1; i < 32; i++) if ((unsigned)s & (1u << i)) lx_sig_pending |= BIT(i);
  }
}

int sig_has_pending(void) { merge_host(); return (lx_sig_pending & ~sigmask) != 0; }

int sig_restart_wanted(void) {
  merge_host();
  uint64_t p = lx_sig_pending & ~sigmask;
  if (!p) return 0;
  int sig = __builtin_ctzll(p) + 1;
  uint64_t h = sact[sig].handler;
  if (h == 1 || (h == 0 && default_ignored(sig))) return 1;
  return h > 1 && (sact[sig].flags & LX_SA_RESTART);
}

static void deliver(int sig, const struct lx_fault *info, int code) {
  struct g_sigaction *a = &sact[sig];
  uint64_t sp = arch_cur->get_sp();
  if ((a->flags & LX_SA_ONSTACK) && !(lx_altstack_flags & LX_SS_DISABLE) &&
      !(sp > lx_altstack_sp && sp <= lx_altstack_sp + lx_altstack_size))
    sp = lx_altstack_sp + lx_altstack_size;
  uint64_t old = sigmask;
  if (arch_cur->sig_setup(sig, a->handler, a->flags, 0, old, sp, info, code) < 0) { lx_exit_signal(LX_SIGSEGV); return; }
  sigmask |= a->mask;
  if (!(a->flags & LX_SA_NODEFER)) sigmask |= BIT(sig);
  sigmask &= ~UNBLOCKABLE;
  if (a->flags & LX_SA_RESETHAND) { a->handler = 0; a->flags = 0; }
}

void lx_signals(void) {
  merge_host();
  for (;;) {
    uint64_t p = lx_sig_pending & ~sigmask;
    if (!p) return;
    int sig = __builtin_ctzll(p) + 1;
    lx_sig_pending &= ~BIT(sig);
    uint64_t h = sact[sig].handler;
    if (h == 1) continue;
    if (h == 0) {
      if (default_ignored(sig)) continue;
      lx_exit_signal(sig);
      return;
    }
    struct lx_fault info = {0, (uint64_t)hb0(LX_SYS_getpid), 0};
    deliver(sig, &info, 0 /* SI_USER */);
  }
}

/* A fault the CPU raised: deliver now, or terminate if it is blocked or not handled
 * (Linux does the same for synchronous signals). */
void lx_trap(int stop, const struct lx_fault *f) {
  int sig, code = 1;
  switch (stop) {
  case LX_STOP_FAULT: sig = LX_SIGSEGV; break;
  case LX_STOP_MISALIGNED: sig = LX_SIGBUS; break;
  case LX_STOP_BREAK: sig = LX_SIGTRAP; break;
  default: sig = LX_SIGILL; break;
  }
  if (lx_cfg.trace || getenv("LX_DEBUG"))
    fprintf(stderr, "%s: signal %d at pc 0x%llx (address 0x%llx)\n", LX_NAME, sig, (unsigned long long)f->pc, (unsigned long long)f->addr);
  uint64_t h = sact[sig].handler;
  if ((sigmask & BIT(sig)) || h <= 1) { lx_exit_signal(sig); return; }
  deliver(sig, f, code);
}

int64_t sys_rt_sigaction(uint64_t a[6]) {
  int sig = (int)a[0];
  if (sig < 1 || sig > LX_NSIG || a[3] != 8) return -EINVAL;
  if (a[2]) {
    if (!g_okrange(a[2], sizeof(struct g_sigaction))) return -EFAULT;
    memcpy(g2h(a[2]), &sact[sig], sizeof(struct g_sigaction));
  }
  if (a[1]) {
    if (sig == LX_SIGKILL || sig == LX_SIGSTOP) return -EINVAL;
    if (!g_okrange(a[1], sizeof(struct g_sigaction))) return -EFAULT;
    memcpy(&sact[sig], g2h(a[1]), sizeof(struct g_sigaction));
    if (sact[sig].handler == 1 || (sact[sig].handler == 0 && default_ignored(sig))) lx_sig_pending &= ~BIT(sig);
  }
  return 0;
}

int64_t sys_rt_sigprocmask(uint64_t a[6]) {
  if (a[3] != 8) return -EINVAL;
  uint64_t old = sigmask;
  if (a[1]) {
    if (!g_okrange(a[1], 8)) return -EFAULT;
    uint64_t s;
    memcpy(&s, g2h(a[1]), 8);
    switch (a[0]) {
    case 0: sigmask |= s; break;
    case 1: sigmask &= ~s; break;
    case 2: sigmask = s; break;
    default: return -EINVAL;
    }
    sigmask &= ~UNBLOCKABLE;
  }
  if (a[2]) {
    if (!g_okrange(a[2], 8)) return -EFAULT;
    memcpy(g2h(a[2]), &old, 8);
  }
  return 0;
}

int64_t sys_sigaltstack(uint64_t a[6]) {
  uint64_t sp = arch_cur->get_sp();
  int on = !(lx_altstack_flags & LX_SS_DISABLE) && sp > lx_altstack_sp && sp <= lx_altstack_sp + lx_altstack_size;
  if (a[1]) {
    if (!g_okrange(a[1], sizeof(struct g_stack))) return -EFAULT;
    struct g_stack o = {lx_altstack_sp, (lx_altstack_flags & LX_SS_DISABLE) ? LX_SS_DISABLE : (on ? LX_SS_ONSTACK : 0), 0, lx_altstack_size};
    memcpy(g2h(a[1]), &o, sizeof o);
  }
  if (a[0]) {
    if (on) return -EPERM;
    if (!g_okrange(a[0], sizeof(struct g_stack))) return -EFAULT;
    struct g_stack n;
    memcpy(&n, g2h(a[0]), sizeof n);
    if (n.flags & LX_SS_DISABLE) { lx_altstack_flags = LX_SS_DISABLE; lx_altstack_sp = lx_altstack_size = 0; return 0; }
    if (n.flags & ~(LX_SS_DISABLE | (int)0x80000000)) return -EINVAL;
    if (n.size < 2048) return -ENOMEM;
    lx_altstack_sp = n.sp; lx_altstack_size = n.size; lx_altstack_flags = 0;
  }
  return 0;
}

int64_t sys_rt_sigreturn(void) {
  uint64_t m;
  if (arch_cur->sig_return(&m) < 0) { lx_exit_signal(LX_SIGSEGV); return 0; }
  sigmask = m & ~UNBLOCKABLE;
  return 0;
}

int64_t sys_kill(uint64_t a[6], int nr) {
  int64_t self = hb0(LX_SYS_getpid);
  int64_t pid = nr == LX_SYS_kill ? (int64_t)(int32_t)a[0] : nr == LX_SYS_tgkill ? (int64_t)(int32_t)a[1] : (int64_t)(int32_t)a[0];
  int sig = (int)(nr == LX_SYS_tgkill ? a[2] : a[1]);
  if (sig < 0 || sig > LX_NSIG) return -EINVAL;
  if (pid == self || (nr != LX_SYS_kill && pid == lx_gettid()) || (nr == LX_SYS_kill && (pid == 0 || pid == -1 || pid == -hb1(LX_SYS_getpgid, 0)))) {
    if (sig) sig_raise(sig);
    if (nr == LX_SYS_kill && pid != self && pid != 0) {
      /* a group or everyone: also tell the host (the other members are host processes) */
      hb2(LX_SYS_kill, pid, sig);
    }
    return 0;
  }
  return hb2(LX_SYS_kill, pid, sig);
}

int64_t sys_rt_sigsuspend(uint64_t a[6]) {
  if (a[1] != 8) return -EINVAL;
  if (!g_okrange(a[0], 8)) return -EFAULT;
  uint64_t m, old = sigmask;
  memcpy(&m, g2h(a[0]), 8);
  sigmask = m & ~UNBLOCKABLE;
  while (!sig_has_pending()) {
    /* Wait for the host to deliver something. Natively ppoll with no descriptors returns
       on a host signal; on UC.OS nothing can arrive yet, so this sleeps in slices. */
    int64_t r = hb_ppoll(0, 0, 50000000);
    (void)r;
  }
  /* Deliver with the temporary mask in force; the frame records the old mask. */
  uint64_t p = lx_sig_pending & ~sigmask;
  int sig = __builtin_ctzll(p) + 1;
  lx_sig_pending &= ~BIT(sig);
  uint64_t h = sact[sig].handler;
  if (h == 0 && !default_ignored(sig)) { lx_exit_signal(sig); return LX_SIG_DELIVERED; }
  if (h > 1) {
    /* The call returns -EINTR, then the handler runs; sigreturn restores the old mask. */
    arch_cur->sys_ret(-EINTR);
    arch_cur->sys_skip();
    sigmask = old;
    struct lx_fault info = {0, 0, 0};
    uint64_t keep = sact[sig].mask;
    sact[sig].mask |= m; /* the handler runs with at least the suspend mask */
    deliver(sig, &info, 0);
    sact[sig].mask = keep;
    return LX_SIG_DELIVERED;
  }
  sigmask = old;
  return -EINTR;
}

int64_t sys_rt_sigpending(uint64_t a[6]) {
  if (!g_okrange(a[0], 8)) return -EFAULT;
  merge_host();
  uint64_t p = lx_sig_pending & sigmask;
  memcpy(g2h(a[0]), &p, 8);
  return 0;
}

/* The emulated fork saves and restores the handlers around a child. */
void *sig_snapshot(void) {
  void *p = malloc(sizeof sact + 8 * 3);
  if (!p) return 0;
  memcpy(p, sact, sizeof sact);
  uint64_t extra[3] = {lx_altstack_sp, lx_altstack_size, (uint64_t)lx_altstack_flags};
  memcpy((char *)p + sizeof sact, extra, sizeof extra);
  return p;
}
void sig_restore(void *p) {
  memcpy(sact, p, sizeof sact);
  uint64_t extra[3];
  memcpy(extra, (char *)p + sizeof sact, sizeof extra);
  lx_altstack_sp = extra[0]; lx_altstack_size = extra[1]; lx_altstack_flags = (int)extra[2];
}
