// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <limits.h>
static int ident(int x) { return x; }
int main(void) {
  int a = 7, b = -3;
  unsigned u = 7, v = 3;
  printf("%d %d %d %d %d\n", a + b, a - b, a * b, a / b, a % b);
  printf("%u %u %u %u\n", u / v, u % v, u - 10, u * 1000000000u);
  printf("%d %d\n", -7 / 2, -7 % 2);
  printf("%d %d %d\n", a << 3, b >> 1, (int)((unsigned)b >> 28));
  printf("%d %d %d %d\n", a & 5, a | 8, a ^ 2, ~a);
  printf("%d %d %d\n", a < b, (unsigned)a < (unsigned)b, -1 < 0u);
  long long x = 1234567890123LL, y = -987654321;
  printf("%lld %lld %lld %lld %lld\n", x + y, x - y, x * 3, x / y, x % y);
  unsigned long long z = 0xffffffffffffffffULL;
  printf("%llu %llu %llx\n", z, z / 7, z >> 4);
  printf("%lld %lld\n", x << 7, (-x) >> 3);
  char c = 200; unsigned char uc = 200; signed char sc = -5;
  printf("%d %d %d\n", c, uc, sc);
  short s = 40000; unsigned short us = 70000;
  printf("%d %d\n", s, us);
  printf("%d\n", INT_MAX + ident(0) == INT_MAX);
  int i = 0;
  i += 5; i -= 2; i *= 7; i /= 3; i %= 4; i <<= 3; i >>= 1; i |= 1; i &= 13; i ^= 6;
  printf("%d\n", i);
  i++; ++i; i--;
  printf("%d\n", i);
  unsigned char k = 255; k++; printf("%d\n", k);
  printf("%d %d\n", (char)0x1ff, (short)0x18000);
  printf("%ld %lu\n", (long)-5, (unsigned long)-5);
  printf("%d\n", sizeof(long) + sizeof(long long) * 10 + sizeof(void *) * 100);
  int m = INT_MIN;
  printf("%d %u\n", m, (unsigned)m);
  printf("%d %d %d\n", 5 > 3 ? 10 : 20, !5, !0);
  printf("%d\n", (1, 2, 3));
  long long big = 1LL << 40; int sh = 3;
  printf("%lld %lld\n", big >> sh, big << sh);
  return 0;
}
