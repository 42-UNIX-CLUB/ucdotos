// SPDX-License-Identifier: GPL-2.0-or-later
// The RISC-V half of the translator: what each RV64 instruction is in WebAssembly.
// Integer registers are wasm i64 locals, floating-point registers f64 locals holding the
// raw 64 bits (single-precision values NaN-boxed, as the ISA defines). Guest memory is
// reached with xg_addr() (bounds discipline chosen by the engine). Anything not handled
// here is reported as FL_STOP and the engine interprets it.
#include "xgen.h"
#include "rvfp.h"

#define M32 0xffffffffll


static int is_fp(int op) { return op >= RV_FLW && op <= RV_FMV_D_X; }

int arch_decode(uint64_t pc, xinsn *x, const xlat_opts *o) {
  uint32_t raw;
  if (!engine_readable(pc, &raw)) return 0;
  if (!rv_decode(raw, &x->d)) return 0;
  rv_insn *d = &x->d;
  x->pc = pc;
  x->target = 0;
  x->flow = FL_NONE;
  switch (d->op) {
  case RV_JAL:
    x->target = pc + (uint64_t)d->imm;
    x->flow = d->rd ? FL_CALL : FL_JUMP;
    break;
  case RV_JALR:
    if (d->rd == 0 && (d->rs1 == 1 || d->rs1 == 5) && d->imm == 0) x->flow = FL_RET;
    else x->flow = d->rd ? FL_INDCALL : FL_INDJUMP;
    break;
  case RV_BEQ: case RV_BNE: case RV_BLT: case RV_BGE: case RV_BLTU: case RV_BGEU:
    x->target = pc + (uint64_t)d->imm;
    x->flow = FL_BRANCH;
    break;
  case RV_ECALL: if (o->ecall_exits) x->flow = FL_STOP; break;
  case RV_EBREAK: case RV_FENCE_I: case RV_ILLEGAL:
    x->flow = FL_STOP; break;
  case RV_CSRRW: case RV_CSRRS: case RV_CSRRC: case RV_CSRRWI: case RV_CSRRSI: case RV_CSRRCI:
    // the floating-point CSRs (fflags 1, frm 2, fcsr 3) go to a helper; the rest leave
    if (!(arch_fp_enabled && rvfp_translatable(d))) x->flow = FL_STOP;
    break;
  default:
    if (is_fp(d->op) && !arch_fp_enabled) x->flow = FL_STOP;
    if (is_fp(d->op) && !rvfp_translatable(d)) x->flow = FL_STOP;
  }
  return 1;
}

#define X(r) (1ull << (r))
#define F(r) (1ull << (32 + (r)))
void arch_regs(const xinsn *in, uint64_t *use, uint64_t *def) {
  const rv_insn *d = &in->d;
  uint64_t u = 0, w = 0;
  int op = d->op;
  if (op == RV_ECALL) { u = 0xffull << 10; w = X(10); }
  else if (op >= RV_FLW && op <= RV_FMV_D_X) rvfp_regs(d, &u, &w);
  else if (op >= RV_CSRRW && op <= RV_CSRRCI) { if (op <= RV_CSRRC) u = X(d->rs1); w = X(d->rd); }
  else {
    switch (op) {
    case RV_LUI: case RV_AUIPC: case RV_JAL: w = X(d->rd); break;
    case RV_BEQ: case RV_BNE: case RV_BLT: case RV_BGE: case RV_BLTU: case RV_BGEU:
    case RV_SB: case RV_SH: case RV_SW: case RV_SD: u = X(d->rs1) | X(d->rs2); break;
    case RV_FENCE: case RV_FENCE_I: case RV_EBREAK: case RV_ILLEGAL: break;
    case RV_JALR: case RV_LB: case RV_LH: case RV_LW: case RV_LD: case RV_LBU: case RV_LHU: case RV_LWU:
    case RV_ADDI: case RV_SLTI: case RV_SLTIU: case RV_XORI: case RV_ORI: case RV_ANDI: case RV_SLLI: case RV_SRLI: case RV_SRAI:
    case RV_ADDIW: case RV_SLLIW: case RV_SRLIW: case RV_SRAIW:
    case RV_LR_W: case RV_LR_D:
      u = X(d->rs1); w = X(d->rd); break;
    default: u = X(d->rs1) | X(d->rs2); w = X(d->rd); break;   // R-type, M, SC, AMO
    }
  }
  *use = u & ~1ull;
  *def = w & ~1ull;
}

static void bin(xgen *g, const rv_insn *d, int op) { xg_get(g, d->rs1); xg_get(g, d->rs2); w_op(g->b, op); xg_set(g, d->rd); }
static void imm(xgen *g, const rv_insn *d, int op) { xg_get(g, d->rs1); w_i64c(g->b, d->imm); w_op(g->b, op); xg_set(g, d->rd); }
static void w32(xgen *g, const rv_insn *d, int op, int useimm) {
  xg_get(g, d->rs1); w_op(g->b, W_I32_WRAP_I64);
  if (useimm) w_i32c(g->b, (int32_t)d->imm); else { xg_get(g, d->rs2); w_op(g->b, W_I32_WRAP_I64); }
  w_op(g->b, op); w_op(g->b, W_I64_EXTEND_I32_S); xg_set(g, d->rd);
}
static void cmp(xgen *g, const rv_insn *d, int op, int useimm) {
  xg_get(g, d->rs1);
  if (useimm) w_i64c(g->b, d->imm); else xg_get(g, d->rs2);
  w_op(g->b, op); w_op(g->b, W_I64_EXTEND_I32_U); xg_set(g, d->rd);
}
static void if_(xgen *g, int type) { w_if(g->b, type); g->labels++; }
static void else_(xgen *g) { w_op(g->b, W_ELSE); }
static void end_(xgen *g) { w_op(g->b, W_END); g->labels--; }

static void load(xgen *g, const rv_insn *d, int op, int size, int align) {
  uint32_t off = xg_addr(g, d->rs1, d->imm, size);
  w_mem(g->b, op, (uint32_t)align, off);
  xg_set(g, d->rd);
}
static void store(xgen *g, const rv_insn *d, int op, int size, int align) {
  uint32_t off = xg_addr(g, d->rs1, d->imm, size);
  xg_get(g, d->rs2);
  w_mem(g->b, op, (uint32_t)align, off);
}

// division with RISC-V's results for a zero divisor and for overflow (wasm would trap)
static void div64(xgen *g, const rv_insn *d) {
  wbuf *b = g->b;
  int op = d->op, sign = op == RV_DIV || op == RV_REM, isrem = op == RV_REM || op == RV_REMU;
  xg_get(g, d->rs2); w_op(b, W_I64_EQZ);
  if_(g, WT_I64);
  if (isrem) xg_get(g, d->rs1); else w_i64c(b, -1);
  else_(g);
  if (sign && !isrem) {            // MIN / -1 = MIN: computed as 0 - rs1
    xg_get(g, d->rs2); w_i64c(b, -1); w_op(b, W_I64_EQ);
    if_(g, WT_I64);
    w_i64c(b, 0); xg_get(g, d->rs1); w_op(b, W_I64_SUB);
    else_(g);
    xg_get(g, d->rs1); xg_get(g, d->rs2); w_op(b, W_I64_DIV_S);
    end_(g);
  } else {
    xg_get(g, d->rs1); xg_get(g, d->rs2);
    w_op(b, op == RV_DIVU ? W_I64_DIV_U : op == RV_REM ? W_I64_REM_S : W_I64_REM_U);   // rem_s(MIN, -1) = 0 in wasm
  }
  end_(g);
  xg_set(g, d->rd);
}
static void div32(xgen *g, const rv_insn *d) {
  wbuf *b = g->b;
  int op = d->op, sign = op == RV_DIVW || op == RV_REMW, isrem = op == RV_REMW || op == RV_REMUW;
  xg_get(g, d->rs2); w_op(b, W_I32_WRAP_I64); w_op(b, W_I32_EQZ);
  if_(g, WT_I32);
  if (isrem) { xg_get(g, d->rs1); w_op(b, W_I32_WRAP_I64); } else w_i32c(b, -1);
  else_(g);
  if (sign && !isrem) {
    xg_get(g, d->rs2); w_op(b, W_I32_WRAP_I64); w_i32c(b, -1); w_op(b, W_I32_EQ);
    if_(g, WT_I32);
    w_i32c(b, 0); xg_get(g, d->rs1); w_op(b, W_I32_WRAP_I64); w_op(b, W_I32_SUB);
    else_(g);
    xg_get(g, d->rs1); w_op(b, W_I32_WRAP_I64); xg_get(g, d->rs2); w_op(b, W_I32_WRAP_I64); w_op(b, W_I32_DIV_S);
    end_(g);
  } else {
    xg_get(g, d->rs1); w_op(b, W_I32_WRAP_I64); xg_get(g, d->rs2); w_op(b, W_I32_WRAP_I64);
    w_op(b, op == RV_DIVUW ? W_I32_DIV_U : op == RV_REMW ? W_I32_REM_S : W_I32_REM_U);
  }
  end_(g);
  w_op(b, W_I64_EXTEND_I32_S);
  xg_set(g, d->rd);
}

// A extension, single-threaded: LR/SC through a reservation address in the cpu struct.
static void amo(xgen *g, const rv_insn *d) {
  wbuf *b = g->b;
  int op = d->op, dw = op >= RV_LR_D, base = dw ? op - (RV_LR_D - RV_LR_W) : op;
  int size = dw ? 8 : 4;
  if (base == RV_LR_W) {
    xg_cpu_store64_begin(g); xg_get(g, d->rs1); xg_cpu_store64_end(g, g->env->a_res);
    uint32_t off = xg_addr(g, d->rs1, 0, size);
    w_mem(b, dw ? W_I64_LOAD : W_I64_LOAD32_S, dw ? 3 : 2, off);
    xg_set(g, d->rd);
    return;
  }
  if (base == RV_SC_W) {
    xg_cpu_load64(g, g->env->a_res); xg_get(g, d->rs1); w_op(b, W_I64_EQ);
    if_(g, WT_I64);
    uint32_t off = xg_addr(g, d->rs1, 0, size);
    xg_get(g, d->rs2);
    w_mem(b, dw ? W_I64_STORE : W_I64_STORE32, dw ? 3 : 2, off);
    w_i64c(b, 0);
    else_(g);
    w_i64c(b, 1);
    end_(g);
    xg_set(g, d->rd);
    xg_cpu_store64_begin(g); w_i64c(b, -1); xg_cpu_store64_end(g, g->env->a_res);
    return;
  }
  uint32_t off = xg_addr(g, d->rs1, 0, size);
  w_lset(b, L_TI);
  w_lget(b, L_TI); w_mem(b, dw ? W_I64_LOAD : W_I64_LOAD32_S, dw ? 3 : 2, off); w_lset(b, L_T0);
  w_lget(b, L_TI);
  // new value, as i64 (the 32-bit forms store the low half)
  int cmpop = 0;
  switch (base) {
  case RV_AMOSWAP_W: xg_get(g, d->rs2); break;
  case RV_AMOADD_W: w_lget(b, L_T0); xg_get(g, d->rs2); w_op(b, W_I64_ADD); break;
  case RV_AMOXOR_W: w_lget(b, L_T0); xg_get(g, d->rs2); w_op(b, W_I64_XOR); break;
  case RV_AMOAND_W: w_lget(b, L_T0); xg_get(g, d->rs2); w_op(b, W_I64_AND); break;
  case RV_AMOOR_W: w_lget(b, L_T0); xg_get(g, d->rs2); w_op(b, W_I64_OR); break;
  case RV_AMOMIN_W: cmpop = dw ? W_I64_LT_S : W_I32_LT_S; break;
  case RV_AMOMAX_W: cmpop = dw ? W_I64_GT_S : W_I32_GT_S; break;
  case RV_AMOMINU_W: cmpop = dw ? W_I64_LT_U : W_I32_LT_U; break;
  case RV_AMOMAXU_W: cmpop = dw ? W_I64_GT_U : W_I32_GT_U; break;
  }
  if (cmpop) {
    w_lget(b, L_T0); xg_get(g, d->rs2);
    w_lget(b, L_T0); if (!dw) w_op(b, W_I32_WRAP_I64);
    xg_get(g, d->rs2); if (!dw) w_op(b, W_I32_WRAP_I64);
    w_op(b, cmpop);
    w_op(b, W_SELECT);
  }
  w_mem(b, dw ? W_I64_STORE : W_I64_STORE32, dw ? 3 : 2, off);
  w_lget(b, L_T0); xg_set(g, d->rd);
}

// A system call sees the whole guest state in memory: fork copies memory, a signal frame
// holds every register, rt_sigreturn and execve replace them. So: store the pc and every
// register this region changed, call the engine, reload everything this region keeps.
// The engine answers 0 (continue after the ecall), R_HALT, R_JUMP (the state was replaced:
// continue at cpu->pc) or R_INTERP (not handled here: leave before the ecall).
static void ecall(xgen *g, const xinsn *x) {
  wbuf *b = g->b;
  w_i32c(b, 0); w_i64c(b, (int64_t)x->pc); w_mem(b, W_I64_STORE, 3, g->env->a_pc);
  xg_flush(g, g->dirty);
  w_call(b, xg_import(IMP_ECALL));
  w_lset(b, L_TI);
  xg_reload(g, g->cache);
  w_lget(b, L_TI);
  if_(g, WT_VOID);
  w_lget(b, L_TI); w_i32c(b, R_HALT); w_op(b, W_I32_EQ);
  if_(g, WT_VOID);
  xg_exit_const(g, x->pc + x->d.len, R_HALT, g->blk_k + 1);
  else_(g);
  w_lget(b, L_TI); w_i32c(b, R_JUMP); w_op(b, W_I32_EQ);
  if_(g, WT_VOID);
  xg_exit_keep_pc(g, R_JUMP, g->blk_k + 1);
  else_(g);
  xg_exit_const(g, x->pc, R_INTERP, g->blk_k);
  end_(g);
  end_(g);
  end_(g);
}

void arch_emit(xgen *g, const xinsn *x) {
  const rv_insn *d = &x->d;
  wbuf *b = g->b;
  int op = d->op;
  if (d->rd == 0 && ((op >= RV_LUI && op <= RV_AUIPC) || (op >= RV_ADDI && op <= RV_SRAW) || (op >= RV_MUL && op <= RV_REMUW))) return;
  switch (op) {
  case RV_LUI: w_i64c(b, d->imm); xg_set(g, d->rd); break;
  case RV_AUIPC: w_i64c(b, (int64_t)(x->pc + (uint64_t)d->imm)); xg_set(g, d->rd); break;
  case RV_LB: load(g, d, W_I64_LOAD8_S, 1, 0); break;
  case RV_LH: load(g, d, W_I64_LOAD16_S, 2, 1); break;
  case RV_LW: load(g, d, W_I64_LOAD32_S, 4, 2); break;
  case RV_LD: load(g, d, W_I64_LOAD, 8, 3); break;
  case RV_LBU: load(g, d, W_I64_LOAD8_U, 1, 0); break;
  case RV_LHU: load(g, d, W_I64_LOAD16_U, 2, 1); break;
  case RV_LWU: load(g, d, W_I64_LOAD32_U, 4, 2); break;
  case RV_SB: store(g, d, W_I64_STORE8, 1, 0); break;
  case RV_SH: store(g, d, W_I64_STORE16, 2, 1); break;
  case RV_SW: store(g, d, W_I64_STORE32, 4, 2); break;
  case RV_SD: store(g, d, W_I64_STORE, 8, 3); break;
  case RV_ADDI:
    if (d->imm == 0) { xg_get(g, d->rs1); xg_set(g, d->rd); }
    else imm(g, d, W_I64_ADD);
    break;
  case RV_SLTI: cmp(g, d, W_I64_LT_S, 1); break;
  case RV_SLTIU: cmp(g, d, W_I64_LT_U, 1); break;
  case RV_XORI: imm(g, d, W_I64_XOR); break;
  case RV_ORI: imm(g, d, W_I64_OR); break;
  case RV_ANDI: imm(g, d, W_I64_AND); break;
  case RV_SLLI: imm(g, d, W_I64_SHL); break;
  case RV_SRLI: imm(g, d, W_I64_SHR_U); break;
  case RV_SRAI: imm(g, d, W_I64_SHR_S); break;
  case RV_ADD: bin(g, d, W_I64_ADD); break;
  case RV_SUB: bin(g, d, W_I64_SUB); break;
  case RV_SLL: bin(g, d, W_I64_SHL); break;     // wasm masks the count to 6 bits, as RV64 does
  case RV_SLT: cmp(g, d, W_I64_LT_S, 0); break;
  case RV_SLTU: cmp(g, d, W_I64_LT_U, 0); break;
  case RV_XOR: bin(g, d, W_I64_XOR); break;
  case RV_SRL: bin(g, d, W_I64_SHR_U); break;
  case RV_SRA: bin(g, d, W_I64_SHR_S); break;
  case RV_OR: bin(g, d, W_I64_OR); break;
  case RV_AND: bin(g, d, W_I64_AND); break;
  case RV_ADDIW: w32(g, d, W_I32_ADD, 1); break;
  case RV_SLLIW: w32(g, d, W_I32_SHL, 1); break;
  case RV_SRLIW: w32(g, d, W_I32_SHR_U, 1); break;
  case RV_SRAIW: w32(g, d, W_I32_SHR_S, 1); break;
  case RV_ADDW: w32(g, d, W_I32_ADD, 0); break;
  case RV_SUBW: w32(g, d, W_I32_SUB, 0); break;
  case RV_SLLW: w32(g, d, W_I32_SHL, 0); break;  // 5-bit count, as wasm i32 shifts
  case RV_SRLW: w32(g, d, W_I32_SHR_U, 0); break;
  case RV_SRAW: w32(g, d, W_I32_SHR_S, 0); break;
  case RV_MUL: bin(g, d, W_I64_MUL); break;
  case RV_MULW: w32(g, d, W_I32_MUL, 0); break;
  case RV_MULHU: xg_get(g, d->rs1); xg_get(g, d->rs2); w_call(b, xg_helper(g, HLP_MULHU)); xg_set(g, d->rd); break;
  case RV_MULH: xg_get(g, d->rs1); xg_get(g, d->rs2); w_call(b, xg_helper(g, HLP_MULH)); xg_set(g, d->rd); break;
  case RV_MULHSU: xg_get(g, d->rs1); xg_get(g, d->rs2); w_call(b, xg_helper(g, HLP_MULHSU)); xg_set(g, d->rd); break;
  case RV_DIV: case RV_DIVU: case RV_REM: case RV_REMU: div64(g, d); break;
  case RV_DIVW: case RV_DIVUW: case RV_REMW: case RV_REMUW: div32(g, d); break;
  case RV_FENCE: break;
  case RV_ECALL: ecall(g, x); break;
  default:
    if (op >= RV_LR_W && op <= RV_AMOMAXU_D) { amo(g, d); break; }
    if ((op >= RV_FLW && op <= RV_FMV_D_X) || (op >= RV_CSRRW && op <= RV_CSRRCI)) { rvfp_emit(g, x); break; }
    w_op(b, W_UNREACHABLE);    // arch_decode marks everything else FL_STOP
  }
}

void arch_emit_cond(xgen *g, const xinsn *x) {
  const rv_insn *d = &x->d;
  static const uint8_t ops[] = { W_I64_EQ, W_I64_NE, W_I64_LT_S, W_I64_GE_S, W_I64_LT_U, W_I64_GE_U };
  xg_get(g, d->rs1);
  if (d->rs2 == 0 && d->op == RV_BEQ) { w_op(g->b, W_I64_EQZ); return; }
  xg_get(g, d->rs2);
  w_op(g->b, ops[d->op - RV_BEQ]);
}

void arch_emit_link(xgen *g, const xinsn *x) {
  if (!x->d.rd) return;
  w_i64c(g->b, (int64_t)(x->pc + x->d.len));
  xg_set(g, x->d.rd);
}

void arch_emit_indirect_target(xgen *g, const xinsn *x) {
  xg_get(g, x->d.rs1);
  if (x->d.imm) { w_i64c(g->b, x->d.imm); w_op(g->b, W_I64_ADD); }
  w_i64c(g->b, -2); w_op(g->b, W_I64_AND);
}

// module-local helpers: 64x64 -> high 64 bits
int arch_helper_count(uint32_t used) { (void)used; return HLP_COUNT; }
static void body_end(wbuf *code, wbuf *f) { w_op(f, W_END); wb_uleb(code, f->n); wb_bytes(code, f->p, f->n); f->n = 0; }
void arch_emit_helpers(wbuf *code, uint32_t used) {
  (void)used;
  wbuf f;
  wb_init(&f);
  enum { A = 0, Bv = 1, ALO = 2, AHI, BLO, BHI, P1, P2, P0 };
  // mulhu(a, b)
  wb_uleb(&f, 1); wb_uleb(&f, 7); wb_byte(&f, WT_I64);
  w_lget(&f, A); w_i64c(&f, M32); w_op(&f, W_I64_AND); w_lset(&f, ALO);
  w_lget(&f, A); w_i64c(&f, 32); w_op(&f, W_I64_SHR_U); w_lset(&f, AHI);
  w_lget(&f, Bv); w_i64c(&f, M32); w_op(&f, W_I64_AND); w_lset(&f, BLO);
  w_lget(&f, Bv); w_i64c(&f, 32); w_op(&f, W_I64_SHR_U); w_lset(&f, BHI);
  w_lget(&f, ALO); w_lget(&f, BHI); w_op(&f, W_I64_MUL); w_lset(&f, P1);
  w_lget(&f, AHI); w_lget(&f, BLO); w_op(&f, W_I64_MUL); w_lset(&f, P2);
  w_lget(&f, ALO); w_lget(&f, BLO); w_op(&f, W_I64_MUL); w_lset(&f, P0);
  w_lget(&f, AHI); w_lget(&f, BHI); w_op(&f, W_I64_MUL);
  w_lget(&f, P1); w_i64c(&f, 32); w_op(&f, W_I64_SHR_U); w_op(&f, W_I64_ADD);
  w_lget(&f, P2); w_i64c(&f, 32); w_op(&f, W_I64_SHR_U); w_op(&f, W_I64_ADD);
  w_lget(&f, P0); w_i64c(&f, 32); w_op(&f, W_I64_SHR_U);
  w_lget(&f, P1); w_i64c(&f, M32); w_op(&f, W_I64_AND); w_op(&f, W_I64_ADD);
  w_lget(&f, P2); w_i64c(&f, M32); w_op(&f, W_I64_AND); w_op(&f, W_I64_ADD);
  w_i64c(&f, 32); w_op(&f, W_I64_SHR_U); w_op(&f, W_I64_ADD);
  body_end(code, &f);
  // mulh(a, b) = mulhu(a, b) - (a < 0 ? b : 0) - (b < 0 ? a : 0)
  wb_uleb(&f, 0);
  w_lget(&f, A); w_lget(&f, Bv); w_call(&f, IMP_COUNT + HLP_MULHU);
  w_lget(&f, A); w_i64c(&f, 63); w_op(&f, W_I64_SHR_S); w_lget(&f, Bv); w_op(&f, W_I64_AND); w_op(&f, W_I64_SUB);
  w_lget(&f, Bv); w_i64c(&f, 63); w_op(&f, W_I64_SHR_S); w_lget(&f, A); w_op(&f, W_I64_AND); w_op(&f, W_I64_SUB);
  body_end(code, &f);
  // mulhsu(a signed, b unsigned) = mulhu(a, b) - (a < 0 ? b : 0)
  wb_uleb(&f, 0);
  w_lget(&f, A); w_lget(&f, Bv); w_call(&f, IMP_COUNT + HLP_MULHU);
  w_lget(&f, A); w_i64c(&f, 63); w_op(&f, W_I64_SHR_S); w_lget(&f, Bv); w_op(&f, W_I64_AND); w_op(&f, W_I64_SUB);
  body_end(code, &f);
  wb_free(&f);
}
