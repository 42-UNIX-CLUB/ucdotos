// SPDX-License-Identifier: GPL-2.0-or-later
/* cmp A B: say whether two files are identical (UC.OS has no cmp yet). */
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc != 3) { fputs("usage: cmp a b\n", stderr); return 2; }
  FILE *a = fopen(argv[1], "rb"), *b = fopen(argv[2], "rb");
  if (!a || !b) { fputs("cmp: cannot open\n", stderr); return 2; }
  long n = 0;
  for (;;) {
    int x = getc(a), y = getc(b);
    if (x != y) { printf("%s %s differ at byte %ld\n", argv[1], argv[2], n); return 1; }
    if (x == EOF) break;
    n++;
  }
  printf("%s %s identical, %ld bytes\n", argv[1], argv[2], n);
  return 0;
}
