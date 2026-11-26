// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <string.h>
int arr[10];
int *gp = &arr[3];
char *msg = "hello";
char buf[] = "world";
const char *names[] = {"zero", "one", "two"};
int matrix[3][4];
void fill(int *p, int n) { for (int i = 0; i < n; i++) p[i] = i * i; }
int sum(int (*m)[4], int rows) { int s = 0; for (int i = 0; i < rows; i++) for (int j = 0; j < 4; j++) s += m[i][j]; return s; }
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int main(void) {
  fill(arr, 10);
  printf("%d %d %d\n", arr[3], *gp, gp[2]);
  printf("%d %d\n", (int)(gp - arr), (int)(&arr[9] - gp));
  int *p = arr;
  p += 4; printf("%d ", *p); p--; printf("%d ", *p); printf("%d\n", *(p + 2));
  printf("%s %s %s %c\n", msg, buf, names[2], msg[1]);
  for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) matrix[i][j] = i + j;
  printf("%d %d\n", sum(matrix, 3), matrix[2][3]);
  int a = 1, b = 2;
  swap(&a, &b);
  printf("%d %d\n", a, b);
  int x = 5; int *px = &x; int **ppx = &px;
  **ppx = 9;
  printf("%d\n", x);
  char s[20];
  strcpy(s, "abc");
  strcat(s, "def");
  printf("%s %d\n", s, (int)strlen(s));
  void *v = s;
  char *c = v;
  printf("%c\n", c[3]);
  printf("%d\n", p == &arr[3]);
  printf("%d %d\n", (int)sizeof arr, (int)sizeof matrix[0]);
  const char *q = msg;
  while (*q) q++;
  printf("%d\n", (int)(q - msg));
  int *null = 0;
  printf("%d\n", null == NULL);
  return 0;
}
