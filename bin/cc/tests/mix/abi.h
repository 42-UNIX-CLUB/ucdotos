// SPDX-License-Identifier: GPL-2.0-or-later
/* Interfaces compiled by one compiler and called from code built by the other. */
#include <stdarg.h>
struct f1 { float f; };
struct d1 { double d; };
struct i2 { int a, b; };
struct c3 { char a, b, c; };
struct big { long long a, b, c, d; };
struct bits { unsigned a : 3, b : 5; int c : 7; unsigned long long d : 33; };
union u { int i; float f; };
struct e { };
struct f1 lib_f1(struct f1 x, float k);
struct d1 lib_d1(struct d1 x);
struct i2 lib_i2(struct i2 x, int k);
struct c3 lib_c3(struct c3 x);
struct big lib_big(struct big x, long long k);
struct bits lib_bits(struct bits x);
union u lib_union(union u x);
int lib_empty(struct e x, int y);
long long lib_many(char a, short b, int c, long long d, float e, double f, unsigned char g, struct i2 h, int i);
int lib_sum(int n, ...);
double lib_vmix(const char *fmt, ...);
int lib_vsum(int n, va_list ap);
long double lib_ld(long double x);
typedef int (*callback)(int, struct i2);
int lib_apply(callback cb, int v);
extern callback lib_cbs[2];
extern struct i2 lib_global;
extern const char *lib_names[3];
int main_cb(int v, struct i2 s);
