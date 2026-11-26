// SPDX-License-Identifier: GPL-2.0-or-later
/* Debian version numbers, as deb-version(7) and Policy 5.6.12 describe them:
 * [epoch:]upstream[-revision]. The epoch is the part before the first colon,
 * the revision the part after the last hyphen. Parts compare left to right in
 * runs: a run of non-digits compares character by character with "~" before
 * everything (even the end), then the end, then letters, then the rest in
 * ASCII order; a run of digits compares as a number. A missing revision equals
 * "0". Works on the strings in place: no allocation. */
#include "pkg.h"
#include <string.h>

typedef struct { const char *s; size_t n; } slice;

static int order(int c) {
  if (c >= '0' && c <= '9') return 0;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return c;
  if (c == '~') return -1;
  if (c) return c + 256;
  return 0;
}
static int partcmp(slice a, slice b) {
  size_t i = 0, j = 0;
  while (i < a.n || j < b.n) {
    int first_diff = 0;
    while ((i < a.n && !(a.s[i] >= '0' && a.s[i] <= '9')) || (j < b.n && !(b.s[j] >= '0' && b.s[j] <= '9'))) {
      int ac = i < a.n ? order((unsigned char)a.s[i]) : 0;
      int bc = j < b.n ? order((unsigned char)b.s[j]) : 0;
      if (ac != bc) return ac - bc;
      i++; j++;
    }
    /* digit runs: skip leading zeros, then the longer run is larger, then compare */
    while (i < a.n && a.s[i] == '0') i++;
    while (j < b.n && b.s[j] == '0') j++;
    size_t si = i, sj = j;
    while (i < a.n && a.s[i] >= '0' && a.s[i] <= '9') i++;
    while (j < b.n && b.s[j] >= '0' && b.s[j] <= '9') j++;
    size_t li = i - si, lj = j - sj;
    if (li != lj) return li < lj ? -1 : 1;
    for (size_t k = 0; k < li; k++) if (a.s[si + k] != b.s[sj + k]) { first_diff = a.s[si + k] - b.s[sj + k]; break; }
    if (first_diff) return first_diff;
  }
  return 0;
}
static void split(const char *v, slice *epoch, slice *up, slice *rev) {
  while (*v == ' ' || *v == '\t') v++;
  size_t n = strlen(v);
  while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
  const char *colon = memchr(v, ':', n);
  if (colon) { epoch->s = v; epoch->n = colon - v; v = colon + 1; n -= epoch->n + 1; }
  else { epoch->s = ""; epoch->n = 0; }
  const char *dash = 0;
  for (size_t i = 0; i < n; i++) if (v[i] == '-') dash = v + i;
  up->s = v;
  if (dash) { up->n = dash - v; rev->s = dash + 1; rev->n = n - up->n - 1; }
  else { up->n = n; rev->s = ""; rev->n = 0; }
}
int ver_cmp(const char *a, const char *b) {
  slice ea, ua, ra, eb, ub, rb;
  split(a, &ea, &ua, &ra);
  split(b, &eb, &ub, &rb);
  /* epochs are numbers: compare as digit runs */
  int r = partcmp(ea, eb);
  if (r) return r < 0 ? -1 : 1;
  r = partcmp(ua, ub);
  if (r) return r < 0 ? -1 : 1;
  r = partcmp(ra, rb);
  return r < 0 ? -1 : r > 0;
}
const char *ver_check(const char *v) {
  if (!v || !*v) return "version string is empty";
  for (const char *p = v; *p; p++) if (*p == ' ' || *p == '\t' || *p == '\n') return "version string has embedded spaces";
  slice e, u, r;
  split(v, &e, &u, &r);
  if (strchr(v, ':')) {
    if (!e.n) return "epoch in version is empty";
    unsigned long val = 0;
    for (size_t i = 0; i < e.n; i++) {
      if (e.s[i] < '0' || e.s[i] > '9') return "epoch in version is not number";
      val = val * 10 + (e.s[i] - '0');
      if (val > 2147483647ul) return "epoch in version is too big";
    }
  }
  if (!u.n) return "nothing after colon in version number";
  if (strrchr(v, '-') && !r.n) return "revision number is empty";
  if (!(u.s[0] >= '0' && u.s[0] <= '9')) return "version number does not start with digit";
  for (size_t i = 0; i < u.n; i++) {
    char c = u.s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr(".-+~:", c)))
      return "invalid character in version number";
  }
  for (size_t i = 0; i < r.n; i++) {
    char c = r.s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr(".+~", c)))
      return "invalid character in revision number";
  }
  return 0;
}
int ver_satisfies(const char *have, int op, const char *want) {
  if (op == REL_NONE) return 1;
  if (!have) return 0;
  int c = ver_cmp(have, want);
  switch (op) {
  case REL_LT: return c < 0;
  case REL_LE: return c <= 0;
  case REL_EQ: return c == 0;
  case REL_GE: return c >= 0;
  case REL_GT: return c > 0;
  }
  return 0;
}
const char *rel_str(int op) {
  static const char *const s[] = {"", "<<", "<=", "=", ">=", ">>"};
  return op >= 0 && op <= REL_GT ? s[op] : "?";
}
