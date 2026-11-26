// SPDX-License-Identifier: GPL-2.0-or-later
// The translator: guest machine code to WebAssembly.
//
// Split in two halves:
//   xlat.c   architecture-neutral: finds the region's blocks, builds the control flow
//            graph, structures it into wasm block/loop/if/br, keeps guest registers in wasm
//            locals, emits exits, calls, budget checks and the module around it all;
//   arch     one file per guest architecture (rvsem.c for RISC-V): decoding and the
//            semantics of each instruction, behind the functions declared at the end.
#ifndef JIT_XLAT_H
#define JIT_XLAT_H
#include <stdint.h>
#include "wasm.h"
#include "rv.h"

// Why translated code returned to the engine.
enum {
  R_JUMP = 0,     // continue at cpu->pc (branch out of the region, indirect jump, budget)
  R_RET = 1,      // a guest return; continue at cpu->pc
  R_INTERP = 2,   // interpret the instruction at cpu->pc (the translator does not handle it)
  R_FAULT = 3,    // memory fault at cpu->pc, address in cpu->fault_addr
  R_HALT = 4,     // the guest exited
  R_STUB = 5,     // returned by the stub in a table slot that has no translation yet
  R_EBREAK = 6,
};

// How control flow inside a region becomes wasm.
enum { S_BLOCK = 0,      // one region per basic block, the engine dispatches between them
       S_DISPATCH = 1,   // all blocks in one function: loop + br_table over block numbers
       S_STRUCT = 2 };   // structured (loops and blocks from the dominator tree); dispatch if irreducible
enum { B_UNCHECKED = 0,  // wrap to 32 bits, add the guest base; outside the guest's memory traps or aliases
       B_MASKED = 1,     // also AND with (size - 1): never leaves the guest's memory, faults are not reported
       B_CHECKED = 2 };  // compare with the size; a fault leaves with R_FAULT and the exact state

typedef struct {
  int strategy;          // S_*
  int calls;             // guest calls become wasm calls (with a return check) instead of exits
  int bounds;            // B_*
  int budget;            // decrement a counter at back edges and function entries
  int count;             // count retired instructions in cpu->icount (lockstep mode)
  int ecall_exits;       // ecall leaves the region instead of calling the helper (lockstep mode)
  int max_insns, max_blocks;
  int indirect;          // 0: indirect jumps and calls leave; 1: look up and call_indirect in place
  int chain;             // leave a region by a tail call into the next one (return_call_indirect) when it is translated
  int learn;             // record indirect jump targets and make known ones in-region branches
  int abi;               // trust the RISC-V calling convention: callees keep sp, gp, tp, s0-s11, fs0-fs11 (deviation)
  int split;             // indirect calls: one call_indirect per learned target (each always goes to one place)
  int ic;                // a one-entry inline cache (target, slot) per indirect jump or call site
} xlat_opts;

// What the translator needs to know about the engine's memory layout.
// Absolute addresses (in the engine's memory) of every piece of guest state the generated
// code touches, so the translator can follow any engine's CPU state layout.
typedef struct {
  uint32_t a_x, a_f;     // x[0] and f[0]; registers are 8 bytes apart
  uint32_t a_pc, a_fcsr, a_res;
  uint32_t a_budget, a_icount, a_fault, a_site;   // translator bookkeeping (i32, i64, i64, i64)
  uint32_t gbase;        // address of guest byte 0
  uint64_t gsize;        // guest memory size (power of two for B_MASKED)
  uint64_t glo, gspan;   // checked mode: valid when a - glo <= gspan (gspan 0: a <= gsize - size)
} xlat_env;

// The engine answers these while a region is translated.
uint32_t engine_slot_for(uint64_t pc);   // table slot that runs the region at pc (stub if none yet)
int engine_readable(uint64_t pc, uint32_t *raw);  // fetch 4 bytes of guest code; 0 if outside
// Targets seen so far at the indirect jump at `site` (learned at run time); the translator
// makes them ordinary successors. Returns how many (at most cap).
int engine_site_targets(uint64_t site, uint64_t *out, int cap);
void engine_site_in_region(uint64_t site);
// For a translated leaf region at pc: the registers it reads and writes. 0 if not known.
int engine_callee_summary(uint64_t pc, uint64_t *use, uint64_t *def);       // the region being translated contains this site

// One module with up to XL_MAXFUNCS regions. Calls between regions in the same module are
// direct wasm calls; to other regions they go through the engine's table.
#define XL_MAXFUNCS 4096
typedef struct xmodule {
  wbuf code;            // function bodies, each with its size prefix
  uint64_t entry[XL_MAXFUNCS];
  uint32_t slot[XL_MAXFUNCS];
  int nfunc;
  uint32_t helpers_used;
  uint32_t nglobals;     // inline cache globals: (i64 target, i32 slot) pairs
  uint8_t leaf[XL_MAXFUNCS];          // region i makes no calls and leaves only by returning
  uint64_t fuse[XL_MAXFUNCS], fdef[XL_MAXFUNCS];   // registers it reads, writes
  // statistics of the last translate
  int ninsn_total, nblocks_total, irreducible, fallback_dispatch;
} xmodule;

void xl_module_init(xmodule *m);
void xl_module_free(xmodule *m);
// Plan a region for every entry first (so that calls between them can be direct), then emit.
int xl_add_entry(xmodule *m, uint64_t pc, uint32_t slot);
int xl_translate_all(xmodule *m, const xlat_env *env, const xlat_opts *o);
// The finished module: imports e.memory, e.__indirect_function_table and the e.jit_* helpers;
// an active element segment puts function i into table slot m->slot[i].
void xl_module_bytes(xmodule *m, wbuf *out);
// The support module the engine instantiates once: grows the table (filled with the stub)
// and resets one slot to the stub. Writes its two table slots to memory at `out_addr`.
void xl_support_module(wbuf *out, uint32_t out_addr);

// ---- the architecture half (rvsem.c) ----
enum { FL_NONE, FL_BRANCH, FL_JUMP, FL_CALL, FL_INDJUMP, FL_RET, FL_INDCALL, FL_STOP };
typedef struct { uint64_t pc; uint64_t target; rv_insn d; uint8_t flow; } xinsn;
struct xgen;
int arch_decode(uint64_t pc, xinsn *out, const xlat_opts *o);   // 0 if it cannot be fetched or decoded
void arch_regs(const xinsn *in, uint64_t *use, uint64_t *def); // bit r: x r; bit 32+r: f r
void arch_emit(struct xgen *g, const xinsn *in);                 // body of a FL_NONE instruction
void arch_emit_cond(struct xgen *g, const xinsn *in);            // i32 condition of a FL_BRANCH
void arch_emit_link(struct xgen *g, const xinsn *in);            // write the link register (calls, jal, jalr)
void arch_emit_indirect_target(struct xgen *g, const xinsn *in); // i64 target of FL_INDJUMP/RET/INDCALL
void arch_emit_helpers(wbuf *code, uint32_t used);               // bodies of module-local helper functions
int arch_helper_count(uint32_t used);
#endif
