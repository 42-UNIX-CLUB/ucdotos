// SPDX-License-Identifier: GPL-2.0-or-later
/* libm, written from scratch for this system.
 * Approach: reduce the argument exactly (Cody-Waite splits of ln2 and pi/2 with
 * trailing zero bits, Payne-Hanek for huge trig arguments), then evaluate a
 * Taylor series on the small remainder. pow carries log(x) in double-double.
 * Float versions compute in double and round once. */
#include <math.h>
#include <stdint.h>

typedef union { double d; uint64_t u; } dbits;
static uint64_t bits(double x) { dbits b = {x}; return b.u; }
static double frombits(uint64_t u) { dbits b; b.u = u; return b.d; }

double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x) { return __builtin_ceil(x); }
double trunc(double x) { return __builtin_trunc(x); }
double fabs(double x) { return __builtin_fabs(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
double rint(double x) { return __builtin_rint(x); }
double nearbyint(double x) { return __builtin_rint(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
float floorf(float x) { return __builtin_floorf(x); }
float ceilf(float x) { return __builtin_ceilf(x); }
float truncf(float x) { return __builtin_truncf(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
float rintf(float x) { return __builtin_rintf(x); }
float nearbyintf(float x) { return __builtin_rintf(x); }

/* ---- double-double helpers (Dekker; there is no fma in wasm MVP) ---- */
static void two_sum(double a, double b, double *s, double *e) {
  double t = a + b, bb = t - a;
  *s = t; *e = (a - (t - bb)) + (b - bb);
}
static void split(double a, double *hi, double *lo) {
  double c = 134217729.0 * a; /* 2^27 + 1 */
  *hi = c - (c - a); *lo = a - *hi;
}
static void two_prod(double a, double b, double *p, double *e) {
  double ah, al, bh, bl;
  *p = a * b;
  split(a, &ah, &al); split(b, &bh, &bl);
  *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* c[0] + z c[1] + ... + z^(n-1) c[n-1] */
static double horner(double z, const double *c, int n) {
  double p = c[n - 1];
  while (--n) p = c[n - 1] + z * p;
  return p;
}
#define F(n) (1.0 / n)
static const double IFACT[] = {1, 1, F(2), F(6), F(24), F(120), F(720), F(5040), F(40320), F(362880), F(3628800),
  F(39916800), F(479001600), F(6227020800.0), F(87178291200.0)};
static const double SINC[] = {-F(6), F(120), -F(5040), F(362880), -F(39916800), F(6227020800.0), -F(1307674368000.0),
  F(355687428096000.0), -F(121645100408832000.0)}; /* (-1)^n / (2n+1)!, n >= 1 */
static const double COSC[] = {F(24), -F(720), F(40320), -F(3628800), F(479001600), -F(87178291200.0),
  F(20922789888000.0), -F(6402373705728000.0), F(2432902008176640000.0)}; /* (-1)^n / (2n)!, n >= 2 */
static const double LOGC[] = {2 * F(3.0), 2 * F(5.0), 2 * F(7.0), 2 * F(9.0), 2 * F(11.0), 2 * F(13.0), 2 * F(15.0),
  2 * F(17.0), 2 * F(19.0), 2 * F(21.0), 2 * F(23.0)}; /* 2 / (2n+1), n >= 1 */
#undef F

/* ---- exponent manipulation ---- */
double scalbn(double x, int n) {
  if (n > 1023) {
    x *= 0x1p1023; n -= 1023;
    if (n > 1023) { x *= 0x1p1023; n -= 1023; if (n > 1023) n = 1023; }
  } else if (n < -1022) {
    x *= 0x1p-1022 * 0x1p53; n += 1022 - 53; /* the 2^53 keeps one rounding only */
    if (n < -1022) { x *= 0x1p-1022 * 0x1p53; n += 1022 - 53; if (n < -1022) n = -1022; }
  }
  return x * frombits((uint64_t)(0x3ff + n) << 52);
}
double ldexp(double x, int n) { return scalbn(x, n); }
double frexp(double x, int *e) {
  uint64_t u = bits(x);
  int ee = (u >> 52) & 0x7ff;
  if (!ee) {
    if (x == 0) { *e = 0; return x; }
    x = frexp(x * 0x1p64, e); *e -= 64; return x;
  }
  if (ee == 0x7ff) { *e = 0; return x; }
  *e = ee - 1022;
  return frombits((u & 0x800fffffffffffffull) | 0x3fe0000000000000ull);
}
int ilogb(double x) {
  if (x == 0) return -2147483647 - 1;
  if (!isfinite(x)) return 2147483647;
  int e; frexp(x, &e); return e - 1;
}
double logb(double x) {
  if (x == 0) return -HUGE_VAL;
  if (!isfinite(x)) return x * x;
  return ilogb(x);
}
double modf(double x, double *ip) {
  double t = __builtin_trunc(x);
  *ip = t;
  return isinf(x) ? __builtin_copysign(0.0, x) : __builtin_copysign(x - t, x);
}
double round(double x) {
  double t = __builtin_trunc(x);
  if (__builtin_fabs(x - t) >= 0.5) t += __builtin_copysign(1.0, x);
  return t;
}
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
long lrint(double x) { return (long)__builtin_rint(x); }
long long llrint(double x) { return (long long)__builtin_rint(x); }
double fmin(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a < b || (a == b && signbit(a)) ? a : b; }
double fmax(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a > b || (a == b && signbit(b)) ? a : b; }
double fdim(double a, double b) { return isnan(a) || isnan(b) ? a + b : a > b ? a - b : 0; }
double nan(const char *s) { (void)s; return NAN; }
double nextafter(double x, double y) {
  if (isnan(x) || isnan(y)) return x + y;
  if (x == y) return y;
  if (x == 0) return __builtin_copysign(frombits(1), y);
  uint64_t u = bits(x);
  u += ((x < y) == (x > 0)) ? 1 : -1;
  return frombits(u);
}
double fma(double a, double b, double c) {
  double p, e, s, e2;
  two_prod(a, b, &p, &e);
  if (!isfinite(p)) return a * b + c;
  two_sum(p, c, &s, &e2);
  return s + (e + e2);
}

/* fmod is exact: each step subtracts y scaled by a power of two, and
 * t <= ax < 2t makes ax - t exact (Sterbenz). */
double fmod(double x, double y) {
  if (isnan(x) || isnan(y) || isinf(x) || y == 0) return (x * y) / (x * y);
  double ax = __builtin_fabs(x), ay = __builtin_fabs(y);
  if (ax < ay) return x;
  int ey;
  frexp(ay, &ey);
  while (ax >= ay) {
    int ex;
    frexp(ax, &ex);
    double t = scalbn(ay, ex - ey);
    if (t > ax) t *= 0.5;
    ax -= t;
  }
  return __builtin_copysign(ax, x);
}
double remainder(double x, double y) {
  double r = fmod(x, y), ay = __builtin_fabs(y);
  if (isnan(r)) return r;
  double ar = __builtin_fabs(r);
  if (ar > ay * 0.5 || (ar == ay * 0.5 && ar != 0 && fmod(__builtin_trunc(x / y), 2.0) != 0))
    r -= __builtin_copysign(ay, r);
  return r;
}

/* ---- exp / log ---- */
static const double LN2HI = 0x1.62e42fefa3800p-1, LN2LO = 0x1.ef35793c76730p-45; /* LN2HI has 42 bits */
static const double INVLN2 = 0x1.71547652b82fep+0;

/* exp(hi + lo) for |lo| much smaller than |hi| */
static double exp_dd(double hi, double lo) {
  if (hi > 709.782712893384) return 0x1p1023 * 0x1p1023;
  if (hi < -745.1332191019412) return 0x1p-1022 * 0x1p-1022;
  double k = __builtin_rint(hi * INVLN2);
  double r = hi - k * LN2HI, c = lo - k * LN2LO, rr, re;
  two_sum(r, c, &rr, &re);
  double t = rr * rr * horner(rr, IFACT + 2, 12);
  double y = 1 + (rr + (t + re * (1 + rr)));
  return scalbn(y, (int)k);
}
double exp(double x) {
  if (isnan(x)) return x + x;
  return exp_dd(x, 0);
}
double exp2(double x) {
  if (isnan(x)) return x + x;
  if (x == __builtin_trunc(x) && __builtin_fabs(x) < 2000) return scalbn(1.0, (int)x);
  double h, e;
  two_prod(x, 0x1.62e42fefa39efp-1, &h, &e);
  return exp_dd(h, e + x * 0x1.abc9e3b39803fp-56);
}
/* expm1 for |r| <= ln2/2, without adding 1 */
static double expm1_small(double r) {
  return r + r * r * horner(r, IFACT + 2, 12);
}
double expm1(double x) {
  if (isnan(x)) return x + x;
  if (x > 709.782712893384) return 0x1p1023 * 0x1p1023;
  if (x < -40) return -1;
  double k = __builtin_rint(x * INVLN2);
  if (k == 0) return expm1_small(x);
  double r = (x - k * LN2HI) - k * LN2LO;
  /* 2^k (1 + e) - 1 = 2^k e + (2^k - 1) */
  double e = expm1_small(r);
  if (k > 1020) return scalbn(e + 1, (int)k);
  double p = scalbn(1.0, (int)k);
  return p * e + (p - 1);
}

/* log(x) as hi + lo. x must be positive and finite. */
static void log_dd(double x, double *hi, double *lo) {
  int k;
  double m = frexp(x, &k); /* m in [0.5, 1) */
  if (m < 0x1.6a09e667f3bcdp-1) m *= 2, k--; /* now m in [sqrt2/2, sqrt2) */
  double f = m - 1, dh, dl, sh, p, pe;
  two_sum(m, 1.0, &dh, &dl);
  sh = f / dh;
  two_prod(sh, dh, &p, &pe);
  double sl = (((f - p) - pe) - sh * dl) / dh;
  /* tail T = s^3 (2/3 + z rest), kept in double-double: pow amplifies its error by y */
  double zh, zl, s3h, s3l, ph, pl, th, tl;
  two_prod(sh, sh, &zh, &zl);
  two_prod(sh, zh, &s3h, &s3l);
  s3l += sh * zl;
  two_sum(0x1.5555555555555p-1, zh * horner(zh, LOGC + 1, 10), &ph, &pl);
  pl += 0x1.5555555555555p-55;
  two_prod(s3h, ph, &th, &tl);
  tl += s3h * pl + s3l * ph;
  double h, e, H, e2;
  two_sum(2 * sh, th, &h, &e);
  e += tl;
  e += 2 * sl / (1 - zh); /* d/ds of 2 atanh(s) */
  two_sum(k * LN2HI, h, &H, &e2);
  e2 += e + k * LN2LO;
  two_sum(H, e2, hi, lo);
}
/* log(1 + f) for f in [sqrt2/2 - 1, sqrt2 - 1] */
static double log1p_core(double f) {
  double s = f / (2 + f), z = s * s, hfsq = 0.5 * f * f;
  double R = z * horner(z, LOGC, 11);
  return f - (hfsq - s * (hfsq + R));
}
static int log_special(double x, double *r) {
  if (isnan(x)) { *r = x + x; return 1; }
  if (x < 0) { *r = (x - x) / (x - x); return 1; }
  if (x == 0) { *r = -HUGE_VAL; return 1; }
  if (isinf(x)) { *r = x; return 1; }
  return 0;
}
/* log(x) = k ln2 + log(m) with m in [sqrt2/2, sqrt2); returns log(m) and k */
static double log_red(double x, int *k) {
  double m = frexp(x, k);
  if (m < 0x1.6a09e667f3bcdp-1) m *= 2, (*k)--;
  return log1p_core(m - 1);
}
double log(double x) {
  double r;
  if (log_special(x, &r)) return r;
  int k;
  double lm = log_red(x, &k);
  return k * LN2HI + (lm + k * LN2LO);
}
double log2(double x) {
  double r;
  if (log_special(x, &r)) return r;
  int k;
  double lm = log_red(x, &k);
  return k + (lm * 0x1.71547652b82fep+0 + lm * 0x1.777d0ffda0d24p-56);
}
double log10(double x) {
  double r;
  if (log_special(x, &r)) return r;
  double h, l;
  log_dd(x, &h, &l);
  double p, e;
  two_prod(h, 0x1.bcb7b1526e50ep-2, &p, &e);
  r = p + (e + h * 0x1.95355baaafad3p-57 + l * 0x1.bcb7b1526e50ep-2);
  /* exact powers of ten give exact results */
  double n = __builtin_rint(r);
  if (n >= 0 && n <= 22 && __builtin_fabs(r - n) < 1e-12) {
    double p10 = 1;
    for (int i = 0; i < n; i++) p10 *= 10;
    if (p10 == x) return n;
  }
  return r;
}
double log1p(double x) {
  if (isnan(x)) return x + x;
  if (x == -1) return -HUGE_VAL;
  if (x < -1) return (x - x) / (x - x);
  if (isinf(x)) return x;
  if (x > -0.2928932188134524 && x < 0.41421356237309503) return log1p_core(x);
  double u = 1 + x;
  double c = (u > 2) ? 1 - (u - x) : x - (u - 1); /* rounding error of 1 + x */
  int k;
  double lm = log_red(u, &k);
  return k * LN2HI + (lm + (c / u + k * LN2LO));
}

double pow(double x, double y) {
  if (y == 0 || x == 1) return 1;
  if (isnan(x) || isnan(y)) return x + y;
  int yint = 0; /* 0: not an integer, 1: odd, 2: even */
  if (y == __builtin_trunc(y)) yint = (__builtin_fabs(y) >= 0x1p53 || fmod(y, 2.0) == 0) ? 2 : 1;
  if (isinf(y)) {
    double ax = __builtin_fabs(x);
    if (ax == 1) return 1;
    return (ax > 1) == (y > 0) ? HUGE_VAL : 0;
  }
  if (x == 0) {
    if (y < 0) return yint == 1 ? __builtin_copysign(HUGE_VAL, x) : HUGE_VAL;
    return yint == 1 ? x : 0;
  }
  if (isinf(x)) {
    if (x > 0) return y > 0 ? HUGE_VAL : 0;
    if (y > 0) return yint == 1 ? -HUGE_VAL : HUGE_VAL;
    return yint == 1 ? -0.0 : 0.0;
  }
  double sign = 1;
  if (x < 0) {
    if (!yint) return (x - x) / (x - x);
    if (yint == 1) sign = -1;
    x = -x;
  }
  uint64_t u = bits(x);
  if (yint && !(u & 0xfffffffffffffull) && (u >> 52)) { /* power of two: exact */
    double e = (double)((int)(u >> 52) - 1023) * y;
    return sign * (e > 2000 ? HUGE_VAL : e < -2000 ? 0 : scalbn(1.0, (int)e));
  }
  double lh, ll, ph, pe;
  log_dd(x, &lh, &ll);
  two_prod(y, lh, &ph, &pe);
  double r = exp_dd(ph, pe + y * ll);
  if (yint && y > 0 && r < 0x1p48 && x == __builtin_trunc(x)) r = __builtin_rint(r); /* integer results are exact */
  return sign * r;
}

double cbrt(double x) {
  if (x == 0 || !isfinite(x)) return x + x;
  double ax = __builtin_fabs(x);
  int e;
  double m = frexp(ax, &e); /* ax = m 2^e */
  int q = e / 3, rmd = e % 3;
  if (rmd < 0) rmd += 3, q--;
  m = scalbn(m, rmd); /* m in [0.5, 4) */
  double y = 0.6 + 0.4 * m; /* rough guess, then Newton */
  for (int i = 0; i < 6; i++) y = y - (y * y * y - m) / (3 * y * y);
  double a, b, c, d; /* last step with the residual y^3 - m computed exactly */
  two_prod(y, y, &a, &b);
  two_prod(a, y, &c, &d);
  y -= ((c - m) + (d + b * y)) / (3 * y * y);
  return __builtin_copysign(scalbn(y, q), x);
}
double hypot(double x, double y) {
  if (isinf(x) || isinf(y)) return HUGE_VAL;
  if (isnan(x) || isnan(y)) return x + y;
  double a = __builtin_fabs(x), b = __builtin_fabs(y);
  if (a < b) { double t = a; a = b; b = t; }
  if (b == 0) return a;
  int s = 0;
  if (a > 0x1p500) s = 600; else if (b < 0x1p-500) s = -600;
  a = scalbn(a, -s); b = scalbn(b, -s);
  return scalbn(__builtin_sqrt(a * a + b * b), s);
}

/* ---- trigonometry ---- */
static const double PIO2_HI = 0x1.921fb54442d18p+0, PIO2_LO = 0x1.1a62633145c07p-54;
static const double PIO2_1 = 0x1.921fb544p+0, PIO2_2 = 0x1.0b4611a6p-34, PIO2_3 = 0x1.3198a2e037073p-69;
/* 2/pi in binary, preceded by 64 zero bits */
static const uint32_t TWO_OVER_PI[] = {
  0x00000000, 0x00000000, 0xa2f9836e, 0x4e441529, 0xfc2757d1, 0xf534ddc0, 0xdb629599, 0x3c439041, 0xfe5163ab,
  0xdebbc561, 0xb7246e3a, 0x424dd2e0, 0x06492eea, 0x09d1921c, 0xfe1deb1c, 0xb129a73e, 0xe88235f5, 0x2ebb4484,
  0xe99c7026, 0xb45f7e41, 0x3991d639, 0x835339f4, 0x9c845f8b, 0xbdf9283b, 0x1ff897ff, 0xde05980f, 0xef2f118b,
  0x5a0a6d1f, 0x6d367ecf, 0x27cb09b7, 0x4f463f66, 0x9e5fea2d, 0x7527bac7, 0xebe5f17b, 0x3d0739f7, 0x8a5292ea,
  0x6bfb5fb1, 0x1f8d5d08, 0x56033046, 0xfc7b6bab, 0xf0cfbc20, 0x9af4361d, 0xa9e39000};

/* Payne-Hanek: multiply the 53-bit mantissa by a 192-bit window of 2/pi chosen so
 * the product holds the quadrant (mod 8) and ~189 fraction bits. x >= 2^20. */
static int rem_pio2_big(double x, double *rh, double *rl) {
  uint64_t u = bits(x);
  int E = (int)((u >> 52) & 0x7ff) - 1075;
  uint64_t M = (u & 0xfffffffffffffull) | 0x10000000000000ull;
  int j0 = E + 61;
  uint32_t w[6], m[2] = {(uint32_t)M, (uint32_t)(M >> 32)}, p[8] = {0};
  for (int k = 0; k < 6; k++) { /* w[0] is the least significant limb */
    int j = j0 + 32 * k, idx = j >> 5, sh = j & 31;
    uint32_t v = sh ? (TWO_OVER_PI[idx] << sh) | (TWO_OVER_PI[idx + 1] >> (32 - sh)) : TWO_OVER_PI[idx];
    w[5 - k] = v;
  }
  for (int i = 0; i < 2; i++) {
    uint64_t carry = 0;
    for (int k = 0; k < 6; k++) {
      uint64_t t = (uint64_t)m[i] * w[k] + p[i + k] + carry;
      p[i + k] = (uint32_t)t; carry = t >> 32;
    }
    p[i + 6] += (uint32_t)carry;
  }
  int q = (p[5] >> 29) & 7;
  uint64_t hi64 = ((uint64_t)(p[5] & 0x1fffffff) << 35) | ((uint64_t)p[4] << 3) | (p[3] >> 29);
  uint64_t lo64 = ((uint64_t)(p[3] & 0x1fffffff) << 35) | ((uint64_t)p[2] << 3) | (p[1] >> 29);
  double fh = (double)(hi64 >> 11) * 0x1p-53;
  double fl = (double)(hi64 & 0x7ff) * 0x1p-64 + (double)lo64 * 0x1p-128;
  if (fh >= 0.5) { fh -= 1; q++; }
  double s, e, a, b;
  two_sum(fh, fl, &s, &e);
  two_prod(s, PIO2_HI, &a, &b);
  b += s * PIO2_LO + e * PIO2_HI;
  two_sum(a, b, rh, rl);
  return q;
}
/* x = q pi/2 + (rh + rl), |rh| <= ~pi/4 */
static int rem_pio2(double x, double *rh, double *rl) {
  double ax = __builtin_fabs(x);
  if (ax <= 0x1.921fb54442d18p-1) { *rh = x; *rl = 0; return 0; }
  if (ax < 0x1p20) {
    double k = __builtin_rint(x * 0x1.45f306dc9c883p-1);
    double r = x - k * PIO2_1, w = k * PIO2_2, h, l;
    two_sum(r, -w, &h, &l);
    l -= k * PIO2_3;
    two_sum(h, l, rh, rl);
    return (int)(long long)k & 3;
  }
  int q = rem_pio2_big(ax, rh, rl);
  if (x < 0) { *rh = -*rh; *rl = -*rl; q = -q; }
  return q & 3;
}
static double sin_k(double x, double y) {
  double z = x * x;
  double p = z * horner(z, SINC, 9);
  return x + (x * p + y * (1 - 0.5 * z));
}
static double cos_k(double x, double y) {
  double z = x * x, hz = 0.5 * z, w = 1 - hz;
  double q = z * z * horner(z, COSC, 9);
  return w + (((1 - w) - hz) + (q - x * y));
}
double sin(double x) {
  if (!isfinite(x)) return x - x;
  double h, l;
  switch (rem_pio2(x, &h, &l)) {
  case 0: return sin_k(h, l);
  case 1: return cos_k(h, l);
  case 2: return -sin_k(h, l);
  default: return -cos_k(h, l);
  }
}
double cos(double x) {
  if (!isfinite(x)) return x - x;
  double h, l;
  switch (rem_pio2(x, &h, &l)) {
  case 0: return cos_k(h, l);
  case 1: return -sin_k(h, l);
  case 2: return -cos_k(h, l);
  default: return sin_k(h, l);
  }
}
double tan(double x) {
  if (!isfinite(x)) return x - x;
  double h, l;
  int q = rem_pio2(x, &h, &l);
  if (__builtin_fabs(h) < 0x1p-27) return q & 1 ? -1 / h : h;
  double s = sin_k(h, l), c = cos_k(h, l);
  return q & 1 ? -c / s : s / c;
}

static const double ATAN_HI[] = {0x1.dac670561bb4fp-2, 0x1.921fb54442d18p-1, 0x1.f730bd281f69bp-1, 0x1.921fb54442d18p+0};
static const double ATAN_LO[] = {0x1.a2b7f222f65e2p-56, 0x1.1a62633145c07p-55, 0x1.007887af0cbbdp-56, 0x1.1a62633145c07p-54};
static double atan_series(double t) {
  double z = t * t, p = 0;
  for (int n = 22; n >= 1; n--) p = (n & 1 ? -1.0 : 1.0) / (2 * n + 1) + z * p;
  return t * z * p;
}
double atan(double x) {
  if (isnan(x)) return x + x;
  double ax = __builtin_fabs(x), t, r;
  int i;
  if (ax < 0x1p-28) return x;
  if (ax < 7.0 / 16) return x + atan_series(x);
  if (ax < 11.0 / 16) i = 0, t = (2 * ax - 1) / (2 + ax);
  else if (ax < 19.0 / 16) i = 1, t = (ax - 1) / (ax + 1);
  else if (ax < 39.0 / 16) i = 2, t = (2 * ax - 3) / (2 + 3 * ax);
  else if (ax < 0x1p66) i = 3, t = -1 / ax;
  else return __builtin_copysign(PIO2_HI + PIO2_LO, x);
  r = ATAN_HI[i] + (t + (atan_series(t) + ATAN_LO[i]));
  return __builtin_copysign(r, x);
}
double atan2(double y, double x) {
  if (isnan(x) || isnan(y)) return x + y;
  const double pi = 0x1.921fb54442d18p+1, pilo = 0x1.1a62633145c07p-53;
  if (y == 0) {
    if (signbit(x)) return __builtin_copysign(pi, y);
    return y;
  }
  if (x == 0) return __builtin_copysign(PIO2_HI, y);
  if (isinf(x)) {
    if (isinf(y)) return __builtin_copysign(x > 0 ? pi / 4 : 3 * pi / 4, y);
    return x > 0 ? __builtin_copysign(0.0, y) : __builtin_copysign(pi, y);
  }
  if (isinf(y)) return __builtin_copysign(PIO2_HI, y);
  double ax = __builtin_fabs(x), ay = __builtin_fabs(y), r;
  if (ay > ax * 0x1p60) r = PIO2_HI;
  else if (x < 0 && ay < ax * 0x1p-60) r = 0;
  else r = atan(ay / ax);
  if (x < 0) r = pi - (r - pilo);
  return __builtin_copysign(r, y);
}
double asin(double x) {
  if (isnan(x)) return x + x;
  double ax = __builtin_fabs(x);
  if (ax > 1) return (x - x) / (x - x);
  if (ax == 1) return __builtin_copysign(PIO2_HI, x);
  if (ax <= 0.5) return atan(x / __builtin_sqrt(1 - x * x));
  double w = __builtin_sqrt((1 - ax) * 0.5);
  double a = atan(w / __builtin_sqrt(1 - w * w));
  return __builtin_copysign(PIO2_HI - (2 * a - PIO2_LO), x);
}
double acos(double x) {
  if (isnan(x)) return x + x;
  double ax = __builtin_fabs(x);
  if (ax > 1) return (x - x) / (x - x);
  if (ax <= 0.5) return PIO2_HI - (asin(x) - PIO2_LO);
  double w = __builtin_sqrt((1 - ax) * 0.5);
  double a = w == 0 ? 0 : atan(w / __builtin_sqrt(1 - w * w));
  if (x > 0) return 2 * a;
  return 2 * PIO2_HI - (2 * a - 2 * PIO2_LO);
}

/* ---- hyperbolic ---- */
double sinh(double x) {
  double ax = __builtin_fabs(x);
  if (!isfinite(x)) return x;
  if (ax < 22) {
    double t = expm1(ax);
    return __builtin_copysign(0.5 * (t + t / (t + 1)), x);
  }
  if (ax < 709.78) return __builtin_copysign(0.5 * exp(ax), x);
  double e = exp(0.5 * ax);
  return __builtin_copysign(0.5 * e * e, x);
}
double cosh(double x) {
  double ax = __builtin_fabs(x);
  if (!isfinite(x)) return ax;
  if (ax < 0.5) { double t = expm1(ax); return 1 + t * t / (2 * (1 + t)); }
  if (ax < 22) { double e = exp(ax); return 0.5 * (e + 1 / e); }
  if (ax < 709.78) return 0.5 * exp(ax);
  double e = exp(0.5 * ax);
  return 0.5 * e * e;
}
double tanh(double x) {
  if (isnan(x)) return x + x;
  double ax = __builtin_fabs(x);
  if (ax > 22) return __builtin_copysign(1.0, x);
  double t = expm1(2 * ax);
  return __builtin_copysign(t / (t + 2), x);
}
double asinh(double x) {
  double ax = __builtin_fabs(x);
  if (!isfinite(x) || ax < 0x1p-28) return x;
  double r = ax > 0x1p28 ? log(ax) + 0x1.62e42fefa39efp-1 : log1p(ax + ax * ax / (1 + __builtin_sqrt(1 + ax * ax)));
  return __builtin_copysign(r, x);
}
double acosh(double x) {
  if (isnan(x)) return x + x;
  if (x < 1) return (x - x) / (x - x);
  if (x > 0x1p28) return isinf(x) ? x : log(x) + 0x1.62e42fefa39efp-1;
  double t = x - 1;
  return log1p(t + __builtin_sqrt(2 * t + t * t));
}
double atanh(double x) {
  double ax = __builtin_fabs(x);
  if (isnan(x)) return x + x;
  if (ax > 1) return (x - x) / (x - x);
  if (ax == 1) return __builtin_copysign(HUGE_VAL, x);
  return __builtin_copysign(0.5 * log1p(2 * ax / (1 - ax)), x);
}

/* ---- float versions: compute in double, round once ---- */
#define F1(name) float name##f(float x) { return (float)name(x); }
#define F2(name) float name##f(float x, float y) { return (float)name(x, y); }
F1(sin) F1(cos) F1(tan) F1(asin) F1(acos) F1(atan) F2(atan2)
F1(sinh) F1(cosh) F1(tanh) F1(asinh) F1(acosh) F1(atanh)
F1(exp) F1(exp2) F1(expm1) F1(log) F1(log2) F1(log10) F1(log1p)
F2(pow) F1(cbrt) F2(hypot) F2(fmod) F2(remainder) F1(round) F2(fmin) F2(fmax) F2(fdim)
float ldexpf(float x, int n) { return (float)scalbn(x, n); }
float scalbnf(float x, int n) { return (float)scalbn(x, n); }
float frexpf(float x, int *e) { return (float)frexp(x, e); }
float modff(float x, float *ip) { double i, f = modf(x, &i); *ip = (float)i; return (float)f; }
float fmaf(float a, float b, float c) { return (float)((double)a * b + c); } /* the product is exact in double */
float nanf(const char *s) { (void)s; return NAN; }
long lroundf(float x) { return lround(x); }
long lrintf(float x) { return lrint(x); }
float nextafterf(float x, float y) {
  if (isnan(x) || isnan(y)) return x + y;
  if (x == y) return y;
  union { float f; uint32_t u; } b = {x};
  if (x == 0) { b.u = 1; return __builtin_copysignf(b.f, y); }
  b.u += ((x < y) == (x > 0)) ? 1 : -1;
  return b.f;
}
