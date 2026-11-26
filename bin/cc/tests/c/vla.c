// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <alloca.h>
#include <string.h>
static int sum(int n) {
  int a[n];
  for (int i = 0; i < n; i++) a[i] = i * 3;
  int s = 0;
  for (int i = 0; i < n; i++) s += a[i];
  return s + (int)sizeof a;
}
static int deep(int n) {
  char *p = alloca(n + 1);
  memset(p, 'x', n);
  p[n] = 0;
  return n ? (int)strlen(p) + deep(n - 1) : 0;
}
int main(void) {
  printf("%d %d %d\n", sum(1), sum(10), sum(100));
  printf("%d\n", deep(20));
  for (int k = 1; k < 4; k++) {
    double v[k * 2];
    for (int i = 0; i < k * 2; i++) v[i] = i / 2.0;
    printf("%g ", v[k * 2 - 1]);
  }
  printf("\n");
  return 0;
}
