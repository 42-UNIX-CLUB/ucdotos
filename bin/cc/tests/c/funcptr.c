// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdlib.h>
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
typedef int (*binop)(int, int);
static binop ops[] = {add, mul};
struct table { const char *name; binop f; } tab[] = {{"add", add}, {"mul", mul}};
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int apply(int (*f)(int, int), int x, int y) { return f(x, y); }
static binop pick(int i) { return i ? mul : add; }
static void hello(void) { printf("hello\n"); }
int main(void) {
  printf("%d %d\n", ops[0](3, 4), ops[1](3, 4));
  printf("%s %d\n", tab[1].name, tab[1].f(5, 6));
  printf("%d %d\n", apply(add, 1, 2), pick(1)(7, 8));
  int v[] = {5, 3, 9, 1, 7};
  qsort(v, 5, sizeof v[0], cmp);
  for (int i = 0; i < 5; i++) printf("%d ", v[i]);
  printf("\n");
  void (*h)(void) = hello;
  h();
  (*h)();
  binop f = &add;
  printf("%d %d\n", f == add, (*f)(10, 20));
  return 0;
}
