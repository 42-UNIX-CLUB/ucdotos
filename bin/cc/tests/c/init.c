// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <string.h>
struct s { int a; char b[4]; double c; int *p; };
int x = 42;
struct s gs = {.c = 2.5, .a = 1, .b = "ab", .p = &x};
int garr[10] = {[3] = 3, 4, [8] = 8};
char gstr[10] = "hi";
int gmat[2][3] = {1, 2, 3, 4, 5};
struct s garr2[] = {{1, "x"}, {2, "y", 3.5}, [4] = {.a = 9}};
const char *strs[] = {"a", "bb", "ccc"};
static int *ptrs[] = {&garr[1], &garr[3], 0};
long long ll = -1;
float fl = 1.5f;
unsigned char bytes[] = {1, 2, 255};
int computed = sizeof(struct s) * 2 + 1;
char *strp = "text" + 1;
struct { int n; struct { short h; char c; } in[2]; } nest = {7, {{1, 'a'}, {2, 'b'}}};
union u { int i; char c[4]; } gu = {.c = {1, 2, 3, 4}};
int main(void) {
  printf("%d %s %g %d\n", gs.a, gs.b, gs.c, *gs.p);
  for (int i = 0; i < 10; i++) printf("%d", garr[i]);
  printf(" %s %d\n", gstr, (int)sizeof garr2 / (int)sizeof garr2[0]);
  printf("%d %d %d\n", gmat[1][0], gmat[1][1], gmat[1][2]);
  printf("%d %s %g %d\n", garr2[1].a, garr2[1].b, garr2[1].c, garr2[4].a);
  printf("%s %s %d %d\n", strs[2], strp, *ptrs[1], ptrs[2] == 0);
  printf("%lld %g %d %d\n", ll, fl, bytes[2], computed);
  printf("%d %d %c %x\n", nest.n, nest.in[1].h, nest.in[1].c, gu.i);
  struct s ls = {.b = {'q'}, .a = 5};
  printf("%d %s %g %p\n", ls.a, ls.b, ls.c, (void *)ls.p);
  int la[5] = {1, 2};
  printf("%d %d %d\n", la[1], la[2], la[4]);
  char lstr[] = "local";
  char lbuf[8] = "ab";
  printf("%s %d %s %d\n", lstr, (int)sizeof lstr, lbuf, lbuf[7]);
  int lm[2][2] = {{1}, {3, 4}};
  printf("%d %d %d\n", lm[0][1], lm[1][0], lm[1][1]);
  int *cl = (int[]){10, 20, 30};
  printf("%d\n", cl[2]);
  struct s *ps = &(struct s){3, "cl"};
  printf("%d %s\n", ps->a, ps->b);
  int scalar = {5};
  printf("%d\n", scalar);
  static int sl = 77;
  static const char *slp[] = {"s1", "s2"};
  printf("%d %s\n", sl, slp[1]);
  wchar_t w[] = L"wide";
  printf("%d %d\n", (int)(sizeof w / sizeof w[0]), w[1]);
  return 0;
}
