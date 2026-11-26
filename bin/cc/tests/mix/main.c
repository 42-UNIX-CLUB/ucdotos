// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <string.h>
#include "abi.h"
int main_cb(int v, struct i2 s) { return v - s.a - s.b; }
static int vs(int n, ...) { va_list ap; va_start(ap, n); int r = lib_vsum(n, ap); va_end(ap); return r; }
int main(void) {
  struct f1 a = lib_f1((struct f1){1.5f}, 3);
  struct d1 b = lib_d1((struct d1){2.5});
  struct i2 c = lib_i2((struct i2){1, 2}, 10);
  struct c3 d = lib_c3((struct c3){'a', 'b', 'c'});
  struct big e = lib_big((struct big){1, 2, 3, 4}, 1000);
  printf("%g %g %d %d %c%c%c %lld %lld\n", a.f, b.d, c.a, c.b, d.a, d.b, d.c, e.a, e.d);
  struct bits bf = {1, 2, 3, 4};
  bf = lib_bits(bf);
  printf("%u %u %d %llu\n", bf.a, bf.b, bf.c, (unsigned long long)bf.d);
  union u un = lib_union((union u){.f = 2.0f});
  struct e em;
  printf("%g %d\n", un.f, lib_empty(em, 5));
  printf("%lld\n", lib_many(1, 2, 3, 4, 5.5f, 6.5, 7, (struct i2){8, 9}, 10));
  printf("%d %d\n", lib_sum(4, 1, 2, 3, 4), vs(3, 10, 20, 30));
  struct i2 s = {3, 4};
  struct big bg = {1, 0, 0, 2};
  printf("%g\n", lib_vmix("ildsb", 1, 2LL, 3.5, s, bg));
  long double ld = lib_ld(2.5L);
  unsigned long long w[2];
  memcpy(w, &ld, 16);
  printf("%llx %llx\n", w[1], w[0]);
  printf("%d %d %d\n", lib_apply(lib_cbs[0], 5), lib_apply(lib_cbs[1], 5), lib_apply(main_cb, 1000));
  printf("%d %d %s\n", lib_global.a, lib_global.b, lib_names[2]);
  callback mine = main_cb;
  printf("%d\n", mine == main_cb);
  return 0;
}
