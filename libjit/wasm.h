// SPDX-License-Identifier: GPL-2.0-or-later
// A WebAssembly binary emitter: byte buffers, LEB128, instructions, module sections.
// Only what the translator uses. Reference: the WebAssembly core specification, binary format.
#ifndef JIT_WASM_H
#define JIT_WASM_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint8_t *p; uint32_t n, cap; } wbuf;

void wb_init(wbuf *b);
void wb_free(wbuf *b);
void wb_reserve(wbuf *b, uint32_t extra);
static inline void wb_byte(wbuf *b, uint8_t v) { if (b->n == b->cap) wb_reserve(b, 1); b->p[b->n++] = v; }
void wb_bytes(wbuf *b, const void *p, uint32_t n);
void wb_uleb(wbuf *b, uint64_t v);
void wb_sleb(wbuf *b, int64_t v);
void wb_name(wbuf *b, const char *s);
void wb_vec(wbuf *b, uint32_t count, const wbuf *items);   // count, then the bytes of items
void wb_section(wbuf *mod, int id, const wbuf *body);        // id, size, body

// value types
enum { WT_I32 = 0x7f, WT_I64 = 0x7e, WT_F32 = 0x7d, WT_F64 = 0x7c, WT_FUNCREF = 0x70, WT_VOID = 0x40 };

// opcodes used by the translator
enum {
  W_UNREACHABLE = 0x00, W_NOP = 0x01, W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04, W_ELSE = 0x05,
  W_END = 0x0b, W_BR = 0x0c, W_BR_IF = 0x0d, W_BR_TABLE = 0x0e, W_RETURN = 0x0f, W_CALL = 0x10,
  W_CALL_INDIRECT = 0x11, W_RETURN_CALL = 0x12, W_RETURN_CALL_INDIRECT = 0x13, W_DROP = 0x1a, W_SELECT = 0x1b,
  W_LGET = 0x20, W_LSET = 0x21, W_LTEE = 0x22, W_GGET = 0x23, W_GSET = 0x24,
  W_TABLE_GET = 0x25, W_TABLE_SET = 0x26,
  W_I32_LOAD = 0x28, W_I64_LOAD = 0x29, W_F32_LOAD = 0x2a, W_F64_LOAD = 0x2b,
  W_I32_LOAD8_S = 0x2c, W_I32_LOAD8_U = 0x2d, W_I32_LOAD16_S = 0x2e, W_I32_LOAD16_U = 0x2f,
  W_I64_LOAD8_S = 0x30, W_I64_LOAD8_U = 0x31, W_I64_LOAD16_S = 0x32, W_I64_LOAD16_U = 0x33,
  W_I64_LOAD32_S = 0x34, W_I64_LOAD32_U = 0x35,
  W_I32_STORE = 0x36, W_I64_STORE = 0x37, W_F32_STORE = 0x38, W_F64_STORE = 0x39,
  W_I32_STORE8 = 0x3a, W_I32_STORE16 = 0x3b, W_I64_STORE8 = 0x3c, W_I64_STORE16 = 0x3d, W_I64_STORE32 = 0x3e,
  W_MEMORY_SIZE = 0x3f, W_MEMORY_GROW = 0x40,
  W_I32_CONST = 0x41, W_I64_CONST = 0x42, W_F32_CONST = 0x43, W_F64_CONST = 0x44,
  W_I32_EQZ = 0x45, W_I32_EQ, W_I32_NE, W_I32_LT_S, W_I32_LT_U, W_I32_GT_S, W_I32_GT_U, W_I32_LE_S, W_I32_LE_U, W_I32_GE_S, W_I32_GE_U,
  W_I64_EQZ = 0x50, W_I64_EQ, W_I64_NE, W_I64_LT_S, W_I64_LT_U, W_I64_GT_S, W_I64_GT_U, W_I64_LE_S, W_I64_LE_U, W_I64_GE_S, W_I64_GE_U,
  W_F32_EQ = 0x5b, W_F32_NE, W_F32_LT, W_F32_GT, W_F32_LE, W_F32_GE,
  W_F64_EQ = 0x61, W_F64_NE, W_F64_LT, W_F64_GT, W_F64_LE, W_F64_GE,
  W_I32_CLZ = 0x67, W_I32_CTZ, W_I32_POPCNT, W_I32_ADD, W_I32_SUB, W_I32_MUL, W_I32_DIV_S, W_I32_DIV_U,
  W_I32_REM_S, W_I32_REM_U, W_I32_AND, W_I32_OR, W_I32_XOR, W_I32_SHL, W_I32_SHR_S, W_I32_SHR_U, W_I32_ROTL, W_I32_ROTR,
  W_I64_CLZ = 0x79, W_I64_CTZ, W_I64_POPCNT, W_I64_ADD, W_I64_SUB, W_I64_MUL, W_I64_DIV_S, W_I64_DIV_U,
  W_I64_REM_S, W_I64_REM_U, W_I64_AND, W_I64_OR, W_I64_XOR, W_I64_SHL, W_I64_SHR_S, W_I64_SHR_U, W_I64_ROTL, W_I64_ROTR,
  W_F32_ABS = 0x8b, W_F32_NEG, W_F32_CEIL, W_F32_FLOOR, W_F32_TRUNC, W_F32_NEAREST, W_F32_SQRT, W_F32_ADD, W_F32_SUB,
  W_F32_MUL, W_F32_DIV, W_F32_MIN, W_F32_MAX, W_F32_COPYSIGN,
  W_F64_ABS = 0x99, W_F64_NEG, W_F64_CEIL, W_F64_FLOOR, W_F64_TRUNC, W_F64_NEAREST, W_F64_SQRT, W_F64_ADD, W_F64_SUB,
  W_F64_MUL, W_F64_DIV, W_F64_MIN, W_F64_MAX, W_F64_COPYSIGN,
  W_I32_WRAP_I64 = 0xa7, W_I32_TRUNC_F32_S, W_I32_TRUNC_F32_U, W_I32_TRUNC_F64_S, W_I32_TRUNC_F64_U,
  W_I64_EXTEND_I32_S = 0xac, W_I64_EXTEND_I32_U, W_I64_TRUNC_F32_S, W_I64_TRUNC_F32_U, W_I64_TRUNC_F64_S, W_I64_TRUNC_F64_U,
  W_F32_CONVERT_I32_S = 0xb2, W_F32_CONVERT_I32_U, W_F32_CONVERT_I64_S, W_F32_CONVERT_I64_U, W_F32_DEMOTE_F64,
  W_F64_CONVERT_I32_S = 0xb7, W_F64_CONVERT_I32_U, W_F64_CONVERT_I64_S, W_F64_CONVERT_I64_U, W_F64_PROMOTE_F32,
  W_I32_REINTERPRET_F32 = 0xbc, W_I64_REINTERPRET_F64, W_F32_REINTERPRET_I32, W_F64_REINTERPRET_I64,
  W_I32_EXTEND8_S = 0xc0, W_I32_EXTEND16_S, W_I64_EXTEND8_S, W_I64_EXTEND16_S, W_I64_EXTEND32_S,
  W_REF_NULL = 0xd0, W_REF_FUNC = 0xd2,
  W_PREFIX_FC = 0xfc,
};
// 0xfc sub-opcodes
enum { WFC_TABLE_INIT = 12, WFC_TABLE_GROW = 15, WFC_TABLE_SIZE = 16, WFC_TABLE_FILL = 17, WFC_MEMORY_COPY = 10, WFC_MEMORY_FILL = 11 };

static inline void w_op(wbuf *b, int op) { wb_byte(b, (uint8_t)op); }
static inline void w_i32c(wbuf *b, int32_t v) { wb_byte(b, W_I32_CONST); wb_sleb(b, v); }
static inline void w_i64c(wbuf *b, int64_t v) { wb_byte(b, W_I64_CONST); wb_sleb(b, v); }
static inline void w_lget(wbuf *b, uint32_t i) { wb_byte(b, W_LGET); wb_uleb(b, i); }
static inline void w_lset(wbuf *b, uint32_t i) { wb_byte(b, W_LSET); wb_uleb(b, i); }
static inline void w_ltee(wbuf *b, uint32_t i) { wb_byte(b, W_LTEE); wb_uleb(b, i); }
static inline void w_mem(wbuf *b, int op, uint32_t align, uint32_t off) { wb_byte(b, (uint8_t)op); wb_uleb(b, align); wb_uleb(b, off); }
static inline void w_call(wbuf *b, uint32_t f) { wb_byte(b, W_CALL); wb_uleb(b, f); }
static inline void w_call_indirect(wbuf *b, uint32_t type) { wb_byte(b, W_CALL_INDIRECT); wb_uleb(b, type); wb_byte(b, 0); }
static inline void w_block(wbuf *b) { wb_byte(b, W_BLOCK); wb_byte(b, WT_VOID); }
static inline void w_loop(wbuf *b) { wb_byte(b, W_LOOP); wb_byte(b, WT_VOID); }
static inline void w_if(wbuf *b, int type) { wb_byte(b, W_IF); wb_byte(b, (uint8_t)type); }
static inline void w_br(wbuf *b, uint32_t d) { wb_byte(b, W_BR); wb_uleb(b, d); }
static inline void w_br_if(wbuf *b, uint32_t d) { wb_byte(b, W_BR_IF); wb_uleb(b, d); }
static inline void w_fc(wbuf *b, uint32_t sub) { wb_byte(b, W_PREFIX_FC); wb_uleb(b, sub); }
#endif
