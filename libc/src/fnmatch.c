// SPDX-License-Identifier: GPL-2.0-or-later
/* fnmatch: shell patterns with * ? [...] and backslash escapes.
 * The matcher keeps one backtracking point (the last '*'), which is enough
 * because a later star can always absorb what an earlier one would have:
 * time is O(pattern x string). With FNM_PATHNAME a star stops at '/', and
 * a failure that would need a star to cross '/' fails the whole match. */
#include <fnmatch.h>
#include <string.h>
#include <ctype.h>

static int fold(int c, int flags) { return (flags & FNM_CASEFOLD) ? tolower(c) : c; }

static int class_has(const char *name, size_t n, int c) {
#define IS(s) (n == sizeof s - 1 && !memcmp(name, s, n))
  if (IS("alpha")) return isalpha(c);
  if (IS("digit")) return isdigit(c);
  if (IS("alnum")) return isalnum(c);
  if (IS("upper")) return isupper(c);
  if (IS("lower")) return islower(c);
  if (IS("space")) return isspace(c);
  if (IS("blank")) return c == ' ' || c == '\t';
  if (IS("punct")) return ispunct(c);
  if (IS("print")) return isprint(c);
  if (IS("graph")) return isgraph(c);
  if (IS("cntrl")) return iscntrl(c);
  if (IS("xdigit")) return isxdigit(c);
#undef IS
  return -1;
}

/* a bracket expression at p (just after '['): 1 match, 0 no match, -1 not a valid bracket (then '[' is literal).
 * *end gets the position after ']'. */
static int bracket(const char *p, int c, int flags, const char **end) {
  int neg = 0, ok = 0, first = 1;
  if (*p == '!' || *p == '^') { neg = 1; p++; }
  int cf = fold(c, flags);
  for (;;) {
    if (!*p) return -1;
    if (*p == ']' && !first) { p++; break; }
    first = 0;
    int lo, exact = 0; /* glibc does not fold the case of [.x.] and [=x=] */
    if (p[0] == '[' && p[1] == ':') {
      const char *e = strstr(p + 2, ":]");
      if (!e) return -1;
      int r = class_has(p + 2, e - p - 2, c);
      if (r < 0) return -1;
      if (r) ok = 1;
      p = e + 2;
      continue;
    }
    if (p[0] == '[' && (p[1] == '.' || p[1] == '=') && p[2] && p[3] == p[1] && p[4] == ']') { lo = (unsigned char)p[2]; p += 5; exact = 1; }
    else if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1]) { lo = (unsigned char)p[1]; p += 2; }
    else lo = (unsigned char)*p++;
    int hi = lo;
    if (p[0] == '-' && p[1] && p[1] != ']') {
      p++;
      if (p[0] == '[' && (p[1] == '.' || p[1] == '=') && p[2] && p[3] == p[1] && p[4] == ']') { hi = (unsigned char)p[2]; p += 5; }
      else if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1]) { hi = (unsigned char)p[1]; p += 2; }
      else hi = (unsigned char)*p++;
    }
    if (lo <= c && c <= hi) ok = 1;
    else if ((flags & FNM_CASEFOLD) && !exact && ((fold(lo, flags) <= cf && cf <= fold(hi, flags)) || (lo <= toupper(c) && toupper(c) <= hi))) ok = 1;
  }
  *end = p;
  return ok != neg;
}

static int leading_period(const char *s, const char *start, int flags) {
  if (!(flags & FNM_PERIOD) || *s != '.') return 0;
  return s == start || ((flags & FNM_PATHNAME) && s[-1] == '/');
}

int fnmatch(const char *pat, const char *str, int flags) {
  const char *p = pat, *s = str, *star_p = NULL, *star_s = NULL;
  for (;;) {
    if (!*p) {
      if (!*s || ((flags & FNM_LEADING_DIR) && *s == '/')) return 0;
    } else {
      int c = (unsigned char)*p;
      if (c == '*') {
        while (*p == '*') p++;
        if (leading_period(s, str, flags)) goto fail;
        star_p = p;
        star_s = s;
        continue;
      }
      if (*s) {
        if (c == '?') {
          if (((flags & FNM_PATHNAME) && *s == '/') || leading_period(s, str, flags)) goto fail;
          p++; s++;
          continue;
        }
        if (c == '[') {
          const char *e;
          if (((flags & FNM_PATHNAME) && *s == '/') || leading_period(s, str, flags)) goto fail;
          int r = bracket(p + 1, (unsigned char)*s, flags, &e);
          if (r == 1) { p = e; s++; continue; }
          if (r == 0) goto fail;
          /* not a bracket expression: a literal '[' */
        }
        if (c == '\\' && !(flags & FNM_NOESCAPE)) {
          if (!p[1]) return FNM_NOMATCH; /* a trailing backslash matches nothing, as in glibc */
          p++;
          c = (unsigned char)*p;
        }
        if (fold(c, flags) == fold((unsigned char)*s, flags)) { p++; s++; continue; }
      }
    }
  fail:
    /* let the last star absorb one more character and retry */
    if (!star_p || !*star_s) return FNM_NOMATCH;
    if ((flags & FNM_PATHNAME) && *star_s == '/') return FNM_NOMATCH;
    star_s++;
    p = star_p;
    s = star_s;
  }
}
