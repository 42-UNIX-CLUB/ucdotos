// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <math.h>
double half(double x) { return x / 2; }
float fsq(float x) { return x * x; }
int main(void) {
  double d = 3.75;
  float f = 1.5f;
  printf("%.3f %.3f %.3f %.3f\n", d + f, d - f, d * f, d / f);
  printf("%d %d %u %lld\n", (int)d, (int)-d, (unsigned)d, (long long)(d * 1e12));
  printf("%.2f %.2f\n", (double)7, (float)-3);
  printf("%g %g\n", half(5), fsq(3.0f));
  printf("%d %d %d %d\n", d > f, d < f, d == 3.75, f != 1.5);
  printf("%.4f %.4f %.4f\n", sqrt(2.0), floor(-2.5), ceil(2.1));
  printf("%.4f %.4f\n", fabs(-3.25), sin(0.5));
  unsigned long long big = 18000000000000000000ULL;
  printf("%.1f\n", (double)big);
  double back = 1.8e19;
  printf("%llu\n", (unsigned long long)back);
  printf("%d\n", isnan(0.0 / 0.0) && !isnan(1.0));
  printf("%d %d\n", isinf(1e308 * 10), signbit(-0.0));
  float g = 16777217.0f;
  printf("%.1f\n", g);
  double x = 0.1 + 0.2;
  printf("%.17g\n", x);
  int i = 10;
  i *= 2.5;
  printf("%d\n", i);
  d += 1;
  d *= 2;
  printf("%g\n", d);
  printf("%g %g\n", -d, +d);
  float arr[3] = {1.25f, 2.5f, 3.75f};
  float sum = 0;
  for (int k = 0; k < 3; k++) sum += arr[k];
  printf("%g\n", sum);
  printf("%.3e\n", 12345.678);
  return 0;
}
