// SPDX-License-Identifier: GPL-2.0-or-later
/* Differential test of softfp.c against the host's IEEE 754 hardware (x86-64 SSE, which
 * rounds correctly in four modes, detects tininess after rounding like RISC-V, and
 * reports the five flags). Native only. RISC-V-specific results (canonical NaN,
 * saturating conversions, RMM) are checked against the ISA rules instead.
 * Build: see run.sh. Usage: fp_test [iterations] */
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../softfp.h"
#include "../rv_fp.h"

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static uint64_t pick64(void) {
  static const uint64_t sp[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000001ull,
    0x0000000000000001ull, 0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
    0x3fe0000000000000ull, 0x4330000000000000ull, 0x43e0000000000000ull, 0x41e0000000000000ull};
  uint64_t r = rnd();
  switch (r % 8) {
  case 0: return sp[(r >> 8) % 16];
  case 1: return (r & 0x800fffffffffffffull) | ((uint64_t)((r >> 20) % 4) << 52); /* tiny */
  case 2: return (r & 0x800fffffffffffffull) | ((uint64_t)(2043 + (r >> 20) % 4) << 52); /* huge */
  case 3: return (r & 0x8000000000000000ull) | ((uint64_t)(1023 + (int)((r >> 20) % 128) - 64) << 52) | (rnd() & 0xfffffffffffffull);
  case 4: return (r & 0x80000000000003ffull) | ((uint64_t)(1000 + (r >> 20) % 50) << 52); /* few bits */
  case 5: return (r & 0x8000000000000000ull) | ((uint64_t)(1023 + (r >> 30) % 70) << 52) | (rnd() & 0xfff0000000000ull); /* integers */
  default: return rnd();
  }
}
static uint32_t pick32(void) {
  static const uint32_t sp[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0x7f800001u, 1, 0x7fffff, 0x800000, 0x7f7fffff, 0x3f000000u, 0x4b000000u, 0x5f000000u, 0x4f000000u};
  uint32_t r = (uint32_t)rnd();
  switch (r % 8) {
  case 0: return sp[(r >> 8) % 16];
  case 1: return (r & 0x807fffffu) | (((r >> 23) % 4) << 23);
  case 2: return (r & 0x807fffffu) | ((251 + (r >> 23) % 4) << 23);
  case 3: return (r & 0x80000000u) | ((127 + (int)((r >> 23) % 64) - 32) << 23) | ((uint32_t)rnd() & 0x7fffff);
  case 4: return (r & 0x8000000fu) | ((100 + (r >> 23) % 50) << 23);
  case 5: return (r & 0x80000000u) | ((127 + (r >> 24) % 40) << 23) | ((uint32_t)rnd() & 0x7f0000);
  default: return r;
  }
}

static const int hwmode[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};
static unsigned hwflags(void) {
  int e = fetestexcept(FE_ALL_EXCEPT);
  return (e & FE_INEXACT ? FF_NX : 0) | (e & FE_UNDERFLOW ? FF_UF : 0) | (e & FE_OVERFLOW ? FF_OF : 0) |
         (e & FE_DIVBYZERO ? FF_DZ : 0) | (e & FE_INVALID ? FF_NV : 0);
}
static int isnan64(uint64_t x) { return (x & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
static int isnan32(uint32_t x) { return (x & 0x7fffffff) > 0x7f800000u; }
static uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint32_t f2u(float d) { uint32_t u; memcpy(&u, &d, 4); return u; }
static float u2f(uint32_t u) { float d; memcpy(&d, &u, 4); return d; }

static long fails, checks;
#define FAIL(...) do { if (fails++ < 30) { printf(__VA_ARGS__); } } while (0)

static int infzero64(uint64_t a, uint64_t b) {
  uint64_t A = a & 0x7fffffffffffffffull, B = b & 0x7fffffffffffffffull;
  return (A == 0x7ff0000000000000ull && B == 0) || (B == 0x7ff0000000000000ull && A == 0);
}
static int infzero32(uint32_t a, uint32_t b) {
  uint32_t A = a & 0x7fffffff, B = b & 0x7fffffff;
  return (A == 0x7f800000u && B == 0) || (B == 0x7f800000u && A == 0);
}
static void check64(const char *op, int rm, uint64_t a, uint64_t b, uint64_t c, uint64_t got, uint32_t gf, uint64_t want, uint32_t wf) {
  checks++;
  /* RISC-V raises NV for inf * 0 + qNaN; x86 does not */
  if ((!strncmp(op, "fm", 2) || !strncmp(op, "fnm", 3)) && infzero64(a, b)) wf |= FF_NV;
  if (isnan64(want)) want = F64_QNAN; /* RISC-V: every NaN result is the canonical NaN */
  if (got != want || gf != wf)
    FAIL("%s rm%d a=%016llx b=%016llx c=%016llx: got %016llx/%02x want %016llx/%02x\n", op, rm,
         (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)got, gf, (unsigned long long)want, wf);
}
static void check32(const char *op, int rm, uint32_t a, uint32_t b, uint32_t c, uint32_t got, uint32_t gf, uint32_t want, uint32_t wf) {
  checks++;
  if (!strncmp(op, "fma", 3) && infzero32(a, b)) wf |= FF_NV;
  if (isnan32(want)) want = F32_QNAN;
  if (got != want || gf != wf)
    FAIL("%s rm%d a=%08x b=%08x c=%08x: got %08x/%02x want %08x/%02x\n", op, rm, a, b, c, got, gf, want, wf);
}

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 200000;
  for (long i = 0; i < n; i++) {
    int rm = (int)(rnd() % 4);
    uint64_t a = pick64(), b = pick64(), c = pick64();
    uint32_t a3 = pick32(), b3 = pick32(), c3 = pick32();
    volatile double x = u2d(a), y = u2d(b), z = u2d(c), r;
    volatile float x3 = u2f(a3), y3 = u2f(b3), z3 = u2f(c3), r3;
    uint32_t fl;
    fesetround(hwmode[rm]);
#define T64(name, expr, call) feclearexcept(FE_ALL_EXCEPT); r = (expr); { uint32_t wf = hwflags(); fl = 0; uint64_t g = call; check64(name, rm, a, b, c, g, fl, d2u(r), wf); }
#define T32(name, expr, call) feclearexcept(FE_ALL_EXCEPT); r3 = (expr); { uint32_t wf = hwflags(); fl = 0; uint32_t g = call; check32(name, rm, a3, b3, c3, g, fl, f2u(r3), wf); }
    T64("add64", x + y, sf64_add(a, b, rm, &fl));
    T64("sub64", x - y, sf64_sub(a, b, rm, &fl));
    T64("mul64", x * y, sf64_mul(a, b, rm, &fl));
    T64("div64", x / y, sf64_div(a, b, rm, &fl));
    T64("sqrt64", sqrt(x), sf64_sqrt(a, rm, &fl));
    T64("fma64", fma(x, y, z), sf64_fma(a, b, c, 0, 0, rm, &fl));
    T64("fms64", fma(x, y, -z), sf64_fma(a, b, c, 0, 1, rm, &fl));
    T64("fnma64", fma(-x, y, -z), sf64_fma(a, b, c, 1, 1, rm, &fl));
    T32("add32", x3 + y3, sf32_add(a3, b3, rm, &fl));
    T32("sub32", x3 - y3, sf32_sub(a3, b3, rm, &fl));
    T32("mul32", x3 * y3, sf32_mul(a3, b3, rm, &fl));
    T32("div32", x3 / y3, sf32_div(a3, b3, rm, &fl));
    T32("sqrt32", sqrtf(x3), sf32_sqrt(a3, rm, &fl));
    T32("fma32", fmaf(x3, y3, z3), sf32_fma(a3, b3, c3, 0, 0, rm, &fl));
    T32("cvt.s.d", (float)x, sf64_to_f32(a, rm, &fl));
    /* f32 -> f64 is exact: only NV for sNaN */
    feclearexcept(FE_ALL_EXCEPT); r = (double)x3;
    { uint32_t wf = hwflags(); fl = 0; uint64_t g = sf32_to_f64(a3, &fl); check64("cvt.d.s", rm, a3, 0, 0, g, fl, d2u(r), wf); }
    /* int -> float */
    {
      int64_t iv = (int64_t)rnd() >> (rnd() % 64);
      uint64_t uv = rnd() >> (rnd() % 64);
      feclearexcept(FE_ALL_EXCEPT); r = (double)iv;
      { uint32_t wf = hwflags(); fl = 0; uint64_t g = sf64_from_int((uint64_t)iv, 1, rm, &fl); check64("cvt.d.l", rm, (uint64_t)iv, 0, 0, g, fl, d2u(r), wf); }
      feclearexcept(FE_ALL_EXCEPT); r = (double)uv;
      { uint32_t wf = hwflags(); fl = 0; uint64_t g = sf64_from_int(uv, 0, rm, &fl); check64("cvt.d.lu", rm, uv, 0, 0, g, fl, d2u(r), wf); }
      feclearexcept(FE_ALL_EXCEPT); r3 = (float)iv;
      { uint32_t wf = hwflags(); fl = 0; uint32_t g = sf32_from_int((uint64_t)iv, 1, rm, &fl); check32("cvt.s.l", rm, (uint32_t)iv, 0, 0, g, fl, f2u(r3), wf); }
    }
    /* float -> int, in range only (out of range is RISC-V specific, checked below) */
    {
      double v = u2d(a);
      if (!isnan64(a) && fabs(v) < 9.2e18) {
        feclearexcept(FE_ALL_EXCEPT);
        volatile long long w = llrint(v);
        uint32_t wf = hwflags(); fl = 0;
        uint64_t g = sf64_to_int(a, 64, 1, rm, &fl);
        checks++;
        if (g != (uint64_t)w || fl != wf) FAIL("cvt.l.d rm%d a=%016llx got %lld/%x want %lld/%x\n", rm, (unsigned long long)a, (long long)g, fl, (long long)w, wf);
      }
      if (!isnan64(a) && fabs(v) < 2.1e9) {
        feclearexcept(FE_ALL_EXCEPT);
        volatile double t = nearbyint(v);
        int inexact = t != v;
        long long w = (long long)t;
        fl = 0;
        uint64_t g = sf64_to_int(a, 32, 1, rm, &fl);
        checks++;
        if (w >= -2147483648LL && w <= 2147483647LL && (g != (uint64_t)(int64_t)(int32_t)w || fl != (inexact ? FF_NX : 0u)))
          FAIL("cvt.w.d rm%d a=%016llx got %lld/%x want %lld\n", rm, (unsigned long long)a, (long long)g, fl, w);
      }
    }
    /* compares (quiet / signalling) */
    {
      fl = 0; int g = sf64_eq(a, b, &fl);
      feclearexcept(FE_ALL_EXCEPT); int w = x == y; uint32_t wf = hwflags();
      checks++; if (g != w || fl != wf) FAIL("feq.d %016llx %016llx got %d/%x want %d/%x\n", (unsigned long long)a, (unsigned long long)b, g, fl, w, wf);
      fl = 0; g = sf64_lt(a, b, &fl);
      feclearexcept(FE_ALL_EXCEPT); w = __builtin_isless(x, y) ? 1 : 0; wf = (isnan64(a) || isnan64(b)) ? FF_NV : 0;
      checks++; if (g != w || fl != wf) FAIL("flt.d %016llx %016llx got %d/%x want %d/%x\n", (unsigned long long)a, (unsigned long long)b, g, fl, w, wf);
      fl = 0; g = sf64_le(a, b, &fl);
      w = __builtin_islessequal(x, y) ? 1 : 0;
      checks++; if (g != w || fl != wf) FAIL("fle.d %016llx %016llx got %d/%x want %d/%x\n", (unsigned long long)a, (unsigned long long)b, g, fl, w, wf);
    }
  }
  fesetround(FE_TONEAREST);

  /* The interpreter's fast paths (rv_fp.c) must agree with the reference bit for bit,
     flags included, whatever NX already holds. */
  for (long i = 0; i < n; i++) {
    uint64_t a = pick64(), b = pick64();
    uint32_t a3 = pick32(), b3 = pick32();
    uint32_t pre = (rnd() & 1) ? FF_NX : 0;
    int rm = (rnd() % 8) ? RM_RNE : (int)(rnd() % 5);
#define FX64(name, fast, ref) { uint32_t f1 = pre, f2 = pre; uint64_t g1 = fast, g2 = ref; checks++; \
    if (g1 != g2 || f1 != f2) FAIL("fast %s rm%d %016llx %016llx: %016llx/%x vs %016llx/%x\n", name, rm, (unsigned long long)a, (unsigned long long)b, (unsigned long long)g1, f1, (unsigned long long)g2, f2); }
#define FX32(name, fast, ref) { uint32_t f1 = pre, f2 = pre; uint32_t g1 = fast, g2 = ref; checks++; \
    if (g1 != g2 || f1 != f2) FAIL("fast %s rm%d %08x %08x: %08x/%x vs %08x/%x\n", name, rm, a3, b3, g1, f1, g2, f2); }
    FX64("add.d", fpd_add(a, b, rm, &f1), sf64_add(a, b, rm, &f2));
    FX64("sub.d", fpd_sub(a, b, rm, &f1), sf64_sub(a, b, rm, &f2));
    FX64("mul.d", fpd_mul(a, b, rm, &f1), sf64_mul(a, b, rm, &f2));
    FX64("div.d", fpd_div(a, b, rm, &f1), sf64_div(a, b, rm, &f2));
    FX64("sqrt.d", fpd_sqrt(a, rm, &f1), sf64_sqrt(a, rm, &f2));
    FX32("add.s", fps_add(a3, b3, rm, &f1), sf32_add(a3, b3, rm, &f2));
    FX32("sub.s", fps_sub(a3, b3, rm, &f1), sf32_sub(a3, b3, rm, &f2));
    FX32("mul.s", fps_mul(a3, b3, rm, &f1), sf32_mul(a3, b3, rm, &f2));
    FX32("div.s", fps_div(a3, b3, rm, &f1), sf32_div(a3, b3, rm, &f2));
    FX32("sqrt.s", fps_sqrt(a3, rm, &f1), sf32_sqrt(a3, rm, &f2));
    { uint64_t c = pick64(); uint32_t c3 = pick32(); int np = (int)(rnd() & 1), nc = (int)(rnd() & 1);
      FX64("fma.d", fpd_fma(a, b, c, np, nc, rm, &f1), sf64_fma(a, b, c, np, nc, rm, &f2));
      FX32("fma.s", fps_fma(a3, b3, c3, np, nc, rm, &f1), sf32_fma(a3, b3, c3, np, nc, rm, &f2));
      /* cancellation-heavy cases: c close to -a*b */
      uint32_t fx = 0; uint64_t pr = sf64_mul(a, b, RM_RNE, &fx);
      uint64_t cv = pr ^ (rnd() & 0x800000000000000full);
      FX64("fma.d cancel", fpd_fma(a, b, cv, 0, 1, rm, &f1), sf64_fma(a, b, cv, 0, 1, rm, &f2));
    }
    /* near the subnormal boundary on purpose */
    a3 = (a3 & 0x807fffffu) | ((1 + (uint32_t)(rnd() % 3)) << 23);
    b3 = 0x3f800000u ^ ((uint32_t)rnd() & 0x8000000fu);
    FX32("mul.s tiny", fps_mul(a3, b3, rm, &f1), sf32_mul(a3, b3, rm, &f2));
    FX32("add.s tiny", fps_add(a3, b3 & 0x80000000u ? 0x00800001u : 0x80800000u, rm, &f1), sf32_add(a3, b3 & 0x80000000u ? 0x00800001u : 0x80800000u, rm, &f2));
  }

  /* RISC-V specific fixed vectors */
  struct { uint64_t in; int bits, sgn; uint64_t out; uint32_t fl; } cv[] = {
    {0x7ff8000000000000ull, 32, 1, 0x7fffffff, FF_NV},            /* NaN -> max */
    {0xfff0000000000000ull, 32, 1, 0xffffffff80000000ull, FF_NV}, /* -inf -> min */
    {0x41e0000000000000ull, 32, 1, 0x7fffffff, FF_NV},            /* 2^31 overflows */
    {0xc1e0000000000000ull, 32, 1, 0xffffffff80000000ull, 0},     /* -2^31 exact */
    {0xbff0000000000000ull, 32, 0, 0, FF_NV},                     /* -1 -> unsigned: 0, NV */
    {0xbfd0000000000000ull, 32, 0, 0, FF_NX},                     /* -0.25 -> unsigned (rne 0): NX only */
    {0x7ff8000000000000ull, 32, 0, 0xffffffffffffffffull, FF_NV}, /* NaN -> wu max, sign-extended */
    {0x41f0000000000000ull, 32, 0, 0xffffffffffffffffull, FF_NV}, /* 2^32 overflows wu */
    {0x43e0000000000000ull, 64, 1, 0x7fffffffffffffffull, FF_NV}, /* 2^63 overflows l */
    {0xc3e0000000000000ull, 64, 1, 0x8000000000000000ull, 0},     /* -2^63 exact */
    {0x43f0000000000000ull, 64, 0, 0xffffffffffffffffull, FF_NV}, /* 2^64 overflows lu */
    {0x43efffffffffffffull, 64, 0, 0xfffffffffffff800ull, 0},     /* just below 2^64 */
  };
  for (unsigned i = 0; i < sizeof cv / sizeof cv[0]; i++) {
    uint32_t f = 0;
    uint64_t g = sf64_to_int(cv[i].in, cv[i].bits, cv[i].sgn, RM_RNE, &f);
    checks++;
    if (g != cv[i].out || f != cv[i].fl) FAIL("cvt vector %u: got %016llx/%x want %016llx/%x\n", i, (unsigned long long)g, f, (unsigned long long)cv[i].out, cv[i].fl);
  }
  /* min/max: -0 < +0, one NaN returns the other, two NaNs the canonical NaN, sNaN sets NV */
  {
    uint32_t f = 0;
    checks += 6;
    if (sf64_minmax(0x8000000000000000ull, 0, 0, &f) != 0x8000000000000000ull) FAIL("fmin -0 +0\n");
    if (sf64_minmax(0, 0x8000000000000000ull, 1, &f) != 0) FAIL("fmax +0 -0\n");
    if (sf64_minmax(0x7ff8000000000000ull, 0x3ff0000000000000ull, 0, &f) != 0x3ff0000000000000ull || f) FAIL("fmin qnan 1\n");
    if (sf64_minmax(0x7ff0000000000001ull, 0x3ff0000000000000ull, 1, &f) != 0x3ff0000000000000ull || f != FF_NV) FAIL("fmax snan 1\n");
    f = 0;
    if (sf64_minmax(0x7ff0000000000001ull, 0x7ff8000000000000ull, 1, &f) != F64_QNAN || f != FF_NV) FAIL("fmax snan qnan\n");
    f = 0;
    if (sf32_minmax(0xbf800000u, 0x3f800000u, 1, &f) != 0x3f800000u || f) FAIL("fmax.s -1 1\n");
  }
  /* RMM: ties away from zero. 1 + 2^-53 is a tie between 1 and 1+2^-52 in binary64. */
  {
    uint32_t f = 0;
    checks += 3;
    if (sf64_add(0x3ff0000000000000ull, 0x3ca0000000000000ull, RM_RMM, &f) != 0x3ff0000000000001ull || f != FF_NX) FAIL("rmm tie up\n");
    f = 0;
    if (sf64_add(0x3ff0000000000000ull, 0x3ca0000000000000ull, RM_RNE, &f) != 0x3ff0000000000000ull || f != FF_NX) FAIL("rne tie even\n");
    f = 0;
    if (sf64_add(0xbff0000000000000ull, 0xbca0000000000000ull, RM_RMM, &f) != 0xbff0000000000001ull || f != FF_NX) FAIL("rmm tie down neg\n");
  }
  /* fma: inf * 0 + qNaN raises NV on RISC-V */
  {
    uint32_t f = 0;
    checks++;
    if (sf64_fma(0x7ff0000000000000ull, 0, 0x7ff8000000000000ull, 0, 0, RM_RNE, &f) != F64_QNAN || f != FF_NV) FAIL("fma inf*0+qnan\n");
  }
  /* class */
  checks += 3;
  if (sf64_class(0xfff0000000000000ull) != 1) FAIL("class -inf\n");
  if (sf64_class(0x0000000000000001ull) != 32) FAIL("class +sub\n");
  if (sf32_class(0x7fc00000u) != 512) FAIL("class qnan\n");
  printf("fp_test: %ld checks, %ld failed\n", checks, fails);
  return fails != 0;
}
