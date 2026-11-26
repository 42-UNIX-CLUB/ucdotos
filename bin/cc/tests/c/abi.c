// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <string.h>
struct f1 { float f; };
struct d1 { struct { double d; } in; };
struct a1 { int a[1]; };
struct ll1 { long long v; };
struct c3 { char a, b, c; };
struct mix { char c; double d; int i; };
struct e { };
union u2 { int i; float f; };
struct big { long long a, b, c, d; };
struct ptr1 { const char *s; };
static struct f1 pf1(struct f1 x) { x.f *= 2; return x; }
static struct d1 pd1(struct d1 x) { x.in.d += 1; return x; }
static struct a1 pa1(struct a1 x) { x.a[0]++; return x; }
static struct ll1 pll1(struct ll1 x) { x.v <<= 1; return x; }
static struct c3 pc3(struct c3 x) { x.a++; x.c--; return x; }
static struct mix pmix(struct mix m, int k) { m.i += k; m.d *= k; return m; }
static int pe(struct e x, int y) { (void)x; return y + 1; }
static union u2 pu2(union u2 x) { x.i ^= 0x80000000; return x; }
static struct big pbig(struct big b) { struct big r = {b.d, b.c, b.b, b.a}; return r; }
static struct ptr1 pptr(struct ptr1 p) { p.s++; return p; }
static long long many(int a, long long b, double c, float d, char e, short f, unsigned g, long long h, int i, double j) {
  return a + b + (long long)c + (long long)d + e + f + g + h + i + (long long)j;
}
struct node { int v; struct node *next; };
static int walk(struct node *n) { int s = 0; for (; n; n = n->next) s += n->v; return s; }
static struct big global_big;
static struct big *getbig(void) { return &global_big; }
int main(void) {
  struct f1 a = pf1((struct f1){1.5f});
  struct d1 b = pd1((struct d1){{2.25}});
  struct a1 c = pa1((struct a1){{41}});
  struct ll1 d = pll1((struct ll1){1LL << 40});
  struct c3 e = pc3((struct c3){'a', 'b', 'c'});
  struct mix m = pmix((struct mix){'x', 1.5, 10}, 3);
  struct e empty;
  printf("%g %g %d %lld %c%c%c %c %g %d %d\n", a.f, b.in.d, c.a[0], d.v, e.a, e.b, e.c, m.c, m.d, m.i, pe(empty, 4));
  union u2 u = pu2((union u2){.f = 1.0f});
  printf("%x\n", u.i);
  struct big bb = pbig((struct big){1, 2, 3, 4});
  printf("%lld %lld %lld %lld\n", bb.a, bb.b, bb.c, bb.d);
  printf("%s\n", pptr((struct ptr1){"xhello"}).s);
  printf("%lld\n", many(1, 2, 3.5, 4.5f, 5, 6, 7, 8, 9, 10.5));
  struct node n3 = {3, 0}, n2 = {2, &n3}, n1 = {1, &n2};
  printf("%d\n", walk(&n1));
  *getbig() = bb;
  getbig()->a = 100;
  printf("%lld %lld\n", global_big.a, getbig()->d);
  struct mix arr[3];
  memset(arr, 0, sizeof arr);
  arr[1] = m;
  struct mix *pm = &arr[1];
  printf("%d %g %d\n", (int)sizeof arr, pm->d, (pm + 1)->i);
  struct big t = pbig(pbig(bb));
  printf("%lld\n", t.a);
  return 0;
}
