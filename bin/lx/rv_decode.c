// SPDX-License-Identifier: GPL-2.0-or-later
/* RV64GC decoder, written from the RISC-V Unprivileged ISA manual (chapters on RV32I,
 * RV64I, M, A, F, D, C, Zicsr, Zifencei, and the B extensions). Pure function: no state. */
#include "rv.h"

unsigned rv_decode_ext;

const char *const rv_op_name[RV_NOPS] = {
#define RV_NAME(n, f, fl) #n,
  RV_OPS(RV_NAME)
#undef RV_NAME
};
const uint16_t rv_op_flags[RV_NOPS] = {
#define RV_FL(n, f, fl) fl,
  RV_OPS(RV_FL)
#undef RV_FL
};

static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
#define BITS(v, hi, lo) (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static int decode32(uint32_t w, struct rv_insn *o) {
  unsigned opc = w & 0x7f, rd = BITS(w, 11, 7), f3 = BITS(w, 14, 12), rs1 = BITS(w, 19, 15);
  unsigned rs2 = BITS(w, 24, 20), f7 = w >> 25;
  int64_t immI = sext(w >> 20, 12);
  int64_t immS = sext((BITS(w, 31, 25) << 5) | BITS(w, 11, 7), 12);
  int64_t immB = sext((BITS(w, 31, 31) << 12) | (BITS(w, 7, 7) << 11) | (BITS(w, 30, 25) << 5) | (BITS(w, 11, 8) << 1), 13);
  int64_t immU = sext(w & 0xfffff000u, 32);
  int64_t immJ = sext((BITS(w, 31, 31) << 20) | (BITS(w, 19, 12) << 12) | (BITS(w, 20, 20) << 11) | (BITS(w, 30, 21) << 1), 21);
  unsigned ext = rv_decode_ext;
  o->rd = rd; o->rs1 = rs1; o->rs2 = rs2; o->rs3 = w >> 27; o->rm = f3; o->len = 4; o->imm = 0;
  int op = RV_ILLEGAL;
  switch (opc) {
  case 0x37: op = RV_LUI; o->imm = immU; break;
  case 0x17: op = RV_AUIPC; o->imm = immU; break;
  case 0x6f: op = RV_JAL; o->imm = immJ; break;
  case 0x67: if (f3 == 0) { op = RV_JALR; o->imm = immI; } break;
  case 0x63: {
    static const uint8_t t[8] = {RV_BEQ, RV_BNE, 0, 0, RV_BLT, RV_BGE, RV_BLTU, RV_BGEU};
    op = t[f3]; o->imm = immB; break;
  }
  case 0x03: {
    static const uint8_t t[8] = {RV_LB, RV_LH, RV_LW, RV_LD, RV_LBU, RV_LHU, RV_LWU, 0};
    op = t[f3]; o->imm = immI; break;
  }
  case 0x23: {
    static const uint8_t t[8] = {RV_SB, RV_SH, RV_SW, RV_SD, 0, 0, 0, 0};
    op = t[f3]; o->imm = immS; break;
  }
  case 0x13: {
    o->imm = immI;
    unsigned sh = BITS(w, 25, 20), top = w >> 26;
    switch (f3) {
    case 0: op = RV_ADDI; break;
    case 2: op = RV_SLTI; break;
    case 3: op = RV_SLTIU; break;
    case 4: op = RV_XORI; break;
    case 6: op = RV_ORI; break;
    case 7: op = RV_ANDI; break;
    case 1:
      o->imm = sh;
      if (top == 0) op = RV_SLLI;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x600) op = RV_CLZ;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x601) op = RV_CTZ;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x602) op = RV_CPOP;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x604) op = RV_SEXT_B;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x605) op = RV_SEXT_H;
      else if ((ext & RV_EXT_ZBS) && top == 0x12) op = RV_BCLRI;
      else if ((ext & RV_EXT_ZBS) && top == 0x1a) op = RV_BINVI;
      else if ((ext & RV_EXT_ZBS) && top == 0x0a) op = RV_BSETI;
      break;
    case 5:
      o->imm = sh;
      if (top == 0) op = RV_SRLI;
      else if (top == 0x10) op = RV_SRAI;
      else if ((ext & RV_EXT_ZBB) && top == 0x18) op = RV_RORI;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x287) op = RV_ORC_B;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x6b8) op = RV_REV8;
      else if ((ext & RV_EXT_ZBS) && top == 0x12) op = RV_BEXTI;
      break;
    }
    break;
  }
  case 0x1b:
    o->imm = immI;
    if (f3 == 0) op = RV_ADDIW;
    else if (f3 == 1) {
      o->imm = rs2;
      if (f7 == 0) op = RV_SLLIW;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x600) op = RV_CLZW;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x601) op = RV_CTZW;
      else if ((ext & RV_EXT_ZBB) && (w >> 20) == 0x602) op = RV_CPOPW;
      else if ((ext & RV_EXT_ZBA) && (w >> 26) == 0x02) { op = RV_SLLI_UW; o->imm = BITS(w, 25, 20); }
    } else if (f3 == 5) {
      o->imm = rs2;
      if (f7 == 0) op = RV_SRLIW;
      else if (f7 == 0x20) op = RV_SRAIW;
      else if ((ext & RV_EXT_ZBB) && f7 == 0x30) op = RV_RORIW;
    }
    break;
  case 0x33:
    if (f7 == 0) {
      static const uint8_t t[8] = {RV_ADD, RV_SLL, RV_SLT, RV_SLTU, RV_XOR, RV_SRL, RV_OR, RV_AND};
      op = t[f3];
    } else if (f7 == 0x20) {
      if (f3 == 0) op = RV_SUB;
      else if (f3 == 5) op = RV_SRA;
      else if ((ext & RV_EXT_ZBB) && f3 == 7) op = RV_ANDN;
      else if ((ext & RV_EXT_ZBB) && f3 == 6) op = RV_ORN;
      else if ((ext & RV_EXT_ZBB) && f3 == 4) op = RV_XNOR;
    } else if (f7 == 1) {
      static const uint8_t t[8] = {RV_MUL, RV_MULH, RV_MULHSU, RV_MULHU, RV_DIV, RV_DIVU, RV_REM, RV_REMU};
      op = t[f3];
    } else if ((ext & RV_EXT_ZBA) && f7 == 0x10 && (f3 == 2 || f3 == 4 || f3 == 6)) {
      op = f3 == 2 ? RV_SH1ADD : f3 == 4 ? RV_SH2ADD : RV_SH3ADD;
    } else if ((ext & RV_EXT_ZBB) && f7 == 0x05) {
      if (f3 == 4) op = RV_MIN; else if (f3 == 5) op = RV_MINU; else if (f3 == 6) op = RV_MAX; else if (f3 == 7) op = RV_MAXU;
    } else if ((ext & RV_EXT_ZBB) && f7 == 0x30) {
      if (f3 == 1) op = RV_ROL; else if (f3 == 5) op = RV_ROR;
    } else if (ext & RV_EXT_ZBS) {
      if (f7 == 0x24 && f3 == 1) op = RV_BCLR;
      else if (f7 == 0x24 && f3 == 5) op = RV_BEXT;
      else if (f7 == 0x34 && f3 == 1) op = RV_BINV;
      else if (f7 == 0x14 && f3 == 1) op = RV_BSET;
    }
    break;
  case 0x3b:
    if (f7 == 0) { if (f3 == 0) op = RV_ADDW; else if (f3 == 1) op = RV_SLLW; else if (f3 == 5) op = RV_SRLW; }
    else if (f7 == 0x20) { if (f3 == 0) op = RV_SUBW; else if (f3 == 5) op = RV_SRAW; }
    else if (f7 == 1) {
      static const uint8_t t[8] = {RV_MULW, 0, 0, 0, RV_DIVW, RV_DIVUW, RV_REMW, RV_REMUW};
      op = t[f3];
    } else if ((ext & RV_EXT_ZBA) && f7 == 0x04 && f3 == 0) op = RV_ADD_UW;
    else if ((ext & RV_EXT_ZBB) && f7 == 0x04 && f3 == 4 && rs2 == 0) op = RV_ZEXT_H;
    else if ((ext & RV_EXT_ZBA) && f7 == 0x10 && (f3 == 2 || f3 == 4 || f3 == 6)) {
      op = f3 == 2 ? RV_SH1ADD_UW : f3 == 4 ? RV_SH2ADD_UW : RV_SH3ADD_UW;
    } else if ((ext & RV_EXT_ZBB) && f7 == 0x30) { if (f3 == 1) op = RV_ROLW; else if (f3 == 5) op = RV_RORW; }
    break;
  case 0x0f:
    if (f3 == 0) op = RV_FENCE;
    else if (f3 == 1) op = RV_FENCE_I;
    break;
  case 0x73:
    if (f3 == 0) {
      if (w == 0x00000073) op = RV_ECALL;
      else if (w == 0x00100073) op = RV_EBREAK;
    } else if (f3 != 4) {
      static const uint8_t t[8] = {0, RV_CSRRW, RV_CSRRS, RV_CSRRC, 0, RV_CSRRWI, RV_CSRRSI, RV_CSRRCI};
      op = t[f3]; o->imm = w >> 20;
    }
    break;
  case 0x2f: {
    if (f3 != 2 && f3 != 3) break;
    unsigned f5 = w >> 27;
    o->rm = BITS(w, 26, 25);
    int d = f3 == 3 ? (RV_LR_D - RV_LR_W) : 0;
    switch (f5) {
    case 0x02: if (rs2 == 0) op = RV_LR_W + d; break;
    case 0x03: op = RV_SC_W + d; break;
    case 0x01: op = RV_AMOSWAP_W + d; break;
    case 0x00: op = RV_AMOADD_W + d; break;
    case 0x04: op = RV_AMOXOR_W + d; break;
    case 0x0c: op = RV_AMOAND_W + d; break;
    case 0x08: op = RV_AMOOR_W + d; break;
    case 0x10: op = RV_AMOMIN_W + d; break;
    case 0x14: op = RV_AMOMAX_W + d; break;
    case 0x18: op = RV_AMOMINU_W + d; break;
    case 0x1c: op = RV_AMOMAXU_W + d; break;
    }
    break;
  }
  case 0x07:
    if (f3 == 2) op = RV_FLW; else if (f3 == 3) op = RV_FLD;
    o->imm = immI; break;
  case 0x27:
    if (f3 == 2) op = RV_FSW; else if (f3 == 3) op = RV_FSD;
    o->imm = immS; break;
  case 0x43: case 0x47: case 0x4b: case 0x4f: {
    unsigned fmt = BITS(w, 26, 25);
    int base = opc == 0x43 ? RV_FMADD_S : opc == 0x47 ? RV_FMSUB_S : opc == 0x4b ? RV_FNMSUB_S : RV_FNMADD_S;
    if (fmt == 0) op = base;
    else if (fmt == 1) op = base + (RV_FMADD_D - RV_FMADD_S);
    if (f3 == 5 || f3 == 6) op = RV_ILLEGAL;
    break;
  }
  case 0x53: {
    int D = f7 & 1;
    int dd = D ? (RV_FADD_D - RV_FADD_S) : 0;
    int rmok = f3 != 5 && f3 != 6;
    switch (f7 >> 1) {
    case 0x00: if (rmok) op = RV_FADD_S + dd; break;
    case 0x02: if (rmok) op = RV_FSUB_S + dd; break;
    case 0x04: if (rmok) op = RV_FMUL_S + dd; break;
    case 0x06: if (rmok) op = RV_FDIV_S + dd; break;
    case 0x16: if (rmok && rs2 == 0) op = RV_FSQRT_S + dd; break;
    case 0x08: if (f3 <= 2) op = RV_FSGNJ_S + dd + f3; break;
    case 0x0a: if (f3 <= 1) op = (f3 ? RV_FMAX_S : RV_FMIN_S) + dd; break;
    case 0x10:
      if (D && rs2 == 0 && rmok) op = RV_FCVT_D_S;
      else if (!D && rs2 == 1 && rmok) op = RV_FCVT_S_D;
      break;
    case 0x28:
      if (f3 == 2) op = D ? RV_FEQ_D : RV_FEQ_S;
      else if (f3 == 1) op = D ? RV_FLT_D : RV_FLT_S;
      else if (f3 == 0) op = D ? RV_FLE_D : RV_FLE_S;
      break;
    case 0x30: if (rmok && rs2 <= 3) op = (D ? RV_FCVT_W_D : RV_FCVT_W_S) + rs2; break;
    case 0x34: if (rmok && rs2 <= 3) op = (D ? RV_FCVT_D_W : RV_FCVT_S_W) + rs2; break;
    case 0x38:
      if (rs2 == 0 && f3 == 0) op = D ? RV_FMV_X_D : RV_FMV_X_W;
      else if (rs2 == 0 && f3 == 1) op = D ? RV_FCLASS_D : RV_FCLASS_S;
      break;
    case 0x3c: if (rs2 == 0 && f3 == 0) op = D ? RV_FMV_D_X : RV_FMV_W_X; break;
    }
    if ((f7 & 2)) op = RV_ILLEGAL; /* fmt 2 (H) and 3 (Q) are not supported */
    break;
  }
  }
  o->op = op;
  return 4;
}

/* Compressed: expand to the base instruction. Register fields with a prime use x8..x15. */
static int decode16(uint32_t h, struct rv_insn *o) {
  unsigned q = h & 3, f3 = BITS(h, 15, 13);
  unsigned rdp = BITS(h, 4, 2) + 8, rs1p = BITS(h, 9, 7) + 8;
  unsigned rd = BITS(h, 11, 7), rs2 = BITS(h, 6, 2);
  o->len = 2; o->rd = 0; o->rs1 = 0; o->rs2 = 0; o->rs3 = 0; o->rm = 0; o->imm = 0;
  int op = RV_ILLEGAL;
  if (q == 0) {
    switch (f3) {
    case 0: { /* C.ADDI4SPN */
      unsigned imm = (BITS(h, 12, 11) << 4) | (BITS(h, 10, 7) << 6) | (BITS(h, 6, 6) << 2) | (BITS(h, 5, 5) << 3);
      if (imm) { op = RV_ADDI; o->rd = rdp; o->rs1 = 2; o->imm = imm; }
      break;
    }
    case 1: case 3: case 5: case 7: { /* C.FLD C.LD C.FSD C.SD: uimm[5:3|7:6] */
      unsigned imm = (BITS(h, 12, 10) << 3) | (BITS(h, 6, 5) << 6);
      o->rs1 = rs1p; o->imm = imm;
      if (f3 == 1) { op = RV_FLD; o->rd = rdp; }
      else if (f3 == 3) { op = RV_LD; o->rd = rdp; }
      else if (f3 == 5) { op = RV_FSD; o->rs2 = rdp; }
      else { op = RV_SD; o->rs2 = rdp; }
      break;
    }
    case 2: case 6: { /* C.LW C.SW: uimm[5:3|2|6] */
      unsigned imm = (BITS(h, 12, 10) << 3) | (BITS(h, 6, 6) << 2) | (BITS(h, 5, 5) << 6);
      o->rs1 = rs1p; o->imm = imm;
      if (f3 == 2) { op = RV_LW; o->rd = rdp; } else { op = RV_SW; o->rs2 = rdp; }
      break;
    }
    }
  } else if (q == 1) {
    int64_t imm6 = sext((BITS(h, 12, 12) << 5) | BITS(h, 6, 2), 6);
    switch (f3) {
    case 0: op = RV_ADDI; o->rd = rd; o->rs1 = rd; o->imm = imm6; break; /* C.ADDI, C.NOP */
    case 1: if (rd) { op = RV_ADDIW; o->rd = rd; o->rs1 = rd; o->imm = imm6; } break;
    case 2: op = RV_ADDI; o->rd = rd; o->rs1 = 0; o->imm = imm6; break; /* C.LI */
    case 3:
      if (rd == 2) { /* C.ADDI16SP */
        int64_t imm = sext((BITS(h, 12, 12) << 9) | (BITS(h, 6, 6) << 4) | (BITS(h, 5, 5) << 6) |
                           (BITS(h, 4, 3) << 7) | (BITS(h, 2, 2) << 5), 10);
        if (imm) { op = RV_ADDI; o->rd = 2; o->rs1 = 2; o->imm = imm; }
      } else { /* C.LUI */
        int64_t imm = sext((BITS(h, 12, 12) << 17) | (BITS(h, 6, 2) << 12), 18);
        if (imm) { op = RV_LUI; o->rd = rd; o->imm = imm; }
      }
      break;
    case 4: {
      unsigned f2 = BITS(h, 11, 10);
      o->rd = rs1p; o->rs1 = rs1p;
      if (f2 == 0) { op = RV_SRLI; o->imm = (BITS(h, 12, 12) << 5) | BITS(h, 6, 2); }
      else if (f2 == 1) { op = RV_SRAI; o->imm = (BITS(h, 12, 12) << 5) | BITS(h, 6, 2); }
      else if (f2 == 2) { op = RV_ANDI; o->imm = imm6; }
      else {
        o->rs2 = rdp;
        unsigned s = BITS(h, 6, 5);
        if (!BITS(h, 12, 12)) { static const uint8_t t[4] = {RV_SUB, RV_XOR, RV_OR, RV_AND}; op = t[s]; }
        else if (s == 0) op = RV_SUBW;
        else if (s == 1) op = RV_ADDW;
      }
      break;
    }
    case 5: { /* C.J */
      int64_t imm = sext((BITS(h, 12, 12) << 11) | (BITS(h, 11, 11) << 4) | (BITS(h, 10, 9) << 8) |
                         (BITS(h, 8, 8) << 10) | (BITS(h, 7, 7) << 6) | (BITS(h, 6, 6) << 7) |
                         (BITS(h, 5, 3) << 1) | (BITS(h, 2, 2) << 5), 12);
      op = RV_JAL; o->rd = 0; o->imm = imm;
      break;
    }
    case 6: case 7: { /* C.BEQZ C.BNEZ */
      int64_t imm = sext((BITS(h, 12, 12) << 8) | (BITS(h, 11, 10) << 3) | (BITS(h, 6, 5) << 6) |
                         (BITS(h, 4, 3) << 1) | (BITS(h, 2, 2) << 5), 9);
      op = f3 == 6 ? RV_BEQ : RV_BNE; o->rs1 = rs1p; o->rs2 = 0; o->imm = imm;
      break;
    }
    }
  } else if (q == 2) {
    switch (f3) {
    case 0: op = RV_SLLI; o->rd = rd; o->rs1 = rd; o->imm = (BITS(h, 12, 12) << 5) | BITS(h, 6, 2); break;
    case 1: case 3: { /* C.FLDSP C.LDSP: uimm[5|4:3|8:6] */
      unsigned imm = (BITS(h, 12, 12) << 5) | (BITS(h, 6, 5) << 3) | (BITS(h, 4, 2) << 6);
      o->rd = rd; o->rs1 = 2; o->imm = imm;
      if (f3 == 1) op = RV_FLD; else if (rd) op = RV_LD;
      break;
    }
    case 2: { /* C.LWSP: uimm[5|4:2|7:6] */
      unsigned imm = (BITS(h, 12, 12) << 5) | (BITS(h, 6, 4) << 2) | (BITS(h, 3, 2) << 6);
      if (rd) { op = RV_LW; o->rd = rd; o->rs1 = 2; o->imm = imm; }
      break;
    }
    case 4:
      if (!BITS(h, 12, 12)) {
        if (rs2 == 0) { if (rd) { op = RV_JALR; o->rd = 0; o->rs1 = rd; o->imm = 0; } }
        else { op = RV_ADD; o->rd = rd; o->rs1 = 0; o->rs2 = rs2; } /* C.MV */
      } else {
        if (rd == 0 && rs2 == 0) op = RV_EBREAK;
        else if (rs2 == 0) { op = RV_JALR; o->rd = 1; o->rs1 = rd; o->imm = 0; }
        else { op = RV_ADD; o->rd = rd; o->rs1 = rd; o->rs2 = rs2; }
      }
      break;
    case 5: case 7: { /* C.FSDSP C.SDSP: uimm[5:3|8:6] */
      unsigned imm = (BITS(h, 12, 10) << 3) | (BITS(h, 9, 7) << 6);
      o->rs1 = 2; o->rs2 = rs2; o->imm = imm;
      op = f3 == 5 ? RV_FSD : RV_SD;
      break;
    }
    case 6: { /* C.SWSP: uimm[5:2|7:6] */
      unsigned imm = (BITS(h, 12, 9) << 2) | (BITS(h, 8, 7) << 6);
      op = RV_SW; o->rs1 = 2; o->rs2 = rs2; o->imm = imm;
      break;
    }
    }
  }
  /* C.FLD/C.FSD etc. use the dynamic rounding field 0; FP loads ignore rm. */
  o->op = op;
  return 2;
}

int rv_decode(uint32_t raw, struct rv_insn *out) {
  if ((raw & 3) != 3) return decode16(raw & 0xffff, out);
  if ((raw & 0x1f) == 0x1f) { /* 48-bit and longer encodings: not part of RV64GC */
    out->op = RV_ILLEGAL; out->len = 4; out->rd = out->rs1 = out->rs2 = out->rs3 = out->rm = 0; out->imm = 0;
    return 4;
  }
  return decode32(raw, out);
}
