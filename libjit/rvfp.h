// SPDX-License-Identifier: GPL-2.0-or-later
// RISC-V F and D extensions and the floating-point CSRs: for the interpreter (C) and the
// translator (wasm, with helpers where wasm's semantics differ from RISC-V's).
#ifndef JIT_RVFP_H
#define JIT_RVFP_H
#include "xgen.h"

extern int arch_fp_enabled;
extern int rvfp_frm;          // frm assumed by translations made now
int rvfp_translatable(const rv_insn *d);
void rvfp_regs(const rv_insn *d, uint64_t *use, uint64_t *def);
void rvfp_emit(xgen *g, const xinsn *x);
int rvfp_exec(rv_cpu *c, const rv_insn *d);        // -1: done; else an R_* reason
double rvfp_fma64(double a, double b, double c);    // exact fused multiply-add, round to nearest even
float rvfp_fma32(float a, float b, float c);
uint64_t rvfp_help(rv_cpu *c, int op, uint64_t a, uint64_t b, uint64_t c3);
#endif
