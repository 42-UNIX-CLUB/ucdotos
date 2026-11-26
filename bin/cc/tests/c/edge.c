// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stddef.h>
#include <string.h>
struct pk { char c; int i; } __attribute__((packed));
struct al { char c; int i __attribute__((aligned(16))); };
_Alignas(32) static char aligned_buf[10];
typedef struct node { struct node *self; int v; } node;
static node gn = {&gn, 5};
static int arr[] = {1, 2, 3};
static int *end = arr + sizeof arr / sizeof *arr;
static const char *const tbl[] = {[2] = "two", [0] = "zero"};
struct incomplete;
static struct incomplete *ip;
static int (*fparr[3])(void);
static int ret7(void) { return 7; }
static int ternary_chain(int x) { return x < 0 ? -1 : x == 0 ? 0 : x < 10 ? 1 : 2; }
static unsigned long long shifts(unsigned long long v, int s) { return (v << s) | (v >> (64 - s)); }
int main(void) {
  printf("%d %d %d\n", (int)sizeof(struct pk), (int)offsetof(struct al, i), (int)sizeof(struct al));
  printf("%d\n", (int)((size_t)aligned_buf % 32));
  printf("%d %d\n", gn.self->self->v, (int)(end - arr));
  printf("%s %p\n", tbl[2], (void *)tbl[1]);
  printf("%d\n", ip == 0);
  fparr[1] = ret7;
  printf("%d\n", fparr[1]());
  for (int i = -2; i < 13; i += 4) printf("%d ", ternary_chain(i));
  printf("\n%llx\n", shifts(0x8000000000000001ULL, 4));
  int x = 5, *px = &x;
  printf("%d\n", px[0] + 0[px]);
  char s[10] = "abc";
  char *q = s;
  *q++ = 'X';
  *++q = 'Y';
  printf("%s\n", s);
  int i = 0;
  int a2[5] = {0};
  a2[i++] = 4;
  printf("%d %d\n", a2[0], i);
  unsigned u = 0;
  u--;
  printf("%u %d\n", u, u > 0);
  long long ll = 0x7fffffffffffffffLL;
  printf("%lld\n", ll + (ll > 0 ? -ll : 0));
  struct { int a : 4; int b : 4; } nib = {-8, 7};
  nib.a += 1;
  printf("%d %d\n", nib.a, nib.b);
  int m[2][3][4];
  memset(m, 0, sizeof m);
  m[1][2][3] = 9;
  printf("%d %d\n", m[1][2][3], (int)(sizeof m / sizeof m[0][0]));
  int (*pm)[3][4] = m;
  printf("%d\n", pm[1][2][3]);
  const char *w = "x" "y" "z";
  printf("%s %c\n", w, w[sizeof("xy") - 1]);
  double dd[] = {1, 2.5, -3};
  double *pd = dd;
  printf("%g ", *pd++);
  printf("%g\n", *pd);
  return 0;
}
