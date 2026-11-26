// SPDX-License-Identifier: GPL-2.0-or-later
// Internal to the translator: the code generation state shared by xlat.c and the
// architecture file. Not part of the engine's interface.
#ifndef JIT_XGEN_H
#define JIT_XGEN_H
#include "xlat.h"

// wasm locals of every region function
enum {
  L_DEPTH = 0,                // parameter: call depth
  L_X = 0,                    // x1..x31 are locals 1..31 (i64); x0 is never a local
  L_BUDGET = 32, L_REASON = 33,               // i32
  L_T0 = 34, L_T1 = 35, L_T2 = 36, L_IC = 37, // i64
  L_NEXT = 38,                                // i32 (dispatch)
  L_F = 39,                                   // f0..f31: locals 39..70 (f64, raw bits; singles NaN-boxed)
  L_TI = 71, L_TI2 = 72,                      // i32
  L_TF0 = 73, L_TF1 = 74,                     // f64
  L_TS0 = 75, L_TS1 = 76,                     // f32
  L_COUNT = 77
};

// function imports, in this order, in every module
enum { IMP_ECALL, IMP_YIELD, IMP_LOOKUP, IMP_FMA64, IMP_FMA32, IMP_FPHELP, IMP_COUNT };
// module-local helpers (arch-defined), right after the imports
enum { HLP_MULHU, HLP_MULH, HLP_MULHSU, HLP_COUNT };
// types
enum { T_REGION, T_VOID_I32, T_I64_I32, T_F64x3, T_F32x3, T_I64x2_I64, T_FPHELP, T_COUNT };

typedef struct xgen {
  wbuf *b;
  const xlat_env *env;
  const xlat_opts *o;
  xmodule *m;
  uint64_t cache, dirty;   // bit r = x r (r < 32), bit 32 + r = f r
  int labels;              // wasm labels open right now (the function's $exit is label 0)
  // the instruction being emitted, for exits from its middle
  const xinsn *cur;
  int blk_n, blk_k;        // instructions in the current block, index of cur in it
} xgen;

// emission services the architecture file uses
void xg_get(xgen *g, int r);              // push x r (i64)
void xg_set(xgen *g, int r);              // pop into x r (dropped for x0)
void xg_fget(xgen *g, int r);             // push f r (f64 with raw bits)
void xg_fset(xgen *g, int r);
void xg_exit_const(xgen *g, uint64_t pc, int reason, int retired);   // leave: cpu->pc = pc
void xg_exit_dyn(xgen *g, int local_pc, int reason, int retired);    // leave: cpu->pc = local
void xg_exit_keep_pc(xgen *g, int reason, int retired);              // leave; cpu->pc already set
void xg_fault(xgen *g, int local_addr);   // leave with R_FAULT at the current instruction
// Compute a guest address rs1 + imm for an access of `size` bytes, check it per the bounds
// mode, and leave the i32 address (to be used with offset xg_memoff(g, imm)) on the stack.
uint32_t xg_addr(xgen *g, int rs1, int64_t imm, int size);
void xg_flush(xgen *g, uint64_t set);     // locals -> cpu
void xg_reload(xgen *g, uint64_t set);    // cpu -> locals
void xg_cpu_load64(xgen *g, uint32_t addr);   // push i64 from an absolute address (xlat_env)
void xg_cpu_store64_begin(xgen *g);          // push the address part of a store to the cpu struct
void xg_cpu_store64_end(xgen *g, uint32_t addr);
uint32_t xg_helper(xgen *g, int h);       // function index of a module-local helper
uint32_t xg_import(int imp);              // function index of an import
#endif
