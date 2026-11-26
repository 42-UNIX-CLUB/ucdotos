// SPDX-License-Identifier: GPL-2.0-or-later
/* echo [-neE] [string...] */
#include <stdio.h>
#include <string.h>
static int put_escaped(const char *s) {
  for (; *s; s++) {
    if (*s != '\\' || !s[1]) { putchar(*s); continue; }
    switch (*++s) {
    case 'n': putchar('\n'); break;
    case 't': putchar('\t'); break;
    case 'r': putchar('\r'); break;
    case 'a': putchar('\a'); break;
    case 'b': putchar('\b'); break;
    case 'e': putchar(27); break;
    case '\\': putchar('\\'); break;
    case 'c': return 1;
    case '0': { int v = 0, k = 0; while (k < 3 && s[1] >= '0' && s[1] <= '7') v = v * 8 + (*++s - '0'), k++; putchar(v); break; }
    default: putchar('\\'); putchar(*s);
    }
  }
  return 0;
}
int main(int argc, char **argv) {
  int nl = 1, esc = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1] && strspn(argv[i] + 1, "neE") == strlen(argv[i] + 1); i++)
    for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'n') nl = 0; else esc = *f == 'e'; }
  for (; i < argc; i++) {
    if (esc) { if (put_escaped(argv[i])) return 0; } else fputs(argv[i], stdout);
    if (i + 1 < argc) putchar(' ');
  }
  if (nl) putchar('\n');
  return 0;
}
