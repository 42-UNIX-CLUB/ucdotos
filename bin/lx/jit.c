// SPDX-License-Identifier: GPL-2.0-or-later
/* The engine's side of the binary translator (libjit/jitcore.c): the switch, what the
 * translator needs from the engine (system calls, yield, code fetch), and running
 * translated code from the main loop. See docs/BINARY-TRANSLATION.md section 7.
 *
 * The switch: --jit=off|on|lockstep, or LX_JIT=off|on|lockstep (the option wins).
 *   off       interpreter only (the default)
 *   on        hot code is translated to WebAssembly; the interpreter runs the rest
 *   lockstep  testing: every translated region also runs under a reference interpreter
 *             and the two results are compared
 * Translator options: --jit-opts='key=value ...' or LX_JIT_OPTS (see jitcore_options).
 * Translation needs the host to hand over its loader (boot/jit-proto/ext.mjs); without it
 * the engine interprets, and says so only with verbose=1. */
#include "lx.h"
#include "rv.h"
#include "arch.h"
#include "host.h"
#include "sys.h"
#include "../../libjit/jitcore.h"

#ifdef __wasm__

extern struct rv_cpu rv_cpu;
int lx_jit_on, lx_jit_threshold = 50;
void rv_jit_mark(uint64_t pc, int v);

void lx_jit_setup(const char *mode, const char *opts) {
  if (!mode) mode = getenv("LX_JIT");
  if (!opts) opts = getenv("LX_JIT_OPTS");
  int m = !mode || !strcmp(mode, "off") ? JX_OFF : !strcmp(mode, "lockstep") ? JX_LOCKSTEP : !strcmp(mode, "on") ? JX_ON : -1;
  if (m < 0) lx_die("--jit: off, on or lockstep, not %s", mode);
  if (opts && jitcore_options(opts) < 0) lx_die("--jit-opts: unknown option in %s", opts);
  if (m == JX_OFF) return;
  jitcore_mode = m;
  struct jitcore_layout l = {
    .x = (uint32_t)(uintptr_t)&rv_cpu.x[0], .f = (uint32_t)(uintptr_t)&rv_cpu.f[0], .pc = (uint32_t)(uintptr_t)&rv_cpu.pc,
    .fcsr = (uint32_t)(uintptr_t)&rv_cpu.fcsr, .res = (uint32_t)(uintptr_t)&rv_cpu.res_addr,
    .gbase = (uint32_t)(uintptr_t)g_mem, .gsize = g_size, .glo = g_lo, .gspan = g_span,
  };
  if (jitcore_init(&l) == 0) { lx_jit_on = 1; lx_jit_threshold = jitcore_threshold(); }
}

void lx_jit_flush(void) { if (lx_jit_on) jitcore_flush(); }
void lx_jit_frm(int frm) { if (lx_jit_on) jitcore_frm_changed(frm); }
/* A forked child (or a migrated process): its table has none of the parent's code. Its
 * decoded blocks still carry the parent's "translated" marks and heat, which would make it
 * translate every region the parent had at once, although most children exec almost at
 * once. Drop the blocks too: the child translates only what gets hot in the child. */
void rv_flush(uint64_t addr, uint64_t len);
void lx_jit_after_copy(void) {
  if (!lx_jit_on) return;
  jitcore_after_copy();
  rv_flush(0, ~0ull);
  if (jitcore_mode == JX_OFF) lx_jit_on = 0;
}
void lx_jit_report(void) { if (lx_jit_on || jitcore_verbose) jitcore_report(); }

/* The main loop got LX_STOP_JIT: the block at pc is hot or has a translation. */
void lx_jit_run(void) {
  uint64_t pc = rv_cpu.pc;
  if (!jitcore_ready(pc) && !jitcore_translate(pc)) { rv_jit_mark(pc, -1); return; }
  rv_jit_mark(pc, 1);
  uint64_t fa = 0;
  int r = jitcore_enter(&fa);
  if (r == JX_FAULT) {
    struct lx_fault f = { rv_cpu.pc, fa, 1 };
    uint32_t raw = 0;
    if (jitcore_host_fetch(rv_cpu.pc, &raw)) { struct rv_insn in; rv_decode(raw, &in); if (rv_op_flags[in.op] & RVF_STORE) f.kind = 2; }
    lx_trap(LX_STOP_FAULT, &f);
  } else if (r == JX_YIELD) lx_thread_yield();
}

/* ---- what the translator needs ---- */
int jitcore_host_ecall(void) {
  uint64_t pc = rv_cpu.pc;            /* translated code stored the ecall's pc and every register */
  lx_syscall();
  if (lx_sig_pending || hb_pending_signal) lx_signals();
  return rv_cpu.pc != pc + 4;         /* signal, sigreturn, execve, another thread: leave */
}
int jitcore_host_yield(void) {
  hb_tick();
  return lx_thread_count() > 1 || lx_sig_pending || hb_pending_signal;
}
int jitcore_host_fetch(uint64_t pc, uint32_t *raw) {
  if (pc < g_lo || pc + 4 > g_size - LX_PAGE) return 0;
  memcpy(raw, g_mem + pc, 4);
  return 1;
}
void jitcore_host_log(const char *s) {   /* the engine's own copy of stderr: guests may close fd 2 */
  extern int lx_errfd;
  char buf[512];
  int n = snprintf(buf, sizeof buf, "%s: %s\n", LX_NAME, s);
  hb3(LX_SYS_write, lx_errfd, hbp(buf), n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

#else
/* Native builds (the engine's own test suite) cannot run WebAssembly: interpreter only. */
int lx_jit_on, lx_jit_threshold = 50;
void lx_jit_setup(const char *mode, const char *opts) {
  if (!mode) mode = getenv("LX_JIT");
  (void)opts;
  if (mode && strcmp(mode, "off")) lx_log("the translator needs WebAssembly: interpreting");
}
void lx_jit_flush(void) {}
void lx_jit_frm(int frm) { (void)frm; }
void lx_jit_after_copy(void) {}
void lx_jit_report(void) {}
void lx_jit_run(void) {}
#endif
