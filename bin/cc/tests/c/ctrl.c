// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
static const char *kind(int c) {
  switch (c) {
  case 0: return "zero";
  case 1: case 2: return "small";
  case 3: case 4: case 5: case 6: return "medium";
  case 100: return "hundred";
  case -5: return "neg";
  default: return "other";
  }
}
static int dense(int c) {
  int r = 0;
  switch (c) {
  case 1: r += 1;
  case 2: r += 2; break;
  case 3: r += 3;
  case 4: r += 4;
  case 5: r += 5; break;
  case 6: r = 60; break;
  case 8: r = 80;
  }
  return r;
}
static int sparse(long long v) {
  switch (v) {
  case 1000000000000LL: return 1;
  case -1: return 2;
  case 7: return 3;
  }
  return 0;
}
static int ranges(int c) {
  switch (c) { case 'a' ... 'z': return 1; case '0' ... '9': return 2; default: return 0; }
}
int main(void) {
  for (int i = -6; i < 102; i += 7) printf("%s ", kind(i));
  printf("%s\n", kind(100));
  for (int i = 0; i < 10; i++) printf("%d ", dense(i));
  printf("\n%d %d %d %d\n", sparse(1000000000000LL), sparse(-1), sparse(7), sparse(8));
  printf("%d %d %d\n", ranges('q'), ranges('5'), ranges('!'));
  int n = 0, i = 0;
  while (i < 10) { if (i == 7) break; if (i % 2) { i++; continue; } n += i; i++; }
  printf("%d\n", n);
  do { n--; } while (n > 5);
  printf("%d\n", n);
  for (;;) { if (++n > 20) break; }
  printf("%d\n", n);
  int t = 0;
  for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) { if (b > a) break; if ((a + b) % 3 == 0) continue; t += a * b; }
  printf("%d\n", t);
  int x = 3;
  if (x > 2) if (x > 5) printf("big\n"); else printf("mid\n"); else printf("small\n");
  printf("%d\n", x > 2 && x < 5 || x == 10);
  int y = 0;
  (void)(x > 0 || (y = 1));
  printf("%d\n", y);
  return 0;
}
