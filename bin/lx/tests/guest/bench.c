// SPDX-License-Identifier: GPL-2.0-or-later
/* Interpreter speed kernels. Built for riscv64 (run by the engine) and natively (the
 * reference). Each kernel runs in a noinline function; the program prints the best of
 * REPS runs in milliseconds and a checksum (which must match between builds).
 * Usage: bench [kernel] [reps]; kernels: sieve sort sha256 float fib all. */
#include "rt.h"

#if defined(__riscv)
struct ts { long sec, nsec; };
static uint64_t now_ns(void) { struct ts t; rt_sys(113, 1, (long)&t, 0, 0, 0, 0); return (uint64_t)t.sec * 1000000000ull + (uint64_t)t.nsec; }
static void *rt_mmap(size_t n) { return (void *)rt_sys(222, 0, (long)n, 3, 0x22, -1, 0); }
#else
#include <time.h>
#include <sys/mman.h>
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void *rt_mmap(size_t n) { return mmap(0, n, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); }
#endif
#define NOINLINE __attribute__((noinline))

NOINLINE static uint64_t k_sieve(void) {
  enum { N = 2000000 };
  static uint8_t comp[N + 1];
  for (int i = 0; i <= N; i++) comp[i] = 0;
  uint64_t c = 0;
  for (int i = 2; i <= N; i++) {
    if (comp[i]) continue;
    c++;
    for (int j = 2 * i; j <= N; j += i) comp[j] = 1;
  }
  return c;
}

static uint32_t sortbuf[300000];
static void qs(uint32_t *a, int lo, int hi) {
  while (lo < hi) {
    uint32_t p = a[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j) {
      while (a[i] < p) i++;
      while (a[j] > p) j--;
      if (i <= j) { uint32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
    }
    if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
  }
}
NOINLINE static uint64_t k_sort(void) {
  uint32_t x = 12345;
  for (int i = 0; i < 300000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; sortbuf[i] = x; }
  qs(sortbuf, 0, 299999);
  uint64_t h = 0;
  for (int i = 0; i < 300000; i += 1000) h = h * 31 + sortbuf[i];
  return h;
}

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static uint8_t *shabuf;
#define SHA_MB 16
NOINLINE static uint64_t k_sha256(void) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (size_t off = 0; off < (size_t)SHA_MB << 20; off += 64) sha_block(h, shabuf + off);
  return (uint64_t)h[0] << 32 | h[7];
}

NOINLINE static uint64_t k_float(void) {
  /* Mandelbrot 300x200, 256 iterations max */
  uint64_t n = 0;
  for (int y = 0; y < 200; y++)
    for (int x = 0; x < 300; x++) {
      double cr = -2.0 + x * (3.0 / 300), ci = -1.0 + y * (2.0 / 200), zr = 0, zi = 0;
      int i = 0;
      while (i < 256 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; i++; }
      n += (uint64_t)i;
    }
  return n;
}

NOINLINE static uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
NOINLINE static uint64_t k_fib(void) { return fib(27); }

static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void run(const char *name, uint64_t (*k)(void), int reps) {
  uint64_t best = ~0ull, first = 0, chk = 0;
  for (int r = 0; r < reps; r++) {
    uint64_t t0 = now_ns();
    chk = k();
    uint64_t t = now_ns() - t0;
    if (r == 0) first = t;
    if (t < best) best = t;
  }
  rt_puts(name); rt_puts(" best_us "); rt_dec((int64_t)(best / 1000)); rt_puts(" first_us "); rt_dec((int64_t)(first / 1000));
  rt_puts(" check "); rt_hex(chk); rt_puts("\n");
}

#if defined(__riscv)
int main(void) { int argc = rt_argc; char **argv = rt_argv;
#else
int main(int argc, char **argv) {
#endif
  const char *which = argc > 1 ? argv[1] : "all";
  int reps = argc > 2 ? (int)(argv[2][0] - '0') : 3;
  shabuf = rt_mmap((size_t)SHA_MB << 20);
  for (size_t i = 0; i < (size_t)SHA_MB << 20; i++) shabuf[i] = (uint8_t)(i * 2654435761u >> 13);
  if (streq(which, "all") || streq(which, "sieve")) run("sieve", k_sieve, reps);
  if (streq(which, "all") || streq(which, "sort")) run("sort", k_sort, reps);
  if (streq(which, "all") || streq(which, "sha256")) run("sha256", k_sha256, reps);
  if (streq(which, "all") || streq(which, "float")) run("float", k_float, reps);
  if (streq(which, "all") || streq(which, "fib")) run("fib", k_fib, reps);
  return 0;
}
