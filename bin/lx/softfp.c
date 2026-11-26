// SPDX-License-Identifier: GPL-2.0-or-later
/* Software IEEE 754 arithmetic (see softfp.h). One generic implementation serves both
 * formats: numbers are unpacked to (sign, exponent, 63-bit significand with the leading
 * one at bit 62), computed on, and rounded back by round_pack(). */
#include "softfp.h"

struct fmt { int fbits, ebits, p, bias, maxe; uint64_t qnan; };
static const struct fmt F64 = {52, 11, 53, 1023, 2047, F64_QNAN};
static const struct fmt F32 = {23, 8, 24, 127, 255, F32_QNAN};

enum { K_ZERO, K_NUM, K_INF, K_QNAN, K_SNAN };
struct un { int kind, sign; int32_t exp; uint64_t sig; }; /* value = sig * 2^(exp-62) */

static inline int clz64(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline uint64_t jam(uint64_t x, int n) {
  if (n <= 0) return x;
  if (n >= 64) return x != 0;
  return (x >> n) | ((x << (64 - n)) != 0);
}

static struct un unpack(uint64_t x, const struct fmt *F) {
  struct un u;
  u.sign = (int)(x >> (F->fbits + F->ebits)) & 1;
  int e = (int)(x >> F->fbits) & F->maxe;
  uint64_t f = x & ((1ull << F->fbits) - 1);
  u.exp = 0; u.sig = 0;
  if (e == F->maxe) {
    if (!f) u.kind = K_INF;
    else u.kind = (f >> (F->fbits - 1)) & 1 ? K_QNAN : K_SNAN;
  } else if (e == 0) {
    if (!f) u.kind = K_ZERO;
    else {
      u.kind = K_NUM;
      uint64_t s = f << (62 - F->fbits);
      int k = clz64(s) - 1;
      u.sig = s << k;
      u.exp = 1 - F->bias - k;
    }
  } else {
    u.kind = K_NUM;
    u.sig = (f | (1ull << F->fbits)) << (62 - F->fbits);
    u.exp = e - F->bias;
  }
  return u;
}

static inline uint64_t mkinf(int sign, const struct fmt *F) {
  return ((uint64_t)sign << (F->fbits + F->ebits)) | ((uint64_t)F->maxe << F->fbits);
}
static inline uint64_t mkzero(int sign, const struct fmt *F) { return (uint64_t)sign << (F->fbits + F->ebits); }

static int round_inc(uint64_t m, uint64_t rbits, uint64_t half, int sign, int rm) {
  switch (rm) {
  case RM_RNE: return rbits > half || (rbits == half && (m & 1));
  case RM_RTZ: return 0;
  case RM_RDN: return sign && rbits;
  case RM_RUP: return !sign && rbits;
  default: return rbits >= half; /* RMM */
  }
}

/* sig has its leading one at bit 62 (or is a smaller value with exp adjusted so the
 * bits below the target precision act as guard and sticky). */
static uint64_t round_pack(int sign, int32_t exp, uint64_t sig, const struct fmt *F, int rm, uint32_t *fl) {
  if (!sig) return mkzero(sign, F);
  int k = clz64(sig) - 1; /* normalise to bit 62 */
  if (k > 0) { sig <<= k; exp -= k; } else if (k < 0) { sig = jam(sig, 1); exp += 1; }
  int shift = 63 - F->p;
  uint64_t mask = (1ull << shift) - 1, half = 1ull << (shift - 1);
  int32_t E = exp + F->bias;
  int tiny = 0;
  if (E <= 0) {
    if (E < 0) tiny = 1;
    else {
      uint64_t m = sig >> shift;
      m += round_inc(m, sig & mask, half, sign, rm);
      tiny = !(m >> F->p);
    }
    sig = jam(sig, 1 - E);
    E = 0;
  }
  uint64_t rbits = sig & mask, m = sig >> shift;
  m += round_inc(m, rbits, half, sign, rm);
  if (rbits) { *fl |= FF_NX; if (tiny) *fl |= FF_UF; }
  if (E == 0) { if (m >> (F->p - 1)) E = 1; }
  else if (m >> F->p) { m >>= 1; E++; }
  if (E >= F->maxe) {
    *fl |= FF_OF | FF_NX;
    int tomax = rm == RM_RTZ || (rm == RM_RDN && !sign) || (rm == RM_RUP && sign);
    if (tomax) return ((uint64_t)sign << (F->fbits + F->ebits)) | ((uint64_t)(F->maxe - 1) << F->fbits) | ((1ull << F->fbits) - 1);
    return mkinf(sign, F);
  }
  return ((uint64_t)sign << (F->fbits + F->ebits)) | ((uint64_t)E << F->fbits) | (m & ((1ull << F->fbits) - 1));
}

static int isnan_k(int k) { return k == K_QNAN || k == K_SNAN; }

/* ---------- add / sub ---------- */
static uint64_t g_add(uint64_t ra, uint64_t rb, int negb, const struct fmt *F, int rm, uint32_t *fl) {
  struct un a = unpack(ra, F), b = unpack(rb, F);
  if (isnan_k(a.kind) || isnan_k(b.kind)) {
    if (a.kind == K_SNAN || b.kind == K_SNAN) *fl |= FF_NV;
    return F->qnan;
  }
  b.sign ^= negb;
  if (a.kind == K_INF || b.kind == K_INF) {
    if (a.kind == K_INF && b.kind == K_INF && a.sign != b.sign) { *fl |= FF_NV; return F->qnan; }
    return mkinf(a.kind == K_INF ? a.sign : b.sign, F);
  }
  if (a.kind == K_ZERO && b.kind == K_ZERO) return mkzero(a.sign == b.sign ? a.sign : rm == RM_RDN, F);
  if (a.kind == K_ZERO) return round_pack(b.sign, b.exp, b.sig, F, rm, fl);
  if (b.kind == K_ZERO) return round_pack(a.sign, a.exp, a.sig, F, rm, fl);
  if (a.exp < b.exp || (a.exp == b.exp && a.sig < b.sig)) { struct un t = a; a = b; b = t; }
  int d = a.exp - b.exp;
  uint64_t bs = jam(b.sig, d);
  if (a.sign == b.sign) {
    uint64_t s = a.sig + bs; /* at most bit 63 */
    return round_pack(a.sign, a.exp, s, F, rm, fl);
  }
  uint64_t s = a.sig - bs;
  if (!s) return mkzero(rm == RM_RDN, F);
  return round_pack(a.sign, a.exp, s, F, rm, fl);
}

/* ---------- 128-bit helpers ---------- */
struct u128 { uint64_t hi, lo; };
static struct u128 mul64(uint64_t a, uint64_t b) {
  uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  struct u128 r;
  r.lo = (mid << 32) | (uint32_t)p00;
  r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  return r;
}
static struct u128 shr128_jam(struct u128 x, int n) {
  struct u128 r;
  if (n <= 0) return x;
  if (n >= 128) { r.hi = 0; r.lo = (x.hi | x.lo) != 0; return r; }
  if (n >= 64) {
    uint64_t st = x.lo != 0 || (n > 64 && (x.hi << (128 - n)) != 0);
    r.lo = (n == 64 ? x.hi : x.hi >> (n - 64)) | st;
    r.hi = 0;
    return r;
  }
  r.lo = (x.lo >> n) | (x.hi << (64 - n)) | ((x.lo << (64 - n)) != 0);
  r.hi = x.hi >> n;
  return r;
}
static struct u128 add128(struct u128 a, struct u128 b) {
  struct u128 r; r.lo = a.lo + b.lo; r.hi = a.hi + b.hi + (r.lo < a.lo); return r;
}
static struct u128 sub128(struct u128 a, struct u128 b) {
  struct u128 r; r.lo = a.lo - b.lo; r.hi = a.hi - b.hi - (a.lo < b.lo); return r;
}
static int lt128(struct u128 a, struct u128 b) { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
static int clz128(struct u128 x) { return x.hi ? clz64(x.hi) : 64 + clz64(x.lo); }
static struct u128 shl128(struct u128 x, int n) {
  struct u128 r;
  if (n <= 0) return x;
  if (n >= 64) { r.hi = x.lo << (n - 64); r.lo = 0; return r; }
  r.hi = (x.hi << n) | (x.lo >> (64 - n)); r.lo = x.lo << n; return r;
}
/* Reduce a 128-bit significand with value v * 2^(exp - 124) to the 63-bit form. */
static void norm128(struct u128 v, int32_t *exp, uint64_t *sig) {
  int k = clz128(v); /* leading one at bit 127-k; we want it at bit 126 of a 128 frame, then take hi */
  if (k > 1) v = shl128(v, k - 1); else if (k == 0) v = shr128_jam(v, 1);
  *exp += 1 - k + 2; /* leading one was at 127-k, which corresponds to 2^(exp-124+127-k) */
  /* v now has its leading one at bit 126: hi holds bits 127..64, so the leading one is at hi bit 62 */
  *sig = v.hi | (v.lo != 0);
}

/* ---------- mul ---------- */
static uint64_t g_mul(uint64_t ra, uint64_t rb, const struct fmt *F, int rm, uint32_t *fl) {
  struct un a = unpack(ra, F), b = unpack(rb, F);
  int sign = a.sign ^ b.sign;
  if (isnan_k(a.kind) || isnan_k(b.kind)) {
    if (a.kind == K_SNAN || b.kind == K_SNAN) *fl |= FF_NV;
    return F->qnan;
  }
  if (a.kind == K_INF || b.kind == K_INF) {
    if (a.kind == K_ZERO || b.kind == K_ZERO) { *fl |= FF_NV; return F->qnan; }
    return mkinf(sign, F);
  }
  if (a.kind == K_ZERO || b.kind == K_ZERO) return mkzero(sign, F);
  struct u128 p = mul64(a.sig, b.sig); /* leading one at bit 124 or 125 */
  int32_t e = a.exp + b.exp;
  uint64_t sig;
  norm128(p, &e, &sig);
  return round_pack(sign, e, sig, F, rm, fl);
}

/* ---------- div ---------- */
static uint64_t g_div(uint64_t ra, uint64_t rb, const struct fmt *F, int rm, uint32_t *fl) {
  struct un a = unpack(ra, F), b = unpack(rb, F);
  int sign = a.sign ^ b.sign;
  if (isnan_k(a.kind) || isnan_k(b.kind)) {
    if (a.kind == K_SNAN || b.kind == K_SNAN) *fl |= FF_NV;
    return F->qnan;
  }
  if (a.kind == K_INF) {
    if (b.kind == K_INF) { *fl |= FF_NV; return F->qnan; }
    return mkinf(sign, F);
  }
  if (b.kind == K_INF) return mkzero(sign, F);
  if (b.kind == K_ZERO) {
    if (a.kind == K_ZERO) { *fl |= FF_NV; return F->qnan; }
    *fl |= FF_DZ;
    return mkinf(sign, F);
  }
  if (a.kind == K_ZERO) return mkzero(sign, F);
  int32_t e = a.exp - b.exp;
  uint64_t rem = a.sig, q = 0;
  if (rem < b.sig) { rem <<= 1; e--; }
  for (int i = 0; i < 63; i++) {
    q <<= 1;
    if (rem >= b.sig) { rem -= b.sig; q |= 1; }
    rem <<= 1;
  }
  q |= rem != 0;
  return round_pack(sign, e, q, F, rm, fl);
}

/* ---------- sqrt ---------- */
static uint64_t g_sqrt(uint64_t ra, const struct fmt *F, int rm, uint32_t *fl) {
  struct un a = unpack(ra, F);
  if (isnan_k(a.kind)) { if (a.kind == K_SNAN) *fl |= FF_NV; return F->qnan; }
  if (a.kind == K_ZERO) return ra;
  if (a.sign) { *fl |= FF_NV; return F->qnan; }
  if (a.kind == K_INF) return ra;
  /* value = sig * 2^(exp-62). Make the exponent even: v = S * 2^(2h) with S a 126/127-bit
     integer, then sqrt = isqrt(S) * 2^h. */
  int32_t e = a.exp - 62;
  struct u128 S = {a.sig, 0}; /* sig * 2^64: leading one at bit 126 */
  e -= 64;
  if (e & 1) { S = shr128_jam(S, 1); e += 1; } /* S exact here: low bits of sig are zero */
  /* integer square root of S (128 bits) -> 64-bit root, bit by bit */
  struct u128 rem = {0, 0}, root = {0, 0};
  for (int i = 0; i < 64; i++) {
    rem = shl128(rem, 2);
    rem.lo |= S.hi >> 62;
    S = shl128(S, 2);
    root = shl128(root, 1);
    struct u128 t = shl128(root, 1); t.lo |= 1;
    if (!lt128(rem, t)) { rem = sub128(rem, t); root.lo |= 1; }
  }
  uint64_t r = root.lo; /* about 63 bits */
  int32_t er = e / 2; /* value = r * 2^er */
  r |= (rem.hi | rem.lo) != 0 ? 0 : 0;
  int inexact = (rem.hi | rem.lo) != 0;
  /* r has its leading one at bit 62 or 63; express as sig * 2^(exp-62) */
  int32_t exp = er + 62;
  if (r >> 63) { r = jam(r, 1); exp++; }
  return round_pack(0, exp, r | (uint64_t)inexact, F, rm, fl);
}

/* ---------- fused multiply-add ---------- */
static uint64_t g_fma(uint64_t ra, uint64_t rb, uint64_t rc, int negp, int negc, const struct fmt *F, int rm, uint32_t *fl) {
  struct un a = unpack(ra, F), b = unpack(rb, F), c = unpack(rc, F);
  int ps = a.sign ^ b.sign ^ negp;
  c.sign ^= negc;
  if (isnan_k(a.kind) || isnan_k(b.kind) || isnan_k(c.kind)) {
    if (a.kind == K_SNAN || b.kind == K_SNAN || c.kind == K_SNAN) *fl |= FF_NV;
    /* RISC-V: inf * 0 + qNaN still raises invalid */
    if ((a.kind == K_INF && b.kind == K_ZERO) || (a.kind == K_ZERO && b.kind == K_INF)) *fl |= FF_NV;
    return F->qnan;
  }
  if (a.kind == K_INF || b.kind == K_INF) {
    if (a.kind == K_ZERO || b.kind == K_ZERO) { *fl |= FF_NV; return F->qnan; }
    if (c.kind == K_INF && c.sign != ps) { *fl |= FF_NV; return F->qnan; }
    return mkinf(ps, F);
  }
  if (c.kind == K_INF) return mkinf(c.sign, F);
  if (a.kind == K_ZERO || b.kind == K_ZERO) {
    if (c.kind == K_ZERO) return mkzero(ps == c.sign ? ps : rm == RM_RDN, F);
    return round_pack(c.sign, c.exp, c.sig, F, rm, fl);
  }
  /* product: P * 2^(pe - 124), P has its leading one at bit 124 or 125 */
  struct u128 P = mul64(a.sig, b.sig);
  int32_t pe = a.exp + b.exp;
  if (c.kind == K_ZERO) {
    uint64_t sig; int32_t e = pe;
    norm128(P, &e, &sig);
    return round_pack(ps, e, sig, F, rm, fl);
  }
  /* Put both in a frame where value = V * 2^(E - 124). Shift P left by 2 so that headroom
     exists; the addend: c.sig * 2^(c.exp - 62) = (c.sig << 62) * 2^(c.exp - 124). */
  struct u128 C = {c.sig >> 2, c.sig << 62};
  int32_t ce = c.exp;
  /* Align to the larger exponent, shifting the smaller one right with jam. Leave one bit of
     headroom at the top (bit 127) for the carry of an addition. */
  P = shr128_jam(P, 1); pe += 1;
  C = shr128_jam(C, 1); ce += 1;
  int32_t E;
  if (pe >= ce) { C = shr128_jam(C, pe - ce); E = pe; }
  else { P = shr128_jam(P, ce - pe); E = ce; }
  struct u128 V;
  int sign;
  if (ps == c.sign) { V = add128(P, C); sign = ps; }
  else if (lt128(P, C)) { V = sub128(C, P); sign = c.sign; }
  else { V = sub128(P, C); sign = ps; }
  if (!V.hi && !V.lo) return mkzero(rm == RM_RDN, F);
  uint64_t sig;
  norm128(V, &E, &sig);
  return round_pack(sign, E, sig, F, rm, fl);
}

/* ---------- conversions ---------- */
static uint64_t g_to_int(uint64_t x, const struct fmt *F, int bits, int sgn, int rm, uint32_t *fl) {
  struct un a = unpack(x, F);
  uint64_t maxv = sgn ? (bits == 64 ? 0x7fffffffffffffffull : 0x7fffffffull) : (bits == 64 ? ~0ull : 0xffffffffull);
  uint64_t minv = sgn ? (bits == 64 ? 0x8000000000000000ull : 0xffffffff80000000ull) : 0;
  uint64_t res;
  if (isnan_k(a.kind)) { *fl |= FF_NV; res = maxv; goto out; }
  if (a.kind == K_INF) { *fl |= FF_NV; res = a.sign ? minv : maxv; goto out; }
  if (a.kind == K_ZERO) return 0;
  /* integer part: value = sig * 2^(exp-62) */
  uint64_t mag; int inexact;
  if (a.exp > 63) { *fl |= FF_NV; res = a.sign ? minv : maxv; goto out; }
  {
    int sh = 62 - a.exp; /* right shift that leaves the integer part */
    uint64_t ip, frac, half;
    if (sh <= 0) { ip = a.sig << (-sh); frac = 0; half = 1; }
    else if (sh < 64) { ip = a.sig >> sh; frac = a.sig & ((1ull << sh) - 1); half = 1ull << (sh - 1); }
    else { ip = 0; frac = 1; half = 2; } /* value below one half: only "nonzero" matters */
    inexact = frac != 0;
    mag = ip + (uint64_t)round_inc(ip, frac, half, a.sign, rm);
    if (mag < ip) { *fl |= FF_NV; res = a.sign ? minv : maxv; goto out; }
  }
  if (sgn) {
    uint64_t lim = bits == 64 ? (1ull << 63) : (1ull << 31);
    if (a.sign ? mag > lim : mag >= lim) { *fl |= FF_NV; res = a.sign ? minv : maxv; goto out; }
    res = a.sign ? (uint64_t)0 - mag : mag;
  } else {
    if (a.sign && mag) { *fl |= FF_NV; res = 0; goto out; }
    if (bits == 32 && mag > 0xffffffffull) { *fl |= FF_NV; res = maxv; goto out; }
    res = mag;
  }
  if (inexact) *fl |= FF_NX;
out:
  if (bits == 32) res = (uint64_t)(int64_t)(int32_t)(uint32_t)res;
  return res;
}

static uint64_t g_from_int(uint64_t v, int sgn, const struct fmt *F, int rm, uint32_t *fl) {
  int sign = 0;
  if (sgn && (int64_t)v < 0) { sign = 1; v = (uint64_t)0 - v; }
  if (!v) return 0;
  /* value = v * 2^0 = sig * 2^(exp-62) */
  int32_t exp = 62;
  uint64_t sig = v;
  if (sig >> 63) { sig = jam(sig, 1); exp = 63; }
  return round_pack(sign, exp, sig, F, rm, fl);
}

/* ---------- public wrappers ---------- */
uint64_t sf64_add(uint64_t a, uint64_t b, int rm, uint32_t *fl) { return g_add(a, b, 0, &F64, rm, fl); }
uint64_t sf64_sub(uint64_t a, uint64_t b, int rm, uint32_t *fl) { return g_add(a, b, 1, &F64, rm, fl); }
uint64_t sf64_mul(uint64_t a, uint64_t b, int rm, uint32_t *fl) { return g_mul(a, b, &F64, rm, fl); }
uint64_t sf64_div(uint64_t a, uint64_t b, int rm, uint32_t *fl) { return g_div(a, b, &F64, rm, fl); }
uint64_t sf64_sqrt(uint64_t a, int rm, uint32_t *fl) { return g_sqrt(a, &F64, rm, fl); }
uint64_t sf64_fma(uint64_t a, uint64_t b, uint64_t c, int np, int nc, int rm, uint32_t *fl) { return g_fma(a, b, c, np, nc, &F64, rm, fl); }
uint32_t sf32_add(uint32_t a, uint32_t b, int rm, uint32_t *fl) { return (uint32_t)g_add(a, b, 0, &F32, rm, fl); }
uint32_t sf32_sub(uint32_t a, uint32_t b, int rm, uint32_t *fl) { return (uint32_t)g_add(a, b, 1, &F32, rm, fl); }
uint32_t sf32_mul(uint32_t a, uint32_t b, int rm, uint32_t *fl) { return (uint32_t)g_mul(a, b, &F32, rm, fl); }
uint32_t sf32_div(uint32_t a, uint32_t b, int rm, uint32_t *fl) { return (uint32_t)g_div(a, b, &F32, rm, fl); }
uint32_t sf32_sqrt(uint32_t a, int rm, uint32_t *fl) { return (uint32_t)g_sqrt(a, &F32, rm, fl); }
uint32_t sf32_fma(uint32_t a, uint32_t b, uint32_t c, int np, int nc, int rm, uint32_t *fl) { return (uint32_t)g_fma(a, b, c, np, nc, &F32, rm, fl); }
uint64_t sf64_to_int(uint64_t a, int bits, int sgn, int rm, uint32_t *fl) { return g_to_int(a, &F64, bits, sgn, rm, fl); }
uint64_t sf32_to_int(uint32_t a, int bits, int sgn, int rm, uint32_t *fl) { return g_to_int(a, &F32, bits, sgn, rm, fl); }
uint64_t sf64_from_int(uint64_t v, int sgn, int rm, uint32_t *fl) { return g_from_int(v, sgn, &F64, rm, fl); }
uint32_t sf32_from_int(uint64_t v, int sgn, int rm, uint32_t *fl) { return (uint32_t)g_from_int(v, sgn, &F32, rm, fl); }

uint32_t sf64_to_f32(uint64_t x, int rm, uint32_t *fl) {
  struct un a = unpack(x, &F64);
  if (isnan_k(a.kind)) { if (a.kind == K_SNAN) *fl |= FF_NV; return F32_QNAN; }
  if (a.kind == K_INF) return (uint32_t)mkinf(a.sign, &F32);
  if (a.kind == K_ZERO) return (uint32_t)mkzero(a.sign, &F32);
  return (uint32_t)round_pack(a.sign, a.exp, a.sig, &F32, rm, fl);
}
uint64_t sf32_to_f64(uint32_t x, uint32_t *fl) {
  struct un a = unpack(x, &F32);
  if (isnan_k(a.kind)) { if (a.kind == K_SNAN) *fl |= FF_NV; return F64_QNAN; }
  if (a.kind == K_INF) return mkinf(a.sign, &F64);
  if (a.kind == K_ZERO) return mkzero(a.sign, &F64);
  return round_pack(a.sign, a.exp, a.sig, &F64, RM_RNE, fl);
}

/* comparisons on raw patterns */
static int g_isnan(uint64_t x, const struct fmt *F) {
  return ((x >> F->fbits) & F->maxe) == (uint64_t)F->maxe && (x & ((1ull << F->fbits) - 1));
}
static int g_issnan(uint64_t x, const struct fmt *F) { return g_isnan(x, F) && !((x >> (F->fbits - 1)) & 1); }
static int g_lt_core(uint64_t a, uint64_t b, const struct fmt *F, int orequal) {
  int sb = F->fbits + F->ebits;
  uint64_t mag = (1ull << sb) - 1;
  int sa = (int)(a >> sb) & 1, sbb = (int)(b >> sb) & 1;
  uint64_t ma = a & mag, mb = b & mag;
  if (!ma && !mb) return orequal; /* +0 == -0 */
  if (sa != sbb) return sa; /* negative < positive */
  if (ma == mb) return orequal;
  return sa ? ma > mb : ma < mb;
}
static int g_eq(uint64_t a, uint64_t b, const struct fmt *F, uint32_t *fl) {
  if (g_isnan(a, F) || g_isnan(b, F)) { if (g_issnan(a, F) || g_issnan(b, F)) *fl |= FF_NV; return 0; }
  uint64_t mag = (1ull << (F->fbits + F->ebits)) - 1;
  return a == b || (!(a & mag) && !(b & mag));
}
static int g_lt(uint64_t a, uint64_t b, const struct fmt *F, uint32_t *fl, int orequal) {
  if (g_isnan(a, F) || g_isnan(b, F)) { *fl |= FF_NV; return 0; }
  return g_lt_core(a, b, F, orequal);
}
static uint64_t g_minmax(uint64_t a, uint64_t b, int max, const struct fmt *F, uint32_t *fl) {
  int na = g_isnan(a, F), nb = g_isnan(b, F);
  if (g_issnan(a, F) || g_issnan(b, F)) *fl |= FF_NV;
  if (na && nb) return F->qnan;
  if (na) return b;
  if (nb) return a;
  int altb = g_lt_core(a, b, F, 0);
  if (!altb && !g_lt_core(b, a, F, 0)) { /* equal: -0 vs +0 */
    int sb = F->fbits + F->ebits;
    int sa = (int)(a >> sb) & 1;
    return max ? (sa ? b : a) : (sa ? a : b);
  }
  return max ? (altb ? b : a) : (altb ? a : b);
}
static unsigned g_class(uint64_t x, const struct fmt *F) {
  struct un a = unpack(x, F);
  int e = (int)(x >> F->fbits) & F->maxe;
  switch (a.kind) {
  case K_INF: return a.sign ? 1u << 0 : 1u << 7;
  case K_ZERO: return a.sign ? 1u << 3 : 1u << 4;
  case K_SNAN: return 1u << 8;
  case K_QNAN: return 1u << 9;
  default:
    if (e == 0) return a.sign ? 1u << 2 : 1u << 5;
    return a.sign ? 1u << 1 : 1u << 6;
  }
}
int sf64_eq(uint64_t a, uint64_t b, uint32_t *fl) { return g_eq(a, b, &F64, fl); }
int sf64_lt(uint64_t a, uint64_t b, uint32_t *fl) { return g_lt(a, b, &F64, fl, 0); }
int sf64_le(uint64_t a, uint64_t b, uint32_t *fl) { return g_lt(a, b, &F64, fl, 1); }
int sf32_eq(uint32_t a, uint32_t b, uint32_t *fl) { return g_eq(a, b, &F32, fl); }
int sf32_lt(uint32_t a, uint32_t b, uint32_t *fl) { return g_lt(a, b, &F32, fl, 0); }
int sf32_le(uint32_t a, uint32_t b, uint32_t *fl) { return g_lt(a, b, &F32, fl, 1); }
uint64_t sf64_minmax(uint64_t a, uint64_t b, int max, uint32_t *fl) { return g_minmax(a, b, max, &F64, fl); }
uint32_t sf32_minmax(uint32_t a, uint32_t b, int max, uint32_t *fl) { return (uint32_t)g_minmax(a, b, max, &F32, fl); }
unsigned sf64_class(uint64_t a) { return g_class(a, &F64); }
unsigned sf32_class(uint32_t a) { return g_class(a, &F32); }
