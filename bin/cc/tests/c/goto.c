// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
static int find(int *a, int n, int key) {
  int i;
  for (i = 0; i < n; i++)
    if (a[i] == key) goto found;
  return -1;
found:
  return i;
}
static int loopy(int n) {
  int s = 0, i = 0;
top:
  if (i >= n) goto done;
  s += i;
  i++;
  goto top;
done:
  return s;
}
static void duff(char *to, const char *from, int count) {
  int n = (count + 7) / 8;
  switch (count % 8) {
  case 0: do { *to++ = *from++;
  case 7: *to++ = *from++;
  case 6: *to++ = *from++;
  case 5: *to++ = *from++;
  case 4: *to++ = *from++;
  case 3: *to++ = *from++;
  case 2: *to++ = *from++;
  case 1: *to++ = *from++;
          } while (--n > 0);
  }
}
static int nested(int x) {
  int r = 0;
  while (1) {
    for (int j = 0; j < 3; j++) {
      if (x-- <= 0) goto out;
      r += j;
      if (j == 1) continue;
    }
    if (r > 100) break;
  }
out:
  return r;
}
static int into_loop(int k) {
  int s = 0, i = 2;
  if (k) goto inside;
  for (i = 0; i < 4; i++) {
    s += 10;
  inside:
    s += 1;
  }
  return s;
}
int main(void) {
  int a[] = {4, 8, 15, 16, 23, 42};
  printf("%d %d\n", find(a, 6, 23), find(a, 6, 5));
  printf("%d\n", loopy(10));
  char dst[32] = {0};
  duff(dst, "abcdefghijklmnopqrstu", 21);
  printf("%s\n", dst);
  printf("%d %d\n", nested(7), nested(1));
  printf("%d %d\n", into_loop(0), into_loop(1));
  return 0;
}
