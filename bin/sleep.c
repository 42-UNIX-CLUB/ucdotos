// SPDX-License-Identifier: GPL-2.0-or-later
/* sleep number[smhd]...: fractional seconds are fine */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: sleep seconds[smhd]...\n"); return 1; }
  double total = 0;
  for (int i = 1; i < argc; i++) {
    char *e;
    double v = strtod(argv[i], &e);
    if (e == argv[i] || v < 0) { fprintf(stderr, "sleep: invalid time interval '%s'\n", argv[i]); return 1; }
    if (*e == 'm') v *= 60; else if (*e == 'h') v *= 3600; else if (*e == 'd') v *= 86400;
    else if (*e && *e != 's') { fprintf(stderr, "sleep: invalid time interval '%s'\n", argv[i]); return 1; }
    total += v;
  }
  struct timespec ts = {(long long)total, (long)((total - (long long)total) * 1e9)};
  nanosleep(&ts, 0);
  return 0;
}
