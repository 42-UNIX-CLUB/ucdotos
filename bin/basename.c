// SPDX-License-Identifier: GPL-2.0-or-later
/* basename NAME [SUFFIX]  |  basename -a [-s SUFFIX] [-z] NAME... */
#include <stdio.h>
#include <string.h>

static int zero;
static void base(const char *s, const char *suffix) {
  size_t n = strlen(s);
  while (n > 1 && s[n - 1] == '/') n--;      /* trailing slashes */
  size_t st = n;
  while (st > 0 && s[st - 1] != '/') st--;   /* last component */
  if (n == 1 && s[0] == '/') st = 0;          /* "/" and "//" stay "/" */
  size_t len = n - st;
  if (suffix && *suffix) {
    size_t sl = strlen(suffix);
    if (sl < len && !memcmp(s + n - sl, suffix, sl)) len -= sl;
  }
  fwrite(s + st, 1, len, stdout);
  putchar(zero ? 0 : '\n');
}
int main(int argc, char **argv) {
  int i = 1, multi = 0;
  const char *suffix = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--multiple")) { multi = 1; continue; }
    if (!strcmp(a, "--zero")) { zero = 1; continue; }
    if (!strncmp(a, "--suffix=", 9)) { suffix = a + 9; multi = 1; continue; }
    if (a[1] == '-') { fprintf(stderr, "basename: unknown option '%s'\n", a); return 1; }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'a') multi = 1;
      else if (*f == 'z') zero = 1;
      else if (*f == 's') {
        suffix = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!suffix) { fprintf(stderr, "basename: option requires an argument -- 's'\n"); return 1; }
        multi = 1;
        break;
      } else { fprintf(stderr, "basename: invalid option -- '%c'\nusage: basename NAME [SUFFIX]\n", *f); return 1; }
    }
  }
  if (i >= argc) { fprintf(stderr, "basename: missing operand\n"); return 1; }
  if (!multi) {
    if (argc - i > 2) { fprintf(stderr, "basename: extra operand '%s'\n", argv[i + 2]); return 1; }
    base(argv[i], i + 1 < argc ? argv[i + 1] : NULL);
  } else for (; i < argc; i++) base(argv[i], suffix);
  return fflush(stdout) ? 1 : 0;
}
