// SPDX-License-Identifier: GPL-2.0-or-later
/* seq [-w] [-s sep] [first [incr]] last */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int decimals(const char *s) { const char *d = strchr(s, '.'); return d ? (int)strlen(d + 1) : 0; }
int main(int argc, char **argv) {
  const char *sep = "\n";
  int i = 1, wide = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] && !(argv[i][1] >= '0' && argv[i][1] <= '9') && argv[i][1] != '.'; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) sep = argv[++i];
    else if (!strcmp(argv[i], "-w")) wide = 1;
    else { fprintf(stderr, "usage: seq [-w] [-s sep] [first [incr]] last\n"); return 1; }
  }
  int n = argc - i;
  if (n < 1 || n > 3) { fprintf(stderr, "usage: seq [-w] [-s sep] [first [incr]] last\n"); return 1; }
  const char *a = n > 1 ? argv[i] : "1", *inc = n == 3 ? argv[i + 1] : "1", *b = argv[argc - 1];
  int dec = decimals(a);
  if (decimals(inc) > dec) dec = decimals(inc);
  if (decimals(b) > dec) dec = decimals(b);
  setvbuf(stdout, 0, _IOFBF, 0);
  int first = 1;
  if (!dec) {
    long long x = strtoll(a, 0, 10), d = strtoll(inc, 0, 10), y = strtoll(b, 0, 10);
    if (!d) { fprintf(stderr, "seq: zero increment\n"); return 1; }
    int w = 0;
    if (wide) { char t[32]; int wa = snprintf(t, sizeof t, "%lld", x), wb = snprintf(t, sizeof t, "%lld", y); w = wa > wb ? wa : wb; }
    for (; d > 0 ? x <= y : x >= y; x += d) {
      if (!first) fputs(sep, stdout);
      printf(wide ? "%0*lld" : "%*lld", w, x);
      first = 0;
      if (ferror(stdout)) return 1;
    }
  } else {
    double x = strtod(a, 0), d = strtod(inc, 0), y = strtod(b, 0);
    if (d == 0) { fprintf(stderr, "seq: zero increment\n"); return 1; }
    for (long k = 0;; k++) {
      double v = x + k * d;
      if (d > 0 ? v > y + 1e-9 : v < y - 1e-9) break;
      if (!first) fputs(sep, stdout);
      printf("%.*f", dec, v);
      first = 0;
      if (ferror(stdout)) return 1;
    }
  }
  if (!first) putchar('\n');
  fflush(stdout);
  return ferror(stdout);
}
