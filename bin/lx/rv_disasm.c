// SPDX-License-Identifier: GPL-2.0-or-later
/* Disassembler for decoded RV64GC instructions. Output uses canonical mnemonics (no
 * aliases) and x/f register numbers, in a syntax the LLVM assembler accepts, so the
 * decoder can be tested by round trip (bin/lx/tests/decode_test.sh). */
#include "rv.h"
#include "lxlib.h"

static const char *const rmname[8] = {"rne", "rtz", "rdn", "rup", "rmm", "rm5", "rm6", "dyn"};

static void lower(const char *s, char *d) {
  for (; *s; s++, d++) *d = *s == '_' ? '.' : (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
  *d = 0;
}

void rv_disasm(const struct rv_insn *in, uint64_t pc, char *buf, unsigned cap) {
  char m[24];
  (void)pc;
  lower(rv_op_name[in->op], m);
  int op = in->op;
  long long imm = (long long)in->imm;
  unsigned fl = rv_op_flags[op];
  if (op == RV_ILLEGAL) { lx_snprintf(buf, cap, "unknown"); return; }
  if (op == RV_LUI || op == RV_AUIPC) { lx_snprintf(buf, cap, "%s x%d, %lld", m, in->rd, (imm >> 12) & 0xfffff); return; }
  if (op == RV_JAL) { lx_snprintf(buf, cap, "%s x%d, %lld", m, in->rd, imm); return; }
  if (op == RV_JALR) { lx_snprintf(buf, cap, "%s x%d, %lld(x%d)", m, in->rd, imm, in->rs1); return; }
  if (fl & RVF_BRANCH) { lx_snprintf(buf, cap, "%s x%d, x%d, %lld", m, in->rs1, in->rs2, imm); return; }
  if (op == RV_FENCE) { lx_snprintf(buf, cap, "fence"); return; }
  if (op == RV_FENCE_I || op == RV_ECALL || op == RV_EBREAK) { lx_snprintf(buf, cap, "%s", m); return; }
  if (fl & RVF_CSR) {
    if (op >= RV_CSRRWI) lx_snprintf(buf, cap, "%s x%d, %lld, %d", m, in->rd, imm, in->rs1);
    else lx_snprintf(buf, cap, "%s x%d, %lld, x%d", m, in->rd, imm, in->rs1);
    return;
  }
  if (fl & RVF_ATOMIC) {
    const char *o = in->rm == 3 ? ".aqrl" : in->rm == 2 ? ".aq" : in->rm == 1 ? ".rl" : "";
    if (op == RV_LR_W || op == RV_LR_D) lx_snprintf(buf, cap, "%s%s x%d, (x%d)", m, o, in->rd, in->rs1);
    else lx_snprintf(buf, cap, "%s%s x%d, x%d, (x%d)", m, o, in->rd, in->rs2, in->rs1);
    return;
  }
  if (fl & RVF_LOAD) { lx_snprintf(buf, cap, "%s %c%d, %lld(x%d)", m, (fl & RVF_FP) ? 'f' : 'x', in->rd, imm, in->rs1); return; }
  if (fl & RVF_STORE) { lx_snprintf(buf, cap, "%s %c%d, %lld(x%d)", m, (fl & RVF_FP) ? 'f' : 'x', in->rs2, imm, in->rs1); return; }
  if (fl & RVF_FP) {
    const char *rm = rmname[in->rm & 7];
    switch (op) {
    case RV_FMADD_S: case RV_FMSUB_S: case RV_FNMSUB_S: case RV_FNMADD_S:
    case RV_FMADD_D: case RV_FMSUB_D: case RV_FNMSUB_D: case RV_FNMADD_D:
      lx_snprintf(buf, cap, "%s f%d, f%d, f%d, f%d, %s", m, in->rd, in->rs1, in->rs2, in->rs3, rm); return;
    case RV_FADD_S: case RV_FSUB_S: case RV_FMUL_S: case RV_FDIV_S:
    case RV_FADD_D: case RV_FSUB_D: case RV_FMUL_D: case RV_FDIV_D:
      lx_snprintf(buf, cap, "%s f%d, f%d, f%d, %s", m, in->rd, in->rs1, in->rs2, rm); return;
    case RV_FSQRT_S: case RV_FSQRT_D: case RV_FCVT_S_D: case RV_FCVT_D_S:
      lx_snprintf(buf, cap, "%s f%d, f%d, %s", m, in->rd, in->rs1, rm); return;
    case RV_FSGNJ_S: case RV_FSGNJN_S: case RV_FSGNJX_S: case RV_FMIN_S: case RV_FMAX_S:
    case RV_FSGNJ_D: case RV_FSGNJN_D: case RV_FSGNJX_D: case RV_FMIN_D: case RV_FMAX_D:
      lx_snprintf(buf, cap, "%s f%d, f%d, f%d", m, in->rd, in->rs1, in->rs2); return;
    case RV_FEQ_S: case RV_FLT_S: case RV_FLE_S: case RV_FEQ_D: case RV_FLT_D: case RV_FLE_D:
      lx_snprintf(buf, cap, "%s x%d, f%d, f%d", m, in->rd, in->rs1, in->rs2); return;
    case RV_FCVT_W_S: case RV_FCVT_WU_S: case RV_FCVT_L_S: case RV_FCVT_LU_S:
    case RV_FCVT_W_D: case RV_FCVT_WU_D: case RV_FCVT_L_D: case RV_FCVT_LU_D:
      lx_snprintf(buf, cap, "%s x%d, f%d, %s", m, in->rd, in->rs1, rm); return;
    case RV_FCVT_S_W: case RV_FCVT_S_WU: case RV_FCVT_S_L: case RV_FCVT_S_LU:
    case RV_FCVT_D_W: case RV_FCVT_D_WU: case RV_FCVT_D_L: case RV_FCVT_D_LU:
      lx_snprintf(buf, cap, "%s f%d, x%d, %s", m, in->rd, in->rs1, rm); return;
    case RV_FMV_X_W: case RV_FMV_X_D: case RV_FCLASS_S: case RV_FCLASS_D:
      lx_snprintf(buf, cap, "%s x%d, f%d", m, in->rd, in->rs1); return;
    case RV_FMV_W_X: case RV_FMV_D_X:
      lx_snprintf(buf, cap, "%s f%d, x%d", m, in->rd, in->rs1); return;
    }
  }
  switch (op) {
  case RV_ADDI: case RV_SLTI: case RV_SLTIU: case RV_XORI: case RV_ORI: case RV_ANDI:
  case RV_SLLI: case RV_SRLI: case RV_SRAI: case RV_ADDIW: case RV_SLLIW: case RV_SRLIW: case RV_SRAIW:
  case RV_SLLI_UW: case RV_RORI: case RV_RORIW: case RV_BCLRI: case RV_BEXTI: case RV_BINVI: case RV_BSETI:
    lx_snprintf(buf, cap, "%s x%d, x%d, %lld", m, in->rd, in->rs1, imm); return;
  case RV_CLZ: case RV_CTZ: case RV_CPOP: case RV_CLZW: case RV_CTZW: case RV_CPOPW:
  case RV_SEXT_B: case RV_SEXT_H: case RV_ZEXT_H: case RV_ORC_B: case RV_REV8:
    lx_snprintf(buf, cap, "%s x%d, x%d", m, in->rd, in->rs1); return;
  }
  lx_snprintf(buf, cap, "%s x%d, x%d, x%d", m, in->rd, in->rs1, in->rs2);
}
