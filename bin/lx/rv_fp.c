// SPDX-License-Identifier: GPL-2.0-or-later
/* Floating point fast paths. The host (x86-64 natively, WebAssembly in a browser) rounds
 * + - * / and sqrt correctly to nearest-even, so for rm = RNE the host result is the
 * right result. The flags are the difficulty: neither C nor WebAssembly reports them. We
 * take the fast path only when the operands and the result are normal finite numbers
 * (so NV, DZ, OF and UF cannot be raised) and settle NX exactly: it is already set
 * (flags are sticky, so nothing to compute), or an error-free transformation proves the
 * operation exact or inexact. Everything else goes to the soft-float reference.
 * tests/fp_test.c checks these paths against softfp.c. */
#include <string.h>
#include "rv_fp.h"

int fp_fast_enabled = 1;

static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline float u2f(uint32_t u) { float d; memcpy(&d, &u, 4); return d; }
static inline uint32_t f2u(float d) { uint32_t u; memcpy(&u, &d, 4); return u; }

/* Normal finite and not too close to the ends of the range, so that the error-free
 * transformations below cannot overflow or underflow. Exponent field in [lo, hi]. */
static inline int dok(uint64_t x, unsigned lo, unsigned hi) {
  unsigned e = (unsigned)(x >> 52) & 0x7ff;
  return e >= lo && e <= hi;
}
static inline int sok(uint32_t x, unsigned lo, unsigned hi) {
  unsigned e = (x >> 23) & 0xff;
  return e >= lo && e <= hi;
}

/* Dekker's product without FMA: a*b = p + e exactly, if no overflow or underflow. */
static inline double two_prod_err(double a, double b, double p) {
  const double split = 134217729.0; /* 2^27 + 1 */
  double t = split * a, ah = t - (t - a), al = a - ah;
  t = split * b;
  double bh = t - (t - b), bl = b - bh;
  return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

/* Exponent window for the fast path (binary64): operands and result within
 * [2^-900, 2^900] keep every intermediate of the checks normal and finite. */
#define DLO (1023 - 900)
#define DHI (1023 + 900)

uint64_t fpd_add(uint64_t a, uint64_t b, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && dok(a, DLO, DHI) && dok(b, DLO, DHI)) {
    double x = u2d(a), y = u2d(b), s = x + y;
    uint64_t r = d2u(s);
    if (dok(r, DLO, DHI)) {
      if (!(*fl & FF_NX)) {
        double bb = s - x, err = (x - (s - bb)) + (y - bb); /* Knuth's TwoSum */
        if (err != 0) *fl |= FF_NX;
      }
      return r;
    }
  }
  return sf64_add(a, b, rm, fl);
}
uint64_t fpd_sub(uint64_t a, uint64_t b, int rm, uint32_t *fl) { return fpd_add(a, b ^ 0x8000000000000000ull, rm, fl) ; }
uint64_t fpd_mul(uint64_t a, uint64_t b, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && dok(a, 1023 - 450, 1023 + 450) && dok(b, 1023 - 450, 1023 + 450)) {
    double x = u2d(a), y = u2d(b), p = x * y;
    if (!(*fl & FF_NX) && two_prod_err(x, y, p) != 0) *fl |= FF_NX;
    return d2u(p);
  }
  return sf64_mul(a, b, rm, fl);
}
uint64_t fpd_div(uint64_t a, uint64_t b, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && dok(a, 1023 - 450, 1023 + 450) && dok(b, 1023 - 450, 1023 + 450)) {
    double x = u2d(a), y = u2d(b), q = x / y;
    if (!(*fl & FF_NX)) {
      /* exact iff q*y == x exactly */
      double p = q * y;
      if (p != x || two_prod_err(q, y, p) != 0) *fl |= FF_NX;
    }
    return d2u(q);
  }
  return sf64_div(a, b, rm, fl);
}
uint64_t fpd_sqrt(uint64_t a, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && dok(a, DLO, DHI) && !(a >> 63)) {
    double x = u2d(a), r = __builtin_sqrt(x);
    if (!(*fl & FF_NX)) {
      double p = r * r;
      if (p != x || two_prod_err(r, r, p) != 0) *fl |= FF_NX;
    }
    return d2u(r);
  }
  return sf64_sqrt(a, rm, fl);
}

/* binary32: compute in binary64. A product or quotient of two floats rounded once to
 * double and then to float is correctly rounded (53 >= 2*24 + 2), as is a sum. */
#define SLO 1
#define SHI 254
/* Results must stay at least one binade above the subnormal range: a float that rounds up
 * to the smallest normal can still be tiny (RISC-V detects tininess after rounding). */
static inline int sres_ok(uint32_t r) { return sok(r, 2, SHI); }

uint32_t fps_add(uint32_t a, uint32_t b, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && sok(a, SLO, SHI) && sok(b, SLO, SHI)) {
    double x = u2f(a), y = u2f(b), s = x + y; /* exact: exponents differ by < 280 */
    float f = (float)s;
    uint32_t r = f2u(f);
    if (sres_ok(r)) {
      if (!(*fl & FF_NX)) {
        double bb = s - x, err = (x - (s - bb)) + (y - bb);
        if (err != 0 || (double)f != s) *fl |= FF_NX;
      }
      return r;
    }
  }
  return sf32_add(a, b, rm, fl);
}
uint32_t fps_sub(uint32_t a, uint32_t b, int rm, uint32_t *fl) { return fps_add(a, b ^ 0x80000000u, rm, fl); }
uint32_t fps_mul(uint32_t a, uint32_t b, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && sok(a, SLO, SHI) && sok(b, SLO, SHI)) {
    double p = (double)u2f(a) * (double)u2f(b); /* exact */
    float f = (float)p;
    uint32_t r = f2u(f);
    if (sres_ok(r)) {
      if ((double)f != p) *fl |= FF_NX;
      return r;
    }
  }
  return sf32_mul(a, b, rm, fl);
}
uint32_t fps_div(uint32_t a, uint32_t b, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && sok(a, SLO, SHI) && sok(b, SLO, SHI)) {
    double x = u2f(a), y = u2f(b);
    float f = (float)(x / y);
    uint32_t r = f2u(f);
    if (sres_ok(r)) {
      if ((double)f * y != x) *fl |= FF_NX; /* the product of two floats is exact in double */
      return r;
    }
  }
  return sf32_div(a, b, rm, fl);
}
uint32_t fps_sqrt(uint32_t a, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && sok(a, SLO, SHI) && !(a >> 31)) {
    double x = u2f(a);
    float f = (float)__builtin_sqrt(x);
    if ((double)f * (double)f != x) *fl |= FF_NX;
    return f2u(f);
  }
  return sf32_sqrt(a, rm, fl);
}

/* Fused multiply-add. A correctly rounded a*b+c from double operations alone, using the
 * algorithm of Boldo and Melquiond (rounding to odd): a*b = p + e exactly (Dekker),
 * p + c = th + tl exactly (TwoSum), v = (tl + e) rounded to odd, result = RN(th + v).
 * Used only when NX is already set (so no flag needs computing) and every operand is
 * normal and far from the ends of the range (so nothing can overflow, underflow or be
 * invalid); everything else goes to the reference. */
static inline double add_odd(double x, double y) {
  double s = x + y, bb = s - x, err = (x - (s - bb)) + (y - bb);
  uint64_t u = d2u(s);
  if (err != 0 && !(u & 1)) {
    /* move one unit toward the exact value, which lies on err's side */
    int same = (err > 0) == (s > 0);
    u = same ? u + 1 : u - 1;
    s = u2d(u);
  }
  return s;
}
#define FLO (1023 - 400)
#define FHI (1023 + 400)
uint64_t fpd_fma(uint64_t a, uint64_t b, uint64_t c, int negp, int negc, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && (*fl & FF_NX) && dok(a, FLO, FHI) && dok(b, FLO, FHI) &&
      (dok(c, FLO, FHI) || !(c << 1))) {
    double x = u2d(a ^ ((uint64_t)negp << 63)), y = u2d(b), z = u2d(c ^ ((uint64_t)negc << 63));
    double p = x * y, e = two_prod_err(x, y, p);
    double th = p + z, bb = th - p, tl = (p - (th - bb)) + (z - bb);
    double r = th + add_odd(tl, e);
    uint64_t ru = d2u(r);
    if (dok(ru, DLO, DHI)) return ru;
  }
  return sf64_fma(a, b, c, negp, negc, rm, fl);
}
uint32_t fps_fma(uint32_t a, uint32_t b, uint32_t c, int negp, int negc, int rm, uint32_t *fl) {
  if (fp_fast_enabled && rm == RM_RNE && (*fl & FF_NX) && sok(a, SLO, SHI) && sok(b, SLO, SHI) &&
      (sok(c, SLO, SHI) || !(c << 1))) {
    double p = (double)u2f(a ^ ((uint32_t)negp << 31)) * (double)u2f(b); /* exact */
    float f = (float)add_odd(p, (double)u2f(c ^ ((uint32_t)negc << 31)));
    uint32_t r = f2u(f);
    if (sres_ok(r)) return r;
  }
  return sf32_fma(a, b, c, negp, negc, rm, fl);
}
