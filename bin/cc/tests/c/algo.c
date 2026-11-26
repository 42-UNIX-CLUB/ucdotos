// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void quicksort(int *a, int lo, int hi) {
  if (lo >= hi) return;
  int p = a[(lo + hi) / 2], i = lo, j = hi;
  while (i <= j) {
    while (a[i] < p) i++;
    while (a[j] > p) j--;
    if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
  }
  quicksort(a, lo, j);
  quicksort(a, i, hi);
}
typedef struct list { char *word; int count; struct list *next; } list;
static list *add(list *l, const char *w) {
  for (list *p = l; p; p = p->next) if (!strcmp(p->word, w)) { p->count++; return l; }
  list *n = malloc(sizeof *n);
  n->word = strdup(w);
  n->count = 1;
  n->next = l;
  return n;
}
static unsigned hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
static int is_prime(int n) { if (n < 2) return 0; for (int i = 2; i * i <= n; i++) if (n % i == 0) return 0; return 1; }
static long long fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
static int ack(int m, int n) { return m == 0 ? n + 1 : n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1)); }
static int even(int n);
static int odd(int n) { return n == 0 ? 0 : even(n - 1); }
static int even(int n) { return n == 0 ? 1 : odd(n - 1); }
int main(void) {
  int a[50];
  unsigned seed = 12345;
  for (int i = 0; i < 50; i++) { seed = seed * 1103515245 + 12345; a[i] = (seed >> 16) % 1000; }
  quicksort(a, 0, 49);
  for (int i = 0; i < 50; i += 7) printf("%d ", a[i]);
  printf("\n");
  char text[] = "the quick brown fox jumps over the lazy dog the end fox";
  list *l = NULL;
  for (char *w = strtok(text, " "); w; w = strtok(NULL, " ")) l = add(l, w);
  for (list *p = l; p; p = p->next) if (p->count > 1) printf("%s=%d ", p->word, p->count);
  printf("\n%u\n", hash("hello world"));
  int np = 0;
  for (int i = 0; i < 1000; i++) np += is_prime(i);
  printf("%d %lld %d %d\n", np, fact(20), ack(2, 3), even(17));
  char buf[64];
  snprintf(buf, sizeof buf, "%08.3f|%-6s|%+d|%x", 3.14159, "ab", 5, 0xbeef);
  printf("%s\n", buf);
  int sieve[100] = {0}, count = 0;
  for (int i = 2; i < 100; i++) if (!sieve[i]) { count++; for (int j = i * i; j < 100; j += i) sieve[j] = 1; }
  printf("%d\n", count);
  double m[3][3] = {{2, 1, 0}, {1, 3, 1}, {0, 1, 4}}, det;
  det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]);
  printf("%g\n", det);
  return 0;
}
