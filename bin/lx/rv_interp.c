// SPDX-License-Identifier: GPL-2.0-or-later
/* RV64GC reference interpreter.
 *
 * Guest code is decoded once into blocks: straight runs of instructions that end at the
 * first branch, jump, ecall, ebreak, fence.i or illegal instruction (or after
 * BLOCK_MAX instructions). Blocks live in a hash table keyed by guest pc and remember
 * their successors, so a taken branch to a known block costs one compare. The decoded
 * form is struct op: 16 bytes, with the instruction's own pc so faults are precise.
 *
 * Dispatch is a switch in a loop, which clang turns into a jump table natively and a
 * br_table in WebAssembly. Writes to x0 are redirected at decode time to x[32], so no
 * handler tests rd. Self-modifying code follows the RISC-V rule: the guest must execute
 * fence.i (or call riscv_flush_icache) before running code it wrote; both drop every
 * block. munmap and mprotect of executable memory and execve do the same. */
#include "lx.h"
#include "rv.h"
#include "rv_fp.h"
#include "arch.h"
#include "host.h"

struct rv_cpu rv_cpu;

/* Internal opcodes after the RISC-V ones. */
enum {
  OP_FALLTHRU = RV_NOPS, /* end of a block that did not end in a branch: continue at pc */
  OP_FETCHFAULT,         /* the next instruction could not be fetched */
  OP_LI,                 /* lui/addi/c.li with a known constant: x[rd] = imm (fused) */
  OP_MV,                 /* addi rd, rs, 0 */
  OP_AUIPC_ADDI,         /* auipc + addi on the same register: x[rd] = pc + imm (fused) */
  OP_NOP,
  OP_COUNT
};

struct op {
  uint16_t k;
  uint8_t rd, rs1, rs2, rs3, rm, len;
  int32_t imm;
  uint32_t pc;
};
_Static_assert(sizeof(struct op) == 16, "op size");

struct block {
  uint64_t pc;
  struct block *hnext;
  struct block *taken, *fall; /* successor cache (chaining) */
  uint32_t n, ninsn;
  struct op ops[];
};

#define BLOCK_MAX 64
#define HBITS 16
static struct block *htab[1u << HBITS];
static uint8_t *arena;
static size_t arena_used, arena_cap = 32u << 20;
uint64_t rv_blocks_built, rv_flushes;
int rv_fuse = 1; /* superinstructions on/off (for measurement) */

static inline unsigned hidx(uint64_t pc) { return (unsigned)((pc >> 1) ^ (pc >> (HBITS + 1))) & ((1u << HBITS) - 1); }

void rv_flush(uint64_t addr, uint64_t len) {
  (void)addr; (void)len;
  memset(htab, 0, sizeof htab);
  arena_used = 0;
  rv_flushes++;
}

static int writes_xreg(int op) {
  unsigned f = rv_op_flags[op];
  if (f & (RVF_STORE | RVF_BRANCH)) return 0;
  if (op == RV_FENCE || op == RV_FENCE_I || op == RV_ECALL || op == RV_EBREAK || op == RV_ILLEGAL) return 0;
  if (f & RVF_FP) {
    switch (op) {
    case RV_FCVT_W_S: case RV_FCVT_WU_S: case RV_FCVT_L_S: case RV_FCVT_LU_S:
    case RV_FCVT_W_D: case RV_FCVT_WU_D: case RV_FCVT_L_D: case RV_FCVT_LU_D:
    case RV_FMV_X_W: case RV_FMV_X_D: case RV_FEQ_S: case RV_FLT_S: case RV_FLE_S:
    case RV_FEQ_D: case RV_FLT_D: case RV_FLE_D: case RV_FCLASS_S: case RV_FCLASS_D:
      return 1;
    }
    return 0;
  }
  return 1;
}

static inline int fetch_ok(uint64_t a) { return a >= g_lo && a + 2 <= g_size - LX_PAGE; }

/* Peephole fusion over a decoded block. Only rewrites that keep the architectural result
 * of every instruction (the second op of a pair still runs, or its effect is merged). */
static void fuse(struct op *o, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    if (o[i].k == RV_ADDI && o[i].rs1 == 0) { o[i].k = OP_LI; continue; }
    if (o[i].k == RV_LUI) { o[i].k = OP_LI; continue; } /* imm already shifted */
    if (o[i].k == RV_ADDI && o[i].imm == 0) { o[i].k = OP_MV; continue; }
    if (o[i].k == RV_AUIPC && i + 1 < n && o[i + 1].k == RV_ADDI && o[i + 1].rs1 == o[i].rd && o[i + 1].rd == o[i].rd && o[i].rd != 32) {
      /* x[rd] = pc + hi + lo; the second slot becomes a no-op so instruction counts and
         fault pcs stay right */
      int64_t v = (int64_t)o[i].pc + o[i].imm + o[i + 1].imm;
      if (v == (int32_t)v || (v >= 0 && v < (int64_t)g_size)) {
        o[i].k = OP_AUIPC_ADDI; o[i].imm = (int32_t)(v - (int64_t)o[i].pc);
        o[i + 1].k = OP_NOP;
        i++;
      }
    }
  }
}

static struct block *build(uint64_t pc) {
  struct op tmp[BLOCK_MAX + 1];
  unsigned n = 0;
  uint64_t p = pc;
  for (;;) {
    struct op *o = &tmp[n];
    memset(o, 0, sizeof *o);
    o->pc = (uint32_t)p;
    if (!fetch_ok(p)) { o->k = OP_FETCHFAULT; n++; break; }
    uint16_t lo16;
    memcpy(&lo16, g_mem + p, 2);
    uint32_t raw = lo16;
    if ((lo16 & 3) == 3) {
      if (!fetch_ok(p + 2)) { o->k = OP_FETCHFAULT; n++; break; }
      uint16_t hi16;
      memcpy(&hi16, g_mem + p + 2, 2);
      raw |= (uint32_t)hi16 << 16;
    }
    struct rv_insn in;
    rv_decode(raw, &in);
    o->k = in.op; o->rd = in.rd; o->rs1 = in.rs1; o->rs2 = in.rs2; o->rs3 = in.rs3;
    o->rm = in.rm; o->len = in.len; o->imm = (int32_t)in.imm;
    if (in.op == RV_ILLEGAL) o->imm = (int32_t)raw;
    if (o->rd == 0 && writes_xreg(in.op)) o->rd = 32;
    n++;
    p += in.len;
    unsigned fl = rv_op_flags[in.op];
    if (in.op == RV_ILLEGAL || (fl & (RVF_JUMP | RVF_BRANCH | RVF_SYSTEM))) break;
    if (n == BLOCK_MAX) {
      struct op *t = &tmp[n++];
      memset(t, 0, sizeof *t);
      t->k = OP_FALLTHRU; t->pc = (uint32_t)p;
      break;
    }
  }
  unsigned ninsn = n;
  if (tmp[n - 1].k == OP_FALLTHRU || tmp[n - 1].k == OP_FETCHFAULT) ninsn--;
  if (rv_fuse) fuse(tmp, n);
  size_t sz = sizeof(struct block) + n * sizeof(struct op);
  sz = (sz + 15) & ~(size_t)15;
  if (!arena) { arena = malloc(arena_cap); if (!arena) lx_die("no memory for the code cache"); }
  if (arena_used + sz > arena_cap) rv_flush(0, ~0ull);
  struct block *b = (struct block *)(void *)(arena + arena_used);
  arena_used += sz;
  b->pc = pc; b->n = n; b->ninsn = ninsn; b->taken = b->fall = 0;
  memcpy(b->ops, tmp, n * sizeof(struct op));
  unsigned h = hidx(pc);
  b->hnext = htab[h];
  htab[h] = b;
  rv_blocks_built++;
  return b;
}

static inline struct block *lookup(uint64_t pc) {
  for (struct block *b = htab[hidx(pc)]; b; b = b->hnext)
    if (b->pc == pc) return b;
  return build(pc);
}

/* CSRs visible in user mode. */
#define CSR_FFLAGS 0x001
#define CSR_FRM 0x002
#define CSR_FCSR 0x003
#define CSR_CYCLE 0xc00
#define CSR_TIME 0xc01
#define CSR_INSTRET 0xc02

static int csr_read(unsigned csr, uint64_t *v) {
  switch (csr) {
  case CSR_FFLAGS: *v = rv_cpu.fcsr & 0x1f; return 1;
  case CSR_FRM: *v = (rv_cpu.fcsr >> 5) & 7; return 1;
  case CSR_FCSR: *v = rv_cpu.fcsr & 0xff; return 1;
  case CSR_CYCLE: case CSR_INSTRET: *v = rv_cpu.instret; return 1;
  case CSR_TIME: {
    int64_t s, ns;
    hb_clock_gettime(1, &s, &ns);
    *v = (uint64_t)s * 10000000ull + (uint64_t)ns / 100; /* a 10 MHz timebase */
    return 1;
  }
  }
  return 0;
}
static int csr_write(unsigned csr, uint64_t v) {
  switch (csr) {
  case CSR_FFLAGS: rv_cpu.fcsr = (rv_cpu.fcsr & ~0x1fu) | (uint32_t)(v & 0x1f); return 1;
  case CSR_FRM: rv_cpu.fcsr = (rv_cpu.fcsr & ~0xe0u) | (uint32_t)((v & 7) << 5); return 1;
  case CSR_FCSR: rv_cpu.fcsr = (uint32_t)(v & 0xff); return 1;
  }
  return 0;
}

/* The empty asm hides the operands from clang, which would otherwise recognise the
 * pattern as a 128-bit multiply and call __multi3 (compiler-rt, not linked on UC.OS). */
#define OPAQUE(v) __asm__("" : "+r"(v))
static inline uint64_t mulhu64(uint64_t a, uint64_t b) {
  uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
  OPAQUE(a0); OPAQUE(b1);
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  return p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
static inline int64_t mulh64(int64_t a, int64_t b) {
  uint64_t h = mulhu64((uint64_t)a, (uint64_t)b);
  if (a < 0) h -= (uint64_t)b;
  if (b < 0) h -= (uint64_t)a;
  return (int64_t)h;
}
static inline int64_t mulhsu64(int64_t a, uint64_t b) {
  uint64_t h = mulhu64((uint64_t)a, b);
  if (a < 0) h -= b;
  return (int64_t)h;
}

uint64_t lx_budget = 4000; /* see docs/LINUX-BINARIES.md section 5 */

#define X rv_cpu.x
#define F rv_cpu.f
#define SX32(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))
#define U64(v) ((uint64_t)(v))
#define S64(v) ((uint64_t)(int64_t)(v))

/* Memory access. One unsigned compare: a - g_lo <= g_span covers 8-byte accesses. */
#define LD(T, a) ({ T v_; memcpy(&v_, M + (uint32_t)(a), sizeof v_); v_; })
#define ST(T, a, v) do { T v_ = (T)(v); memcpy(M + (uint32_t)(a), &v_, sizeof v_); } while (0)

/* LX_MEMCHECK selects the access check, for measuring what safety costs:
 *   0  none: an out-of-window address is not caught (unsafe; measurement only)
 *   1  the window check (default): one unsigned compare
 *   2  the window check plus the protection of the page holding the first byte (an
 *      access that crosses into the next page is judged by its first page) */
#ifndef LX_MEMCHECK
#define LX_MEMCHECK 1
#endif
extern uint8_t *pf;
#if LX_MEMCHECK == 0
#define BADADDR(a, kind, glo, gspan) 0
#elif LX_MEMCHECK == 1
#define BADADDR(a, kind, glo, gspan) __builtin_expect((uint64_t)((a) - (glo)) > (gspan), 0)
#else
#define BADADDR(a, kind, glo, gspan) __builtin_expect((uint64_t)((a) - (glo)) > (gspan) || \
    !(pf[(a) >> 12] & ((kind) == 2 ? 4 : 2)), 0)
#endif

/* Exit information shared by both dispatch modes. */
static struct { uint64_t fa; int fk; int64_t left; } xs;

/* A block transition: find (or decode) the next block, remember it in the slot, charge
 * its instructions to the budget. A flush during decoding frees the current block, so
 * the slot is only written if no flush happened. */
static inline struct block *next_block(struct block *b, struct block **slot, uint64_t t) {
  struct block *nb = *slot;
  if (__builtin_expect(nb && nb->pc == t, 1)) return nb;
  uint64_t fl = rv_flushes;
  nb = lookup(t);
  if (fl == rv_flushes) *slot = nb;
  (void)b;
  return nb;
}

#ifdef LX_PROFILE
/* Dynamic counts of consecutive op pairs inside blocks (build with -DLX_PROFILE and
 * --dispatch=switch); printed at exit by rv_prof_dump(). */
static uint64_t rv_prof[OP_COUNT][OP_COUNT];
static const char *opname(int k) {
  static const char *in[] = {"FALLTHRU", "FETCHFAULT", "LI", "MV", "AUIPC_ADDI", "NOP"};
  return k < RV_NOPS ? rv_op_name[k] : in[k - RV_NOPS];
}
void rv_prof_dump(void) {
  uint64_t tot = 0;
  for (int i = 0; i < OP_COUNT; i++) for (int j = 0; j < OP_COUNT; j++) tot += rv_prof[i][j];
  for (int n = 0; n < 25; n++) {
    int bi = 0, bj = 0;
    for (int i = 0; i < OP_COUNT; i++) for (int j = 0; j < OP_COUNT; j++) if (rv_prof[i][j] > rv_prof[bi][bj]) bi = i, bj = j;
    if (!rv_prof[bi][bj]) break;
    fprintf(stderr, "pair %-10s %-10s %5.2f%%\n", opname(bi), opname(bj), 100.0 * (double)rv_prof[bi][bj] / (double)tot);
    rv_prof[bi][bj] = 0;
  }
}
#else
void rv_prof_dump(void) {}
#endif

/* ---------------- dispatch 1: switch in a loop ---------------- */
static int run_switch(void) {
  uint8_t *const M = g_mem;
  const uint64_t glo = g_lo, gspan = g_span;
  (void)glo; (void)gspan;
  int64_t left = (int64_t)lx_budget;
  const struct op *o;
  struct block *b = lookup(rv_cpu.pc);
  uint64_t target;
  int stop;
  left -= b->ninsn;
  if (left < 0) { rv_cpu.pc = b->pc; xs.left = left + b->ninsn; return LX_STOP_BUDGET; }
  o = b->ops;
#define OP(name) case name:
#ifdef LX_PROFILE
#define NEXT do { rv_prof[o->k][o[1].k]++; o++; goto next; } while (0)
#else
#define NEXT do { o++; goto next; } while (0)
#endif
#define TAKEN(e) do { target = (e); b = next_block(b, &b->taken, target); goto enter; } while (0)
#define FALL(e) do { target = (e); b = next_block(b, &b->fall, target); goto enter; } while (0)
#define INDIRECT(e) TAKEN(e)
#define EXIT(code, addr, kind) do { stop = (code); xs.fa = (addr); xs.fk = (kind); rv_cpu.pc = o->pc; goto out; } while (0)
#define FAULT(a, kind) EXIT((kind) == 4 ? LX_STOP_MISALIGNED : LX_STOP_FAULT, (a), (kind))
#define ILLEGAL EXIT(LX_STOP_ILLEGAL, 0, 0)
#define CHK(a, kind) do { if (BADADDR((a), (kind), glo, gspan)) FAULT((a), (kind)); } while (0)
#define FENCEI() do { rv_cpu.pc = (uint64_t)o->pc + o->len; rv_flush(0, ~0ull); b = lookup(rv_cpu.pc); goto enter; } while (0)
  for (;;) {
  next:
    switch (o->k) {
#include "rv_ops.inc"
    default: ILLEGAL;
    }
  enter:
    left -= b->ninsn;
    if (__builtin_expect(left < 0, 0)) { rv_cpu.pc = b->pc; left += b->ninsn; stop = LX_STOP_BUDGET; goto out; }
    o = b->ops;
  }
out:
  xs.left = left;
  return stop;
#undef OP
#undef NEXT
#undef TAKEN
#undef FALL
#undef INDIRECT
#undef EXIT
#undef FAULT
#undef ILLEGAL
#undef CHK
#undef FENCEI
}

/* ---------------- dispatch 2: tail-call threaded ----------------
 * Every instruction kind is a function with the same signature; each one ends by tail
 * calling the handler of the next op (musttail; return_call_indirect in WebAssembly), so
 * every handler has its own indirect branch. State travels in the arguments. */
#ifndef LX_NO_TAILCALL
#define HARGS const struct op *o, uint8_t *const M, int64_t left, struct block *b, uint64_t t
#define HPASS o, M, left, b, t
typedef int hfn(HARGS);
static hfn *const htab_ops[OP_COUNT];
#define MUSTTAIL __attribute__((musttail))
static int x_exit(const struct op *o, int code, uint64_t addr, int kind, int64_t left) {
  xs.fa = addr; xs.fk = kind; xs.left = left; rv_cpu.pc = o->pc;
  return code;
}
static int x_enter(HARGS) {
  left -= b->ninsn;
  if (__builtin_expect(left < 0, 0)) { rv_cpu.pc = b->pc; xs.left = left + b->ninsn; return LX_STOP_BUDGET; }
  o = b->ops;
  MUSTTAIL return htab_ops[o->k](HPASS);
}
static int x_taken(HARGS) { b = next_block(b, &b->taken, t); MUSTTAIL return x_enter(HPASS); }
static int x_fall(HARGS) { b = next_block(b, &b->fall, t); MUSTTAIL return x_enter(HPASS); }
#define OP(name) static int h_##name(HARGS)
#define NEXT do { o++; MUSTTAIL return htab_ops[o->k](HPASS); } while (0)
#define TAKEN(e) do { t = (e); MUSTTAIL return x_taken(HPASS); } while (0)
#define FALL(e) do { t = (e); MUSTTAIL return x_fall(HPASS); } while (0)
#define INDIRECT(e) TAKEN(e)
#define EXIT(code, addr, kind) return x_exit(o, (code), (addr), (kind), left)
#define FAULT(a, kind) EXIT((kind) == 4 ? LX_STOP_MISALIGNED : LX_STOP_FAULT, (a), (kind))
#define ILLEGAL EXIT(LX_STOP_ILLEGAL, 0, 0)
#define CHK(a, kind) do { if (BADADDR((a), (kind), g_lo, g_span)) FAULT((a), (kind)); } while (0)
#define FENCEI() do { rv_cpu.pc = (uint64_t)o->pc + o->len; rv_flush(0, ~0ull); b = lookup(rv_cpu.pc); MUSTTAIL return x_enter(HPASS); } while (0)
#include "rv_ops.inc"
#undef OP
#define HENT(n, f, fl) [RV_##n] = h_RV_##n,
static hfn *const htab_ops[OP_COUNT] = {
  RV_OPS(HENT)
  [OP_FALLTHRU] = h_OP_FALLTHRU, [OP_FETCHFAULT] = h_OP_FETCHFAULT, [OP_LI] = h_OP_LI, [OP_MV] = h_OP_MV,
  [OP_AUIPC_ADDI] = h_OP_AUIPC_ADDI, [OP_NOP] = h_OP_NOP,
};
#undef HENT
static int run_tail(void) {
  struct block *b = lookup(rv_cpu.pc);
  return x_enter(b->ops, g_mem, (int64_t)lx_budget, b, 0);
}
#endif

/* --dispatch=switch|tail. Measured 2026-11-26 (docs/LINUX-BINARIES.md section 5): the
 * tail-call version is about 10% faster natively and 1.4 to 2.1 times slower under V8,
 * so WebAssembly builds default to the switch. */
#ifdef __wasm__
int rv_dispatch_tail = 0;
#else
int rv_dispatch_tail = 1;
#endif

static int rv_run(struct lx_fault *f) {
  rv_cpu.res_addr = ~0ull; /* any trap into the engine breaks an LR/SC sequence */
  int64_t budget = (int64_t)lx_budget;
  int stop;
#ifndef LX_NO_TAILCALL
  if (rv_dispatch_tail) stop = run_tail(); else
#endif
  stop = run_switch();
  f->pc = rv_cpu.pc; f->addr = xs.fa; f->kind = xs.fk;
  rv_cpu.instret += (uint64_t)(budget - xs.left);
  return stop;
}

/* ---- the arch interface ---- */
void rv_exec_reset(void);
static void rv_reset(uint64_t entry, uint64_t sp) {
  rv_exec_reset(); /* the signal trampoline belonged to the old image */
  memset(&rv_cpu, 0, sizeof rv_cpu);
  rv_cpu.pc = entry;
  rv_cpu.x[RV_REG_SP] = sp;
  rv_cpu.res_addr = ~0ull;
  rv_flush(0, ~0ull);
}
static int rv_sys_nr(void) { return (int)rv_cpu.x[RV_REG_A7]; }
static void rv_sys_args(uint64_t a[6]) { for (int i = 0; i < 6; i++) a[i] = rv_cpu.x[RV_REG_A0 + i]; }
static void rv_sys_ret(int64_t v) { rv_cpu.x[RV_REG_A0] = (uint64_t)v; }
static void rv_sys_skip(void) { rv_cpu.pc += 4; }
static uint64_t rv_get_pc(void) { return rv_cpu.pc; }
static uint64_t rv_get_instret(void) { return rv_cpu.instret; }
static void rv_set_pc(uint64_t pc) { rv_cpu.pc = pc; }
static uint64_t rv_get_sp(void) { return rv_cpu.x[RV_REG_SP]; }
static void rv_set_sp(uint64_t sp) { rv_cpu.x[RV_REG_SP] = sp; }
static void rv_set_tp(uint64_t tp) { rv_cpu.x[RV_REG_TP] = tp; }
static void rv_save(void *buf) { memcpy(buf, &rv_cpu, sizeof rv_cpu); }
static void rv_load(const void *buf) { memcpy(&rv_cpu, buf, sizeof rv_cpu); }

int rv_sig_setup(int sig, uint64_t handler, uint64_t flags, uint64_t restorer, uint64_t mask_old,
                 uint64_t sp, const struct lx_fault *info, int sicode);
int rv_sig_return(uint64_t *mask_out);
int64_t rv_sys_arch(int nr, uint64_t a[6]);

/* AT_HWCAP: one bit per single-letter extension, bit (letter - 'a'). IMAFDC. */
#define HW(c) (1ull << ((c) - 'a'))
const struct lx_arch arch_riscv64 = {
  .name = "riscv64", .e_machine = RV_EM_RISCV,
  .hwcap = HW('i') | HW('m') | HW('a') | HW('f') | HW('d') | HW('c'),
  .platform = 0, .uname_machine = "riscv64",
  .reset = rv_reset, .run = rv_run, .sys_nr = rv_sys_nr, .sys_args = rv_sys_args,
  .sys_ret = rv_sys_ret, .sys_skip = rv_sys_skip, .get_pc = rv_get_pc, .get_instret = rv_get_instret, .set_pc = rv_set_pc,
  .get_sp = rv_get_sp, .set_tp = rv_set_tp, .set_sp = rv_set_sp,
  .sig_setup = rv_sig_setup, .sig_return = rv_sig_return, .flush_code = rv_flush,
  .sys_arch = rv_sys_arch, .state_size = sizeof(struct rv_cpu), .save = rv_save, .load = rv_load,
};
const struct lx_arch *arch_cur = &arch_riscv64;
