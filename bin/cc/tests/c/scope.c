// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
int x = 1;
static int y = 2;
int tentative;
int tentative;
extern int later;
int later = 5;
static int counter(void);
typedef int T;
struct S { int s; };
int main(void) {
  printf("%d %d %d %d\n", x, y, tentative, later);
  int x = 10;
  {
    int x = 100;
    printf("%d\n", x);
    {
      extern int x;
      printf("%d\n", x);
    }
  }
  printf("%d\n", x);
  T T = 3;
  printf("%d\n", T);
  struct S { int a, b; } s = {7, 8};
  printf("%d %d\n", s.a, s.b);
  for (int x = 0; x < 2; x++) { int x = 50; printf("%d ", x); }
  printf("\n");
  counter(); counter();
  printf("%d\n", counter());
  enum { A = 3 };
  { enum { A = 4 }; printf("%d ", A); }
  printf("%d\n", A);
  return 0;
}
static int counter(void) { static int n = 10; return n++; }
