// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdarg.h>
struct pair { int a, b; };
static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++) s += va_arg(ap, int);
  va_end(ap);
  return s;
}
static double mixed(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  double t = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p == 'i') t += va_arg(ap, int);
    else if (*p == 'd') t += va_arg(ap, double);
    else if (*p == 'l') t += (double)va_arg(ap, long long);
    else if (*p == 's') { struct pair q = va_arg(ap, struct pair); t += q.a * 100 + q.b; }
  }
  va_end(ap);
  return t;
}
static void myprintf(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  vprintf(fmt, ap);
  char buf[64];
  vsnprintf(buf, sizeof buf, fmt, ap2);
  printf("[%s]", buf);
  va_end(ap);
  va_end(ap2);
}
int main(void) {
  printf("%d %d\n", sum(3, 1, 2, 3), sum(0));
  struct pair p = {3, 4};
  printf("%g\n", mixed("idlsi", 1, 2.5, 10000000000LL, p, 7));
  myprintf("%d-%s-%c-%.2f\n", 42, "x", 'y', 3.14159);
  printf("%s %d %f %lld %c %x\n", "multi", -1, 1.0 / 3, 1LL << 50, 'z', 255u);
  return 0;
}
