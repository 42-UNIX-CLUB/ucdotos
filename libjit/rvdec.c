// SPDX-License-Identifier: GPL-2.0-or-later
// RV64GC decoder: RV64I, M, A, F, D, Zicsr, Zifencei and C (compressed instructions are
// expanded into the same form as their 32-bit equivalents).
#include "rv.h"

#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(x, n) (((x) >> (n)) & 1u)
static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

const char *const rv_names[RV_NOPS] = {
  "illegal", "lui", "auipc", "jal", "jalr", "beq", "bne", "blt", "bge", "bltu", "bgeu",
  "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "sb", "sh", "sw", "sd",
  "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
  "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
  "addiw", "slliw", "srliw", "sraiw", "addw", "subw", "sllw", "srlw", "sraw",
  "fence", "fence.i", "ecall", "ebreak", "csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci",
  "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu", "mulw", "divw", "divuw", "remw", "remuw",
  "lr.w", "sc.w", "amoswap.w", "amoadd.w", "amoxor.w", "amoand.w", "amoor.w", "amomin.w", "amomax.w", "amominu.w", "amomaxu.w",
  "lr.d", "sc.d", "amoswap.d", "amoadd.d", "amoxor.d", "amoand.d", "amoor.d", "amomin.d", "amomax.d", "amominu.d", "amomaxu.d",
  "flw", "fld", "fsw", "fsd",
  "fmadd.s", "fmsub.s", "fnmsub.s", "fnmadd.s", "fmadd.d", "fmsub.d", "fnmsub.d", "fnmadd.d",
  "fadd.s", "fsub.s", "fmul.s", "fdiv.s", "fsqrt.s", "fsgnj.s", "fsgnjn.s", "fsgnjx.s", "fmin.s", "fmax.s",
  "fadd.d", "fsub.d", "fmul.d", "fdiv.d", "fsqrt.d", "fsgnj.d", "fsgnjn.d", "fsgnjx.d", "fmin.d", "fmax.d",
  "fcvt.s.d", "fcvt.d.s", "feq.s", "flt.s", "fle.s", "feq.d", "flt.d", "fle.d",
  "fcvt.w.s", "fcvt.wu.s", "fcvt.l.s", "fcvt.lu.s", "fcvt.w.d", "fcvt.wu.d", "fcvt.l.d", "fcvt.lu.d",
  "fcvt.s.w", "fcvt.s.wu", "fcvt.s.l", "fcvt.s.lu", "fcvt.d.w", "fcvt.d.wu", "fcvt.d.l", "fcvt.d.lu",
  "fmv.x.w", "fclass.s", "fmv.x.d", "fclass.d", "fmv.w.x", "fmv.d.x",
};

static int set(rv_insn *d, int op, int rd, int rs1, int rs2, int64_t imm) {
  d->op = (uint16_t)op; d->rd = (uint8_t)rd; d->rs1 = (uint8_t)rs1; d->rs2 = (uint8_t)rs2; d->imm = imm;
  return op != RV_ILLEGAL;
}

static int decode16(uint32_t c, rv_insn *d) {
  int q = c & 3, f3 = BITS(c, 15, 13);
  int rd = BITS(c, 11, 7), rs2 = BITS(c, 6, 2);
  int rdp = 8 + BITS(c, 4, 2), rs1p = 8 + BITS(c, 9, 7);
  d->len = 2;
  if (q == 0) {
    uint32_t ld_d = BITS(c, 12, 10) << 3 | BITS(c, 6, 5) << 6;              // C.LD/C.SD/C.FLD/C.FSD offset
    uint32_t ld_w = BITS(c, 12, 10) << 3 | BIT(c, 6) << 2 | BIT(c, 5) << 6;  // C.LW/C.SW offset
    switch (f3) {
    case 0: {
      uint32_t imm = BITS(c, 12, 11) << 4 | BITS(c, 10, 7) << 6 | BIT(c, 6) << 2 | BIT(c, 5) << 3;
      if (!imm) return 0;
      return set(d, RV_ADDI, rdp, 2, 0, imm);
    }
    case 1: return set(d, RV_FLD, rdp, rs1p, 0, ld_d);
    case 2: return set(d, RV_LW, rdp, rs1p, 0, ld_w);
    case 3: return set(d, RV_LD, rdp, rs1p, 0, ld_d);
    case 5: return set(d, RV_FSD, 0, rs1p, rdp, ld_d);
    case 6: return set(d, RV_SW, 0, rs1p, rdp, ld_w);
    case 7: return set(d, RV_SD, 0, rs1p, rdp, ld_d);
    default: return 0;
    }
  }
  if (q == 1) {
    int64_t imm6 = sext(BIT(c, 12) << 5 | BITS(c, 6, 2), 6);
    switch (f3) {
    case 0: return set(d, RV_ADDI, rd, rd, 0, imm6);
    case 1: if (!rd) return 0; return set(d, RV_ADDIW, rd, rd, 0, imm6);
    case 2: return set(d, RV_ADDI, rd, 0, 0, imm6);
    case 3:
      if (rd == 2) {
        int64_t imm = sext(BIT(c, 12) << 9 | BIT(c, 6) << 4 | BIT(c, 5) << 6 | BITS(c, 4, 3) << 7 | BIT(c, 2) << 5, 10);
        if (!imm) return 0;
        return set(d, RV_ADDI, 2, 2, 0, imm);
      }
      if (!imm6) return 0;
      return set(d, RV_LUI, rd, 0, 0, imm6 << 12);
    case 4: {
      int f2 = BITS(c, 11, 10);
      uint32_t sh = BIT(c, 12) << 5 | BITS(c, 6, 2);
      if (f2 == 0) return set(d, RV_SRLI, rs1p, rs1p, 0, sh);
      if (f2 == 1) return set(d, RV_SRAI, rs1p, rs1p, 0, sh);
      if (f2 == 2) return set(d, RV_ANDI, rs1p, rs1p, 0, imm6);
      static const uint16_t ops[8] = { RV_SUB, RV_XOR, RV_OR, RV_AND, RV_SUBW, RV_ADDW, 0, 0 };
      int op = ops[BIT(c, 12) << 2 | BITS(c, 6, 5)];
      return set(d, op, rs1p, rs1p, rdp, 0);
    }
    case 5: {
      int64_t imm = sext(BIT(c, 12) << 11 | BIT(c, 11) << 4 | BITS(c, 10, 9) << 8 | BIT(c, 8) << 10 |
                         BIT(c, 7) << 6 | BIT(c, 6) << 7 | BITS(c, 5, 3) << 1 | BIT(c, 2) << 5, 12);
      return set(d, RV_JAL, 0, 0, 0, imm);
    }
    default: {
      int64_t imm = sext(BIT(c, 12) << 8 | BITS(c, 11, 10) << 3 | BITS(c, 6, 5) << 6 | BITS(c, 4, 3) << 1 | BIT(c, 2) << 5, 9);
      return set(d, f3 == 6 ? RV_BEQ : RV_BNE, 0, rs1p, 0, imm);
    }
    }
  }
  if (q == 2) {
    uint32_t sp_d = BIT(c, 12) << 5 | BITS(c, 6, 5) << 3 | BITS(c, 4, 2) << 6;   // C.LDSP, C.FLDSP
    uint32_t sp_w = BIT(c, 12) << 5 | BITS(c, 6, 4) << 2 | BITS(c, 3, 2) << 6;   // C.LWSP
    uint32_t ss_d = BITS(c, 12, 10) << 3 | BITS(c, 9, 7) << 6;                   // C.SDSP, C.FSDSP
    uint32_t ss_w = BITS(c, 12, 9) << 2 | BITS(c, 8, 7) << 6;                    // C.SWSP
    switch (f3) {
    case 0: return set(d, RV_SLLI, rd, rd, 0, BIT(c, 12) << 5 | BITS(c, 6, 2));
    case 1: return set(d, RV_FLD, rd, 2, 0, sp_d);
    case 2: if (!rd) return 0; return set(d, RV_LW, rd, 2, 0, sp_w);
    case 3: if (!rd) return 0; return set(d, RV_LD, rd, 2, 0, sp_d);
    case 4:
      if (!BIT(c, 12)) {
        if (!rs2) { if (!rd) return 0; return set(d, RV_JALR, 0, rd, 0, 0); }
        return set(d, RV_ADD, rd, 0, rs2, 0);
      }
      if (!rs2) {
        if (!rd) return set(d, RV_EBREAK, 0, 0, 0, 0);
        return set(d, RV_JALR, 1, rd, 0, 0);
      }
      return set(d, RV_ADD, rd, rd, rs2, 0);
    case 5: return set(d, RV_FSD, 0, 2, rs2, ss_d);
    case 6: return set(d, RV_SW, 0, 2, rs2, ss_w);
    default: return set(d, RV_SD, 0, 2, rs2, ss_d);
    }
  }
  return 0;
}

int rv_decode(uint32_t w, rv_insn *d) {
  d->rs3 = 0; d->rm = 0;
  if ((w & 3) != 3) return decode16(w & 0xffff, d) ? 2 : 0;
  d->len = 4;
  int opc = w & 0x7f, rd = BITS(w, 11, 7), f3 = BITS(w, 14, 12), rs1 = BITS(w, 19, 15), rs2 = BITS(w, 24, 20);
  int f7 = BITS(w, 31, 25);
  int64_t immI = sext(w >> 20, 12);
  int64_t immS = sext(BITS(w, 31, 25) << 5 | BITS(w, 11, 7), 12);
  int64_t immB = sext(BIT(w, 31) << 12 | BIT(w, 7) << 11 | BITS(w, 30, 25) << 5 | BITS(w, 11, 8) << 1, 13);
  int64_t immU = sext(w & 0xfffff000u, 32);
  int64_t immJ = sext(BIT(w, 31) << 20 | BITS(w, 19, 12) << 12 | BIT(w, 20) << 11 | BITS(w, 30, 21) << 1, 21);
  int r;
  switch (opc) {
  case 0x37: r = set(d, RV_LUI, rd, 0, 0, immU); break;
  case 0x17: r = set(d, RV_AUIPC, rd, 0, 0, immU); break;
  case 0x6f: r = set(d, RV_JAL, rd, 0, 0, immJ); break;
  case 0x67: r = f3 == 0 && set(d, RV_JALR, rd, rs1, 0, immI); break;
  case 0x63: {
    static const uint16_t ops[8] = { RV_BEQ, RV_BNE, 0, 0, RV_BLT, RV_BGE, RV_BLTU, RV_BGEU };
    r = set(d, ops[f3], 0, rs1, rs2, immB); break;
  }
  case 0x03: {
    static const uint16_t ops[8] = { RV_LB, RV_LH, RV_LW, RV_LD, RV_LBU, RV_LHU, RV_LWU, 0 };
    r = set(d, ops[f3], rd, rs1, 0, immI); break;
  }
  case 0x23: {
    static const uint16_t ops[8] = { RV_SB, RV_SH, RV_SW, RV_SD, 0, 0, 0, 0 };
    r = set(d, ops[f3], 0, rs1, rs2, immS); break;
  }
  case 0x13:
    switch (f3) {
    case 0: r = set(d, RV_ADDI, rd, rs1, 0, immI); break;
    case 2: r = set(d, RV_SLTI, rd, rs1, 0, immI); break;
    case 3: r = set(d, RV_SLTIU, rd, rs1, 0, immI); break;
    case 4: r = set(d, RV_XORI, rd, rs1, 0, immI); break;
    case 6: r = set(d, RV_ORI, rd, rs1, 0, immI); break;
    case 7: r = set(d, RV_ANDI, rd, rs1, 0, immI); break;
    case 1: r = (w >> 26) == 0 && set(d, RV_SLLI, rd, rs1, 0, BITS(w, 25, 20)); break;
    default:
      if ((w >> 26) == 0) r = set(d, RV_SRLI, rd, rs1, 0, BITS(w, 25, 20));
      else if ((w >> 26) == 0x10) r = set(d, RV_SRAI, rd, rs1, 0, BITS(w, 25, 20));
      else r = 0;
    }
    break;
  case 0x1b:
    if (f3 == 0) r = set(d, RV_ADDIW, rd, rs1, 0, immI);
    else if (f3 == 1 && f7 == 0) r = set(d, RV_SLLIW, rd, rs1, 0, rs2);
    else if (f3 == 5 && f7 == 0) r = set(d, RV_SRLIW, rd, rs1, 0, rs2);
    else if (f3 == 5 && f7 == 0x20) r = set(d, RV_SRAIW, rd, rs1, 0, rs2);
    else r = 0;
    break;
  case 0x33: {
    static const uint16_t base[8] = { RV_ADD, RV_SLL, RV_SLT, RV_SLTU, RV_XOR, RV_SRL, RV_OR, RV_AND };
    static const uint16_t mul[8] = { RV_MUL, RV_MULH, RV_MULHSU, RV_MULHU, RV_DIV, RV_DIVU, RV_REM, RV_REMU };
    if (f7 == 0) r = set(d, base[f3], rd, rs1, rs2, 0);
    else if (f7 == 1) r = set(d, mul[f3], rd, rs1, rs2, 0);
    else if (f7 == 0x20 && f3 == 0) r = set(d, RV_SUB, rd, rs1, rs2, 0);
    else if (f7 == 0x20 && f3 == 5) r = set(d, RV_SRA, rd, rs1, rs2, 0);
    else r = 0;
    break;
  }
  case 0x3b: {
    static const uint16_t base[8] = { RV_ADDW, RV_SLLW, 0, 0, 0, RV_SRLW, 0, 0 };
    static const uint16_t mul[8] = { RV_MULW, 0, 0, 0, RV_DIVW, RV_DIVUW, RV_REMW, RV_REMUW };
    if (f7 == 0) r = set(d, base[f3], rd, rs1, rs2, 0);
    else if (f7 == 1) r = set(d, mul[f3], rd, rs1, rs2, 0);
    else if (f7 == 0x20 && f3 == 0) r = set(d, RV_SUBW, rd, rs1, rs2, 0);
    else if (f7 == 0x20 && f3 == 5) r = set(d, RV_SRAW, rd, rs1, rs2, 0);
    else r = 0;
    break;
  }
  case 0x0f: r = f3 == 0 ? set(d, RV_FENCE, 0, 0, 0, 0) : f3 == 1 ? set(d, RV_FENCE_I, 0, 0, 0, 0) : 0; break;
  case 0x73:
    if (w == 0x00000073) r = set(d, RV_ECALL, 0, 0, 0, 0);
    else if (w == 0x00100073) r = set(d, RV_EBREAK, 0, 0, 0, 0);
    else {
      static const uint16_t ops[8] = { 0, RV_CSRRW, RV_CSRRS, RV_CSRRC, 0, RV_CSRRWI, RV_CSRRSI, RV_CSRRCI };
      r = set(d, ops[f3], rd, rs1, 0, w >> 20);
    }
    break;
  case 0x2f: {
    int f5 = w >> 27, op = 0;
    if (f3 != 2 && f3 != 3) { r = 0; break; }
    switch (f5) {
    case 0x02: op = rs2 == 0 ? RV_LR_W : 0; break;
    case 0x03: op = RV_SC_W; break;
    case 0x01: op = RV_AMOSWAP_W; break;
    case 0x00: op = RV_AMOADD_W; break;
    case 0x04: op = RV_AMOXOR_W; break;
    case 0x0c: op = RV_AMOAND_W; break;
    case 0x08: op = RV_AMOOR_W; break;
    case 0x10: op = RV_AMOMIN_W; break;
    case 0x14: op = RV_AMOMAX_W; break;
    case 0x18: op = RV_AMOMINU_W; break;
    case 0x1c: op = RV_AMOMAXU_W; break;
    }
    if (op && f3 == 3) op += RV_LR_D - RV_LR_W;
    r = set(d, op, rd, rs1, rs2, 0);
    break;
  }
  case 0x07: r = f3 == 2 ? set(d, RV_FLW, rd, rs1, 0, immI) : f3 == 3 ? set(d, RV_FLD, rd, rs1, 0, immI) : 0; break;
  case 0x27: r = f3 == 2 ? set(d, RV_FSW, 0, rs1, rs2, immS) : f3 == 3 ? set(d, RV_FSD, 0, rs1, rs2, immS) : 0; break;
  case 0x43: case 0x47: case 0x4b: case 0x4f: {
    int fmt = BITS(w, 26, 25);
    if (fmt > 1) { r = 0; break; }
    int op = (fmt ? RV_FMADD_D : RV_FMADD_S) + ((opc - 0x43) >> 2);
    r = set(d, op, rd, rs1, rs2, 0);
    d->rs3 = (uint8_t)(w >> 27);
    d->rm = (uint8_t)f3;
    break;
  }
  case 0x53: {
    int op = 0;
    d->rm = (uint8_t)f3;
    switch (f7) {
    case 0x00: op = RV_FADD_S; break;  case 0x01: op = RV_FADD_D; break;
    case 0x04: op = RV_FSUB_S; break;  case 0x05: op = RV_FSUB_D; break;
    case 0x08: op = RV_FMUL_S; break;  case 0x09: op = RV_FMUL_D; break;
    case 0x0c: op = RV_FDIV_S; break;  case 0x0d: op = RV_FDIV_D; break;
    case 0x2c: op = rs2 == 0 ? RV_FSQRT_S : 0; break;
    case 0x2d: op = rs2 == 0 ? RV_FSQRT_D : 0; break;
    case 0x10: op = f3 < 3 ? RV_FSGNJ_S + f3 : 0; break;
    case 0x11: op = f3 < 3 ? RV_FSGNJ_D + f3 : 0; break;
    case 0x14: op = f3 < 2 ? RV_FMIN_S + f3 : 0; break;
    case 0x15: op = f3 < 2 ? RV_FMIN_D + f3 : 0; break;
    case 0x20: op = rs2 == 1 ? RV_FCVT_S_D : 0; break;
    case 0x21: op = rs2 == 0 ? RV_FCVT_D_S : 0; break;
    case 0x50: op = f3 == 2 ? RV_FEQ_S : f3 == 1 ? RV_FLT_S : f3 == 0 ? RV_FLE_S : 0; break;
    case 0x51: op = f3 == 2 ? RV_FEQ_D : f3 == 1 ? RV_FLT_D : f3 == 0 ? RV_FLE_D : 0; break;
    case 0x60: op = rs2 < 4 ? RV_FCVT_W_S + rs2 : 0; break;
    case 0x61: op = rs2 < 4 ? RV_FCVT_W_D + rs2 : 0; break;
    case 0x68: op = rs2 < 4 ? RV_FCVT_S_W + rs2 : 0; break;
    case 0x69: op = rs2 < 4 ? RV_FCVT_D_W + rs2 : 0; break;
    case 0x70: op = rs2 ? 0 : f3 == 0 ? RV_FMV_X_W : f3 == 1 ? RV_FCLASS_S : 0; break;
    case 0x71: op = rs2 ? 0 : f3 == 0 ? RV_FMV_X_D : f3 == 1 ? RV_FCLASS_D : 0; break;
    case 0x78: op = rs2 == 0 && f3 == 0 ? RV_FMV_W_X : 0; break;
    case 0x79: op = rs2 == 0 && f3 == 0 ? RV_FMV_D_X : 0; break;
    }
    r = set(d, op, rd, rs1, rs2, 0);
    d->rm = (uint8_t)f3;
    break;
  }
  default: r = 0;
  }
  return r ? 4 : 0;
}
