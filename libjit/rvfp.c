// SPDX-License-Identifier: GPL-2.0-or-later
// RISC-V F and D, for the interpreter (C) and the translator (wasm).
//
// What matches wasm exactly and is emitted inline: add, sub, mul, div, sqrt, conversions
// and comparisons when the rounding mode is round-to-nearest-even (static rm 0, or dynamic
// rm with frm = 0 when the region is translated), with NaN results replaced by RISC-V's
// canonical NaN; sign injection; min/max with RISC-V's NaN rules; moves, loads, stores;
// float-to-integer conversions with rtz/rne/rdn/rup (wasm's saturating truncation plus a
// NaN fix-up). What goes to a helper (rvfp_eval in C, exact): fused multiply-add (wasm has
// none), every other rounding mode, fclass.
//
// Not implemented anywhere: the accrued exception flags (fflags). Translated code and the
// interpreter both leave them as software last wrote them. Documented deviation.
//
// The software arithmetic below (round_pack and friends) is written from the IEEE 754
// rules: an exact value (sign, integer significand, exponent, sticky bit) is rounded once.
#include "rvfp.h"
#include "engine.h"

int arch_fp_enabled = 1;
int rvfp_frm;               // frm assumed by translations made now (the engine keeps it current)
int rvfp_fast_fma;          // measurement only: fmadd as an unfused multiply and add (a deviation)

#define CANON64 0x7ff8000000000000ull
#define CANON32 0x7fc00000u
#define BOX 0xffffffff00000000ull
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t unbox(uint64_t v) { return (v >> 32) == 0xffffffffu ? (uint32_t)v : CANON32; }
static inline uint64_t box(uint32_t v) { return BOX | v; }
static inline uint64_t canon_d(double d) { return d != d ? CANON64 : d2u(d); }
static inline uint64_t canon_s(float f) { return f != f ? box(CANON32) : box(f2u(f)); }

// ---- 128-bit helpers ----
typedef struct { uint64_t hi, lo; } u128;
static inline u128 U(uint64_t hi, uint64_t lo) { u128 r = { hi, lo }; return r; }
static inline int is0(u128 a) { return !a.hi && !a.lo; }
static u128 mul64(uint64_t a, uint64_t b) {
  uint64_t alo = a & 0xffffffffu, ahi = a >> 32, blo = b & 0xffffffffu, bhi = b >> 32;
  uint64_t p0 = alo * blo, p1 = alo * bhi, p2 = ahi * blo, p3 = ahi * bhi;
  __asm__("" : "+r"(p1));
  uint64_t mid = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
  return U(p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32), (mid << 32) | (p0 & 0xffffffffu));
}
static int clz128(u128 a) { return a.hi ? __builtin_clzll(a.hi) : a.lo ? 64 + __builtin_clzll(a.lo) : 128; }
static u128 shl(u128 a, int n) {
  if (n <= 0) return a;
  if (n >= 128) return U(0, 0);
  if (n >= 64) return U(a.lo << (n - 64), 0);
  return U(a.hi << n | a.lo >> (64 - n), a.lo << n);
}
static u128 shr(u128 a, int n) {
  if (n <= 0) return a;
  if (n >= 128) return U(0, 0);
  if (n >= 64) return U(0, a.hi >> (n - 64));
  return U(a.hi >> n, a.lo >> n | a.hi << (64 - n));
}
static u128 shr_jam(u128 a, int n) {      // shift right, OR every lost bit into bit 0
  if (n <= 0) return a;
  u128 r = shr(a, n);
  if (!is0(a) && (n >= 128 || !is0(U(a.hi ^ shl(r, n).hi, a.lo ^ shl(r, n).lo)))) r.lo |= 1;
  return r;
}
static u128 add(u128 a, u128 b) { u128 r = U(a.hi + b.hi, a.lo + b.lo); if (r.lo < a.lo) r.hi++; return r; }
static u128 sub(u128 a, u128 b) { u128 r = U(a.hi - b.hi, a.lo - b.lo); if (a.lo < b.lo) r.hi--; return r; }
static int cmp(u128 a, u128 b) { return a.hi != b.hi ? (a.hi < b.hi ? -1 : 1) : a.lo != b.lo ? (a.lo < b.lo ? -1 : 1) : 0; }

typedef struct { int mant, bias, ebits; } ffmt;
static const ffmt FD = { 52, 1023, 11 }, FS = { 23, 127, 8 };

enum { RNE, RTZ, RDN, RUP, RMM };

// Decide whether to add one unit to the kept significand.
static int round_up(int rm, int sign, int c, int inexact, uint64_t keep) {
  switch (rm) {
  case RNE: return c > 0 || (c == 0 && (keep & 1));
  case RTZ: return 0;
  case RDN: return sign && inexact;
  case RUP: return !sign && inexact;
  default: return c >= 0;
  }
}

// Round (-1)^sign * m * 2^e (+ a sticky bit below bit 0) to the format.
static uint64_t round_pack(int sign, u128 m, int e, int sticky, const ffmt *f, int rm) {
  uint64_t sbit = (uint64_t)sign << (f->mant + f->ebits);
  if (is0(m)) return sbit;                      // callers handle exact zero signs
  int msb = 127 - clz128(m);
  int E = msb + e, biased = E + f->bias;
  int L = biased >= 1 ? E - f->mant : 1 - f->bias - f->mant;   // weight of the lowest kept bit
  int drop = L - e;
  uint64_t keep;
  int c, inexact;
  if (drop <= 0) { keep = shl(m, -drop).lo; c = sticky ? -1 : -2; inexact = sticky; }
  else {
    u128 k = shr(m, drop);
    keep = k.lo;
    u128 rem = sub(m, shl(k, drop));
    if (drop > 128) { c = -1; inexact = 1; }
    else {
      u128 half = shl(U(0, 1), drop - 1);
      c = cmp(rem, half);
      if (c == 0 && sticky) c = 1;
      inexact = !is0(rem) || sticky;
    }
  }
  if (c == -2) c = -1;
  if (inexact && round_up(rm, sign, c, inexact, keep)) keep++;
  uint64_t bits = biased >= 1 ? ((uint64_t)(biased - 1) << f->mant) + keep : keep;
  uint64_t emax = (1ull << f->ebits) - 1;
  if ((bits >> f->mant) >= emax) {              // overflow
    int to_inf = rm == RNE || rm == RMM || (rm == RUP && !sign) || (rm == RDN && sign);
    bits = to_inf ? emax << f->mant : (emax << f->mant) - 1;
  }
  return sbit | bits;
}

// value classes
enum { C_ZERO, C_FIN, C_INF, C_NAN };
static int unpack(uint64_t u, const ffmt *f, int *sign, uint64_t *m, int *e) {
  int total = f->mant + f->ebits;
  *sign = (int)(u >> total) & 1;
  uint64_t ex = (u >> f->mant) & ((1ull << f->ebits) - 1), fr = u & ((1ull << f->mant) - 1);
  if (ex == (1ull << f->ebits) - 1) return fr ? C_NAN : C_INF;
  if (ex == 0) { if (!fr) return C_ZERO; *m = fr; *e = 1 - f->bias - f->mant; return C_FIN; }
  *m = fr | 1ull << f->mant; *e = (int)ex - f->bias - f->mant;
  return C_FIN;
}
static uint64_t canon(const ffmt *f) { return f == &FD ? CANON64 : CANON32; }
static uint64_t inf(const ffmt *f, int s) { return (uint64_t)s << (f->mant + f->ebits) | ((1ull << f->ebits) - 1) << f->mant; }
static uint64_t zero(const ffmt *f, int s) { return (uint64_t)s << (f->mant + f->ebits); }

// (-1)^negp * a * b + (-1)^negc * c, rounded once. noadd: the product only.
static uint64_t soft_fma(uint64_t a, uint64_t b, uint64_t c, int negp, int negc, int noadd, const ffmt *f, int rm) {
  int sa, sb, sc, ea, eb, ec;
  uint64_t ma, mb, mc;
  int ka = unpack(a, f, &sa, &ma, &ea), kb = unpack(b, f, &sb, &mb, &eb), kc = noadd ? C_ZERO : unpack(c, f, &sc, &mc, &ec);
  if (noadd) sc = 0;
  int sp = sa ^ sb ^ negp;
  sc ^= negc;
  if (ka == C_NAN || kb == C_NAN || kc == C_NAN) return canon(f);
  if (ka == C_INF || kb == C_INF) {
    if (ka == C_ZERO || kb == C_ZERO) return canon(f);
    if (kc == C_INF && sc != sp) return canon(f);
    return inf(f, sp);
  }
  if (kc == C_INF) return inf(f, sc);
  if (ka == C_ZERO || kb == C_ZERO) {
    if (kc != C_ZERO) return c ^ ((uint64_t)negc << (f->mant + f->ebits));
    if (noadd) return zero(f, sp);
    return zero(f, sp == sc ? sp : rm == RDN);
  }
  u128 P = mul64(ma, mb);
  int ep = ea + eb;
  if (kc == C_ZERO) return round_pack(sp, P, ep, 0, f, rm);
  u128 C = U(0, mc);
  // put both leading bits at bit 125, then align
  int sh = 125 - (127 - clz128(P)); P = shl(P, sh); ep -= sh;
  sh = 125 - (127 - clz128(C)); C = shl(C, sh); ec -= sh;
  int e;
  if (ep >= ec) { C = shr_jam(C, ep - ec); e = ep; } else { P = shr_jam(P, ec - ep); e = ec; }
  u128 S;
  int s;
  if (sp == sc) { S = add(P, C); s = sp; }
  else {
    int k = cmp(P, C);
    if (k == 0) return zero(f, rm == RDN);
    if (k > 0) { S = sub(P, C); s = sp; } else { S = sub(C, P); s = sc; }
  }
  return round_pack(s, S, e, 0, f, rm);
}

static uint64_t soft_div(uint64_t a, uint64_t b, const ffmt *f, int rm) {
  int sa, sb, ea, eb;
  uint64_t ma, mb;
  int ka = unpack(a, f, &sa, &ma, &ea), kb = unpack(b, f, &sb, &mb, &eb), s = sa ^ sb;
  if (ka == C_NAN || kb == C_NAN) return canon(f);
  if (ka == C_INF) return kb == C_INF ? canon(f) : inf(f, s);
  if (kb == C_INF) return zero(f, s);
  if (kb == C_ZERO) return ka == C_ZERO ? canon(f) : inf(f, s);
  if (ka == C_ZERO) return zero(f, s);
  while (!(ma >> f->mant)) { ma <<= 1; ea--; }
  while (!(mb >> f->mant)) { mb <<= 1; eb--; }
  // q = (ma << 64) / mb, by shift and subtract
  u128 n = U(ma, 0), q = U(0, 0), r = U(0, 0);
  for (int i = 127; i >= 0; i--) {
    r = shl(r, 1);
    if ((i >= 64 ? n.hi >> (i - 64) : n.lo >> i) & 1) r.lo |= 1;
    q = shl(q, 1);
    if (cmp(r, U(0, mb)) >= 0) { r = sub(r, U(0, mb)); q.lo |= 1; }
  }
  return round_pack(s, q, ea - eb - 64, !is0(r), f, rm);
}

static uint64_t soft_sqrt(uint64_t a, const ffmt *f, int rm) {
  int sa, ea;
  uint64_t ma;
  int ka = unpack(a, f, &sa, &ma, &ea);
  if (ka == C_NAN) return canon(f);
  if (ka == C_ZERO) return a;
  if (sa) return canon(f);
  if (ka == C_INF) return a;
  while (!(ma >> f->mant)) { ma <<= 1; ea--; }
  if (ea & 1) { ma <<= 1; ea--; }
  u128 n = U(ma, 0), root = U(0, 0), rem = U(0, 0);   // n = ma * 2^64
  for (int i = 63; i >= 0; i--) {                     // two bits of n per step
    rem = shl(rem, 2);
    int hi = 2 * i + 1;
    uint64_t two = ((hi >= 64 ? n.hi >> (hi - 64) : n.lo >> hi) & 1) << 1 | ((hi - 1 >= 64 ? n.hi >> (hi - 65) : n.lo >> (hi - 1)) & 1);
    rem.lo |= two;
    u128 t = add(shl(root, 2), U(0, 1));
    root = shl(root, 1);
    if (cmp(rem, t) >= 0) { rem = sub(rem, t); root.lo |= 1; }
  }
  return round_pack(0, root, (ea - 64) / 2, !is0(rem), f, rm);
}

// float -> integer, RISC-V saturation (NaN gives the maximum)
static uint64_t to_int(uint64_t a, const ffmt *f, int rm, int bits, int issigned) {
  int s, e;
  uint64_t m;
  int k = unpack(a, f, &s, &m, &e);
  uint64_t umax = bits == 64 ? ~0ull : (1ull << bits) - 1;
  int64_t smax = bits == 64 ? INT64_MAX : (int64_t)((1ull << (bits - 1)) - 1), smin = -smax - 1;
  uint64_t r;
  if (k == C_NAN) r = issigned ? (uint64_t)smax : umax;
  else if (k == C_INF) r = issigned ? (uint64_t)(s ? smin : smax) : (s ? 0 : umax);
  else if (k == C_ZERO) r = 0;
  else {
    uint64_t mag;
    int big = 0;
    if (e >= 0) { if (63 - __builtin_clzll(m) + e >= 64) big = 1; mag = big ? 0 : m << e; }
    else {
      int drop = -e;
      uint64_t keep = drop >= 64 ? 0 : m >> drop;
      u128 rem = drop >= 64 ? U(0, m) : U(0, m - (keep << drop));
      int c = drop > 64 ? -1 : cmp(rem, drop == 64 ? U(0, 1ull << 63) : U(0, 1ull << (drop - 1)));
      int inexact = !is0(rem);
      if (inexact && round_up(rm, s, c, inexact, keep)) keep++;
      mag = keep;
    }
    if (issigned) {
      if (big || (!s && mag > (uint64_t)smax) || (s && mag > (uint64_t)smax + 1)) r = (uint64_t)(s ? smin : smax);
      else r = s ? 0 - mag : mag;
    } else {
      if (s && mag) r = 0;
      else if (big || mag > umax) r = umax;
      else r = mag;
    }
  }
  if (bits == 32) r = (uint64_t)(int64_t)(int32_t)(uint32_t)r;   // W results are sign-extended
  return r;
}

static uint64_t from_int(uint64_t v, int issigned, int bits, const ffmt *f, int rm) {
  int s = 0;
  if (bits == 32) v = issigned ? (uint64_t)(int64_t)(int32_t)v : (uint32_t)v;
  if (issigned && (int64_t)v < 0) { s = 1; v = 0 - v; }
  if (!v) return 0;
  return round_pack(s, U(0, v), 0, 0, f, rm);
}

static uint64_t fclass(uint64_t u, const ffmt *f) {
  int s, e;
  uint64_t m;
  int k = unpack(u, f, &s, &m, &e);
  if (k == C_NAN) return (u >> (f->mant - 1)) & 1 ? 1u << 9 : 1u << 8;
  if (k == C_INF) return s ? 1u << 0 : 1u << 7;
  if (k == C_ZERO) return s ? 1u << 3 : 1u << 4;
  int sub_ = !((u >> f->mant) & ((1ull << f->ebits) - 1));
  return s ? (sub_ ? 1u << 2 : 1u << 1) : (sub_ ? 1u << 5 : 1u << 6);
}

double rvfp_fma64(double a, double b, double c) { return u2d(soft_fma(d2u(a), d2u(b), d2u(c), 0, 0, 0, &FD, RNE)); }
float rvfp_fma32(float a, float b, float c) { return u2f((uint32_t)soft_fma(f2u(a), f2u(b), f2u(c), 0, 0, 0, &FS, RNE)); }

// Evaluate one F/D compute instruction with an explicit rounding mode (0..4). Operands and
// result are raw register bits (f registers boxed; x registers as they are).
uint64_t rvfp_eval(int op, int rm, uint64_t a, uint64_t b, uint64_t c) {
  int S = (op >= RV_FADD_S && op <= RV_FMAX_S) || op == RV_FMADD_S || op == RV_FMSUB_S || op == RV_FNMSUB_S || op == RV_FNMADD_S || op == RV_FEQ_S || op == RV_FLT_S || op == RV_FLE_S ||
          (op >= RV_FCVT_W_S && op <= RV_FCVT_LU_S) || (op >= RV_FCVT_S_W && op <= RV_FCVT_S_LU) || op == RV_FCLASS_S;
  const ffmt *f = S ? &FS : &FD;
  uint64_t A = S ? unbox(a) : a, B = S ? unbox(b) : b, Cc = S ? unbox(c) : c;
#define RES(x) (S ? box((uint32_t)(x)) : (x))
  if (rm == RNE) {               // the hardware's arithmetic is exactly this
    switch (op) {
    case RV_FADD_S: return canon_s(u2f((uint32_t)A) + u2f((uint32_t)B));
    case RV_FSUB_S: return canon_s(u2f((uint32_t)A) - u2f((uint32_t)B));
    case RV_FMUL_S: return canon_s(u2f((uint32_t)A) * u2f((uint32_t)B));
    case RV_FDIV_S: return canon_s(u2f((uint32_t)A) / u2f((uint32_t)B));
    case RV_FADD_D: return canon_d(u2d(A) + u2d(B));
    case RV_FSUB_D: return canon_d(u2d(A) - u2d(B));
    case RV_FMUL_D: return canon_d(u2d(A) * u2d(B));
    case RV_FDIV_D: return canon_d(u2d(A) / u2d(B));
    }
  }
  switch (op) {
  case RV_FADD_S: case RV_FADD_D: return RES(soft_fma(A, S ? 0x3f800000u : 0x3ff0000000000000ull, B, 0, 0, 0, f, rm));
  case RV_FSUB_S: case RV_FSUB_D: return RES(soft_fma(A, S ? 0x3f800000u : 0x3ff0000000000000ull, B, 0, 1, 0, f, rm));
  case RV_FMUL_S: case RV_FMUL_D: return RES(soft_fma(A, B, 0, 0, 0, 1, f, rm));
  case RV_FDIV_S: case RV_FDIV_D: return RES(soft_div(A, B, f, rm));
  case RV_FSQRT_S: case RV_FSQRT_D: return RES(soft_sqrt(A, f, rm));
  case RV_FMADD_S: case RV_FMADD_D: return RES(soft_fma(A, B, Cc, 0, 0, 0, f, rm));
  case RV_FMSUB_S: case RV_FMSUB_D: return RES(soft_fma(A, B, Cc, 0, 1, 0, f, rm));
  case RV_FNMSUB_S: case RV_FNMSUB_D: return RES(soft_fma(A, B, Cc, 1, 0, 0, f, rm));
  case RV_FNMADD_S: case RV_FNMADD_D: return RES(soft_fma(A, B, Cc, 1, 1, 0, f, rm));
  case RV_FCVT_W_S: case RV_FCVT_W_D: return to_int(A, f, rm, 32, 1);
  case RV_FCVT_WU_S: case RV_FCVT_WU_D: return to_int(A, f, rm, 32, 0);
  case RV_FCVT_L_S: case RV_FCVT_L_D: return to_int(A, f, rm, 64, 1);
  case RV_FCVT_LU_S: case RV_FCVT_LU_D: return to_int(A, f, rm, 64, 0);
  case RV_FCVT_S_W: case RV_FCVT_D_W: return RES(from_int(a, 1, 32, f, rm));
  case RV_FCVT_S_WU: case RV_FCVT_D_WU: return RES(from_int(a, 0, 32, f, rm));
  case RV_FCVT_S_L: case RV_FCVT_D_L: return RES(from_int(a, 1, 64, f, rm));
  case RV_FCVT_S_LU: case RV_FCVT_D_LU: return RES(from_int(a, 0, 64, f, rm));
  case RV_FCVT_S_D: {            // double -> single: round the exact double value
    int s, e; uint64_t m;
    int k = unpack(a, &FD, &s, &m, &e);
    if (k == C_NAN) return box(CANON32);
    if (k == C_INF) return box((uint32_t)inf(&FS, s));
    if (k == C_ZERO) return box((uint32_t)zero(&FS, s));
    return box((uint32_t)round_pack(s, U(0, m), e, 0, &FS, rm));
  }
  case RV_FCVT_D_S: { float x = u2f(unbox(a)); return canon_d((double)x); }
  case RV_FCLASS_S: case RV_FCLASS_D: return fclass(A, f);
  }
  return 0;
}

// ---- interpreter ----
static int eff_rm(const rv_cpu *c, int rm) { return rm == 7 ? (int)(c->fcsr >> 5 & 7) : rm; }

int rvfp_exec(rv_cpu *c, const rv_insn *d) {
  int op = d->op;
  if (op >= RV_CSRRW && op <= RV_CSRRCI) {
    uint64_t old;
    switch (d->imm) {
    case 1: old = c->fcsr & 31; break;
    case 2: old = c->fcsr >> 5 & 7; break;
    case 3: old = c->fcsr & 0xff; break;
    case 0xc00: case 0xc02: old = c->icount; break;
    case 0xc01: old = (uint64_t)(jit_now_ms() * 1e4); break;   // a 10 MHz time counter
    default: return R_INTERP;
    }
    uint64_t src = op >= RV_CSRRWI ? d->rs1 : c->x[d->rs1], nv;
    int base = op >= RV_CSRRWI ? op - 3 : op;
    nv = base == RV_CSRRW ? src : base == RV_CSRRS ? old | src : old & ~src;
    if (base == RV_CSRRW || d->rs1 != 0) {
      if (d->imm >= 0xc00) return R_INTERP;
      uint32_t before = c->fcsr;
      if (d->imm == 1) c->fcsr = (c->fcsr & ~31u) | (uint32_t)(nv & 31);
      else if (d->imm == 2) c->fcsr = (c->fcsr & 31u) | (uint32_t)(nv & 7) << 5;
      else c->fcsr = (uint32_t)(nv & 0xff);
      if ((before ^ c->fcsr) >> 5) engine_frm_changed();
    }
    if (d->rd) c->x[d->rd] = old;
    return -1;
  }
  uint64_t *F = c->f, *X = c->x;
  int rm = eff_rm(c, d->rm);
  switch (op) {
  case RV_FLW: case RV_FLD: {
    uint64_t ea = X[d->rs1] + (uint64_t)d->imm;
    int n = op == RV_FLW ? 4 : 8;
    if (ea > rv_memsize - (uint64_t)n) { c->fault_addr = ea; return R_FAULT; }
    if (op == RV_FLW) { uint32_t v; memcpy(&v, rv_mem + ea, 4); F[d->rd] = box(v); }
    else memcpy(&F[d->rd], rv_mem + ea, 8);
    return -1;
  }
  case RV_FSW: case RV_FSD: {
    uint64_t ea = X[d->rs1] + (uint64_t)d->imm;
    int n = op == RV_FSW ? 4 : 8;
    if (ea > rv_memsize - (uint64_t)n) { c->fault_addr = ea; return R_FAULT; }
    rv_store_hook(ea, n);
    memcpy(rv_mem + ea, &F[d->rs2], (size_t)n);
    return -1;
  }
  case RV_FSGNJ_S: case RV_FSGNJN_S: case RV_FSGNJX_S: {
    uint32_t a = unbox(F[d->rs1]), b = unbox(F[d->rs2]), s = 0x80000000u;
    uint32_t r = op == RV_FSGNJ_S ? (a & ~s) | (b & s) : op == RV_FSGNJN_S ? (a & ~s) | (~b & s) : a ^ (b & s);
    F[d->rd] = box(r);
    return -1;
  }
  case RV_FSGNJ_D: case RV_FSGNJN_D: case RV_FSGNJX_D: {
    uint64_t a = F[d->rs1], b = F[d->rs2], s = 1ull << 63;
    F[d->rd] = op == RV_FSGNJ_D ? (a & ~s) | (b & s) : op == RV_FSGNJN_D ? (a & ~s) | (~b & s) : a ^ (b & s);
    return -1;
  }
  case RV_FMIN_S: case RV_FMAX_S: {
    float a = u2f(unbox(F[d->rs1])), b = u2f(unbox(F[d->rs2]));
    float r;
    if (a != a) r = b != b ? u2f(CANON32) : b;
    else if (b != b) r = a;
    else if (a == b) r = (op == RV_FMIN_S) == !!(f2u(a) >> 31) ? a : b;   // -0 < +0
    else r = (op == RV_FMIN_S) == (a < b) ? a : b;
    F[d->rd] = box(f2u(r));
    return -1;
  }
  case RV_FMIN_D: case RV_FMAX_D: {
    double a = u2d(F[d->rs1]), b = u2d(F[d->rs2]);
    double r;
    if (a != a) r = b != b ? u2d(CANON64) : b;
    else if (b != b) r = a;
    else if (a == b) r = (op == RV_FMIN_D) == !!(d2u(a) >> 63) ? a : b;
    else r = (op == RV_FMIN_D) == (a < b) ? a : b;
    F[d->rd] = d2u(r);
    return -1;
  }
  case RV_FEQ_S: case RV_FLT_S: case RV_FLE_S: {
    float a = u2f(unbox(F[d->rs1])), b = u2f(unbox(F[d->rs2]));
    uint64_t r = op == RV_FEQ_S ? a == b : op == RV_FLT_S ? a < b : a <= b;
    if (d->rd) X[d->rd] = r;
    return -1;
  }
  case RV_FEQ_D: case RV_FLT_D: case RV_FLE_D: {
    double a = u2d(F[d->rs1]), b = u2d(F[d->rs2]);
    uint64_t r = op == RV_FEQ_D ? a == b : op == RV_FLT_D ? a < b : a <= b;
    if (d->rd) X[d->rd] = r;
    return -1;
  }
  case RV_FMV_X_W: if (d->rd) X[d->rd] = (uint64_t)(int64_t)(int32_t)(uint32_t)F[d->rs1]; return -1;
  case RV_FMV_X_D: if (d->rd) X[d->rd] = F[d->rs1]; return -1;
  case RV_FMV_W_X: F[d->rd] = box((uint32_t)X[d->rs1]); return -1;
  case RV_FMV_D_X: F[d->rd] = X[d->rs1]; return -1;
  }
  if (rm > 4) return R_INTERP;   // reserved rounding mode: illegal instruction
  uint64_t r = rvfp_eval(op, rm, (op >= RV_FCVT_S_W && op <= RV_FCVT_D_LU) ? X[d->rs1] : F[d->rs1], F[d->rs2], F[d->rs3]);
  int to_x = (op >= RV_FCVT_W_S && op <= RV_FCVT_LU_D) || op == RV_FCLASS_S || op == RV_FCLASS_D;
  if (to_x) { if (d->rd) X[d->rd] = r; }
  else F[d->rd] = r;
  return -1;
}

uint64_t rvfp_help(rv_cpu *c, int op, uint64_t a, uint64_t b, uint64_t c3) { (void)c; return rvfp_eval(op & 0xffff, op >> 16, a, b, c3); }

// ---- translator ----
static int is_csr_read(const rv_insn *d) {
  if (d->imm < 1 || d->imm > 3) return 0;
  if (d->op == RV_CSRRS || d->op == RV_CSRRC) return d->rs1 == 0;
  if (d->op == RV_CSRRSI || d->op == RV_CSRRCI) return d->rs1 == 0;
  return 0;
}
int rvfp_translatable(const rv_insn *d) {
  if (d->op >= RV_CSRRW && d->op <= RV_CSRRCI) return is_csr_read(d);
  switch (d->op) {   // instructions without a rounding mode
  case RV_FLW: case RV_FLD: case RV_FSW: case RV_FSD: case RV_FEQ_S: case RV_FLT_S: case RV_FLE_S: case RV_FEQ_D: case RV_FLT_D: case RV_FLE_D:
  case RV_FMV_X_W: case RV_FMV_X_D: case RV_FMV_W_X: case RV_FMV_D_X: case RV_FCLASS_S: case RV_FCLASS_D:
  case RV_FSGNJ_S: case RV_FSGNJN_S: case RV_FSGNJX_S: case RV_FSGNJ_D: case RV_FSGNJN_D: case RV_FSGNJX_D:
  case RV_FMIN_S: case RV_FMAX_S: case RV_FMIN_D: case RV_FMAX_D:
    return 1;
  }
  int rm = d->rm == 7 ? rvfp_frm : d->rm;
  return rm <= 4;   // 5 and 6 are reserved: illegal, the interpreter reports it
}

void rvfp_regs(const rv_insn *d, uint64_t *use, uint64_t *def) {
  int op = d->op;
  uint64_t u = 0, w = 0;
#define XR(r) (1ull << (r))
#define FR(r) (1ull << (32 + (r)))
  if (op == RV_FLW || op == RV_FLD) { u = XR(d->rs1); w = FR(d->rd); }
  else if (op == RV_FSW || op == RV_FSD) u = XR(d->rs1) | FR(d->rs2);
  else if (op >= RV_FMADD_S && op <= RV_FNMADD_D) { u = FR(d->rs1) | FR(d->rs2) | FR(d->rs3); w = FR(d->rd); }
  else if (op == RV_FEQ_S || op == RV_FLT_S || op == RV_FLE_S || op == RV_FEQ_D || op == RV_FLT_D || op == RV_FLE_D) { u = FR(d->rs1) | FR(d->rs2); w = XR(d->rd); }
  else if ((op >= RV_FCVT_W_S && op <= RV_FCVT_LU_D) || op == RV_FMV_X_W || op == RV_FMV_X_D || op == RV_FCLASS_S || op == RV_FCLASS_D) { u = FR(d->rs1); w = XR(d->rd); }
  else if ((op >= RV_FCVT_S_W && op <= RV_FCVT_D_LU) || op == RV_FMV_W_X || op == RV_FMV_D_X) { u = XR(d->rs1); w = FR(d->rd); }
  else if (op == RV_FSQRT_S || op == RV_FSQRT_D || op == RV_FCVT_S_D || op == RV_FCVT_D_S) { u = FR(d->rs1); w = FR(d->rd); }
  else { u = FR(d->rs1) | FR(d->rs2); w = FR(d->rd); }
  *use = u; *def = w & ~1ull;
}

// emission helpers
#define BUF (g->b)
static void fd(xgen *g, int r) { xg_fget(g, r); }                    // f64 as it is
static void fs(xgen *g, int r) {                                       // f32, unboxed (canonical NaN if not boxed)
  xg_fget(g, r); w_op(BUF, W_I64_REINTERPRET_F64); w_ltee(BUF, L_T1);
  w_op(BUF, W_I32_WRAP_I64); w_op(BUF, W_F32_REINTERPRET_I32);
  w_op(BUF, W_F32_CONST); wb_bytes(BUF, "\x00\x00\xc0\x7f", 4);
  w_lget(BUF, L_T1); w_i64c(BUF, 32); w_op(BUF, W_I64_SHR_U); w_i64c(BUF, 0xffffffffll); w_op(BUF, W_I64_EQ);
  w_op(BUF, W_SELECT);
}
static void set_d(xgen *g, int r) {                                    // canonicalise a NaN, store
  w_ltee(BUF, L_TF0);
  w_op(BUF, W_F64_CONST); wb_bytes(BUF, "\x00\x00\x00\x00\x00\x00\xf8\x7f", 8);
  w_lget(BUF, L_TF0); w_lget(BUF, L_TF0); w_op(BUF, W_F64_EQ);
  w_op(BUF, W_SELECT);
  xg_fset(g, r);
}
static void box_i32(xgen *g) {                                         // i32 bits -> boxed f64
  w_op(BUF, W_I64_EXTEND_I32_U); w_i64c(BUF, (int64_t)BOX); w_op(BUF, W_I64_OR); w_op(BUF, W_F64_REINTERPRET_I64);
}
static void set_s(xgen *g, int r) {
  w_ltee(BUF, L_TS0);
  w_op(BUF, W_F32_CONST); wb_bytes(BUF, "\x00\x00\xc0\x7f", 4);
  w_lget(BUF, L_TS0); w_lget(BUF, L_TS0); w_op(BUF, W_F32_EQ);
  w_op(BUF, W_SELECT);
  w_op(BUF, W_I32_REINTERPRET_F32); box_i32(g);
  xg_fset(g, r);
}
static void helper(xgen *g, const rv_insn *d, int rm, int to_x, int a_is_x) {
  w_i32c(BUF, d->op | rm << 16);
  if (a_is_x) xg_get(g, d->rs1); else { xg_fget(g, d->rs1); w_op(BUF, W_I64_REINTERPRET_F64); }
  xg_fget(g, d->rs2); w_op(BUF, W_I64_REINTERPRET_F64);
  xg_fget(g, d->rs3); w_op(BUF, W_I64_REINTERPRET_F64);
  w_call(BUF, xg_import(IMP_FPHELP));
  if (to_x) xg_set(g, d->rd); else { w_op(BUF, W_F64_REINTERPRET_I64); xg_fset(g, d->rd); }
}

static void minmax(xgen *g, const rv_insn *d, int single, int ismax) {
  // a NaN: the other operand (canonical NaN if both are); otherwise wasm min/max (which order -0 < +0)
  int T = single ? WT_F32 : WT_F64;
  int la = single ? L_TS0 : L_TF0, lb = single ? L_TS1 : L_TF1;
  if (single) { fs(g, d->rs1); w_lset(BUF, la); fs(g, d->rs2); w_lset(BUF, lb); }
  else { fd(g, d->rs1); w_lset(BUF, la); fd(g, d->rs2); w_lset(BUF, lb); }
  int eq = single ? W_F32_NE : W_F64_NE;
  w_lget(BUF, la); w_lget(BUF, la); w_op(BUF, eq);
  w_if(BUF, T); g->labels++;
  w_lget(BUF, lb);
  if (single) { w_op(BUF, W_F32_CONST); wb_bytes(BUF, "\x00\x00\xc0\x7f", 4); }
  else { w_op(BUF, W_F64_CONST); wb_bytes(BUF, "\x00\x00\x00\x00\x00\x00\xf8\x7f", 8); }
  w_lget(BUF, lb); w_lget(BUF, lb); w_op(BUF, single ? W_F32_EQ : W_F64_EQ);
  w_op(BUF, W_SELECT);
  w_op(BUF, W_ELSE);
  w_lget(BUF, la); w_lget(BUF, lb);
  w_op(BUF, single ? (ismax ? W_F32_MAX : W_F32_MIN) : (ismax ? W_F64_MAX : W_F64_MIN));
  w_lget(BUF, la);
  w_lget(BUF, lb); w_lget(BUF, lb); w_op(BUF, single ? W_F32_EQ : W_F64_EQ);
  w_op(BUF, W_SELECT);
  w_op(BUF, W_END); g->labels--;
  if (single) { w_op(BUF, W_I32_REINTERPRET_F32); box_i32(g); xg_fset(g, d->rd); }
  else xg_fset(g, d->rd);
}

static void cvt_to_int(xgen *g, const rv_insn *d, int single, int rm) {
  int op = d->op, base = single ? op - RV_FCVT_W_S : op - RV_FCVT_W_D;   // 0 W, 1 WU, 2 L, 3 LU
  int ltmp = single ? L_TS0 : L_TF0;
  if (single) fs(g, d->rs1); else fd(g, d->rs1);
  if (rm == RNE) w_op(BUF, single ? W_F32_NEAREST : W_F64_NEAREST);
  else if (rm == RDN) w_op(BUF, single ? W_F32_FLOOR : W_F64_FLOOR);
  else if (rm == RUP) w_op(BUF, single ? W_F32_CEIL : W_F64_CEIL);
  w_ltee(BUF, ltmp);
  static const int sat[2][4] = { { 2, 3, 6, 7 }, { 0, 1, 4, 5 } };   // trunc_sat sub-opcodes [single][base]
  w_fc(BUF, (uint32_t)sat[single][base]);
  // NaN: the maximum
  if (base < 2) w_i32c(BUF, base == 0 ? 0x7fffffff : -1); else w_i64c(BUF, base == 2 ? INT64_MAX : -1);
  w_lget(BUF, ltmp); w_lget(BUF, ltmp); w_op(BUF, single ? W_F32_EQ : W_F64_EQ);
  w_op(BUF, W_SELECT);
  if (base < 2) w_op(BUF, W_I64_EXTEND_I32_S);
  xg_set(g, d->rd);
}

void rvfp_emit(xgen *g, const xinsn *x) {
  const rv_insn *d = &x->d;
  int op = d->op;
  int rm = d->rm == 7 ? rvfp_frm : d->rm;
  if (op >= RV_CSRRW && op <= RV_CSRRCI) {   // reads of fflags, frm, fcsr
    if (!d->rd) return;
    w_i32c(BUF, 0); w_mem(BUF, W_I32_LOAD, 2, g->env->a_fcsr);
    if (d->imm == 1) { w_i32c(BUF, 31); w_op(BUF, W_I32_AND); }
    else if (d->imm == 2) { w_i32c(BUF, 5); w_op(BUF, W_I32_SHR_U); w_i32c(BUF, 7); w_op(BUF, W_I32_AND); }
    else { w_i32c(BUF, 0xff); w_op(BUF, W_I32_AND); }
    w_op(BUF, W_I64_EXTEND_I32_U);
    xg_set(g, d->rd);
    return;
  }
  switch (op) {
  case RV_FLW: { uint32_t off = xg_addr(g, d->rs1, d->imm, 4); w_mem(BUF, W_I32_LOAD, 2, off); box_i32(g); xg_fset(g, d->rd); return; }
  case RV_FLD: { uint32_t off = xg_addr(g, d->rs1, d->imm, 8); w_mem(BUF, W_F64_LOAD, 3, off); xg_fset(g, d->rd); return; }
  case RV_FSW: { uint32_t off = xg_addr(g, d->rs1, d->imm, 4); xg_fget(g, d->rs2); w_op(BUF, W_I64_REINTERPRET_F64); w_mem(BUF, W_I64_STORE32, 2, off); return; }
  case RV_FSD: { uint32_t off = xg_addr(g, d->rs1, d->imm, 8); xg_fget(g, d->rs2); w_mem(BUF, W_F64_STORE, 3, off); return; }
  case RV_FSGNJ_D: case RV_FSGNJN_D: case RV_FSGNJX_D:
    if (d->rs1 == d->rs2 && op == RV_FSGNJ_D) { xg_fget(g, d->rs1); xg_fset(g, d->rd); return; }   // fmv.d
    if (d->rs1 == d->rs2 && op == RV_FSGNJN_D) { xg_fget(g, d->rs1); w_op(BUF, W_F64_NEG); xg_fset(g, d->rd); return; }   // fneg.d (bit exact)
    if (d->rs1 == d->rs2 && op == RV_FSGNJX_D) { xg_fget(g, d->rs1); w_op(BUF, W_F64_ABS); xg_fset(g, d->rd); return; }   // fabs.d
    xg_fget(g, d->rs1); w_op(BUF, W_I64_REINTERPRET_F64); w_i64c(BUF, INT64_MAX); w_op(BUF, W_I64_AND);
    xg_fget(g, d->rs2); w_op(BUF, W_I64_REINTERPRET_F64);
    if (op == RV_FSGNJN_D) { w_i64c(BUF, -1); w_op(BUF, W_I64_XOR); }
    if (op == RV_FSGNJX_D) { xg_fget(g, d->rs1); w_op(BUF, W_I64_REINTERPRET_F64); w_op(BUF, W_I64_XOR); }
    w_i64c(BUF, INT64_MIN); w_op(BUF, W_I64_AND); w_op(BUF, W_I64_OR);
    w_op(BUF, W_F64_REINTERPRET_I64); xg_fset(g, d->rd);
    return;
  case RV_FSGNJ_S: case RV_FSGNJN_S: case RV_FSGNJX_S:
    fs(g, d->rs1); w_op(BUF, W_I32_REINTERPRET_F32); w_i32c(BUF, 0x7fffffff); w_op(BUF, W_I32_AND);
    fs(g, d->rs2); w_op(BUF, W_I32_REINTERPRET_F32);
    if (op == RV_FSGNJN_S) { w_i32c(BUF, -1); w_op(BUF, W_I32_XOR); }
    if (op == RV_FSGNJX_S) { fs(g, d->rs1); w_op(BUF, W_I32_REINTERPRET_F32); w_op(BUF, W_I32_XOR); }
    w_i32c(BUF, (int32_t)0x80000000u); w_op(BUF, W_I32_AND); w_op(BUF, W_I32_OR);
    box_i32(g); xg_fset(g, d->rd);
    return;
  case RV_FMIN_S: case RV_FMAX_S: minmax(g, d, 1, op == RV_FMAX_S); return;
  case RV_FMIN_D: case RV_FMAX_D: minmax(g, d, 0, op == RV_FMAX_D); return;
  case RV_FEQ_S: case RV_FLT_S: case RV_FLE_S:
    fs(g, d->rs1); fs(g, d->rs2); w_op(BUF, op == RV_FEQ_S ? W_F32_EQ : op == RV_FLT_S ? W_F32_LT : W_F32_LE);
    w_op(BUF, W_I64_EXTEND_I32_U); xg_set(g, d->rd); return;
  case RV_FEQ_D: case RV_FLT_D: case RV_FLE_D:
    fd(g, d->rs1); fd(g, d->rs2); w_op(BUF, op == RV_FEQ_D ? W_F64_EQ : op == RV_FLT_D ? W_F64_LT : W_F64_LE);
    w_op(BUF, W_I64_EXTEND_I32_U); xg_set(g, d->rd); return;
  case RV_FMV_X_W: xg_fget(g, d->rs1); w_op(BUF, W_I64_REINTERPRET_F64); w_op(BUF, W_I32_WRAP_I64); w_op(BUF, W_I64_EXTEND_I32_S); xg_set(g, d->rd); return;
  case RV_FMV_X_D: xg_fget(g, d->rs1); w_op(BUF, W_I64_REINTERPRET_F64); xg_set(g, d->rd); return;
  case RV_FMV_W_X: xg_get(g, d->rs1); w_i64c(BUF, (int64_t)BOX); w_op(BUF, W_I64_OR); w_op(BUF, W_F64_REINTERPRET_I64); xg_fset(g, d->rd); return;
  case RV_FMV_D_X: xg_get(g, d->rs1); w_op(BUF, W_F64_REINTERPRET_I64); xg_fset(g, d->rd); return;
  case RV_FCLASS_S: case RV_FCLASS_D: helper(g, d, 0, 1, 0); return;
  case RV_FCVT_D_S: fs(g, d->rs1); w_op(BUF, W_F64_PROMOTE_F32); set_d(g, d->rd); return;   // exact
  case RV_FCVT_D_W: xg_get(g, d->rs1); w_op(BUF, W_I32_WRAP_I64); w_op(BUF, W_F64_CONVERT_I32_S); xg_fset(g, d->rd); return;   // exact
  case RV_FCVT_D_WU: xg_get(g, d->rs1); w_op(BUF, W_I32_WRAP_I64); w_op(BUF, W_F64_CONVERT_I32_U); xg_fset(g, d->rd); return;
  }
  if (rm != RNE) {
    int to_x = op >= RV_FCVT_W_S && op <= RV_FCVT_LU_D;
    if (to_x && rm != RMM) { cvt_to_int(g, d, op <= RV_FCVT_LU_S, rm); return; }
    helper(g, d, rm, to_x, op >= RV_FCVT_S_W && op <= RV_FCVT_D_LU);
    return;
  }
  switch (op) {
  case RV_FADD_S: fs(g, d->rs1); fs(g, d->rs2); w_op(BUF, W_F32_ADD); set_s(g, d->rd); return;
  case RV_FSUB_S: fs(g, d->rs1); fs(g, d->rs2); w_op(BUF, W_F32_SUB); set_s(g, d->rd); return;
  case RV_FMUL_S: fs(g, d->rs1); fs(g, d->rs2); w_op(BUF, W_F32_MUL); set_s(g, d->rd); return;
  case RV_FDIV_S: fs(g, d->rs1); fs(g, d->rs2); w_op(BUF, W_F32_DIV); set_s(g, d->rd); return;
  case RV_FSQRT_S: fs(g, d->rs1); w_op(BUF, W_F32_SQRT); set_s(g, d->rd); return;
  case RV_FADD_D: fd(g, d->rs1); fd(g, d->rs2); w_op(BUF, W_F64_ADD); set_d(g, d->rd); return;
  case RV_FSUB_D: fd(g, d->rs1); fd(g, d->rs2); w_op(BUF, W_F64_SUB); set_d(g, d->rd); return;
  case RV_FMUL_D: fd(g, d->rs1); fd(g, d->rs2); w_op(BUF, W_F64_MUL); set_d(g, d->rd); return;
  case RV_FDIV_D: fd(g, d->rs1); fd(g, d->rs2); w_op(BUF, W_F64_DIV); set_d(g, d->rd); return;
  case RV_FSQRT_D: fd(g, d->rs1); w_op(BUF, W_F64_SQRT); set_d(g, d->rd); return;
  case RV_FCVT_S_D: fd(g, d->rs1); w_op(BUF, W_F32_DEMOTE_F64); set_s(g, d->rd); return;
  case RV_FCVT_S_W: xg_get(g, d->rs1); w_op(BUF, W_I32_WRAP_I64); w_op(BUF, W_F32_CONVERT_I32_S); w_op(BUF, W_I32_REINTERPRET_F32); box_i32(g); xg_fset(g, d->rd); return;
  case RV_FCVT_S_WU: xg_get(g, d->rs1); w_op(BUF, W_I32_WRAP_I64); w_op(BUF, W_F32_CONVERT_I32_U); w_op(BUF, W_I32_REINTERPRET_F32); box_i32(g); xg_fset(g, d->rd); return;
  case RV_FCVT_S_L: xg_get(g, d->rs1); w_op(BUF, W_F32_CONVERT_I64_S); w_op(BUF, W_I32_REINTERPRET_F32); box_i32(g); xg_fset(g, d->rd); return;
  case RV_FCVT_S_LU: xg_get(g, d->rs1); w_op(BUF, W_F32_CONVERT_I64_U); w_op(BUF, W_I32_REINTERPRET_F32); box_i32(g); xg_fset(g, d->rd); return;
  case RV_FCVT_D_L: xg_get(g, d->rs1); w_op(BUF, W_F64_CONVERT_I64_S); xg_fset(g, d->rd); return;
  case RV_FCVT_D_LU: xg_get(g, d->rs1); w_op(BUF, W_F64_CONVERT_I64_U); xg_fset(g, d->rd); return;
  case RV_FCVT_W_S: case RV_FCVT_WU_S: case RV_FCVT_L_S: case RV_FCVT_LU_S: cvt_to_int(g, d, 1, RNE); return;
  case RV_FCVT_W_D: case RV_FCVT_WU_D: case RV_FCVT_L_D: case RV_FCVT_LU_D: cvt_to_int(g, d, 0, RNE); return;
  }
  if (op >= RV_FMADD_S && op <= RV_FNMADD_D) {
    int single = op <= RV_FNMADD_S, k = single ? op - RV_FMADD_S : op - RV_FMADD_D;   // 0 madd 1 msub 2 nmsub 3 nmadd
    int negp = k >= 2, negc = k == 1 || k == 3;
    if (single) { fs(g, d->rs1); if (negp) w_op(BUF, W_F32_NEG); fs(g, d->rs2); fs(g, d->rs3); if (negc) w_op(BUF, W_F32_NEG); }
    else { fd(g, d->rs1); if (negp) w_op(BUF, W_F64_NEG); fd(g, d->rs2); fd(g, d->rs3); if (negc) w_op(BUF, W_F64_NEG); }
    if (rvfp_fast_fma) {   // deviation, for measurement: two roundings
      if (single) { w_lset(BUF, L_TS1); w_op(BUF, W_F32_MUL); w_lget(BUF, L_TS1); w_op(BUF, W_F32_ADD); }
      else { w_lset(BUF, L_TF1); w_op(BUF, W_F64_MUL); w_lget(BUF, L_TF1); w_op(BUF, W_F64_ADD); }
    } else w_call(BUF, xg_import(single ? IMP_FMA32 : IMP_FMA64));
    if (single) set_s(g, d->rd); else set_d(g, d->rd);
    return;
  }
  w_op(BUF, W_UNREACHABLE);
}
