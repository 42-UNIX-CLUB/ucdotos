// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest threads, as green threads inside one engine process.
 *
 * The engine owns every guest register, so a thread is a saved register set
 * (arch->save/load). One runs at a time; the engine switches at a futex wait, at a
 * thread's exit, and when the instruction budget runs out (round robin, so a thread that
 * spins on a lock lets the others run). futex wait and wake are queues kept here. All
 * threads share the guest memory, descriptors and signal handlers; the signal mask is
 * per process (an approximation). With one host thread the guest's memory model never
 * comes into play: every access is sequentially consistent.
 *
 * A host system call that blocks (a read on an empty pipe) stops every guest thread:
 * the engine does not overlap it with other threads' work. */
#include "sys.h"
#include "host.h"

#define LX_MAXTHREADS 64

struct lx_thread {
  int used, tid, waiting;
  uint64_t wait_addr, wait_bits;
  int64_t deadline_ns;       /* 0: no deadline */
  uint64_t clear_tid;
  uint8_t regs[1024];
};
static struct lx_thread th[LX_MAXTHREADS];
static int cur = -1, nthreads = 1;
static int next_tid = 200000;

static int64_t now_ns(void) {
  int64_t s, ns;
  hb_clock_gettime(1, &s, &ns);
  return s * 1000000000 + ns;
}

static void ensure_init(void) {
  if (cur >= 0) return;
  cur = 0;
  th[0].used = 1;
  th[0].tid = (int)hb0(LX_SYS_getpid);
  th[0].clear_tid = lx_clear_tid;
}

int lx_thread_count(void) { return nthreads; }
int64_t lx_gettid(void) { return cur >= 0 ? th[cur].tid : hb0(LX_SYS_getpid); }

/* After a fork or an execve only the calling thread exists. */
void lx_threads_reset(void) {
  if (cur < 0) return;
  struct lx_thread keep = th[cur];
  memset(th, 0, sizeof th);
  th[0] = keep;
  th[0].tid = (int)hb0(LX_SYS_getpid);
  th[0].waiting = 0;
  cur = 0;
  nthreads = 1;
}

static void switch_to(int n) {
  if (n == cur) return;
  arch_cur->save(th[cur].regs);
  th[cur].clear_tid = lx_clear_tid;
  cur = n;
  arch_cur->load(th[cur].regs);
  lx_clear_tid = th[cur].clear_tid;
}

/* Pick the next thread that can run, waking waiters whose deadline passed. If none can,
 * sleep until the earliest deadline; with no deadline at all every thread waits for
 * another: the process hangs, as it would on Linux. */
static void schedule(void) {
  for (;;) {
    int64_t t = 0, earliest = 0;
    for (int k = 1; k <= LX_MAXTHREADS; k++) {
      int n = (cur + k) % LX_MAXTHREADS;
      struct lx_thread *x = &th[n];
      if (!x->used) continue;
      if (x->waiting && x->deadline_ns) {
        if (!t) t = now_ns();
        if (t >= x->deadline_ns) {
          x->waiting = 0;
          /* the wait returns -ETIMEDOUT: patch the saved result register */
          if (n == cur) arch_cur->sys_ret(-ETIMEDOUT);
          else { switch_to(n); arch_cur->sys_ret(-ETIMEDOUT); return; }
        } else if (!earliest || x->deadline_ns < earliest) earliest = x->deadline_ns;
      }
      if (!x->waiting) { switch_to(n); return; }
    }
    if (earliest) { int64_t d = earliest - now_ns(); if (d > 0) hb_nanosleep(d / 1000000000, d % 1000000000); }
    else hb_ppoll(0, 0, 100000000); /* deadlock: wait for a signal (natively) */
    if (sig_has_pending()) {
      /* let the current thread take the signal; its futex wait returns -EINTR */
      if (th[cur].waiting) { th[cur].waiting = 0; arch_cur->sys_ret(-EINTR); }
      return;
    }
  }
}

/* The budget ran out: give another runnable thread a turn. */
void lx_thread_yield(void) {
  if (nthreads < 2) return;
  for (int k = 1; k < LX_MAXTHREADS; k++) {
    int n = (cur + k) % LX_MAXTHREADS;
    if (th[n].used && !th[n].waiting) { switch_to(n); return; }
  }
}

/* clone with CLONE_VM | CLONE_THREAD. Called at the system call: the new thread starts
 * after the ecall with a0 = 0. */
int64_t lx_thread_create(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid) {
  ensure_init();
  int n = -1;
  for (int k = 0; k < LX_MAXTHREADS; k++) if (!th[k].used) { n = k; break; }
  if (n < 0) return -EAGAIN;
  int tid = next_tid++;
  /* the child: a copy of the caller's registers, returning 0 from clone */
  arch_cur->sys_ret(0);
  arch_cur->sys_skip();
  uint8_t save[1024];
  arch_cur->save(save);
  if (newsp) arch_cur->set_sp(newsp);
  if (flags & LX_CLONE_SETTLS) arch_cur->set_tp(tls);
  memset(&th[n], 0, sizeof th[n]);
  th[n].used = 1;
  th[n].tid = tid;
  th[n].clear_tid = (flags & LX_CLONE_CHILD_CLEARTID) ? ctid : 0;
  arch_cur->save(th[n].regs);
  arch_cur->load(save);
  /* back to the parent's registers, before its own result is written by the caller */
  arch_cur->set_pc(arch_cur->get_pc() - 4);
  if ((flags & LX_CLONE_PARENT_SETTID) && g_okrange(ptid, 4)) { uint32_t t = (uint32_t)tid; memcpy(g2h(ptid), &t, 4); }
  if ((flags & LX_CLONE_CHILD_SETTID) && g_okrange(ctid, 4)) { uint32_t t = (uint32_t)tid; memcpy(g2h(ctid), &t, 4); }
  nthreads++;
  return tid;
}

static int futex_wake(uint64_t addr, int n, uint64_t bits) {
  int woken = 0;
  for (int k = 0; k < LX_MAXTHREADS && woken < n; k++) {
    struct lx_thread *x = &th[k];
    if (x->used && x->waiting && x->wait_addr == addr && (x->wait_bits & bits)) {
      x->waiting = 0;
      woken++;
    }
  }
  return woken;
}

/* exit of one thread. Returns 1 if other threads remain (and one of them now runs). */
int lx_thread_exit(void) {
  ensure_init();
  if (nthreads < 2) return 0;
  uint64_t ct = lx_clear_tid;
  if (ct && g_okrange(ct, 4)) { memset(g2h(ct), 0, 4); futex_wake(ct, 1, ~0ull); }
  th[cur].used = 0;
  nthreads--;
  for (int k = 1; k <= LX_MAXTHREADS; k++) {
    int n = (cur + k) % LX_MAXTHREADS;
    if (th[n].used) {
      cur = n;                    /* the exited thread's registers are dropped */
      arch_cur->load(th[n].regs);
      lx_clear_tid = th[n].clear_tid;
      if (th[n].waiting) schedule();
      return 1;
    }
  }
  return 0;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3). Returns the result, or LX_SIG_DELIVERED
 * style NORET when the calling thread was put to sleep and another one now runs. */
int64_t lx_futex(uint64_t a[6], int *noret) {
  ensure_init();
  uint64_t addr = a[0];
  int op = (int)a[1] & 0x7f, rt = ((int)a[1] & 256) != 0;
  uint32_t val = (uint32_t)a[2];
  if (!g_okrange(addr, 4) || (addr & 3)) return -EFAULT;
  uint32_t cur_val;
  memcpy(&cur_val, g2h(addr), 4);
  switch (op) {
  case 0: case 9: { /* WAIT, WAIT_BITSET */
    uint64_t bits = op == 9 ? (uint32_t)a[5] : ~0ull;
    if (!bits) return -EINVAL;
    if (cur_val != val) return -EAGAIN;
    int64_t deadline = 0;
    if (a[3]) {
      if (!g_okrange(a[3], 16)) return -EFAULT;
      struct g_timespec ts;
      memcpy(&ts, g2h(a[3]), 16);
      if (op == 0) deadline = now_ns() + ts.tv_sec * 1000000000 + ts.tv_nsec;   /* relative */
      else {                                                                   /* absolute */
        int64_t s, ns;
        hb_clock_gettime(rt ? 0 : 1, &s, &ns);
        deadline = now_ns() + (ts.tv_sec - s) * 1000000000 + (ts.tv_nsec - ns);
      }
      if (deadline <= now_ns()) return -ETIMEDOUT;
    }
    if (nthreads < 2 && !deadline) return -EAGAIN; /* nobody can ever wake it */
    /* sleep: the result (0 for a wake) is written now; a timeout patches it */
    arch_cur->sys_ret(0);
    arch_cur->sys_skip();
    *noret = 1;
    th[cur].waiting = 1;
    th[cur].wait_addr = addr;
    th[cur].wait_bits = bits;
    th[cur].deadline_ns = deadline;
    schedule();
    return 0;
  }
  case 1: case 10: /* WAKE, WAKE_BITSET */
    return futex_wake(addr, (int)val, op == 10 ? (uint32_t)a[5] : ~0ull);
  case 3: case 4: { /* REQUEUE, CMP_REQUEUE */
    if (op == 4 && cur_val != (uint32_t)a[5]) return -EAGAIN;
    int woken = futex_wake(addr, (int)val, ~0ull);
    int moved = 0;
    for (int k = 0; k < LX_MAXTHREADS && moved < (int)(uint32_t)a[3]; k++)
      if (th[k].used && th[k].waiting && th[k].wait_addr == addr) { th[k].wait_addr = a[4]; moved++; }
    return woken + moved;
  }
  case 5: { /* WAKE_OP: *uaddr2 = op(*uaddr2, oparg); wake val on uaddr; if cmp, wake val2 on uaddr2 */
    if (!g_okrange(a[4], 4)) return -EFAULT;
    uint32_t e = (uint32_t)a[5], old;
    memcpy(&old, g2h(a[4]), 4);
    int wop = (e >> 28) & 7, cmp = (e >> 24) & 15;
    uint32_t oparg = (e >> 12) & 0xfff, cmparg = e & 0xfff;
    if ((e >> 31) & 1) oparg = 1u << (oparg & 31);
    uint32_t nv = wop == 0 ? oparg : wop == 1 ? old + oparg : wop == 2 ? old | oparg : wop == 3 ? old & ~oparg : old ^ oparg;
    memcpy(g2h(a[4]), &nv, 4);
    int r = futex_wake(addr, (int)val, ~0ull);
    int c = cmp == 0 ? old == cmparg : cmp == 1 ? old != cmparg : cmp == 2 ? old < cmparg : cmp == 3 ? old <= cmparg : cmp == 4 ? old > cmparg : old >= cmparg;
    if (c) r += futex_wake(a[4], (int)(uint32_t)a[3], ~0ull);
    return r;
  }
  case 6: case 7: case 8: return -ENOSYS; /* priority-inheritance locks */
  }
  return -ENOSYS;
}
