// SPDX-License-Identifier: GPL-2.0-or-later
// A tiny bytecode interpreter: a dense switch, compiled to a jump table (an indirect jump
// inside one function, the worst case for translators that structure control flow).
#define REPS 2
#include "rt.h"
enum { OP_PUSH, OP_ADD, OP_SUB, OP_MUL, OP_DUP, OP_SWAP, OP_JNZ, OP_DEC, OP_XOR, OP_SHL, OP_SHR, OP_POP, OP_HALT, OP_NOP1, OP_NOP2, OP_INC };
static const uint8_t prog[] = {
  OP_PUSH, 0, OP_PUSH, 200,                  // acc, counter (counter on top)
  /* 4 */ OP_SWAP, OP_PUSH, 3, OP_MUL, OP_PUSH, 7, OP_ADD, OP_DUP, OP_PUSH, 5, OP_SHR, OP_XOR, OP_INC, OP_NOP1, OP_NOP2,
  OP_SWAP, OP_DEC, OP_DUP, OP_JNZ, 4, OP_POP, OP_HALT };
__attribute__((noinline)) static uint64_t vm(uint64_t seed) {
  uint64_t st[64]; int sp = 0; uint32_t pc = 0;
  for (;;) {
    switch (prog[pc++]) {
    case OP_PUSH: st[sp++] = prog[pc++] + seed; seed = 0; break;
    case OP_ADD: sp--; st[sp - 1] += st[sp]; break;
    case OP_SUB: sp--; st[sp - 1] -= st[sp]; break;
    case OP_MUL: sp--; st[sp - 1] *= st[sp]; break;
    case OP_DUP: st[sp] = st[sp - 1]; sp++; break;
    case OP_SWAP: { uint64_t t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; break; }
    case OP_JNZ: { uint8_t t = prog[pc++]; if (st[--sp]) pc = t; break; }
    case OP_DEC: st[sp - 1]--; break;
    case OP_XOR: sp--; st[sp - 1] ^= st[sp]; break;
    case OP_SHL: sp--; st[sp - 1] <<= st[sp] & 63; break;
    case OP_SHR: sp--; st[sp - 1] >>= st[sp] & 63; break;
    case OP_POP: sp--; break;
    case OP_HALT: return st[sp - 1];
    case OP_INC: st[sp - 1]++; break;
    default: break;
    }
  }
}
uint64_t work(void) {
  uint64_t h = 0;
  for (uint32_t i = 0; i < 2500; i++) h = h * 31 + vm(i);
  return h;
}
