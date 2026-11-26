// SPDX-License-Identifier: GPL-2.0-or-later
/* make: memory, strings, hash tables, patterns, paths and globbing. */
#include "make.h"

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "%s: *** virtual memory exhausted.  Stop.\n", progname); exit(2); }
  return p;
}
void *xcalloc(size_t a, size_t b) {
  void *p = calloc(a ? a : 1, b ? b : 1);
  if (!p) { fprintf(stderr, "%s: *** virtual memory exhausted.  Stop.\n", progname); exit(2); }
  return p;
}
void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) { fprintf(stderr, "%s: *** virtual memory exhausted.  Stop.\n", progname); exit(2); }
  return p;
}
char *xstrndup(const char *s, size_t n) {
  char *d = xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xconcat(const char *a, const char *b, const char *c) {
  size_t la = strlen(a), lb = strlen(b), lc = c ? strlen(c) : 0;
  char *d = xmalloc(la + lb + lc + 1);
  memcpy(d, a, la); memcpy(d + la, b, lb); if (lc) memcpy(d + la + lb, c, lc);
  d[la + lb + lc] = 0;
  return d;
}

void bgrow(buf *b, size_t extra) {
  if (b->n + extra + 1 <= b->cap) return;
  size_t c = b->cap ? b->cap * 2 : 64;
  while (c < b->n + extra + 1) c *= 2;
  b->s = xrealloc(b->s, c);
  b->cap = c;
}
void bput(buf *b, const char *s, size_t n) {
  if (!n) return;
  bgrow(b, n);
  memcpy(b->s + b->n, s, n);
  b->n += n;
}
void bputs(buf *b, const char *s) { bput(b, s, strlen(s)); }
void bputc(buf *b, int c) { bgrow(b, 1); b->s[b->n++] = (char)c; }
char *bstr(buf *b) { bgrow(b, 0); b->s[b->n] = 0; return b->s; }
char *bdetach(buf *b) { char *s = bstr(b); b->s = 0; b->n = b->cap = 0; return s; }
void bfree(buf *b) { free(b->s); b->s = 0; b->n = b->cap = 0; }

void sl_add(strlist *l, char *s) {
  if (l->n + 1 >= l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = xrealloc(l->v, l->cap * sizeof *l->v); }
  l->v[l->n++] = s;
  l->v[l->n] = 0;
}
void sl_free(strlist *l) {
  for (int i = 0; i < l->n; i++) free(l->v[i]);
  free(l->v);
  l->v = 0; l->n = l->cap = 0;
}

/* FNV-1a */
unsigned hhash(const char *k, size_t n) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= (unsigned char)k[i]; h *= 16777619u; }
  return h;
}
void *hget(htab *h, const char *k, size_t n) {
  if (!h->cap) return 0;
  unsigned hv = hhash(k, n), m = h->cap - 1;
  for (unsigned i = hv & m;; i = (i + 1) & m) {
    hent *e = &h->t[i];
    if (!e->key) return 0;
    if (e->h == hv && !strncmp(e->key, k, n) && !e->key[n]) return e->val;
  }
}
static void hresize(htab *h) {
  unsigned oc = h->cap;
  hent *ot = h->t;
  h->cap = oc ? oc * 2 : 64;
  h->t = xcalloc(h->cap, sizeof *h->t);
  unsigned m = h->cap - 1;
  for (unsigned i = 0; i < oc; i++) {
    if (!ot[i].key) continue;
    unsigned j = ot[i].h & m;
    while (h->t[j].key) j = (j + 1) & m;
    h->t[j] = ot[i];
  }
  free(ot);
}
void **hput(htab *h, const char *k, size_t n, int *isnew) {
  if ((h->n + 1) * 2 > h->cap) hresize(h);
  unsigned hv = hhash(k, n), m = h->cap - 1;
  for (unsigned i = hv & m;; i = (i + 1) & m) {
    hent *e = &h->t[i];
    if (!e->key) {
      e->key = xstrndup(k, n);
      e->h = hv;
      e->val = 0;
      h->n++;
      if (isnew) *isnew = 1;
      return &e->val;
    }
    if (e->h == hv && !strncmp(e->key, k, n) && !e->key[n]) {
      if (isnew) *isnew = 0;
      return &e->val;
    }
  }
}

const char *skip_ws(const char *p) {
  while (is_space(*p)) p++;
  return p;
}
const char *next_word(const char **pp, size_t *len) {
  const char *p = *pp;
  while (is_space(*p)) p++;
  if (!*p) { *pp = p; return 0; }
  const char *s = p;
  while (*p && !is_space(*p)) p++;
  *len = p - s;
  *pp = p;
  return s;
}
void strip_into(buf *out, const char *s, size_t n) {
  const char *e = s + n;
  int first = 1;
  while (s < e) {
    while (s < e && is_space(*s)) s++;
    if (s >= e) break;
    const char *w = s;
    while (s < e && !is_space(*s)) s++;
    if (!first) bputc(out, ' ');
    bput(out, w, s - w);
    first = 0;
  }
}

/* A % preceded by a backslash is literal; GNU Make also lets backslashes escape
 * backslashes that come before a %. */
const char *find_percent(const char *s) {
  for (const char *p = s; *p; p++) {
    if (*p == '%') {
      int k = 0;
      for (const char *q = p; q > s && q[-1] == '\\'; q--) k++;
      if (!(k & 1)) return p;
    }
  }
  return 0;
}
char *unescape_percent(const char *s, const char **pct) {
  /* only rewrite when there is a backslash before some % */
  *pct = 0;
  if (!strchr(s, '\\')) { char *d = xstrdup(s); char *p = strchr(d, '%'); *pct = p; return d; }
  buf b = {0};
  const char *p = s;
  long at = -1;
  while (*p) {
    if (*p == '\\') {
      const char *q = p;
      while (*q == '\\') q++;
      if (*q == '%') {
        size_t k = q - p;
        for (size_t i = 0; i < k / 2; i++) bputc(&b, '\\');
        if (k & 1) { bputc(&b, '%'); }
        else { if (at < 0) at = b.n; bputc(&b, '%'); }
        p = q + 1;
        continue;
      }
      bput(&b, p, q - p);
      p = q;
      continue;
    }
    if (*p == '%' && at < 0) at = b.n;
    bputc(&b, *p++);
  }
  char *d = bdetach(&b);
  if (!d) d = xstrdup("");
  *pct = at >= 0 ? d + at : 0;
  return d;
}
int pattern_match(const char *pat, const char *pct, const char *s, size_t sn, size_t *stem_off, size_t *stem_len) {
  if (!pct) {
    size_t pl = strlen(pat);
    if (pl != sn || memcmp(pat, s, sn)) return 0;
    if (stem_off) { *stem_off = 0; *stem_len = 0; }
    return 1;
  }
  size_t pre = pct - pat, suf = strlen(pct + 1);
  if (sn < pre + suf) return 0;
  if (memcmp(s, pat, pre) || memcmp(s + sn - suf, pct + 1, suf)) return 0;
  if (stem_off) { *stem_off = pre; *stem_len = sn - pre - suf; }
  return 1;
}
void pattern_subst(buf *out, const char *pat, const char *pct, const char *stem, size_t sl) {
  if (!pct) { bputs(out, pat); return; }
  bput(out, pat, pct - pat);
  bput(out, stem, sl);
  bputs(out, pct + 1);
}
void path_dir(buf *out, const char *s, size_t n) {
  size_t i = n;
  while (i > 0 && s[i - 1] != '/') i--;
  if (i == 0) bputs(out, "./");
  else bput(out, s, i);
}
void path_notdir(buf *out, const char *s, size_t n) {
  size_t i = n;
  while (i > 0 && s[i - 1] != '/') i--;
  bput(out, s + i, n - i);
}
/* Resolve . and .. textually, as GNU Make's abspath does. */
char *abspath_of(const char *p, const char *cwd) {
  buf b = {0};
  if (*p != '/') { bputs(&b, cwd); }
  const char *s = p;
  while (*s) {
    while (*s == '/') s++;
    const char *e = s;
    while (*e && *e != '/') e++;
    size_t n = e - s;
    if (n == 0) break;
    if (n == 1 && s[0] == '.') {}
    else if (n == 2 && s[0] == '.' && s[1] == '.') {
      while (b.n > 0 && b.s[b.n - 1] != '/') b.n--;
      if (b.n > 0) b.n--;
    } else {
      bputc(&b, '/');
      bput(&b, s, n);
    }
    s = e;
  }
  if (b.n == 0) bputc(&b, '/');
  return bdetach(&b);
}

int has_glob_chars(const char *s) {
  for (; *s; s++) {
    if (*s == '\\' && s[1]) { s++; continue; }
    if (*s == '*' || *s == '?' || *s == '[') return 1;
  }
  return 0;
}
int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* fnmatch for one path component: * ? [...] [!...] and backslash escapes */
static int gmatch(const char *p, const char *s) {
  for (;;) {
    char c = *p++;
    switch (c) {
    case 0: return *s == 0;
    case '?': if (!*s) return 0; s++; break;
    case '*':
      while (*p == '*') p++;
      if (!*p) return 1;
      for (; *s; s++) if (gmatch(p, s)) return 1;
      return gmatch(p, s);
    case '[': {
      if (!*s) return 0;
      const char *q = p;
      int neg = 0, ok = 0;
      if (*q == '!' || *q == '^') { neg = 1; q++; }
      int first = 1;
      while (*q && (first || *q != ']')) {
        char lo = *q, hi;
        if (lo == '\\' && q[1]) lo = *++q;
        q++;
        if (*q == '-' && q[1] && q[1] != ']') { hi = q[1]; if (hi == '\\' && q[2]) { hi = q[2]; q++; } q += 2; }
        else hi = lo;
        if ((unsigned char)*s >= (unsigned char)lo && (unsigned char)*s <= (unsigned char)hi) ok = 1;
        first = 0;
      }
      if (*q != ']') { /* no closing bracket: literal [ */
        if (*s != '[') return 0;
        s++;
        break;
      }
      if (ok == neg) return 0;
      p = q + 1;
      s++;
      break;
    }
    case '\\':
      if (*p) c = *p++;
      /* fall through */
    default:
      if (*s != c) return 0;
      s++;
    }
  }
}
static void unescape_glob(buf *b, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) i++;
    bputc(b, s[i]);
  }
}
static void glob_rec(const char *base, const char *rest, strlist *out) {
  /* base: path built so far ("" or ends with '/'); rest: remaining pattern */
  const char *slash = strchr(rest, '/');
  size_t clen = slash ? (size_t)(slash - rest) : strlen(rest);
  char *comp = xstrndup(rest, clen);
  if (!has_glob_chars(comp)) {
    buf b = {0};
    bputs(&b, base);
    unescape_glob(&b, rest, clen);
    if (slash) {
      bputc(&b, '/');
      const char *r = slash + 1;
      while (*r == '/') { bputc(&b, '/'); r++; }
      if (*r) glob_rec(bstr(&b), r, out);
      else if (sys_isdir(bstr(&b))) sl_add(out, xstrdup(bstr(&b)));
    } else {
      sysstat st;
      if (sys_stat(bstr(&b), &st) == 0 && st.exists) sl_add(out, xstrdup(bstr(&b)));
    }
    bfree(&b);
    free(comp);
    return;
  }
  strlist names = {0};
  sys_list_dir(*base ? base : ".", &names);
  qsort(names.v, names.n, sizeof *names.v, str_cmp);
  for (int i = 0; i < names.n; i++) {
    const char *nm = names.v[i];
    if (nm[0] == '.' && comp[0] != '.') continue;
    if (!gmatch(comp, nm)) continue;
    buf b = {0};
    bputs(&b, base);
    bputs(&b, nm);
    if (slash) {
      const char *r = slash + 1;
      bputc(&b, '/');
      while (*r == '/') r++;
      if (*r) { if (sys_isdir(bstr(&b))) glob_rec(bstr(&b), r, out); }
      else if (sys_isdir(bstr(&b))) sl_add(out, xstrdup(bstr(&b)));
    } else sl_add(out, xstrdup(bstr(&b)));
    bfree(&b);
  }
  sl_free(&names);
  free(comp);
}
int glob_expand(const char *pat, strlist *out) {
  int before = out->n;
  buf tb = {0};
  if (pat[0] == '~') { /* tilde: only the plain ~ and ~/ forms */
    const char *e = pat + 1;
    while (*e && *e != '/') e++;
    if (e == pat + 1) {
      const char *home = getenv("HOME");
      if (home) { bputs(&tb, home); bputs(&tb, e); pat = bstr(&tb); }
    }
  }
  if (pat[0] == '/') {
    const char *r = pat;
    buf base = {0};
    while (*r == '/') { bputc(&base, '/'); r++; }
    if (*r) glob_rec(bstr(&base), r, out);
    else sl_add(out, xstrdup(bstr(&base)));
    bfree(&base);
  } else if (*pat) glob_rec("", pat, out);
  bfree(&tb);
  int n = out->n - before;
  if (n > 1) qsort(out->v + before, n, sizeof *out->v, str_cmp);
  return n;
}
