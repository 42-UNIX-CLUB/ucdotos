// SPDX-License-Identifier: GPL-2.0-or-later
// The translator as a component of an engine (the core engine bin/lx links it). Plain C
// types only, so the engine's own headers and the translator's never meet in one file.
//
// The engine interprets; its interpreter calls jitcore_hot() when a block is hot and stops
// at blocks that have a translation; the engine's main loop then calls jitcore_enter(),
// which runs translated code (chained from region to region) until it reaches code that
// is not translated, a fault, or a yield point.
#ifndef JIT_CORE_H
#define JIT_CORE_H
#include <stdint.h>

enum { JX_OFF = 0, JX_ON = 1, JX_LOCKSTEP = 2 };       // the switch
enum { JX_INTERP = 0, JX_FAULT = 1, JX_YIELD = 2 };     // why jitcore_enter returned

// Where the engine keeps the guest state (absolute addresses in this wasm memory) and the
// guest window: guest address a is at gbase + a, valid when a - glo <= gspan.
struct jitcore_layout {
  uint32_t x, f, pc, fcsr, res;   // x[0], f[0], pc, fcsr, LR/SC reservation address
  uint32_t gbase;
  uint64_t gsize, glo, gspan;
};

extern int jitcore_mode;          // JX_*; the engine sets it from its option or environment
extern int jitcore_verbose;

// Parse "key=value ..." translator options (strategy, bounds, threshold, budget, calls,
// chain, learn, ic, abi, fp, cache (KiB), maxblocks, maxinsns, verbose). Returns 0, or -1 for an unknown key.
int jitcore_options(const char *s);
// After the guest window exists. Returns 0, or -1 when the host cannot load generated
// code (no extension): the engine then keeps interpreting.
int jitcore_init(const struct jitcore_layout *l);
int jitcore_threshold(void);
// A block at pc got hot: translate the region that starts there. 1 if it has code now.
int jitcore_translate(uint64_t pc);
int jitcore_ready(uint64_t pc);
// Run translated code from the guest pc. *fault_addr is set for JX_FAULT.
int jitcore_enter(uint64_t *fault_addr);
// Drop every translation (fence.i, flush_icache, munmap/mprotect of code, execve).
void jitcore_flush(void);
// The guest wrote frm: code translated for the old rounding mode is dropped.
void jitcore_frm_changed(int frm);
// This instance is a copy (fork through the kernel's clone, or a migration): its function
// table is new and no generated module exists. Re-create lazily.
void jitcore_after_copy(void);
// The loader the host hands over: a slot in this program's table (see boot/jit-proto/ext.mjs).
void jitcore_attach(int slot);
void jitcore_report(void);        // statistics on stderr (verbose)

// What the engine provides.
int jitcore_host_ecall(void);     // registers and pc are in memory: 0 continue after the ecall, 1 state replaced
int jitcore_host_yield(void);     // budget expired: 0 continue in place, 1 leave to the engine
int jitcore_host_fetch(uint64_t pc, uint32_t *raw);   // 4 bytes of guest code, 0 if not fetchable
void jitcore_host_log(const char *s);
#endif
