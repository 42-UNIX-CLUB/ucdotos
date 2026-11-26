// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
volatile int vi = -1;
volatile unsigned vu = 0xffffffffu;
volatile long long vl = -1;
volatile double vd = -1.5;
volatile float vf = 3.75f;
int main(void) {
  signed char sc = (signed char)vi; unsigned char uc = (unsigned char)vi;
  short ss = (short)vu; unsigned short us = (unsigned short)vu;
  printf("%d %d %d %d\n", sc, uc, ss, us);
  long long a = vi, b = vu, c = (unsigned)vi;
  unsigned long long d = vi;
  printf("%lld %lld %lld %llu\n", a, b, c, d);
  int e = (int)vl; unsigned f = (unsigned)vl; char g = (char)vl;
  printf("%d %u %d\n", e, f, g);
  printf("%d %u %lld %d\n", (int)vd, (unsigned)-vd, (long long)vd, (char)vd);
  printf("%g %g %g\n", (double)vi, (double)vu, (double)vl);
  printf("%g %g %g\n", (float)vi, (float)vu, (double)(float)vd);
  printf("%d %d\n", (int)vf, (unsigned char)vf);
  _Bool b1 = vd, b2 = 0.0, b3 = vl, b4 = (void *)0;
  printf("%d %d %d %d\n", b1, b2, b3, b4);
  unsigned char x = 250;
  x += 10;
  printf("%d\n", x);
  short y = 32767;
  y++;
  printf("%d\n", y);
  printf("%d %d\n", (unsigned char)-1 > 0, (signed char)128);
  printf("%d\n", 3u > -1);
  printf("%ld\n", 3L - 5UL > 0 ? 1L : 0L);
  long long big = 0x123456789LL;
  int trunc = big;
  printf("%x\n", trunc);
  double dd = 1e10;
  printf("%lld\n", (long long)dd);
  float ff = 0.1f;
  double fd = ff;
  printf("%.10f\n", fd);
  printf("%d\n", (int)3.99 + (int)-3.99);
  unsigned long long ull = 1ULL << 63;
  printf("%g\n", (double)ull);
  int neg = -7;
  printf("%d %d %u\n", neg >> 1, neg / 2, (unsigned)neg >> 1);
  return 0;
}
