// SPDX-License-Identifier: GPL-2.0-or-later
/* A small backtracking matcher for vi's / ? and :s: literal text plus ^ $ . * [...] and \ escapes.
 * Patterns follow vim's default "magic" subset; '.' matches one UTF-8 character. */
#include "vi.h"

/* length of the atom at re (".", "[...]", "\x" or one byte) */
static int atom_len(const char *re) {
  if (re[0] == '\\' && re[1]) return 2;
  if (re[0] == '[') {
    const char *p = re + 1;
    if (*p == '^') p++;
    if (*p == ']') p++;
    while (*p && *p != ']') p++;
    if (*p == ']') return (int)(p - re) + 1;
  }
  return 1;
}
/* does the atom match at t? returns bytes consumed, or 0 */
static int atom_match(const char *re, int alen, const char *t, const char *end) {
  if (t >= end) return 0;
  unsigned char c = *t;
  int clen = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
  if (t + clen > end) clen = (int)(end - t);
  if (re[0] == '.' && alen == 1) return clen;
  if (re[0] == '\\') {
    char e = re[1];
    if (e == 't') return c == '\t';
    if (e == 's') return c == ' ' || c == '\t';
    if (e == 'd') return c >= '0' && c <= '9';
    if (e == 'w') return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    return c == (unsigned char)e;
  }
  if (re[0] == '[' && alen > 1) {
    const char *p = re + 1, *stop = re + alen - 1;
    int neg = 0, hit = 0;
    if (*p == '^') { neg = 1; p++; }
    for (int first = 1; p < stop; first = 0) {
      unsigned char a = *p;
      if (a == '\\' && p + 1 < stop) { p++; a = *p; }
      else if (a == ']' && !first) break;
      if (p + 2 < stop && p[1] == '-') {
        unsigned char b = p[2];
        if (c >= a && c <= b) hit = 1;
        p += 3;
      } else {
        if (c == a) hit = 1;
        p++;
      }
    }
    return hit != neg ? clen : 0;
  }
  return c == (unsigned char)re[0];
}
static const char *match_here(const char *re, const char *t, const char *end);
static const char *match_star(const char *atom, int alen, const char *rest, const char *t, const char *end) {
  const char *pos[1024];
  int n = 0;
  pos[n++] = t;
  int m;
  while (n < 1024 && (m = atom_match(atom, alen, t, end)) > 0) { t += m; pos[n++] = t; }
  while (n > 0) { /* greedy: longest first */
    const char *r = match_here(rest, pos[--n], end);
    if (r) return r;
  }
  return 0;
}
static const char *match_here(const char *re, const char *t, const char *end) {
  for (;;) {
    if (!*re) return t;
    if (re[0] == '$' && !re[1]) return t == end ? t : 0;
    int alen = atom_len(re);
    if (re[alen] == '*') return match_star(re, alen, re + alen + 1, t, end);
    int m = atom_match(re, alen, t, end);
    if (!m) return 0;
    t += m;
    re += alen;
  }
}
/* search s[0..len) for re starting at byte from; on success stores [*ms, *me) and returns 1 */
int re_search(const char *re, const char *s, int len, int from, int *ms, int *me) {
  const char *end = s + len;
  if (re[0] == '^') {
    if (from > 0) return 0;
    const char *r = match_here(re + 1, s, end);
    if (r) { *ms = 0; *me = (int)(r - s); return 1; }
    return 0;
  }
  for (int i = from; i <= len; i++) {
    if (i > 0 && i < len && ((unsigned char)s[i] & 0xc0) == 0x80) continue;
    const char *r = match_here(re, s + i, end);
    if (r) { *ms = i; *me = (int)(r - s); return 1; }
  }
  return 0;
}
