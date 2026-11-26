// SPDX-License-Identifier: GPL-2.0-or-later
/* Differential instruction test. Computes a hash per group of operations over many
 * pseudo-random operands. Built natively and for riscv64 (several flag sets): the
 * printed lines must be identical. Only operations whose C result is fully defined are
 * used (no signed overflow, no out-of-range float to integer conversions). */
#include "rt.h"

static uint64_t s = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h; }
static uint64_t interesting(void) {
  static const uint64_t sp[] = {0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000ull,
    0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, ~1ull};
  uint64_t r = rnd();
  switch (r & 7) {
  case 0: return sp[(r >> 3) % 18];
  case 1: return r >> (r % 64);
  case 2: return (uint64_t)(int64_t)(int32_t)r;
  default: return rnd();
  }
}
static double dval(void) {
  uint64_t r = rnd();
  double d;
  switch (r & 3) {
  case 0: d = (double)(int64_t)(r >> 20) / 1024.0; break;
  case 1: d = (double)(r % 1000) - 500; break;
  case 2: { union { uint64_t u; double d; } u = {(r & 0x800fffffffffffffull) | ((uint64_t)(1023 + (int)((r >> 52) % 80) - 40) << 52)}; d = u.d; break; }
  default: d = (double)(int32_t)r * 0.001; break;
  }
  return d;
}
static uint64_t dbits(double d) { union { double d; uint64_t u; } u = {d}; return u.u; }
static uint32_t fbits(float f) { union { float f; uint32_t u; } u = {f}; return u.u; }

#define N 20000
#define NOINLINE __attribute__((noinline))

NOINLINE static uint64_t g_int64(void) {
  uint64_t h = 0;
  for (int i = 0; i < N; i++) {
    uint64_t a = interesting(), b = interesting();
    unsigned sh = (unsigned)(b & 63);
    h = mix(h, a + b); h = mix(h, a - b); h = mix(h, a ^ b); h = mix(h, a | b); h = mix(h, a & b);
    h = mix(h, a << sh); h = mix(h, a >> sh); h = mix(h, (uint64_t)((int64_t)a >> sh));
    h = mix(h, (int64_t)a < (int64_t)b); h = mix(h, a < b); h = mix(h, a == b);
    h = mix(h, a * b);
    h = mix(h, (uint64_t)(((unsigned __int128)a * b) >> 64));
    h = mix(h, (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64));
    h = mix(h, (uint64_t)(((__int128)(int64_t)a * (unsigned __int128)b) >> 64));
    if (b) { h = mix(h, a / b); h = mix(h, a % b); }
    if (b && !((int64_t)a == INT64_MIN && (int64_t)b == -1)) { h = mix(h, (uint64_t)((int64_t)a / (int64_t)b)); h = mix(h, (uint64_t)((int64_t)a % (int64_t)b)); }
    h = mix(h, (uint64_t)(int64_t)(int8_t)a); h = mix(h, (uint64_t)(int64_t)(int16_t)a); h = mix(h, (uint16_t)a);
    h = mix(h, (uint64_t)__builtin_popcountll(a)); h = mix(h, a ? (uint64_t)__builtin_clzll(a) : 99);
    h = mix(h, a ? (uint64_t)__builtin_ctzll(a) : 99); h = mix(h, __builtin_bswap64(a));
    h = mix(h, (a >> sh) | (a << ((64 - sh) & 63)));
    h = mix(h, (int64_t)a > (int64_t)b ? a : b); h = mix(h, a < b ? a : b);
  }
  return h;
}

NOINLINE static uint64_t g_int32(void) {
  uint64_t h = 0;
  for (int i = 0; i < N; i++) {
    uint32_t a = (uint32_t)interesting(), b = (uint32_t)interesting();
    unsigned sh = b & 31;
    h = mix(h, (uint64_t)(int64_t)(int32_t)(a + b)); h = mix(h, (uint64_t)(int64_t)(int32_t)(a - b));
    h = mix(h, a << sh); h = mix(h, a >> sh); h = mix(h, (uint64_t)(int64_t)((int32_t)a >> sh));
    h = mix(h, (uint64_t)(int64_t)(int32_t)(a * b));
    if (b) { h = mix(h, a / b); h = mix(h, a % b); }
    if (b && !((int32_t)a == INT32_MIN && (int32_t)b == -1)) { h = mix(h, (uint64_t)(int64_t)((int32_t)a / (int32_t)b)); h = mix(h, (uint64_t)(int64_t)((int32_t)a % (int32_t)b)); }
    h = mix(h, (uint64_t)(int64_t)(int32_t)a < (int64_t)(int32_t)b);
    h = mix(h, (uint64_t)__builtin_popcount(a)); h = mix(h, a ? (uint64_t)__builtin_clz(a) : 99);
    h = mix(h, (uint64_t)a + ((uint64_t)b << 3));
  }
  return h;
}

static volatile uint8_t mem[4096];
NOINLINE static uint64_t g_mem(void) {
  uint64_t h = 0;
  for (int i = 0; i < 4096; i++) mem[i] = (uint8_t)rnd();
  for (int i = 0; i < N; i++) {
    unsigned o = (unsigned)(rnd() % 4000);
    volatile uint8_t *p = mem + o;
    h = mix(h, (uint64_t)(int64_t)*(volatile int8_t *)p); h = mix(h, *p);
    h = mix(h, (uint64_t)(int64_t)*(volatile int16_t *)(volatile void *)p); /* misaligned on purpose */
    h = mix(h, *(volatile uint16_t *)(volatile void *)p);
    h = mix(h, (uint64_t)(int64_t)*(volatile int32_t *)(volatile void *)p); h = mix(h, *(volatile uint32_t *)(volatile void *)p);
    h = mix(h, *(volatile uint64_t *)(volatile void *)p);
    uint64_t v = rnd();
    switch (v & 3) {
    case 0: *(volatile uint8_t *)p = (uint8_t)v; break;
    case 1: *(volatile uint16_t *)(volatile void *)p = (uint16_t)v; break;
    case 2: *(volatile uint32_t *)(volatile void *)p = (uint32_t)v; break;
    default: *(volatile uint64_t *)(volatile void *)p = v; break;
    }
  }
  for (int i = 0; i < 4096; i++) h = mix(h, mem[i]);
  return h;
}

NOINLINE static uint64_t g_double(void) {
  uint64_t h = 0;
  for (int i = 0; i < N; i++) {
    volatile double a = dval(), b = dval(), c = dval();
    h = mix(h, dbits(a + b)); h = mix(h, dbits(a - b)); h = mix(h, dbits(a * b));
    if (b != 0) h = mix(h, dbits(a / b));
    if (a >= 0) h = mix(h, dbits(__builtin_sqrt(a)));
    h = mix(h, dbits(__builtin_fma(a, b, c)));
    h = mix(h, dbits(__builtin_fma(a, b, -c)));
    h = mix(h, dbits(__builtin_fmin(a, b))); h = mix(h, dbits(__builtin_fmax(a, b)));
    h = mix(h, dbits(__builtin_copysign(a, b))); h = mix(h, dbits(__builtin_fabs(a))); h = mix(h, dbits(-a));
    h = mix(h, a < b); h = mix(h, a <= b); h = mix(h, a == b);
    if (a > -9.2e18 && a < 9.2e18) { h = mix(h, (uint64_t)(int64_t)a); }
    if (a > -2.1e9 && a < 2.1e9) { h = mix(h, (uint64_t)(int64_t)(int32_t)a); }
    if (a >= 0 && a < 1.8e19) h = mix(h, (uint64_t)a);
    if (a >= 0 && a < 4.2e9) h = mix(h, (uint32_t)a);
    int64_t iv = (int64_t)interesting();
    h = mix(h, dbits((double)iv)); h = mix(h, dbits((double)(uint64_t)iv));
    h = mix(h, dbits((double)(int32_t)iv)); h = mix(h, dbits((double)(uint32_t)iv));
    float f = (float)a;
    h = mix(h, fbits(f)); h = mix(h, dbits((double)f));
    h = mix(h, (uint64_t)__builtin_isnan(a + b * 1e308) + 2 * (uint64_t)__builtin_isinf(a * 1e308));
  }
  return h;
}

NOINLINE static uint64_t g_float(void) {
  uint64_t h = 0;
  for (int i = 0; i < N; i++) {
    volatile float a = (float)dval(), b = (float)dval(), c = (float)dval();
    h = mix(h, fbits(a + b)); h = mix(h, fbits(a - b)); h = mix(h, fbits(a * b));
    if (b != 0) h = mix(h, fbits(a / b));
    if (a >= 0) h = mix(h, fbits(__builtin_sqrtf(a)));
    h = mix(h, fbits(__builtin_fmaf(a, b, c)));
    h = mix(h, fbits(__builtin_fminf(a, b))); h = mix(h, fbits(__builtin_fmaxf(a, b)));
    h = mix(h, fbits(__builtin_copysignf(a, b))); h = mix(h, a < b); h = mix(h, a == b);
    if (a > -2.1e9f && a < 2.1e9f) h = mix(h, (uint64_t)(int64_t)(int32_t)a);
    if (a > -9.2e18f && a < 9.2e18f) h = mix(h, (uint64_t)(int64_t)a);
    int64_t iv = (int64_t)interesting();
    h = mix(h, fbits((float)iv)); h = mix(h, fbits((float)(uint64_t)iv)); h = mix(h, fbits((float)(int32_t)iv));
  }
  return h;
}

NOINLINE static uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int cmpu(const void *x, const void *y) { uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y; return a < b ? -1 : a > b; }
NOINLINE static uint64_t g_control(void) {
  uint64_t h = fib(24);
  static uint64_t arr[2000];
  for (int i = 0; i < 2000; i++) arr[i] = rnd() % 100000;
  /* insertion sort through a function pointer */
  int (*volatile cmp)(const void *, const void *) = cmpu;
  for (int i = 1; i < 2000; i++) {
    uint64_t v = arr[i]; int j = i - 1;
    while (j >= 0 && cmp(&arr[j], &v) > 0) { arr[j + 1] = arr[j]; j--; }
    arr[j + 1] = v;
  }
  for (int i = 0; i < 2000; i++) h = mix(h, arr[i]);
  switch (h & 7) { case 0: h += 1; break; case 1: h += 7; break; case 2: h ^= 3; break; case 5: h *= 3; break; default: h -= 9; }
  return h;
}

NOINLINE static uint64_t g_atomic(void) {
  static volatile uint64_t v64; static volatile uint32_t v32;
  uint64_t h = 0;
  for (int i = 0; i < 1000; i++) {
    uint64_t r = interesting();
    h = mix(h, __atomic_fetch_add(&v64, r, __ATOMIC_SEQ_CST));
    h = mix(h, __atomic_fetch_xor(&v64, r >> 3, __ATOMIC_SEQ_CST));
    h = mix(h, __atomic_fetch_and(&v32, (uint32_t)r | 0xff00ff00u, __ATOMIC_SEQ_CST));
    h = mix(h, __atomic_fetch_or(&v32, (uint32_t)(r >> 7), __ATOMIC_SEQ_CST));
    h = mix(h, __atomic_exchange_n(&v32, (uint32_t)r, __ATOMIC_SEQ_CST));
    uint64_t exp = v64;
    h = mix(h, __atomic_compare_exchange_n(&v64, &exp, r, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    uint32_t e32 = 12345;
    h = mix(h, __atomic_compare_exchange_n(&v32, &e32, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    int64_t sv = (int64_t)r;
    volatile int64_t m = (int64_t)v64;
    h = mix(h, (uint64_t)(sv < m ? sv : m));
  }
  return h ^ v64 ^ v32;
}

int main(void) {
  rt_puts("int64   "); rt_hex(g_int64()); rt_puts("\n");
  rt_puts("int32   "); rt_hex(g_int32()); rt_puts("\n");
  rt_puts("memory  "); rt_hex(g_mem()); rt_puts("\n");
  rt_puts("double  "); rt_hex(g_double()); rt_puts("\n");
  rt_puts("float   "); rt_hex(g_float()); rt_puts("\n");
  rt_puts("control "); rt_hex(g_control()); rt_puts("\n");
  rt_puts("atomic  "); rt_hex(g_atomic()); rt_puts("\n");
  return 0;
}
