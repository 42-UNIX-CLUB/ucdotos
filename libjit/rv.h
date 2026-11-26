// SPDX-License-Identifier: GPL-2.0-or-later
// RISC-V RV64GC: decoded instruction, CPU state, decoder. This is the translator's own
// small decoder (the engine's full one lives in the core engine; see
// docs/BINARY-TRANSLATION.md section 7 for how the two meet).
// Reference: The RISC-V Instruction Set Manual, Volume I (unprivileged), 2019-12 and later.
#ifndef JIT_RV_H
#define JIT_RV_H
#include <stdint.h>

// Linked into the core engine, which has its own decoder: keep the names apart.
#ifdef JIT_RENAME
#define rv_decode xj_rv_decode
#define rv_names xj_rv_names
#endif

enum rv_op {
  RV_ILLEGAL = 0,
  RV_LUI, RV_AUIPC, RV_JAL, RV_JALR,
  RV_BEQ, RV_BNE, RV_BLT, RV_BGE, RV_BLTU, RV_BGEU,
  RV_LB, RV_LH, RV_LW, RV_LD, RV_LBU, RV_LHU, RV_LWU,
  RV_SB, RV_SH, RV_SW, RV_SD,
  RV_ADDI, RV_SLTI, RV_SLTIU, RV_XORI, RV_ORI, RV_ANDI, RV_SLLI, RV_SRLI, RV_SRAI,
  RV_ADD, RV_SUB, RV_SLL, RV_SLT, RV_SLTU, RV_XOR, RV_SRL, RV_SRA, RV_OR, RV_AND,
  RV_ADDIW, RV_SLLIW, RV_SRLIW, RV_SRAIW, RV_ADDW, RV_SUBW, RV_SLLW, RV_SRLW, RV_SRAW,
  RV_FENCE, RV_FENCE_I, RV_ECALL, RV_EBREAK,
  RV_CSRRW, RV_CSRRS, RV_CSRRC, RV_CSRRWI, RV_CSRRSI, RV_CSRRCI,
  // M
  RV_MUL, RV_MULH, RV_MULHSU, RV_MULHU, RV_DIV, RV_DIVU, RV_REM, RV_REMU,
  RV_MULW, RV_DIVW, RV_DIVUW, RV_REMW, RV_REMUW,
  // A (W then D forms)
  RV_LR_W, RV_SC_W, RV_AMOSWAP_W, RV_AMOADD_W, RV_AMOXOR_W, RV_AMOAND_W, RV_AMOOR_W,
  RV_AMOMIN_W, RV_AMOMAX_W, RV_AMOMINU_W, RV_AMOMAXU_W,
  RV_LR_D, RV_SC_D, RV_AMOSWAP_D, RV_AMOADD_D, RV_AMOXOR_D, RV_AMOAND_D, RV_AMOOR_D,
  RV_AMOMIN_D, RV_AMOMAX_D, RV_AMOMINU_D, RV_AMOMAXU_D,
  // F and D (S then D forms where both exist)
  RV_FLW, RV_FLD, RV_FSW, RV_FSD,
  RV_FMADD_S, RV_FMSUB_S, RV_FNMSUB_S, RV_FNMADD_S, RV_FMADD_D, RV_FMSUB_D, RV_FNMSUB_D, RV_FNMADD_D,
  RV_FADD_S, RV_FSUB_S, RV_FMUL_S, RV_FDIV_S, RV_FSQRT_S, RV_FSGNJ_S, RV_FSGNJN_S, RV_FSGNJX_S, RV_FMIN_S, RV_FMAX_S,
  RV_FADD_D, RV_FSUB_D, RV_FMUL_D, RV_FDIV_D, RV_FSQRT_D, RV_FSGNJ_D, RV_FSGNJN_D, RV_FSGNJX_D, RV_FMIN_D, RV_FMAX_D,
  RV_FCVT_S_D, RV_FCVT_D_S,
  RV_FEQ_S, RV_FLT_S, RV_FLE_S, RV_FEQ_D, RV_FLT_D, RV_FLE_D,
  RV_FCVT_W_S, RV_FCVT_WU_S, RV_FCVT_L_S, RV_FCVT_LU_S, RV_FCVT_W_D, RV_FCVT_WU_D, RV_FCVT_L_D, RV_FCVT_LU_D,
  RV_FCVT_S_W, RV_FCVT_S_WU, RV_FCVT_S_L, RV_FCVT_S_LU, RV_FCVT_D_W, RV_FCVT_D_WU, RV_FCVT_D_L, RV_FCVT_D_LU,
  RV_FMV_X_W, RV_FCLASS_S, RV_FMV_X_D, RV_FCLASS_D, RV_FMV_W_X, RV_FMV_D_X,
  RV_NOPS
};

typedef struct {
  uint16_t op;
  uint8_t rd, rs1, rs2, rs3;
  uint8_t len;        // 2 (compressed) or 4
  uint8_t rm;         // rounding mode field of F/D instructions
  int64_t imm;        // immediate; CSR number for CSR instructions
} rv_insn;

// Decode the instruction whose first 16 or 32 bits are `raw` (pass 32 bits; only the low
// 16 are looked at when the instruction is compressed). Returns its length, 0 if illegal.
int rv_decode(uint32_t raw, rv_insn *d);
extern const char *const rv_names[RV_NOPS];

// The guest CPU state, at a fixed address in the engine's memory. Translated code reads
// and writes these fields with constant offsets, so the layout is part of the contract.
typedef struct rv_cpu {
  uint64_t x[32];       // 0    integer registers; x[0] stays 0
  uint64_t pc;          // 256
  uint64_t f[32];       // 264  floating-point registers (single precision values NaN-boxed)
  uint32_t fcsr;        // 520  frm << 5 | fflags
  int32_t budget;       // 524  back edges and calls left before the next time-slice check
  uint64_t res_addr;    // 528  LR/SC reservation (-1: none)
  uint64_t icount;      // 536  retired instructions (interpreter; translated code in lockstep mode)
  uint64_t fault_addr;  // 544  address of the last memory fault
  uint64_t exit_code;   // 552
  uint32_t halted;      // 560
  uint32_t pad;
  uint64_t site;        // 568  pc of the indirect jump whose target is being looked up (0: none)
} rv_cpu;
#define CPU_OFF_X(r) (8u * (r))
#define CPU_OFF_PC 256u
#define CPU_OFF_F(r) (264u + 8u * (r))
#define CPU_OFF_FCSR 520u
#define CPU_OFF_BUDGET 524u
#define CPU_OFF_RES 528u
#define CPU_OFF_ICOUNT 536u
#define CPU_OFF_FAULT 544u
#define CPU_OFF_SITE 568u
#endif
