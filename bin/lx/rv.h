// SPDX-License-Identifier: GPL-2.0-or-later
/* RISC-V (RV64GC) front end: CPU state and instruction decoder.
 *
 * This header is the contract between the decoder, the reference interpreter and the
 * binary translator. It depends on nothing but <stdint.h>.
 *
 * The decoder is pure: it turns one instruction (16 or 32 bits) into a struct rv_insn
 * and never touches memory or CPU state. Compressed instructions come out as the base
 * instruction they expand to, so a consumer handles only base opcodes. */
#ifndef LX_RV_H
#define LX_RV_H
#include <stdint.h>

/* Every operation the decoder can produce. Columns: name, format letter for the
 * disassembler (R reg-reg, I reg-imm, L load, S store, B branch, U upper, J jump,
 * F float, A atomic, C csr, X special), flags (see RVF_*). */
#define RV_OPS(X) \
  X(ILLEGAL, X, 0) \
  /* RV64I */ \
  X(LUI, U, 0) X(AUIPC, U, 0) X(JAL, J, RVF_JUMP) X(JALR, I, RVF_JUMP) \
  X(BEQ, B, RVF_BRANCH) X(BNE, B, RVF_BRANCH) X(BLT, B, RVF_BRANCH) X(BGE, B, RVF_BRANCH) \
  X(BLTU, B, RVF_BRANCH) X(BGEU, B, RVF_BRANCH) \
  X(LB, L, RVF_LOAD) X(LH, L, RVF_LOAD) X(LW, L, RVF_LOAD) X(LD, L, RVF_LOAD) \
  X(LBU, L, RVF_LOAD) X(LHU, L, RVF_LOAD) X(LWU, L, RVF_LOAD) \
  X(SB, S, RVF_STORE) X(SH, S, RVF_STORE) X(SW, S, RVF_STORE) X(SD, S, RVF_STORE) \
  X(ADDI, I, 0) X(SLTI, I, 0) X(SLTIU, I, 0) X(XORI, I, 0) X(ORI, I, 0) X(ANDI, I, 0) \
  X(SLLI, I, 0) X(SRLI, I, 0) X(SRAI, I, 0) \
  X(ADD, R, 0) X(SUB, R, 0) X(SLL, R, 0) X(SLT, R, 0) X(SLTU, R, 0) X(XOR, R, 0) \
  X(SRL, R, 0) X(SRA, R, 0) X(OR, R, 0) X(AND, R, 0) \
  X(ADDIW, I, 0) X(SLLIW, I, 0) X(SRLIW, I, 0) X(SRAIW, I, 0) \
  X(ADDW, R, 0) X(SUBW, R, 0) X(SLLW, R, 0) X(SRLW, R, 0) X(SRAW, R, 0) \
  X(FENCE, X, 0) X(FENCE_I, X, RVF_SYSTEM) X(ECALL, X, RVF_SYSTEM) X(EBREAK, X, RVF_SYSTEM) \
  /* Zicsr */ \
  X(CSRRW, C, RVF_CSR) X(CSRRS, C, RVF_CSR) X(CSRRC, C, RVF_CSR) \
  X(CSRRWI, C, RVF_CSR) X(CSRRSI, C, RVF_CSR) X(CSRRCI, C, RVF_CSR) \
  /* M */ \
  X(MUL, R, 0) X(MULH, R, 0) X(MULHSU, R, 0) X(MULHU, R, 0) \
  X(DIV, R, 0) X(DIVU, R, 0) X(REM, R, 0) X(REMU, R, 0) \
  X(MULW, R, 0) X(DIVW, R, 0) X(DIVUW, R, 0) X(REMW, R, 0) X(REMUW, R, 0) \
  /* A (aq/rl bits in rm: bit 1 aq, bit 0 rl) */ \
  X(LR_W, A, RVF_ATOMIC) X(SC_W, A, RVF_ATOMIC) X(AMOSWAP_W, A, RVF_ATOMIC) X(AMOADD_W, A, RVF_ATOMIC) \
  X(AMOXOR_W, A, RVF_ATOMIC) X(AMOAND_W, A, RVF_ATOMIC) X(AMOOR_W, A, RVF_ATOMIC) \
  X(AMOMIN_W, A, RVF_ATOMIC) X(AMOMAX_W, A, RVF_ATOMIC) X(AMOMINU_W, A, RVF_ATOMIC) X(AMOMAXU_W, A, RVF_ATOMIC) \
  X(LR_D, A, RVF_ATOMIC) X(SC_D, A, RVF_ATOMIC) X(AMOSWAP_D, A, RVF_ATOMIC) X(AMOADD_D, A, RVF_ATOMIC) \
  X(AMOXOR_D, A, RVF_ATOMIC) X(AMOAND_D, A, RVF_ATOMIC) X(AMOOR_D, A, RVF_ATOMIC) \
  X(AMOMIN_D, A, RVF_ATOMIC) X(AMOMAX_D, A, RVF_ATOMIC) X(AMOMINU_D, A, RVF_ATOMIC) X(AMOMAXU_D, A, RVF_ATOMIC) \
  /* F and D. rm holds the rounding mode field (7 = dynamic). */ \
  X(FLW, L, RVF_LOAD | RVF_FP) X(FSW, S, RVF_STORE | RVF_FP) X(FLD, L, RVF_LOAD | RVF_FP) X(FSD, S, RVF_STORE | RVF_FP) \
  X(FMADD_S, F, RVF_FP) X(FMSUB_S, F, RVF_FP) X(FNMSUB_S, F, RVF_FP) X(FNMADD_S, F, RVF_FP) \
  X(FADD_S, F, RVF_FP) X(FSUB_S, F, RVF_FP) X(FMUL_S, F, RVF_FP) X(FDIV_S, F, RVF_FP) X(FSQRT_S, F, RVF_FP) \
  X(FSGNJ_S, F, RVF_FP) X(FSGNJN_S, F, RVF_FP) X(FSGNJX_S, F, RVF_FP) X(FMIN_S, F, RVF_FP) X(FMAX_S, F, RVF_FP) \
  X(FCVT_W_S, F, RVF_FP) X(FCVT_WU_S, F, RVF_FP) X(FCVT_L_S, F, RVF_FP) X(FCVT_LU_S, F, RVF_FP) \
  X(FMV_X_W, F, RVF_FP) X(FEQ_S, F, RVF_FP) X(FLT_S, F, RVF_FP) X(FLE_S, F, RVF_FP) X(FCLASS_S, F, RVF_FP) \
  X(FCVT_S_W, F, RVF_FP) X(FCVT_S_WU, F, RVF_FP) X(FCVT_S_L, F, RVF_FP) X(FCVT_S_LU, F, RVF_FP) X(FMV_W_X, F, RVF_FP) \
  X(FMADD_D, F, RVF_FP) X(FMSUB_D, F, RVF_FP) X(FNMSUB_D, F, RVF_FP) X(FNMADD_D, F, RVF_FP) \
  X(FADD_D, F, RVF_FP) X(FSUB_D, F, RVF_FP) X(FMUL_D, F, RVF_FP) X(FDIV_D, F, RVF_FP) X(FSQRT_D, F, RVF_FP) \
  X(FSGNJ_D, F, RVF_FP) X(FSGNJN_D, F, RVF_FP) X(FSGNJX_D, F, RVF_FP) X(FMIN_D, F, RVF_FP) X(FMAX_D, F, RVF_FP) \
  X(FCVT_S_D, F, RVF_FP) X(FCVT_D_S, F, RVF_FP) \
  X(FCVT_W_D, F, RVF_FP) X(FCVT_WU_D, F, RVF_FP) X(FCVT_L_D, F, RVF_FP) X(FCVT_LU_D, F, RVF_FP) \
  X(FMV_X_D, F, RVF_FP) X(FEQ_D, F, RVF_FP) X(FLT_D, F, RVF_FP) X(FLE_D, F, RVF_FP) X(FCLASS_D, F, RVF_FP) \
  X(FCVT_D_W, F, RVF_FP) X(FCVT_D_WU, F, RVF_FP) X(FCVT_D_L, F, RVF_FP) X(FCVT_D_LU, F, RVF_FP) X(FMV_D_X, F, RVF_FP) \
  /* Zba, Zbb, Zbs (bit manipulation; not in RV64GC, decoded only if enabled) */ \
  X(SH1ADD, R, RVF_B) X(SH2ADD, R, RVF_B) X(SH3ADD, R, RVF_B) X(ADD_UW, R, RVF_B) \
  X(SH1ADD_UW, R, RVF_B) X(SH2ADD_UW, R, RVF_B) X(SH3ADD_UW, R, RVF_B) X(SLLI_UW, I, RVF_B) \
  X(ANDN, R, RVF_B) X(ORN, R, RVF_B) X(XNOR, R, RVF_B) X(CLZ, R, RVF_B) X(CTZ, R, RVF_B) X(CPOP, R, RVF_B) \
  X(CLZW, R, RVF_B) X(CTZW, R, RVF_B) X(CPOPW, R, RVF_B) X(MAX, R, RVF_B) X(MAXU, R, RVF_B) X(MIN, R, RVF_B) \
  X(MINU, R, RVF_B) X(SEXT_B, R, RVF_B) X(SEXT_H, R, RVF_B) X(ZEXT_H, R, RVF_B) X(ROL, R, RVF_B) X(ROLW, R, RVF_B) \
  X(ROR, R, RVF_B) X(RORI, I, RVF_B) X(RORIW, I, RVF_B) X(RORW, R, RVF_B) X(ORC_B, R, RVF_B) X(REV8, R, RVF_B) \
  X(BCLR, R, RVF_B) X(BCLRI, I, RVF_B) X(BEXT, R, RVF_B) X(BEXTI, I, RVF_B) X(BINV, R, RVF_B) X(BINVI, I, RVF_B) \
  X(BSET, R, RVF_B) X(BSETI, I, RVF_B)

#define RVF_JUMP 1     /* unconditional control transfer */
#define RVF_BRANCH 2   /* conditional branch */
#define RVF_LOAD 4
#define RVF_STORE 8
#define RVF_SYSTEM 16  /* ends a block: ecall, ebreak, fence.i */
#define RVF_CSR 32
#define RVF_ATOMIC 64
#define RVF_FP 128
#define RVF_B 256

enum rv_op {
#define RV_ENUM(n, f, fl) RV_##n,
  RV_OPS(RV_ENUM)
#undef RV_ENUM
  RV_NOPS
};

/* One decoded instruction. Register numbers are architectural (0..31; for FP ops the
 * f registers). imm is sign-extended to 64 bits where the ISA says so. For CSR ops imm
 * is the CSR number and rs1 is the 5-bit immediate of the *I forms. len is 2 or 4. */
struct rv_insn {
  uint16_t op;    /* enum rv_op */
  uint8_t rd, rs1, rs2, rs3;
  uint8_t rm;     /* FP rounding mode, or aq/rl for atomics */
  uint8_t len;
  int64_t imm;
};

/* Extensions the decoder accepts beyond RV64GC. */
#define RV_EXT_ZBA 1
#define RV_EXT_ZBB 2
#define RV_EXT_ZBS 4
extern unsigned rv_decode_ext; /* default 0: strict RV64GC */

/* Decode the instruction whose low 16 bits are `lo` and high 16 bits `hi` (hi is
 * ignored for a compressed instruction, so a caller at the end of a mapping can pass 0
 * and look at the returned length). Returns len (2 or 4); op is RV_ILLEGAL for an
 * encoding that is not a valid user-mode RV64GC instruction (len still says how many
 * bytes the encoding claims). */
int rv_decode(uint32_t raw, struct rv_insn *out);
static inline int rv_insn_len(uint16_t lo) { return (lo & 3) == 3 ? 4 : 2; }

extern const char *const rv_op_name[RV_NOPS];
extern const uint16_t rv_op_flags[RV_NOPS];
/* Disassemble into buf (at least 64 bytes), for traces and tests. */
void rv_disasm(const struct rv_insn *in, uint64_t pc, char *buf, unsigned cap);

/* ---- CPU state ----
 * Layout is fixed and documented in docs/LINUX-BINARIES.md: a translator may address
 * these fields directly from generated code. x[0] reads as zero and is never written by
 * the interpreter (writes to x0 go to x[32], a sink). f[] holds raw 64-bit patterns;
 * single-precision values are NaN-boxed (upper 32 bits all ones) as the ISA requires. */
struct rv_cpu {
  uint64_t x[33];     /* offset 0; x[32] is the write sink for rd = x0 */
  uint64_t pc;        /* offset 264 */
  uint64_t f[32];     /* offset 272 */
  uint32_t fcsr;      /* offset 528: frm in bits 7:5, fflags in bits 4:0 */
  uint32_t pad0;
  uint64_t res_addr;  /* LR/SC reservation: address, or ~0 when none */
  uint64_t res_val;   /* value loaded by LR (SC succeeds only if memory still holds it) */
  uint64_t instret;   /* retired instructions, maintained per block */
};
#define RV_CPU_OFF_PC 264
#define RV_CPU_OFF_F 272
#define RV_CPU_OFF_FCSR 528

/* Linux riscv64 ABI facts the system call layer and loader need. */
#define RV_REG_RA 1
#define RV_REG_SP 2
#define RV_REG_GP 3
#define RV_REG_TP 4
#define RV_REG_A0 10
#define RV_REG_A7 17
#define RV_EM_RISCV 243

#endif
