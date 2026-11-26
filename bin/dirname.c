// SPDX-License-Identifier: GPL-2.0-or-later
/* dirname [-z] NAME... */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  int i = 1, zero = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strcmp(argv[i], "-z") || !strcmp(argv[i], "--zero")) zero = 1;
    else { fprintf(stderr, "basename: invalid option '%s'\nusage: dirname [-z] NAME...\n", argv[i]); return 1; }
  }
  if (i >= argc) { fprintf(stderr, "dirname: missing operand\n"); return 1; }
  for (; i < argc; i++) {
    const char *s = argv[i];
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') n--;   /* trailing slashes */
    while (n > 0 && s[n - 1] != '/') n--;   /* the last component */
    if (n == 0) fputs(".", stdout);
    else {
      while (n > 1 && s[n - 1] == '/') n--; /* slashes before it */
      /* POSIX: a result of exactly "//" may stay as it is; GNU keeps it */
      fwrite(s, 1, n, stdout);
    }
    putchar(zero ? 0 : '\n');
  }
  return fflush(stdout) ? 1 : 0;
}
