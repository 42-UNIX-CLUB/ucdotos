// SPDX-License-Identifier: GPL-2.0-or-later
/* tr [-cCdst] string1 [string2]: translate, delete or squeeze bytes.
 * Strings take ranges (a-z), classes ([:alpha:]), equivalence classes ([=a=]),
 * repeats in string2 ([x*] and [x*n]) and the escapes \\ \a \b \f \n \r \t \v \NNN.
 * As in GNU tr, a short string2 is padded with its last byte unless -t is given. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

static void die(const char *msg, const char *arg) {
  fprintf(stderr, "tr: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
  exit(1);
}

struct set { int c[1 << 15]; int n; int fill_at; int fill_c; int has_class; int only_case; };

static int class_has(const char *name, int c) {
  if (!strcmp(name, "alpha")) return isalpha(c);
  if (!strcmp(name, "digit")) return isdigit(c);
  if (!strcmp(name, "alnum")) return isalnum(c);
  if (!strcmp(name, "upper")) return isupper(c);
  if (!strcmp(name, "lower")) return islower(c);
  if (!strcmp(name, "space")) return isspace(c);
  if (!strcmp(name, "blank")) return c == ' ' || c == '\t';
  if (!strcmp(name, "punct")) return ispunct(c);
  if (!strcmp(name, "print")) return isprint(c);
  if (!strcmp(name, "graph")) return isgraph(c);
  if (!strcmp(name, "cntrl")) return iscntrl(c);
  if (!strcmp(name, "xdigit")) return isxdigit(c);
  return -1;
}

/* one possibly escaped byte at *s; advances *s */
static int esc(const char **s) {
  const char *p = *s;
  int c = (unsigned char)*p++;
  if (c == '\\' && *p) {
    c = (unsigned char)*p++;
    switch (c) {
    case 'a': c = 7; break;
    case 'b': c = 8; break;
    case 'f': c = 12; break;
    case 'n': c = 10; break;
    case 'r': c = 13; break;
    case 't': c = 9; break;
    case 'v': c = 11; break;
    default:
      if (c >= '0' && c <= '7') {
        int v = c - '0', k = 1;
        while (k < 3 && *p >= '0' && *p <= '7' && v * 8 + (*p - '0') < 256) { v = v * 8 + (*p++ - '0'); k++; }
        c = v;
      }
    }
  }
  *s = p;
  return c;
}

static void add(struct set *st, int c) {
  if (st->n >= (int)(sizeof st->c / sizeof st->c[0])) die("string too long", NULL);
  st->c[st->n++] = c;
}

/* expand a string into a sequence of bytes */
static void expand(const char *s, struct set *st, int second) {
  st->n = 0; st->fill_at = -1; st->has_class = 0; st->only_case = 1;
  while (*s) {
    if (s[0] == '[' && s[1] == ':') {
      const char *e = strstr(s + 2, ":]");
      if (e) {
        char name[32];
        size_t len = e - s - 2;
        if (len < sizeof name) {
          memcpy(name, s + 2, len); name[len] = 0;
          if (class_has(name, 'a') < 0) die("invalid character class", name);
          st->has_class = 1;
          if (strcmp(name, "upper") && strcmp(name, "lower")) st->only_case = 0;
          for (int c = 0; c < 256; c++) if (class_has(name, c)) add(st, c);
          s = e + 2;
          continue;
        }
      }
    }
    if (s[0] == '[' && s[1] == '=') {
      const char *p = s + 2;
      int c = esc(&p);
      if (p[0] == '=' && p[1] == ']') {
        if (second) die("[=c=] expressions may not appear in string2 when translating", NULL);
        add(st, c);
        s = p + 2;
        continue;
      }
    }
    if (s[0] == '[' && s[1]) { /* [c*n] */
      const char *p = s + 1;
      int c = esc(&p);
      if (*p == '*') {
        const char *q = p + 1;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == ']') {
          if (!second) die("the [c*] repeat construct may not appear in string1", NULL);
          long n = q > p + 1 ? strtol(p + 1, NULL, p[1] == '0' ? 8 : 10) : 0;
          if (n == 0) {
            if (st->fill_at >= 0) die("only one [c*] repeat construct may appear in string2", NULL);
            st->fill_at = st->n; st->fill_c = c;
          } else while (n--) add(st, c);
          s = q + 1;
          continue;
        }
      }
    }
    int c = esc(&s);
    if (s[0] == '-' && s[1]) {
      const char *p = s + 1;
      int hi = esc(&p);
      if (hi < c) {
        char msg[80];
        snprintf(msg, sizeof msg, "range-endpoints of '%c-%c' are in reverse collating sequence order", c, hi);
        die(msg, NULL);
      }
      for (int x = c; x <= hi; x++) add(st, x);
      s = p;
      continue;
    }
    add(st, c);
  }
}

int main(int argc, char **argv) {
  int comp = 0, del = 0, sq = 0, trunc = 0, i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    if (!strcmp(argv[i], "--complement")) { comp = 1; continue; }
    if (!strcmp(argv[i], "--delete")) { del = 1; continue; }
    if (!strcmp(argv[i], "--squeeze-repeats")) { sq = 1; continue; }
    if (!strcmp(argv[i], "--truncate-set1")) { trunc = 1; continue; }
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'c' || *f == 'C') comp = 1;
      else if (*f == 'd') del = 1;
      else if (*f == 's') sq = 1;
      else if (*f == 't') trunc = 1;
      else { fprintf(stderr, "tr: invalid option -- '%c'\nusage: tr [-cCdst] string1 [string2]\n", *f); return 1; }
    }
  }
  int nargs = argc - i;
  if (nargs < 1) die("missing operand", NULL);
  int translating = !del && nargs >= 2;
  if (del && !sq && nargs > 1) die("extra operand", argv[i + 1]);
  if (!del && !sq && nargs < 2) die("missing operand after", argv[i]);
  if (nargs > 2) die("extra operand", argv[i + 2]);
  static struct set s1, s2;
  expand(argv[i], &s1, 0);
  if (nargs > 1) expand(argv[i + 1], &s2, translating);

  unsigned char in1[256] = {0};
  for (int k = 0; k < s1.n; k++) in1[s1.c[k]] = 1;
  if (comp) { /* the complement, in ascending order */
    s1.n = 0;
    for (int c = 0; c < 256; c++) if (!in1[c]) s1.c[s1.n++] = c;
    for (int c = 0; c < 256; c++) in1[c] = !in1[c];
  }
  int map[256];
  for (int c = 0; c < 256; c++) map[c] = c;
  unsigned char sqset[256] = {0};
  if (translating) {
    if (s2.has_class && !s2.only_case) die("when translating, the only character classes that may appear in string2 are 'upper' and 'lower'", NULL);
    if (s2.fill_at >= 0) { /* [c*]: fill string2 up to the length of string1 */
      int need = s1.n - s2.n;
      if (need < 0) need = 0;
      memmove(s2.c + s2.fill_at + need, s2.c + s2.fill_at, (s2.n - s2.fill_at) * sizeof s2.c[0]);
      for (int k = 0; k < need; k++) s2.c[s2.fill_at + k] = s2.fill_c;
      s2.n += need;
    }
    if (trunc && s1.n > s2.n) s1.n = s2.n;
    if (s2.n == 0 && s1.n > 0) die("when not truncating set1, string2 must be non-empty", NULL);
    for (int k = 0; k < s1.n; k++) map[s1.c[k]] = s2.c[k < s2.n ? k : s2.n - 1];
    for (int k = 0; k < s2.n; k++) sqset[s2.c[k]] = 1;
  } else if (sq) {
    struct set *last = nargs > 1 ? &s2 : &s1;
    if (last == &s1) memcpy(sqset, in1, 256);
    else for (int k = 0; k < s2.n; k++) sqset[s2.c[k]] = 1;
  }

  static unsigned char buf[65536], obuf[65536];
  long n;
  int prev = -1;
  while ((n = read(0, buf, sizeof buf)) > 0) {
    long o = 0;
    for (long k = 0; k < n; k++) {
      int c = buf[k];
      if (del && in1[c]) continue;
      if (translating) c = map[c];
      if (sq && sqset[c] && c == prev) continue;
      prev = c;
      obuf[o++] = c;
    }
    if (o && write(1, obuf, o) < 0) { perror("tr: write error"); return 1; }
  }
  if (n < 0) { perror("tr: read error"); return 1; }
  return 0;
}
