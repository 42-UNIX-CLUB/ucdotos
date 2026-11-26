// SPDX-License-Identifier: GPL-2.0-or-later
/* comm [-123] [--output-delimiter=STR] [--total] [-z] file1 file2: lines only
 * in file1, only in file2, and in both. The files must be sorted (byte order);
 * as in GNU comm, an unsorted input is reported and makes the exit status 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int show[4] = {0, 1, 1, 1}, eol = '\n';
static const char *sep = "\t";
static void put(int col, const char *s, long n) {
  if (!show[col]) return;
  for (int k = 1; k < col; k++) if (show[k]) fputs(sep, stdout);
  fwrite(s, 1, n, stdout);
  putchar(eol);
}
static int cmpl(const char *a, long na, const char *b, long nb) {
  long m = na < nb ? na : nb;
  int c = memcmp(a, b, m);
  return c ? c : (na > nb) - (na < nb);
}
int main(int argc, char **argv) {
  int i = 1, total = 0, check = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strncmp(a, "--output-delimiter=", 19)) { sep = a + 19; continue; }
    if (!strcmp(a, "--total")) { total = 1; continue; }
    if (!strcmp(a, "--zero-terminated")) { eol = 0; continue; }
    if (!strcmp(a, "--nocheck-order")) { check = 0; continue; }
    if (!strcmp(a, "--check-order")) { check = 2; continue; }
    for (char *f = a + 1; *f; f++) {
      if (*f >= '1' && *f <= '3') show[*f - '0'] = 0;
      else if (*f == 'z') eol = 0;
      else { fprintf(stderr, "comm: invalid option -- '%c'\nusage: comm [-123] file1 file2\n", *f); return 1; }
    }
  }
  if (argc - i != 2) { fprintf(stderr, argc - i < 2 ? "comm: missing operand\n" : "comm: extra operand\n"); return 1; }
  FILE *f[2];
  for (int k = 0; k < 2; k++) {
    f[k] = strcmp(argv[i + k], "-") ? fopen(argv[i + k], "r") : stdin;
    if (!f[k]) { fprintf(stderr, "comm: %s: %s\n", argv[i + k], strerror(errno)); return 1; }
  }
  setvbuf(stdout, NULL, _IOFBF, 65536);
  char *l[2] = {NULL, NULL}, *prev[2] = {NULL, NULL};
  size_t cap[2] = {0, 0};
  long n[2], pn[2] = {-1, -1}, cnt[4] = {0};
  int unsorted = 0, warned[2] = {0, 0};
#define READ(k) do { \
    n[k] = getdelim(&l[k], &cap[k], eol, f[k]); \
    if (n[k] > 0 && l[k][n[k] - 1] == eol) n[k]--; \
    if (n[k] >= 0 && check) { \
      if (pn[k] >= 0 && cmpl(prev[k], pn[k], l[k], n[k]) > 0 && !warned[k]) { \
        fflush(stdout); fprintf(stderr, "comm: file %d is not in sorted order\n", k + 1); warned[k] = 1; unsorted = 1; \
        if (check == 2) return 1; } \
      prev[k] = realloc(prev[k], n[k] + 1); memcpy(prev[k], l[k], n[k]); pn[k] = n[k]; } \
  } while (0)
  READ(0);
  READ(1);
  while (n[0] >= 0 || n[1] >= 0) {
    int c = n[0] < 0 ? 1 : n[1] < 0 ? -1 : cmpl(l[0], n[0], l[1], n[1]);
    if (c < 0) { put(1, l[0], n[0]); cnt[1]++; READ(0); }
    else if (c > 0) { put(2, l[1], n[1]); cnt[2]++; READ(1); }
    else { put(3, l[0], n[0]); cnt[3]++; READ(0); READ(1); }
  }
  if (total) printf("%ld%s%ld%s%ld%stotal%c", cnt[1], sep, cnt[2], sep, cnt[3], sep, eol);
  if (fflush(stdout)) return 1;
  if (unsorted) { fprintf(stderr, "comm: input is not in sorted order\n"); return 1; }
  return 0;
}
