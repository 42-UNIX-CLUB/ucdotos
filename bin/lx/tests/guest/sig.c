// SPDX-License-Identifier: GPL-2.0-or-later
/* Signals raised by the guest itself: raise() through tgkill, a SIGSEGV handler that
 * fixes the faulting context and returns, SIGILL, masks, sigaltstack, SA_RESETHAND, and
 * the riscv64 signal frame (registers and the ucontext pc). riscv64 only. */
#include "rt.h"

static long sys(long n, long a, long b, long c, long d) { return rt_sys(n, a, b, c, d, 0, 0); }
struct ksa { void *handler; unsigned long flags; unsigned long mask; };
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESETHAND 0x80000000
#define SA_NODEFER 0x40000000
static int pass, fail;
static void check(const char *name, uint64_t got, uint64_t want) {
  if (got == want) { pass++; return; }
  fail++;
  rt_puts("FAIL "); rt_puts(name); rt_puts(": got "); rt_hex(got); rt_puts(" want "); rt_hex(want); rt_puts("\n");
}
static int sigaction_(int sig, void *h, unsigned long flags) {
  struct ksa a = {h, flags, 0};
  return (int)sys(134, sig, (long)&a, 0, 8);
}
static long getpid_(void) { return sys(172, 0, 0, 0, 0); }
static void raise_(int sig) { sys(131, getpid_(), getpid_(), sig, 0); }

static volatile int got_usr1, got_sig, nested;
static volatile uint64_t fault_addr, handler_sp;
static void on_usr1(int s) { got_usr1 = s; }
/* ucontext: uc_mcontext at offset 176; sc_regs[0] is the pc */
static void on_segv(int s, void *info, void *uc) {
  got_sig = s;
  fault_addr = *(uint64_t *)((char *)info + 16);
  uint64_t *pc = (uint64_t *)((char *)uc + 176);
  *pc += 4; /* skip the faulting 4-byte load */
}
static void on_ill(int s, void *info, void *uc) { (void)info; got_sig = s; *(uint64_t *)((char *)uc + 176) += 4; }
static void on_alt(int s) { volatile int here; handler_sp = (uint64_t)&here; got_sig = s; }
static void on_nest(int s) { nested++; if (nested == 1) raise_(10); (void)s; }

int main(void) {
  sigaction_(10, (void *)on_usr1, 0);
  raise_(10);
  check("raise SIGUSR1", (uint64_t)got_usr1, 10);

  /* blocked signal stays pending until unblocked */
  got_usr1 = 0;
  unsigned long set = 1ul << 9, old = 0;
  sys(135, 0, (long)&set, (long)&old, 8);    /* block SIGUSR1 */
  raise_(10);
  check("blocked: not delivered", (uint64_t)got_usr1, 0);
  unsigned long pend = 0;
  sys(136, (long)&pend, 8, 0, 0);
  check("rt_sigpending", pend, 1ul << 9);
  sys(135, 1, (long)&set, 0, 8);             /* unblock */
  check("delivered at unblock", (uint64_t)got_usr1, 10);

  /* SIGSEGV: the handler moves the pc past the faulting instruction */
  sigaction_(11, (void *)on_segv, SA_SIGINFO);
  volatile uint64_t *bad = (volatile uint64_t *)8;
  uint64_t x;
  __asm__ volatile(".option push\n.option norvc\nld %0, 0(%1)\n.option pop" : "=r"(x) : "r"(bad) : "memory");
  (void)x;
  check("SIGSEGV handled", (uint64_t)got_sig, 11);
  check("si_addr", fault_addr, 8);

  /* SIGILL from an illegal instruction */
  got_sig = 0;
  sigaction_(4, (void *)on_ill, SA_SIGINFO);
  __asm__ volatile(".4byte 0xffffffff");
  check("SIGILL handled", (uint64_t)got_sig, 4);

  /* sigaltstack */
  static char alt[16384] __attribute__((aligned(16)));
  struct { void *sp; int flags; int pad; unsigned long size; } ss = {alt, 0, 0, sizeof alt};
  check("sigaltstack", (uint64_t)sys(132, (long)&ss, 0, 0, 0), 0);
  sigaction_(12, (void *)on_alt, SA_ONSTACK);
  got_sig = 0;
  raise_(12);
  check("handler ran", (uint64_t)got_sig, 12);
  check("handler on the alternate stack", handler_sp >= (uint64_t)alt && handler_sp < (uint64_t)alt + sizeof alt, 1);

  /* SA_RESETHAND: second raise uses the default action (ignore for SIGCHLD) */
  got_sig = 0;
  sigaction_(17, (void *)on_alt, SA_RESETHAND);
  raise_(17); check("resethand first", (uint64_t)got_sig, 17);
  got_sig = 0; raise_(17); check("resethand second ignored", (uint64_t)got_sig, 0);

  /* without SA_NODEFER a signal raised in its own handler waits until the handler returns */
  sigaction_(10, (void *)on_nest, 0);
  raise_(10);
  check("deferred nesting", (uint64_t)nested, 2);

  rt_puts("sig: "); rt_dec(pass + fail); rt_puts(" checks, "); rt_dec(fail); rt_puts(" failed\n");
  return fail != 0;
}
