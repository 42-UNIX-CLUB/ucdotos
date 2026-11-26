// SPDX-License-Identifier: GPL-2.0-or-later
#include "abi.h"
struct f1 lib_f1(struct f1 x, float k) { x.f *= k; return x; }
struct d1 lib_d1(struct d1 x) { x.d = -x.d; return x; }
struct i2 lib_i2(struct i2 x, int k) { struct i2 r = {x.b * k, x.a * k}; return r; }
struct c3 lib_c3(struct c3 x) { struct c3 r = {x.c, x.b, x.a}; return r; }
struct big lib_big(struct big x, long long k) { x.a += k; x.d -= k; return x; }
struct bits lib_bits(struct bits x) { x.a++; x.b += 2; x.c = -x.c; x.d <<= 1; return x; }
union u lib_union(union u x) { x.f += 1.0f; return x; }
int lib_empty(struct e x, int y) { (void)x; return y * 3; }
long long lib_many(char a, short b, int c, long long d, float e, double f, unsigned char g, struct i2 h, int i) {
  return a + b + c + d + (long long)e + (long long)f + g + h.a + h.b + i;
}
int lib_sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = lib_vsum(n, ap);
  va_end(ap);
  return s;
}
int lib_vsum(int n, va_list ap) { int s = 0; while (n--) s += va_arg(ap, int); return s; }
double lib_vmix(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  double t = 0;
  for (; *fmt; fmt++) {
    if (*fmt == 'i') t += va_arg(ap, int);
    else if (*fmt == 'l') t += va_arg(ap, long long);
    else if (*fmt == 'd') t += va_arg(ap, double);
    else if (*fmt == 's') { struct i2 s = va_arg(ap, struct i2); t += s.a * 10 + s.b; }
    else if (*fmt == 'b') { struct big b = va_arg(ap, struct big); t += b.a + b.d; }
  }
  va_end(ap);
  return t;
}
long double lib_ld(long double x) { return x; }
static int twice(int v, struct i2 s) { return 2 * v + s.a; }
static int thrice(int v, struct i2 s) { return 3 * v + s.b; }
callback lib_cbs[2] = {twice, thrice};
int lib_apply(callback cb, int v) { struct i2 s = {100, 200}; return cb(v, s); }
struct i2 lib_global = {7, 8};
const char *lib_names[3] = {"alpha", "beta", "gamma"};
