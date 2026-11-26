// SPDX-License-Identifier: GPL-2.0-or-later
// Native test of the software floating point in libjit/rvfp.c against the host's IEEE
// hardware (x86-64 SSE): add, sub, mul, div, sqrt, fused multiply-add, in single and double
// precision, in the four rounding modes the host has (RNE, RTZ, RDN, RUP; RISC-V's RMM has
// no x86 equivalent), on random and edge-case operands. Also integer conversions in all modes.
//   cc -O1 -frounding-math -Ilibjit tools/jit/fptest.c libjit/rvfp.c libjit/wasm.c libjit/fmt.c libjit/port_native.c -lm -o fptest
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rvfp.h"

uint64_t rvfp_eval(int op, int rm, uint64_t a, uint64_t b, uint64_t c);
// unused engine hooks
uint8_t *rv_mem; uint64_t rv_memsize;
void rv_store_hook(uint64_t a, int n) { (void)a; (void)n; }
void engine_frm_changed(void) {}
void xg_get(xgen *g, int r) { (void)g; (void)r; } void xg_set(xgen *g, int r) { (void)g; (void)r; }
void xg_fget(xgen *g, int r) { (void)g; (void)r; } void xg_fset(xgen *g, int r) { (void)g; (void)r; }
uint32_t xg_addr(xgen *g, int rs1, int64_t imm, int size) { (void)g; (void)rs1; (void)imm; (void)size; return 0; }
uint32_t xg_import(int i) { return (uint32_t)i; }

static uint64_t rng = 88172645463325252ull;
static uint64_t r64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint64_t pick64(void) {
  uint64_t v = r64();
  switch (r64() % 8) {
  case 0: return v & 0x800fffffffffffffull;                       // subnormal or zero
  case 1: return (v & 0x800fffffffffffffull) | 0x7fe0000000000000ull; // huge
  case 2: return (v & 0x80000000000fffffull) | 0x3ff0000000000000ull; // near 1, few bits
  case 3: return (v & 0x8000000000000000ull) | ((uint64_t)(1023 + (int)(r64() % 60) - 30) << 52) | (v & 0xfffff); // short mantissa
  default: return v;
  }
}
static uint32_t pick32(void) {
  uint32_t v = (uint32_t)r64();
  switch (r64() % 8) {
  case 0: return v & 0x807fffffu;
  case 1: return (v & 0x807fffffu) | 0x7f000000u;
  case 2: return (v & 0x800003ffu) | 0x3f800000u;
  default: return v;
  }
}
static double D(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t Ud(double d) { uint64_t u; memcpy(&u, &d, 8); return d != d ? 0x7ff8000000000000ull : u; }
static float F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t Uf(float f) { uint32_t u; memcpy(&u, &f, 4); return 0xffffffff00000000ull | (f != f ? 0x7fc00000u : u); }

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 200000;
  static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD };
  long checks = 0, fails = 0;
  for (int m = 0; m < 4; m++) {
    for (long i = 0; i < n; i++) {
      uint64_t a = pick64(), b = pick64(), c = pick64();
      uint32_t x = pick32(), y = pick32(), z = pick32();
      volatile double da = D(a), db = D(b), dc = D(c);
      volatile float fx = F(x), fy = F(y), fz = F(z);
      fesetround(modes[m]);
      uint64_t hw[12] = { Ud(da + db), Ud(da - db), Ud(da * db), Ud(da / db), Ud(sqrt(fabs(da))), Ud(fma(da, db, dc)),
                          Uf(fx + fy), Uf(fx - fy), Uf(fx * fy), Uf(fx / fy), Uf(sqrtf(fabsf(fx))), Uf(fmaf(fx, fy, fz)) };
      fesetround(FE_TONEAREST);
      uint64_t ab = a & ~(1ull << 63);
      uint64_t xb = 0xffffffff00000000ull | x, yb = 0xffffffff00000000ull | y, zb = 0xffffffff00000000ull | z, xab = xb & ~0x80000000ull;
      uint64_t sw[12] = { rvfp_eval(RV_FADD_D, m, a, b, 0), rvfp_eval(RV_FSUB_D, m, a, b, 0), rvfp_eval(RV_FMUL_D, m, a, b, 0),
                          rvfp_eval(RV_FDIV_D, m, a, b, 0), rvfp_eval(RV_FSQRT_D, m, ab, 0, 0), rvfp_eval(RV_FMADD_D, m, a, b, c),
                          rvfp_eval(RV_FADD_S, m, xb, yb, 0), rvfp_eval(RV_FSUB_S, m, xb, yb, 0), rvfp_eval(RV_FMUL_S, m, xb, yb, 0),
                          rvfp_eval(RV_FDIV_S, m, xb, yb, 0), rvfp_eval(RV_FSQRT_S, m, xab, 0, 0), rvfp_eval(RV_FMADD_S, m, xb, yb, zb) };
      static const char *nm[12] = { "fadd.d", "fsub.d", "fmul.d", "fdiv.d", "fsqrt.d", "fmadd.d", "fadd.s", "fsub.s", "fmul.s", "fdiv.s", "fsqrt.s", "fmadd.s" };
      for (int k = 0; k < 12; k++) {
        checks++;
        if (hw[k] != sw[k]) {
          if (fails++ < 20) printf("FAIL %s rm=%d a=%016llx b=%016llx c=%016llx x=%08x y=%08x z=%08x: hw %016llx sw %016llx\n", nm[k], m,
                                   (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, x, y, z, (unsigned long long)hw[k], (unsigned long long)sw[k]);
        }
      }
      // conversions double -> int64 / int32 against the host in range (RISC-V saturation checked separately)
      double v = D(pick64());
      if (v == v && fabs(v) < 9e18) {
        fesetround(modes[m]);
        long long hl = llrint(v);
        long hw32 = (fabs(v) < 2e9) ? lrint(v) : 0;
        fesetround(FE_TONEAREST);
        checks++;
        if ((long long)rvfp_eval(RV_FCVT_L_D, m, Ud(v), 0, 0) != hl) { if (fails++ < 20) printf("FAIL fcvt.l.d rm=%d %a: hw %lld sw %lld\n", m, v, hl, (long long)rvfp_eval(RV_FCVT_L_D, m, Ud(v), 0, 0)); }
        if (fabs(v) < 2e9) { checks++; if ((long long)rvfp_eval(RV_FCVT_W_D, m, Ud(v), 0, 0) != hw32) { if (fails++ < 20) printf("FAIL fcvt.w.d rm=%d %a\n", m, v); } }
      }
      int64_t iv = (int64_t)r64() >> (r64() % 64);
      fesetround(modes[m]);
      volatile double hd = (double)iv; volatile float hf = (float)iv;
      fesetround(FE_TONEAREST);
      checks += 2;
      if (rvfp_eval(RV_FCVT_D_L, m, (uint64_t)iv, 0, 0) != Ud(hd)) { if (fails++ < 20) printf("FAIL fcvt.d.l rm=%d %lld\n", m, (long long)iv); }
      if (rvfp_eval(RV_FCVT_S_L, m, (uint64_t)iv, 0, 0) != Uf(hf)) { if (fails++ < 20) printf("FAIL fcvt.s.l rm=%d %lld\n", m, (long long)iv); }
      volatile double dd = D(pick64());
      fesetround(modes[m]);
      volatile float demoted = (float)dd;
      fesetround(FE_TONEAREST);
      checks++;
      if (rvfp_eval(RV_FCVT_S_D, m, Ud(dd), 0, 0) != Uf(demoted)) { if (fails++ < 20) printf("FAIL fcvt.s.d rm=%d %a\n", m, dd); }
    }
  }
  // RISC-V specific edge cases, by hand
  struct { int op, rm; uint64_t a, b; uint64_t want; const char *what; } e[] = {
    { RV_FCVT_W_D, 1, 0x7ff8000000000000ull, 0, 0x7fffffffull, "fcvt.w.d NaN -> 2^31-1" },
    { RV_FCVT_WU_D, 1, 0x7ff8000000000000ull, 0, 0xffffffffffffffffull, "fcvt.wu.d NaN -> 2^32-1, sign-extended" },
    { RV_FCVT_L_D, 1, 0xfff0000000000000ull, 0, 0x8000000000000000ull, "fcvt.l.d -inf" },
    { RV_FCVT_LU_D, 1, 0xbff0000000000000ull, 0, 0, "fcvt.lu.d -1.0 -> 0" },
    { RV_FCVT_W_D, 1, 0x41e0000000000000ull, 0, 0x7fffffffull, "fcvt.w.d 2^31 saturates" },
    { RV_FCVT_W_D, 4, 0x4004000000000000ull, 0, 3, "fcvt.w.d 2.5 rmm -> 3" },
    { RV_FCVT_W_D, 0, 0x4004000000000000ull, 0, 2, "fcvt.w.d 2.5 rne -> 2" },
    { RV_FADD_D, 4, 0x3ff0000000000000ull, 0x3ca0000000000000ull, 0x3ff0000000000001ull, "1 + 2^-53 rmm rounds away" },
    { RV_FADD_D, 0, 0x3ff0000000000000ull, 0x3ca0000000000000ull, 0x3ff0000000000000ull, "1 + 2^-53 rne ties to even" },
    { RV_FSUB_D, 2, 0x3ff0000000000000ull, 0x3ff0000000000000ull, 0x8000000000000000ull, "1 - 1 in rdn is -0" },
    { RV_FCVT_D_S, 0, 0xffffffff7fc00001ull, 0, 0x7ff8000000000000ull, "fcvt.d.s NaN payload -> canonical" },
    { RV_FADD_S, 0, 0x000000003f800000ull, 0xffffffff3f800000ull, 0xffffffff7fc00000ull, "unboxed single reads as canonical NaN" },
  };
  for (unsigned i = 0; i < sizeof e / sizeof e[0]; i++) {
    checks++;
    uint64_t r = rvfp_eval(e[i].op, e[i].rm, e[i].a, e[i].b, 0);
    if (r != e[i].want) { fails++; printf("FAIL %s: got %016llx want %016llx\n", e[i].what, (unsigned long long)r, (unsigned long long)e[i].want); }
  }
  printf("fptest: %ld checks, %ld failed\n", checks, fails);
  return fails != 0;
}
