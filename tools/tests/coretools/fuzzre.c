// SPDX-License-Identifier: GPL-2.0-or-later
/* Fuzz libc/src/regex.c: mutated patterns, random flags and subjects, for a
 * fixed time. Checks that nothing crashes (run it with ASan and UBSan), that
 * results are well formed, and how long the slowest regexec took. Built with
 * a small backtracking budget (-DRE_STEP_BUDGET) so blow-ups show as counts.
 *   cc -O1 -g -fsanitize=address,undefined -DRE_STEP_BUDGET=2000000 -Ilibc/include-copy \
 *      tools/tests/coretools/fuzzre.c libc/src/regex.c -o fuzzre && ./fuzzre SECONDS SEED */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <regex.h>

extern long __re_budget_hits;
static unsigned long long rs;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)rs; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static const char *seeds[] = {
  "a*b", "\\(a\\|b\\)*c", "(a|b)+c", "[[:alpha:]]+", "^x.*y$", "\\(.\\)\\1", "(.)\\1", "a\\{2,5\\}", "a{2,5}", "[^]a-z]",
  "\\<word\\>", "(a*)*b", "(a|aa)*c", "\\(a*\\)*\\1", "x(y(z)?)*", "[[.-.][=a=]]", "a|b|c|d", "((a)|b)+", ".*.*.*x", "(x+x+)+y",
};
static const char alphabet[] = "ab.*+?|()[]{}^$\\-:,0123456789xyz[:alpha:]=<>`'wWsSbB";

int main(int argc, char **argv) {
  double limit = argc > 1 ? atof(argv[1]) : 50;
  rs = argc > 2 ? strtoull(argv[2], 0, 10) : 1;
  if (!rs) rs = 1;
  double start = now(), worst = 0;
  long iters = 0, compiled = 0, matched = 0, bad = 0;
  char worst_pat[300] = "", pat[300], sub[400];
  while (now() - start < limit) {
    iters++;
    strcpy(pat, seeds[rnd() % (sizeof seeds / sizeof *seeds)]);
    int muts = rnd() % 6;
    for (int m = 0; m < muts; m++) {
      size_t n = strlen(pat), at = n ? rnd() % (n + 1) : 0;
      int op = rnd() % 3;
      if (op == 0 && n < 250) { memmove(pat + at + 1, pat + at, n - at + 1); pat[at] = alphabet[rnd() % (sizeof alphabet - 1)]; }
      else if (op == 1 && n) { if (at == n) at--; memmove(pat + at, pat + at + 1, n - at); }
      else if (n) { if (at == n) at--; pat[at] = alphabet[rnd() % (sizeof alphabet - 1)]; }
    }
    if (rnd() % 8 == 0 && strlen(pat) < 120) { char tmp[300]; snprintf(tmp, sizeof tmp, "(%s){%u}", pat, rnd() % 40); strcpy(pat, tmp); }
    int cf = (rnd() & 1 ? REG_EXTENDED : 0) | (rnd() % 4 == 0 ? REG_ICASE : 0) | (rnd() % 4 == 0 ? REG_NEWLINE : 0);
    regex_t re;
    if (regcomp(&re, pat, cf)) continue;
    compiled++;
    for (int k = 0; k < 4; k++) {
      int len = rnd() % (k == 3 ? 400 : 40);
      for (int i = 0; i < len; i++) sub[i] = "aabxyz \n-:"[rnd() % 10];
      sub[len] = 0;
      regmatch_t m[10];
      int ef = rnd() % 4 == 0 ? REG_NOTBOL : 0;
      if (rnd() % 3 == 0) { m[0].rm_so = len ? rnd() % (len + 1) : 0; m[0].rm_eo = len; ef |= rnd() & 1 ? REG_STARTEND : REG_UC_OFFSET; }
      double t0 = now();
      int r = regexec(&re, sub, 10, m, ef);
      double dt = now() - t0;
      if (dt > worst) { worst = dt; snprintf(worst_pat, sizeof worst_pat, "%s (flags %d, subject %d bytes)", pat, cf, len); }
      if (r == 0) {
        matched++;
        if (m[0].rm_so < 0 || m[0].rm_eo < m[0].rm_so || m[0].rm_eo > len) { bad++; printf("bad match bounds: %s\n", pat); }
        for (size_t g = 1; g <= re.re_nsub && g < 10; g++)
          if (m[g].rm_so != -1 && (m[g].rm_so > m[g].rm_eo || m[g].rm_eo > len || m[g].rm_so < 0)) { bad++; printf("bad group %zu: %s\n", g, pat); }
      }
    }
    regfree(&re);
  }
  printf("fuzzre: %ld patterns, %ld compiled, %ld matches, %ld malformed results, %ld backtracking budget hits, slowest regexec %.3f ms: %s\n",
         iters, compiled, matched, bad, __re_budget_hits, worst * 1000, worst_pat);
  return bad != 0;
}
