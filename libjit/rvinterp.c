// SPDX-License-Identifier: GPL-2.0-or-later
// A deliberately simple RV64GC interpreter: fetch, decode, execute, one instruction at a
// time, no caching of decoded instructions. It is (1) in the measurements (the trivial
// baseline), the engine's fallback for code the translator does not take, and the
// reference side of lockstep checking. The optimised interpreter is the core engine's.
#include "engine.h"
#include "rvfp.h"

static inline int inb(uint64_t a, int n) { return a <= rv_memsize - (uint64_t)n; }
#define LD(T, a) (*(T *)(rv_mem + (a)))

static int fault(rv_cpu *c, uint64_t a) { c->fault_addr = a; return R_FAULT; }

static inline uint64_t mulhu(uint64_t a, uint64_t b) {
  uint64_t alo = a & 0xffffffffu, ahi = a >> 32, blo = b & 0xffffffffu, bhi = b >> 32;
  uint64_t p0 = alo * blo, p1 = alo * bhi, p2 = ahi * blo, p3 = ahi * bhi;
  __asm__("" : "+r"(p1));   // keeps the compiler from turning this into a 128-bit multiply (__multi3)
  uint64_t mid = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
  return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

// Execute the decoded instruction d at c->pc. Returns -1 to go on, or an R_* reason
// (the pc then points at the instruction, except for R_HALT, where it is past it).
int rv_exec(rv_cpu *c, const rv_insn *d) {
  uint64_t *x = c->x, pc = c->pc, next = pc + d->len;
  uint64_t a = x[d->rs1], b = x[d->rs2], r = 0;
  int wr = 1;
  switch (d->op) {
  case RV_LUI: r = (uint64_t)d->imm; break;
  case RV_AUIPC: r = pc + (uint64_t)d->imm; break;
  case RV_JAL: r = next; next = pc + (uint64_t)d->imm; break;
  case RV_JALR: r = next; next = (a + (uint64_t)d->imm) & ~1ull; break;
  case RV_BEQ: wr = 0; if (a == b) next = pc + (uint64_t)d->imm; break;
  case RV_BNE: wr = 0; if (a != b) next = pc + (uint64_t)d->imm; break;
  case RV_BLT: wr = 0; if ((int64_t)a < (int64_t)b) next = pc + (uint64_t)d->imm; break;
  case RV_BGE: wr = 0; if ((int64_t)a >= (int64_t)b) next = pc + (uint64_t)d->imm; break;
  case RV_BLTU: wr = 0; if (a < b) next = pc + (uint64_t)d->imm; break;
  case RV_BGEU: wr = 0; if (a >= b) next = pc + (uint64_t)d->imm; break;
  case RV_LB: case RV_LH: case RV_LW: case RV_LD: case RV_LBU: case RV_LHU: case RV_LWU: {
    static const uint8_t sz[] = { 1, 2, 4, 8, 1, 2, 4 };
    uint64_t ea = a + (uint64_t)d->imm;
    if (!inb(ea, sz[d->op - RV_LB])) return fault(c, ea);
    switch (d->op) {
    case RV_LB: r = (uint64_t)(int64_t)LD(int8_t, ea); break;
    case RV_LH: r = (uint64_t)(int64_t)LD(int16_t, ea); break;
    case RV_LW: r = (uint64_t)(int64_t)LD(int32_t, ea); break;
    case RV_LD: r = LD(uint64_t, ea); break;
    case RV_LBU: r = LD(uint8_t, ea); break;
    case RV_LHU: r = LD(uint16_t, ea); break;
    default: r = LD(uint32_t, ea); break;
    }
    break;
  }
  case RV_SB: case RV_SH: case RV_SW: case RV_SD: {
    static const uint8_t sz[] = { 1, 2, 4, 8 };
    uint64_t ea = a + (uint64_t)d->imm;
    wr = 0;
    if (!inb(ea, sz[d->op - RV_SB])) return fault(c, ea);
    rv_store_hook(ea, sz[d->op - RV_SB]);
    switch (d->op) {
    case RV_SB: LD(uint8_t, ea) = (uint8_t)b; break;
    case RV_SH: LD(uint16_t, ea) = (uint16_t)b; break;
    case RV_SW: LD(uint32_t, ea) = (uint32_t)b; break;
    default: LD(uint64_t, ea) = b; break;
    }
    break;
  }
  case RV_ADDI: r = a + (uint64_t)d->imm; break;
  case RV_SLTI: r = (int64_t)a < d->imm; break;
  case RV_SLTIU: r = a < (uint64_t)d->imm; break;
  case RV_XORI: r = a ^ (uint64_t)d->imm; break;
  case RV_ORI: r = a | (uint64_t)d->imm; break;
  case RV_ANDI: r = a & (uint64_t)d->imm; break;
  case RV_SLLI: r = a << (d->imm & 63); break;
  case RV_SRLI: r = a >> (d->imm & 63); break;
  case RV_SRAI: r = (uint64_t)((int64_t)a >> (d->imm & 63)); break;
  case RV_ADD: r = a + b; break;
  case RV_SUB: r = a - b; break;
  case RV_SLL: r = a << (b & 63); break;
  case RV_SLT: r = (int64_t)a < (int64_t)b; break;
  case RV_SLTU: r = a < b; break;
  case RV_XOR: r = a ^ b; break;
  case RV_SRL: r = a >> (b & 63); break;
  case RV_SRA: r = (uint64_t)((int64_t)a >> (b & 63)); break;
  case RV_OR: r = a | b; break;
  case RV_AND: r = a & b; break;
  case RV_ADDIW: r = (uint64_t)(int64_t)(int32_t)(uint32_t)(a + (uint64_t)d->imm); break;
  case RV_SLLIW: r = (uint64_t)(int64_t)(int32_t)((uint32_t)a << (d->imm & 31)); break;
  case RV_SRLIW: r = (uint64_t)(int64_t)(int32_t)((uint32_t)a >> (d->imm & 31)); break;
  case RV_SRAIW: r = (uint64_t)(int64_t)((int32_t)a >> (d->imm & 31)); break;
  case RV_ADDW: r = (uint64_t)(int64_t)(int32_t)(uint32_t)(a + b); break;
  case RV_SUBW: r = (uint64_t)(int64_t)(int32_t)(uint32_t)(a - b); break;
  case RV_SLLW: r = (uint64_t)(int64_t)(int32_t)((uint32_t)a << (b & 31)); break;
  case RV_SRLW: r = (uint64_t)(int64_t)(int32_t)((uint32_t)a >> (b & 31)); break;
  case RV_SRAW: r = (uint64_t)(int64_t)((int32_t)a >> (b & 31)); break;
  case RV_FENCE: wr = 0; break;
  case RV_FENCE_I: wr = 0; engine_fence_i(); break;
  case RV_ECALL: {
    c->pc = pc;
    int e = engine_ecall();
    if (e == R_HALT) { c->pc = next; return R_HALT; }
    if (e) return e;
    wr = 0;
    break;
  }
  case RV_EBREAK: return R_EBREAK;
  case RV_MUL: r = a * b; break;
  case RV_MULH: r = mulhu(a, b) - ((int64_t)a < 0 ? b : 0) - ((int64_t)b < 0 ? a : 0); break;
  case RV_MULHSU: r = mulhu(a, b) - ((int64_t)a < 0 ? b : 0); break;
  case RV_MULHU: r = mulhu(a, b); break;
  case RV_DIV: r = b == 0 ? ~0ull : ((int64_t)a == INT64_MIN && (int64_t)b == -1) ? a : (uint64_t)((int64_t)a / (int64_t)b); break;
  case RV_DIVU: r = b == 0 ? ~0ull : a / b; break;
  case RV_REM: r = b == 0 ? a : ((int64_t)b == -1) ? 0 : (uint64_t)((int64_t)a % (int64_t)b); break;
  case RV_REMU: r = b == 0 ? a : a % b; break;
  case RV_MULW: r = (uint64_t)(int64_t)(int32_t)(uint32_t)(a * b); break;
  case RV_DIVW: {
    int32_t p = (int32_t)a, q = (int32_t)b;
    r = (uint64_t)(int64_t)(q == 0 ? -1 : (p == INT32_MIN && q == -1) ? p : p / q); break;
  }
  case RV_DIVUW: { uint32_t p = (uint32_t)a, q = (uint32_t)b; r = (uint64_t)(int64_t)(int32_t)(q == 0 ? ~0u : p / q); break; }
  case RV_REMW: {
    int32_t p = (int32_t)a, q = (int32_t)b;
    r = (uint64_t)(int64_t)(q == 0 ? p : q == -1 ? 0 : p % q); break;
  }
  case RV_REMUW: { uint32_t p = (uint32_t)a, q = (uint32_t)b; r = (uint64_t)(int64_t)(int32_t)(q == 0 ? p : p % q); break; }
  default:
    if (d->op >= RV_LR_W && d->op <= RV_AMOMAXU_D) {
      int dw = d->op >= RV_LR_D, op = dw ? d->op - (RV_LR_D - RV_LR_W) : d->op, n = dw ? 8 : 4;
      if (!inb(a, n)) return fault(c, a);
      uint64_t old = dw ? LD(uint64_t, a) : (uint64_t)(int64_t)LD(int32_t, a), nv = 0;
      int st = 1;
      switch (op) {
      case RV_LR_W: c->res_addr = a; r = old; st = 0; break;
      case RV_SC_W:
        st = c->res_addr == a; r = !st; nv = b; c->res_addr = ~0ull; break;
      case RV_AMOSWAP_W: nv = b; break;
      case RV_AMOADD_W: nv = old + b; break;
      case RV_AMOXOR_W: nv = old ^ b; break;
      case RV_AMOAND_W: nv = old & b; break;
      case RV_AMOOR_W: nv = old | b; break;
      case RV_AMOMIN_W: nv = dw ? ((int64_t)old < (int64_t)b ? old : b) : ((int32_t)old < (int32_t)b ? old : b); break;
      case RV_AMOMAX_W: nv = dw ? ((int64_t)old > (int64_t)b ? old : b) : ((int32_t)old > (int32_t)b ? old : b); break;
      case RV_AMOMINU_W: nv = dw ? (old < b ? old : b) : ((uint32_t)old < (uint32_t)b ? old : b); break;
      case RV_AMOMAXU_W: nv = dw ? (old > b ? old : b) : ((uint32_t)old > (uint32_t)b ? old : b); break;
      }
      if (op != RV_LR_W && op != RV_SC_W) r = old;
      if (st) { rv_store_hook(a, n); if (dw) LD(uint64_t, a) = nv; else LD(uint32_t, a) = (uint32_t)nv; }
      break;
    }
    if ((d->op >= RV_FLW && d->op <= RV_FMV_D_X) || (d->op >= RV_CSRRW && d->op <= RV_CSRRCI)) {
      int e = rvfp_exec(c, d);
      if (e >= 0) return e;
      wr = 0;
      break;
    }
    return R_INTERP;   // illegal instruction
  }
  if (wr && d->rd) x[d->rd] = r;
  c->pc = next;
  c->icount++;
  return -1;
}

int rv_interp_one(rv_cpu *c) {
  rv_insn d;
  if (!inb(c->pc, 4)) return fault(c, c->pc);
  if (!rv_decode(LD(uint32_t, c->pc), &d)) return R_INTERP;
  return rv_exec(c, &d);
}

// Run until a control transfer has executed (end of a basic block), or max instructions.
int rv_interp_block(rv_cpu *c, int max) {
  for (int i = 0; i < max; i++) {
    rv_insn d;
    if (!inb(c->pc, 4)) return fault(c, c->pc);
    if (!rv_decode(LD(uint32_t, c->pc), &d)) return R_INTERP;
    int e = rv_exec(c, &d);
    if (e >= 0) return e;
    if (d.op == RV_JAL || d.op == RV_JALR || (d.op >= RV_BEQ && d.op <= RV_BGEU) || d.op == RV_ECALL) return R_JUMP;
  }
  return R_JUMP;
}

// ---- predecoding interpreter ----
// Decodes each basic block once and keeps it; then runs the decoded instructions with no
// fetch or decode. Still a C switch per instruction. It is a stand-in for an optimised
// interpreter (case (1b) in the measurements), not the core engine's.
typedef struct pblock { uint64_t pc; struct pblock *next; int n; rv_insn d[]; } pblock;
#define PBH (1u << 16)
static pblock *pbh[PBH];
static pblock *pb_get(uint64_t pc) {
  uint32_t h = (uint32_t)((pc >> 1) * 2654435761u) >> 16;
  for (pblock *b = pbh[h]; b; b = b->next) if (b->pc == pc) return b;
  rv_insn tmp[64];
  int n = 0;
  uint64_t p = pc;
  while (n < 64) {
    if (!inb(p, 4) || !rv_decode(LD(uint32_t, p), &tmp[n])) break;
    int op = tmp[n].op;
    p += tmp[n].len;
    n++;
    if (op == RV_JAL || op == RV_JALR || (op >= RV_BEQ && op <= RV_BGEU) || op == RV_ECALL || op == RV_FENCE_I ||
        (op >= RV_CSRRW && op <= RV_CSRRCI)) break;
  }
  if (!n) return 0;
  pblock *b = jit_malloc(sizeof(pblock) + sizeof(rv_insn) * (size_t)n);
  b->pc = pc; b->n = n;
  memcpy(b->d, tmp, sizeof(rv_insn) * (size_t)n);
  b->next = pbh[h]; pbh[h] = b;
  return b;
}
void rv_predecode_flush(void) {
  for (uint32_t h = 0; h < PBH; h++) { pblock *b = pbh[h]; while (b) { pblock *n = b->next; jit_free(b); b = n; } pbh[h] = 0; }
}
// Run up to max blocks. Returns -1, or an R_* reason that needs the engine.
int rv_interp_pre(rv_cpu *c, int max) {
  for (int i = 0; i < max; i++) {
    pblock *b = pb_get(c->pc);
    if (!b) return rv_interp_one(c) < 0 ? -1 : R_INTERP;
    for (int k = 0; k < b->n; k++) {
      int e = rv_exec(c, &b->d[k]);
      if (e >= 0) return e;
    }
  }
  return -1;
}
