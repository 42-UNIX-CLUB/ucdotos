// SPDX-License-Identifier: GPL-2.0-or-later
/* The interpreter's instruction handlers, written once. rv_interp.c includes this file
 * twice: inside a switch (OP(x) = case x:) and as one function per instruction for
 * tail-call threading (OP(x) = a handler function). Terminators: NEXT (next op in the
 * block), TAKEN/FALL/INDIRECT (leave the block), EXIT, FAULT, ILLEGAL, FENCEI. */
    OP(OP_NOP) { NEXT; }
    OP(OP_LI) { X[o->rd] = (uint64_t)(int64_t)o->imm; NEXT; }
    OP(OP_MV) { X[o->rd] = X[o->rs1]; NEXT; }
    OP(OP_AUIPC_ADDI) { X[o->rd] = (uint64_t)((int64_t)o->pc + o->imm); NEXT; }
    OP(RV_LUI) { X[o->rd] = (uint64_t)(int64_t)o->imm; NEXT; }
    OP(RV_AUIPC) { X[o->rd] = (uint64_t)((int64_t)o->pc + o->imm); NEXT; }
    OP(RV_ADDI) { X[o->rd] = X[o->rs1] + (uint64_t)(int64_t)o->imm; NEXT; }
    OP(RV_SLTI) { X[o->rd] = (int64_t)X[o->rs1] < (int64_t)o->imm; NEXT; }
    OP(RV_SLTIU) { X[o->rd] = X[o->rs1] < (uint64_t)(int64_t)o->imm; NEXT; }
    OP(RV_XORI) { X[o->rd] = X[o->rs1] ^ (uint64_t)(int64_t)o->imm; NEXT; }
    OP(RV_ORI) { X[o->rd] = X[o->rs1] | (uint64_t)(int64_t)o->imm; NEXT; }
    OP(RV_ANDI) { X[o->rd] = X[o->rs1] & (uint64_t)(int64_t)o->imm; NEXT; }
    OP(RV_SLLI) { X[o->rd] = X[o->rs1] << (o->imm & 63); NEXT; }
    OP(RV_SRLI) { X[o->rd] = X[o->rs1] >> (o->imm & 63); NEXT; }
    OP(RV_SRAI) { X[o->rd] = (uint64_t)((int64_t)X[o->rs1] >> (o->imm & 63)); NEXT; }
    OP(RV_ADD) { X[o->rd] = X[o->rs1] + X[o->rs2]; NEXT; }
    OP(RV_SUB) { X[o->rd] = X[o->rs1] - X[o->rs2]; NEXT; }
    OP(RV_SLL) { X[o->rd] = X[o->rs1] << (X[o->rs2] & 63); NEXT; }
    OP(RV_SLT) { X[o->rd] = (int64_t)X[o->rs1] < (int64_t)X[o->rs2]; NEXT; }
    OP(RV_SLTU) { X[o->rd] = X[o->rs1] < X[o->rs2]; NEXT; }
    OP(RV_XOR) { X[o->rd] = X[o->rs1] ^ X[o->rs2]; NEXT; }
    OP(RV_SRL) { X[o->rd] = X[o->rs1] >> (X[o->rs2] & 63); NEXT; }
    OP(RV_SRA) { X[o->rd] = (uint64_t)((int64_t)X[o->rs1] >> (X[o->rs2] & 63)); NEXT; }
    OP(RV_OR) { X[o->rd] = X[o->rs1] | X[o->rs2]; NEXT; }
    OP(RV_AND) { X[o->rd] = X[o->rs1] & X[o->rs2]; NEXT; }
    OP(RV_ADDIW) { X[o->rd] = SX32(X[o->rs1] + (uint64_t)(int64_t)o->imm); NEXT; }
    OP(RV_SLLIW) { X[o->rd] = SX32((uint32_t)X[o->rs1] << (o->imm & 31)); NEXT; }
    OP(RV_SRLIW) { X[o->rd] = SX32((uint32_t)X[o->rs1] >> (o->imm & 31)); NEXT; }
    OP(RV_SRAIW) { X[o->rd] = (uint64_t)(int64_t)((int32_t)X[o->rs1] >> (o->imm & 31)); NEXT; }
    OP(RV_ADDW) { X[o->rd] = SX32(X[o->rs1] + X[o->rs2]); NEXT; }
    OP(RV_SUBW) { X[o->rd] = SX32(X[o->rs1] - X[o->rs2]); NEXT; }
    OP(RV_SLLW) { X[o->rd] = SX32((uint32_t)X[o->rs1] << (X[o->rs2] & 31)); NEXT; }
    OP(RV_SRLW) { X[o->rd] = SX32((uint32_t)X[o->rs1] >> (X[o->rs2] & 31)); NEXT; }
    OP(RV_SRAW) { X[o->rd] = (uint64_t)(int64_t)((int32_t)X[o->rs1] >> (X[o->rs2] & 31)); NEXT; }

    /* M */
    OP(RV_MUL) { X[o->rd] = X[o->rs1] * X[o->rs2]; NEXT; }
    OP(RV_MULH) { X[o->rd] = (uint64_t)mulh64((int64_t)X[o->rs1], (int64_t)X[o->rs2]); NEXT; }
    OP(RV_MULHSU) { X[o->rd] = (uint64_t)mulhsu64((int64_t)X[o->rs1], X[o->rs2]); NEXT; }
    OP(RV_MULHU) { X[o->rd] = mulhu64(X[o->rs1], X[o->rs2]); NEXT; }
    OP(RV_DIV) {
      int64_t a = (int64_t)X[o->rs1], d = (int64_t)X[o->rs2];
      X[o->rd] = d == 0 ? ~0ull : (a == INT64_MIN && d == -1) ? (uint64_t)a : (uint64_t)(a / d);
      NEXT;
    }
    OP(RV_DIVU) { { uint64_t a = X[o->rs1], d = X[o->rs2]; X[o->rd] = d ? a / d : ~0ull; NEXT; } }
    OP(RV_REM) {
      int64_t a = (int64_t)X[o->rs1], d = (int64_t)X[o->rs2];
      X[o->rd] = d == 0 ? (uint64_t)a : (a == INT64_MIN && d == -1) ? 0 : (uint64_t)(a % d);
      NEXT;
    }
    OP(RV_REMU) { { uint64_t a = X[o->rs1], d = X[o->rs2]; X[o->rd] = d ? a % d : a; NEXT; } }
    OP(RV_MULW) { X[o->rd] = SX32((uint32_t)X[o->rs1] * (uint32_t)X[o->rs2]); NEXT; }
    OP(RV_DIVW) {
      int32_t a = (int32_t)X[o->rs1], d = (int32_t)X[o->rs2];
      X[o->rd] = d == 0 ? ~0ull : (a == INT32_MIN && d == -1) ? SX32(a) : SX32(a / d);
      NEXT;
    }
    OP(RV_DIVUW) { { uint32_t a = (uint32_t)X[o->rs1], d = (uint32_t)X[o->rs2]; X[o->rd] = d ? SX32(a / d) : ~0ull; NEXT; } }
    OP(RV_REMW) {
      int32_t a = (int32_t)X[o->rs1], d = (int32_t)X[o->rs2];
      X[o->rd] = d == 0 ? SX32(a) : (a == INT32_MIN && d == -1) ? 0 : SX32(a % d);
      NEXT;
    }
    OP(RV_REMUW) { { uint32_t a = (uint32_t)X[o->rs1], d = (uint32_t)X[o->rs2]; X[o->rd] = d ? SX32(a % d) : SX32(a); NEXT; } }

    /* loads and stores */
#define LOADOP(OPC, T, EXT) OP(OPC) { uint64_t a = X[o->rs1] + (uint64_t)(int64_t)o->imm; CHK(a, 1); X[o->rd] = EXT(LD(T, a)); NEXT; }
    LOADOP(RV_LB, int8_t, S64)
    LOADOP(RV_LH, int16_t, S64)
    LOADOP(RV_LW, int32_t, S64)
    LOADOP(RV_LD, uint64_t, U64)
    LOADOP(RV_LBU, uint8_t, U64)
    LOADOP(RV_LHU, uint16_t, U64)
    LOADOP(RV_LWU, uint32_t, U64)
#define STOREOP(OPC, T) OP(OPC) { uint64_t a = X[o->rs1] + (uint64_t)(int64_t)o->imm; CHK(a, 2); ST(T, a, X[o->rs2]); NEXT; }
    STOREOP(RV_SB, uint8_t)
    STOREOP(RV_SH, uint16_t)
    STOREOP(RV_SW, uint32_t)
    STOREOP(RV_SD, uint64_t)
    OP(RV_FLW) { { uint64_t a = X[o->rs1] + (uint64_t)(int64_t)o->imm; CHK(a, 1); F[o->rd] = fp_box(LD(uint32_t, a)); NEXT; } }
    OP(RV_FLD) { { uint64_t a = X[o->rs1] + (uint64_t)(int64_t)o->imm; CHK(a, 1); F[o->rd] = LD(uint64_t, a); NEXT; } }
    OP(RV_FSW) { { uint64_t a = X[o->rs1] + (uint64_t)(int64_t)o->imm; CHK(a, 2); ST(uint32_t, a, (uint32_t)F[o->rs2]); NEXT; } }
    OP(RV_FSD) { { uint64_t a = X[o->rs1] + (uint64_t)(int64_t)o->imm; CHK(a, 2); ST(uint64_t, a, F[o->rs2]); NEXT; } }

    /* control flow */
    OP(RV_JAL) { X[o->rd] = (uint64_t)o->pc + o->len; TAKEN((uint64_t)((int64_t)o->pc + o->imm)); }
    OP(RV_JALR) {
      uint64_t tj = (X[o->rs1] + (uint64_t)(int64_t)o->imm) & ~1ull;
      X[o->rd] = (uint64_t)o->pc + o->len;
      INDIRECT(tj);
    }
#define BR(OPC, cond) OP(OPC) { if (cond) TAKEN((uint64_t)((int64_t)o->pc + o->imm)); FALL((uint64_t)o->pc + o->len); }
    BR(RV_BEQ, X[o->rs1] == X[o->rs2])
    BR(RV_BNE, X[o->rs1] != X[o->rs2])
    BR(RV_BLT, (int64_t)X[o->rs1] < (int64_t)X[o->rs2])
    BR(RV_BGE, (int64_t)X[o->rs1] >= (int64_t)X[o->rs2])
    BR(RV_BLTU, X[o->rs1] < X[o->rs2])
    BR(RV_BGEU, X[o->rs1] >= X[o->rs2])
    OP(OP_FALLTHRU) { FALL(o->pc); }

    OP(RV_FENCE) { NEXT; }
    OP(RV_FENCE_I) { FENCEI(); }
    OP(RV_ECALL) { EXIT(LX_STOP_SYSCALL, 0, 0); }
    OP(RV_EBREAK) { EXIT(LX_STOP_BREAK, 0, 0); }
    OP(RV_ILLEGAL) { EXIT(LX_STOP_ILLEGAL, (uint32_t)o->imm, 0); }
    OP(OP_FETCHFAULT) { EXIT(LX_STOP_FAULT, o->pc, 3); }

    /* Zicsr */
#define CSROP(OPC) OP(OPC) { \
      unsigned csr = (unsigned)o->imm & 0xfff; \
      uint64_t old, src = (OPC >= RV_CSRRWI) ? o->rs1 : X[o->rs1]; \
      int wr = (OPC == RV_CSRRW || OPC == RV_CSRRWI) || o->rs1 != 0; \
      if (!csr_read(csr, &old)) ILLEGAL; \
      if (wr) { \
        uint64_t nv = (OPC == RV_CSRRW || OPC == RV_CSRRWI) ? src : (OPC == RV_CSRRS || OPC == RV_CSRRSI) ? old | src : old & ~src; \
        if (!csr_write(csr, nv)) ILLEGAL; \
      } \
      X[o->rd] = old; \
      NEXT; \
    }
    CSROP(RV_CSRRW) CSROP(RV_CSRRS) CSROP(RV_CSRRC) CSROP(RV_CSRRWI) CSROP(RV_CSRRSI) CSROP(RV_CSRRCI)


    /* A: single hart. LR records the address and value; SC succeeds if the reservation
       is still held and memory still has that value (a store by this hart to the same
       place in between is allowed to let SC succeed, and must not corrupt anything). */
#define AMO_ALIGN(a, n) if ((a) & ((n) - 1)) FAULT((a), 4);
    OP(RV_LR_W) { { uint64_t a = X[o->rs1]; AMO_ALIGN(a, 4); CHK(a, 1); X[o->rd] = S64(LD(int32_t, a)); rv_cpu.res_addr = a; NEXT; } }
    OP(RV_LR_D) { { uint64_t a = X[o->rs1]; AMO_ALIGN(a, 8); CHK(a, 1); X[o->rd] = LD(uint64_t, a); rv_cpu.res_addr = a; NEXT; } }
    OP(RV_SC_W) {
      uint64_t a = X[o->rs1]; AMO_ALIGN(a, 4); CHK(a, 2);
      if (rv_cpu.res_addr == a) { ST(uint32_t, a, X[o->rs2]); X[o->rd] = 0; } else X[o->rd] = 1;
      rv_cpu.res_addr = ~0ull; NEXT;
    }
    OP(RV_SC_D) {
      uint64_t a = X[o->rs1]; AMO_ALIGN(a, 8); CHK(a, 2);
      if (rv_cpu.res_addr == a) { ST(uint64_t, a, X[o->rs2]); X[o->rd] = 0; } else X[o->rd] = 1;
      rv_cpu.res_addr = ~0ull; NEXT;
    }
#define AMOW(OPC, expr) OP(OPC) { uint64_t a = X[o->rs1]; AMO_ALIGN(a, 4); CHK(a, 2); \
      int32_t m = LD(int32_t, a); int32_t s = (int32_t)X[o->rs2]; (void)s; ST(int32_t, a, (expr)); X[o->rd] = S64(m); NEXT; }
#define AMOD(OPC, expr) OP(OPC) { uint64_t a = X[o->rs1]; AMO_ALIGN(a, 8); CHK(a, 2); \
      uint64_t m = LD(uint64_t, a); uint64_t s = X[o->rs2]; ST(uint64_t, a, (expr)); X[o->rd] = m; NEXT; }
    AMOW(RV_AMOSWAP_W, s)
    AMOW(RV_AMOADD_W, (int32_t)((uint32_t)m + (uint32_t)s))
    AMOW(RV_AMOXOR_W, m ^ s)
    AMOW(RV_AMOAND_W, m & s)
    AMOW(RV_AMOOR_W, m | s)
    AMOW(RV_AMOMIN_W, m < s ? m : s)
    AMOW(RV_AMOMAX_W, m > s ? m : s)
    AMOW(RV_AMOMINU_W, (uint32_t)m < (uint32_t)s ? m : s)
    AMOW(RV_AMOMAXU_W, (uint32_t)m > (uint32_t)s ? m : s)
    AMOD(RV_AMOSWAP_D, s)
    AMOD(RV_AMOADD_D, m + s)
    AMOD(RV_AMOXOR_D, m ^ s)
    AMOD(RV_AMOAND_D, m & s)
    AMOD(RV_AMOOR_D, m | s)
    AMOD(RV_AMOMIN_D, (int64_t)m < (int64_t)s ? m : s)
    AMOD(RV_AMOMAX_D, (int64_t)m > (int64_t)s ? m : s)
    AMOD(RV_AMOMINU_D, m < s ? m : s)
    AMOD(RV_AMOMAXU_D, m > s ? m : s)

    /* F and D */
#define RM() ({ int r_ = o->rm == 7 ? (int)((rv_cpu.fcsr >> 5) & 7) : o->rm; if (r_ > 4) ILLEGAL; r_; })
#define FL (&rv_cpu.fcsr)
#define S1 fp_unbox(F[o->rs1])
#define S2 fp_unbox(F[o->rs2])
#define S3 fp_unbox(F[o->rs3])
    OP(RV_FADD_S) { { int rm = RM(); F[o->rd] = fp_box(fps_add(S1, S2, rm, FL)); NEXT; } }
    OP(RV_FSUB_S) { { int rm = RM(); F[o->rd] = fp_box(fps_sub(S1, S2, rm, FL)); NEXT; } }
    OP(RV_FMUL_S) { { int rm = RM(); F[o->rd] = fp_box(fps_mul(S1, S2, rm, FL)); NEXT; } }
    OP(RV_FDIV_S) { { int rm = RM(); F[o->rd] = fp_box(fps_div(S1, S2, rm, FL)); NEXT; } }
    OP(RV_FSQRT_S) { { int rm = RM(); F[o->rd] = fp_box(fps_sqrt(S1, rm, FL)); NEXT; } }
    OP(RV_FMADD_S) { { int rm = RM(); F[o->rd] = fp_box(fps_fma(S1, S2, S3, 0, 0, rm, FL)); NEXT; } }
    OP(RV_FMSUB_S) { { int rm = RM(); F[o->rd] = fp_box(fps_fma(S1, S2, S3, 0, 1, rm, FL)); NEXT; } }
    OP(RV_FNMSUB_S) { { int rm = RM(); F[o->rd] = fp_box(fps_fma(S1, S2, S3, 1, 0, rm, FL)); NEXT; } }
    OP(RV_FNMADD_S) { { int rm = RM(); F[o->rd] = fp_box(fps_fma(S1, S2, S3, 1, 1, rm, FL)); NEXT; } }
    OP(RV_FSGNJ_S) { F[o->rd] = fp_box((S1 & 0x7fffffffu) | (S2 & 0x80000000u)); NEXT; }
    OP(RV_FSGNJN_S) { F[o->rd] = fp_box((S1 & 0x7fffffffu) | (~S2 & 0x80000000u)); NEXT; }
    OP(RV_FSGNJX_S) { F[o->rd] = fp_box(S1 ^ (S2 & 0x80000000u)); NEXT; }
    OP(RV_FMIN_S) { F[o->rd] = fp_box(sf32_minmax(S1, S2, 0, FL)); NEXT; }
    OP(RV_FMAX_S) { F[o->rd] = fp_box(sf32_minmax(S1, S2, 1, FL)); NEXT; }
    OP(RV_FCVT_W_S) { { int rm = RM(); X[o->rd] = sf32_to_int(S1, 32, 1, rm, FL); NEXT; } }
    OP(RV_FCVT_WU_S) { { int rm = RM(); X[o->rd] = sf32_to_int(S1, 32, 0, rm, FL); NEXT; } }
    OP(RV_FCVT_L_S) { { int rm = RM(); X[o->rd] = sf32_to_int(S1, 64, 1, rm, FL); NEXT; } }
    OP(RV_FCVT_LU_S) { { int rm = RM(); X[o->rd] = sf32_to_int(S1, 64, 0, rm, FL); NEXT; } }
    OP(RV_FMV_X_W) { X[o->rd] = S64((int32_t)(uint32_t)F[o->rs1]); NEXT; }
    OP(RV_FEQ_S) { X[o->rd] = (uint64_t)sf32_eq(S1, S2, FL); NEXT; }
    OP(RV_FLT_S) { X[o->rd] = (uint64_t)sf32_lt(S1, S2, FL); NEXT; }
    OP(RV_FLE_S) { X[o->rd] = (uint64_t)sf32_le(S1, S2, FL); NEXT; }
    OP(RV_FCLASS_S) { X[o->rd] = sf32_class(S1); NEXT; }
    OP(RV_FCVT_S_W) { { int rm = RM(); F[o->rd] = fp_box(sf32_from_int(S64((int32_t)X[o->rs1]), 1, rm, FL)); NEXT; } }
    OP(RV_FCVT_S_WU) { { int rm = RM(); F[o->rd] = fp_box(sf32_from_int((uint32_t)X[o->rs1], 0, rm, FL)); NEXT; } }
    OP(RV_FCVT_S_L) { { int rm = RM(); F[o->rd] = fp_box(sf32_from_int(X[o->rs1], 1, rm, FL)); NEXT; } }
    OP(RV_FCVT_S_LU) { { int rm = RM(); F[o->rd] = fp_box(sf32_from_int(X[o->rs1], 0, rm, FL)); NEXT; } }
    OP(RV_FMV_W_X) { F[o->rd] = fp_box((uint32_t)X[o->rs1]); NEXT; }

    OP(RV_FADD_D) { { int rm = RM(); F[o->rd] = fpd_add(F[o->rs1], F[o->rs2], rm, FL); NEXT; } }
    OP(RV_FSUB_D) { { int rm = RM(); F[o->rd] = fpd_sub(F[o->rs1], F[o->rs2], rm, FL); NEXT; } }
    OP(RV_FMUL_D) { { int rm = RM(); F[o->rd] = fpd_mul(F[o->rs1], F[o->rs2], rm, FL); NEXT; } }
    OP(RV_FDIV_D) { { int rm = RM(); F[o->rd] = fpd_div(F[o->rs1], F[o->rs2], rm, FL); NEXT; } }
    OP(RV_FSQRT_D) { { int rm = RM(); F[o->rd] = fpd_sqrt(F[o->rs1], rm, FL); NEXT; } }
    OP(RV_FMADD_D) { { int rm = RM(); F[o->rd] = fpd_fma(F[o->rs1], F[o->rs2], F[o->rs3], 0, 0, rm, FL); NEXT; } }
    OP(RV_FMSUB_D) { { int rm = RM(); F[o->rd] = fpd_fma(F[o->rs1], F[o->rs2], F[o->rs3], 0, 1, rm, FL); NEXT; } }
    OP(RV_FNMSUB_D) { { int rm = RM(); F[o->rd] = fpd_fma(F[o->rs1], F[o->rs2], F[o->rs3], 1, 0, rm, FL); NEXT; } }
    OP(RV_FNMADD_D) { { int rm = RM(); F[o->rd] = fpd_fma(F[o->rs1], F[o->rs2], F[o->rs3], 1, 1, rm, FL); NEXT; } }
    OP(RV_FSGNJ_D) { F[o->rd] = (F[o->rs1] & ~(1ull << 63)) | (F[o->rs2] & (1ull << 63)); NEXT; }
    OP(RV_FSGNJN_D) { F[o->rd] = (F[o->rs1] & ~(1ull << 63)) | (~F[o->rs2] & (1ull << 63)); NEXT; }
    OP(RV_FSGNJX_D) { F[o->rd] = F[o->rs1] ^ (F[o->rs2] & (1ull << 63)); NEXT; }
    OP(RV_FMIN_D) { F[o->rd] = sf64_minmax(F[o->rs1], F[o->rs2], 0, FL); NEXT; }
    OP(RV_FMAX_D) { F[o->rd] = sf64_minmax(F[o->rs1], F[o->rs2], 1, FL); NEXT; }
    OP(RV_FCVT_S_D) { { int rm = RM(); F[o->rd] = fp_box(sf64_to_f32(F[o->rs1], rm, FL)); NEXT; } }
    OP(RV_FCVT_D_S) { F[o->rd] = sf32_to_f64(S1, FL); NEXT; }
    OP(RV_FCVT_W_D) { { int rm = RM(); X[o->rd] = sf64_to_int(F[o->rs1], 32, 1, rm, FL); NEXT; } }
    OP(RV_FCVT_WU_D) { { int rm = RM(); X[o->rd] = sf64_to_int(F[o->rs1], 32, 0, rm, FL); NEXT; } }
    OP(RV_FCVT_L_D) { { int rm = RM(); X[o->rd] = sf64_to_int(F[o->rs1], 64, 1, rm, FL); NEXT; } }
    OP(RV_FCVT_LU_D) { { int rm = RM(); X[o->rd] = sf64_to_int(F[o->rs1], 64, 0, rm, FL); NEXT; } }
    OP(RV_FMV_X_D) { X[o->rd] = F[o->rs1]; NEXT; }
    OP(RV_FEQ_D) { X[o->rd] = (uint64_t)sf64_eq(F[o->rs1], F[o->rs2], FL); NEXT; }
    OP(RV_FLT_D) { X[o->rd] = (uint64_t)sf64_lt(F[o->rs1], F[o->rs2], FL); NEXT; }
    OP(RV_FLE_D) { X[o->rd] = (uint64_t)sf64_le(F[o->rs1], F[o->rs2], FL); NEXT; }
    OP(RV_FCLASS_D) { X[o->rd] = sf64_class(F[o->rs1]); NEXT; }
    OP(RV_FCVT_D_W) { { int rm = RM(); F[o->rd] = sf64_from_int(S64((int32_t)X[o->rs1]), 1, rm, FL); NEXT; } }
    OP(RV_FCVT_D_WU) { { int rm = RM(); F[o->rd] = sf64_from_int((uint32_t)X[o->rs1], 0, rm, FL); NEXT; } }
    OP(RV_FCVT_D_L) { { int rm = RM(); F[o->rd] = sf64_from_int(X[o->rs1], 1, rm, FL); NEXT; } }
    OP(RV_FCVT_D_LU) { { int rm = RM(); F[o->rd] = sf64_from_int(X[o->rs1], 0, rm, FL); NEXT; } }
    OP(RV_FMV_D_X) { F[o->rd] = X[o->rs1]; NEXT; }

    /* Zba, Zbb, Zbs (decoded only when enabled) */
    OP(RV_SH1ADD) { X[o->rd] = (X[o->rs1] << 1) + X[o->rs2]; NEXT; }
    OP(RV_SH2ADD) { X[o->rd] = (X[o->rs1] << 2) + X[o->rs2]; NEXT; }
    OP(RV_SH3ADD) { X[o->rd] = (X[o->rs1] << 3) + X[o->rs2]; NEXT; }
    OP(RV_ADD_UW) { X[o->rd] = (uint32_t)X[o->rs1] + X[o->rs2]; NEXT; }
    OP(RV_SH1ADD_UW) { X[o->rd] = ((uint64_t)(uint32_t)X[o->rs1] << 1) + X[o->rs2]; NEXT; }
    OP(RV_SH2ADD_UW) { X[o->rd] = ((uint64_t)(uint32_t)X[o->rs1] << 2) + X[o->rs2]; NEXT; }
    OP(RV_SH3ADD_UW) { X[o->rd] = ((uint64_t)(uint32_t)X[o->rs1] << 3) + X[o->rs2]; NEXT; }
    OP(RV_SLLI_UW) { X[o->rd] = (uint64_t)(uint32_t)X[o->rs1] << (o->imm & 63); NEXT; }
    OP(RV_ANDN) { X[o->rd] = X[o->rs1] & ~X[o->rs2]; NEXT; }
    OP(RV_ORN) { X[o->rd] = X[o->rs1] | ~X[o->rs2]; NEXT; }
    OP(RV_XNOR) { X[o->rd] = ~(X[o->rs1] ^ X[o->rs2]); NEXT; }
    OP(RV_CLZ) { X[o->rd] = X[o->rs1] ? (uint64_t)__builtin_clzll(X[o->rs1]) : 64; NEXT; }
    OP(RV_CTZ) { X[o->rd] = X[o->rs1] ? (uint64_t)__builtin_ctzll(X[o->rs1]) : 64; NEXT; }
    OP(RV_CPOP) { X[o->rd] = (uint64_t)__builtin_popcountll(X[o->rs1]); NEXT; }
    OP(RV_CLZW) { X[o->rd] = (uint32_t)X[o->rs1] ? (uint64_t)__builtin_clz((uint32_t)X[o->rs1]) : 32; NEXT; }
    OP(RV_CTZW) { X[o->rd] = (uint32_t)X[o->rs1] ? (uint64_t)__builtin_ctz((uint32_t)X[o->rs1]) : 32; NEXT; }
    OP(RV_CPOPW) { X[o->rd] = (uint64_t)__builtin_popcount((uint32_t)X[o->rs1]); NEXT; }
    OP(RV_MAX) { X[o->rd] = (int64_t)X[o->rs1] > (int64_t)X[o->rs2] ? X[o->rs1] : X[o->rs2]; NEXT; }
    OP(RV_MAXU) { X[o->rd] = X[o->rs1] > X[o->rs2] ? X[o->rs1] : X[o->rs2]; NEXT; }
    OP(RV_MIN) { X[o->rd] = (int64_t)X[o->rs1] < (int64_t)X[o->rs2] ? X[o->rs1] : X[o->rs2]; NEXT; }
    OP(RV_MINU) { X[o->rd] = X[o->rs1] < X[o->rs2] ? X[o->rs1] : X[o->rs2]; NEXT; }
    OP(RV_SEXT_B) { X[o->rd] = S64((int8_t)X[o->rs1]); NEXT; }
    OP(RV_SEXT_H) { X[o->rd] = S64((int16_t)X[o->rs1]); NEXT; }
    OP(RV_ZEXT_H) { X[o->rd] = (uint16_t)X[o->rs1]; NEXT; }
    OP(RV_ROL) { { unsigned s = X[o->rs2] & 63; uint64_t v = X[o->rs1]; X[o->rd] = s ? (v << s) | (v >> (64 - s)) : v; NEXT; } }
    OP(RV_ROR) { { unsigned s = X[o->rs2] & 63; uint64_t v = X[o->rs1]; X[o->rd] = s ? (v >> s) | (v << (64 - s)) : v; NEXT; } }
    OP(RV_RORI) { { unsigned s = o->imm & 63; uint64_t v = X[o->rs1]; X[o->rd] = s ? (v >> s) | (v << (64 - s)) : v; NEXT; } }
    OP(RV_ROLW) { { unsigned s = X[o->rs2] & 31; uint32_t v = (uint32_t)X[o->rs1]; X[o->rd] = SX32(s ? (v << s) | (v >> (32 - s)) : v); NEXT; } }
    OP(RV_RORW) { { unsigned s = X[o->rs2] & 31; uint32_t v = (uint32_t)X[o->rs1]; X[o->rd] = SX32(s ? (v >> s) | (v << (32 - s)) : v); NEXT; } }
    OP(RV_RORIW) { { unsigned s = o->imm & 31; uint32_t v = (uint32_t)X[o->rs1]; X[o->rd] = SX32(s ? (v >> s) | (v << (32 - s)) : v); NEXT; } }
    OP(RV_ORC_B) {
      uint64_t v = X[o->rs1], r = 0;
      for (int i = 0; i < 8; i++) if ((v >> (8 * i)) & 0xff) r |= 0xffull << (8 * i);
      X[o->rd] = r; NEXT;
    }
    OP(RV_REV8) { X[o->rd] = __builtin_bswap64(X[o->rs1]); NEXT; }
    OP(RV_BCLR) { X[o->rd] = X[o->rs1] & ~(1ull << (X[o->rs2] & 63)); NEXT; }
    OP(RV_BCLRI) { X[o->rd] = X[o->rs1] & ~(1ull << (o->imm & 63)); NEXT; }
    OP(RV_BEXT) { X[o->rd] = (X[o->rs1] >> (X[o->rs2] & 63)) & 1; NEXT; }
    OP(RV_BEXTI) { X[o->rd] = (X[o->rs1] >> (o->imm & 63)) & 1; NEXT; }
    OP(RV_BINV) { X[o->rd] = X[o->rs1] ^ (1ull << (X[o->rs2] & 63)); NEXT; }
    OP(RV_BINVI) { X[o->rd] = X[o->rs1] ^ (1ull << (o->imm & 63)); NEXT; }
    OP(RV_BSET) { X[o->rd] = X[o->rs1] | (1ull << (X[o->rs2] & 63)); NEXT; }
    OP(RV_BSETI) { X[o->rd] = X[o->rs1] | (1ull << (o->imm & 63)); NEXT; }
