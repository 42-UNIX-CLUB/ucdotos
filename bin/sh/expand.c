// SPDX-License-Identifier: GPL-2.0-or-later
/* Word expansion: tilde, parameters, command substitution, arithmetic,
 * field splitting, pathname expansion (globbing) and quote removal. */
#include "sh.h"
#include <dirent.h>

/* An expanded word is a string of characters, each with flags. */
#define QQ 1 /* quoted: never split, never a glob character */
#define QE 2 /* produced by an unquoted expansion: subject to field splitting */
#define QB 4 /* hard field break, from "$@" (no character) */
#define QM 8 /* marks that a field exists even if empty, from "" (no character) */
#define QS 16 /* soft field break between the arguments of unquoted $@ and $* (a space) */
struct xb { char *c; unsigned char *q; int n, cap; };
static void xput(struct xb *b, char c, int q) {
  if (b->n + 1 >= b->cap) {
    b->cap = b->cap ? b->cap * 2 : 64;
    b->c = realloc(b->c, b->cap);
    b->q = realloc(b->q, b->cap);
  }
  b->c[b->n] = c; b->q[b->n++] = q;
}
static void xputs(struct xb *b, const char *s, int q) { while (*s) xput(b, *s++, q); }
static void xfree(struct xb *b) { free(b->c); free(b->q); }
int expand_error, subst_ran;
static void unset_error(const char *name) {
  if (!expand_error) outf(2, "sh: %s: parameter not set\n", name);
  expand_error = 1;
}

/* ---- parameters ---- */
static char numbuf[32];
static const char *special(char c) {
  switch (c) {
  case '?': snprintf(numbuf, sizeof numbuf, "%d", last_status); return numbuf;
  case '$': snprintf(numbuf, sizeof numbuf, "%d", getpid()); return numbuf;
  case '#': snprintf(numbuf, sizeof numbuf, "%d", frame->argc); return numbuf;
  case '!': if (!last_bg_pid) return ""; snprintf(numbuf, sizeof numbuf, "%d", last_bg_pid); return numbuf;
  case '-': return dash_flags();
  case '0': return shell_name;
  }
  if (c >= '1' && c <= '9') return c - '0' <= frame->argc ? frame->argv[c - '1'] : NULL;
  return NULL;
}
/* value of a named parameter; NULL if unset */
static const char *param(const char *name) {
  if (!name[1] && !isalpha((unsigned char)name[0]) && name[0] != '_') return special(name[0]);
  if (isdigit((unsigned char)name[0])) { int n = atoi(name); return n >= 1 && n <= frame->argc ? frame->argv[n - 1] : n == 0 ? shell_name : NULL; }
  if (name[0] == 'L' && !strcmp(name, "LINENO")) { snprintf(numbuf, sizeof numbuf, "%d", lineno); return numbuf; }
  return getvar(name);
}

static void xword(const char *s, struct xb *b, int dq);

/* "$@" gives one field per argument; "$*" joins them with the first character
 * of IFS; unquoted, both give one field per argument, each split further */
static void put_args(struct xb *b, int q, int at, int *sawat) {
  if (sawat) *sawat = 1;
  const char *ifs = getvar("IFS");
  for (int i = 0; i < frame->argc; i++) {
    if (q != QQ) { if (i) xput(b, ' ', QS); }
    else if (at) { if (i) xput(b, 0, QB); xput(b, 0, QM); }
    else if (i && (!ifs || *ifs)) xput(b, ifs ? *ifs : ' ', QQ);
    xputs(b, frame->argv[i], q);
  }
}

static void put_subst(struct xb *b, const char *inner, int q) {
  int st;
  char *out = capture(inner, &st);
  size_t n = strlen(out);
  while (n && out[n - 1] == '\n') out[--n] = 0;
  xputs(b, out, q);
  free(out);
  last_status = st;
  subst_ran = 1;
}

/* find the end of a ${...} body starting at s[i] (just after the brace) */
static int brace_end(const char *s, int i) {
  int depth = 1;
  for (; s[i]; i++) {
    if (s[i] == '\\' && s[i + 1]) i++;
    else if (s[i] == '\'') { while (s[i + 1] && s[i + 1] != '\'') i++; i++; }
    else if (s[i] == '{') depth++;
    else if (s[i] == '}' && !--depth) return i;
  }
  return i;
}

static void do_brace(const char *body, struct xb *b, int q, int *sawat) {
  const char *p = body;
  int len_op = 0;
  if (*p == '#' && p[1]) { len_op = 1; p++; }
  char name[128];
  int n = 0;
  if (isalpha((unsigned char)*p) || *p == '_') while ((isalnum((unsigned char)*p) || *p == '_') && n < 127) name[n++] = *p++;
  else if (isdigit((unsigned char)*p)) while (isdigit((unsigned char)*p) && n < 127) name[n++] = *p++;
  else if (*p && strchr("?$#!-@*0", *p)) name[n++] = *p++;
  name[n] = 0;
  if (!n) { if (len_op && !*p) { xputs(b, special('#'), q); return; } outs(2, "sh: bad substitution\n"); expand_error = 1; return; }
  int is_at = !strcmp(name, "@") || !strcmp(name, "*");
  const char *val = is_at ? (frame->argc ? "" : NULL) : param(name);
  if (!val && !is_at && opt_u && (len_op || !*p || *p == '#' || *p == '%')) { unset_error(name); return; }
  if (len_op) {
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%d", is_at ? frame->argc : val ? (int)strlen(val) : 0);
    xputs(b, tmp, q);
    return;
  }
  if (!*p) {
    if (is_at) put_args(b, q, name[0] == '@', sawat);
    else if (val) xputs(b, val, q);
    return;
  }
  int colon = *p == ':';
  if (colon) p++;
  char op = *p++;
  int set = val && (!colon || *val);
  char *joined = NULL;
  if (is_at && val) { /* operate on "$*" for the pattern operators */
    struct xb t = {0};
    put_args(&t, QQ, 0, NULL);
    xput(&t, 0, 0);
    joined = strdup(t.c);
    xfree(&t);
    val = joined;
  }
  switch (op) {
  case '-': case '+':
    if ((op == '-') == !set) {
      if (q == QQ) { char *w = expand_str(p, 0); xputs(b, w, QQ); free(w); }
      else { /* unquoted: the word's own text is split like any expansion */
        int from = b->n;
        xword(p, b, 0);
        for (int k = from; k < b->n; k++) if (!b->q[k]) b->q[k] = QE;
      }
    } else if (op == '-') {
      if (is_at) put_args(b, q, name[0] == '@', sawat); else xputs(b, val, q);
    }
    break;
  case '=':
    if (!set) { char *w = expand_str(p, 0); setvar(name, w, 0); xputs(b, w, q); free(w); }
    else xputs(b, val, q);
    break;
  case '?':
    if (!set) {
      char *w = expand_str(p, 0);
      outf(2, "sh: %s: %s\n", name, *w ? w : "parameter null or not set");
      free(w);
      expand_error = 1;
    } else xputs(b, val, q);
    break;
  case '#': case '%': {
    int longest = *p == op;
    if (longest) p++;
    char *pat = expand_pattern(p, NULL);
    const char *v = val ? val : "";
    int L = strlen(v), done = 0;
    char *tmp = malloc(L + 1);
    for (int k = 0; k <= L && !done; k++) {
      int cut = op == '#' ? (longest ? L - k : k) : (longest ? k : L - k);
      if (op == '#') { memcpy(tmp, v, cut); tmp[cut] = 0; if (pmatch(pat, tmp)) { xputs(b, v + cut, q); done = 1; } }
      else if (pmatch(pat, v + cut)) { memcpy(tmp, v, cut); tmp[cut] = 0; xputs(b, tmp, q); done = 1; }
    }
    if (!done) xputs(b, v, q);
    free(tmp);
    free(pat);
    break;
  }
  default:
    outs(2, "sh: bad substitution\n");
    expand_error = 1;
  }
  free(joined);
}

/* s[i] == '$'. Appends the expansion and returns the index after it. */
static int do_dollar(const char *s, int i, struct xb *b, int q, int *sawat) {
  char c = s[i + 1];
  if (c == '(') {
    long se = subst_end(s, i);
    int end = se < 0 ? (int)strlen(s) : (int)se, j = se < 0 ? end : end - 1;
    if (s[i + 2] == '(' && j > i + 3 && s[j - 1] == ')') { /* $(( arithmetic )) */
      char *inner = strndup(s + i + 3, j - 1 - (i + 3));
      char *e = expand_str(inner, 1);
      int err = 0;
      long long v = arith(e, &err);
      char tmp[32];
      snprintf(tmp, sizeof tmp, "%lld", v);
      if (err) expand_error = 1; else xputs(b, tmp, q);
      free(inner); free(e);
    } else {
      char *inner = strndup(s + i + 2, j - (i + 2));
      put_subst(b, inner, q);
      free(inner);
    }
    return end;
  }
  if (c == '{') {
    int e = brace_end(s, i + 2);
    char *body = strndup(s + i + 2, e - (i + 2));
    do_brace(body, b, q, sawat);
    free(body);
    return s[e] ? e + 1 : e;
  }
  if (c == '@' || c == '*') { put_args(b, q, c == '@', sawat); return i + 2; }
  if (c && strchr("?$#!-0123456789", c)) {
    const char *v = special(c);
    if (v) xputs(b, v, q);
    else if (opt_u) { char nm[2] = {c, 0}; unset_error(nm); }
    return i + 2;
  }
  if (isalpha((unsigned char)c) || c == '_') {
    int j = i + 1;
    while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
    char *name = strndup(s + i + 1, j - i - 1);
    const char *v = param(name);
    if (v) xputs(b, v, q);
    else if (opt_u) unset_error(name);
    free(name);
    return j;
  }
  xput(b, '$', q == QE ? 0 : q);
  return i + 1;
}

static int do_bquote(const char *s, int i, struct xb *b, int q) {
  char *inner = malloc(strlen(s) + 1);
  int n = 0, j = i + 1;
  for (; s[j] && s[j] != '`'; j++) {
    if (s[j] == '\\' && (s[j + 1] == '`' || s[j + 1] == '\\' || s[j + 1] == '$')) j++;
    inner[n++] = s[j];
  }
  inner[n] = 0;
  put_subst(b, inner, q);
  free(inner);
  return s[j] ? j + 1 : j;
}

static void tilde(const char *s, int *i, struct xb *b) {
  int j = *i + 1;
  while (s[j] && s[j] != '/' && s[j] != ':' && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-' || s[j] == '.')) j++;
  if (s[j] && s[j] != '/' && s[j] != ':') { xput(b, '~', 0); (*i)++; return; }
  if (j == *i + 1) { const char *h = getvar("HOME"); xputs(b, h ? h : "/", QQ); }
  else { xputs(b, "/home/", QQ); for (int k = *i + 1; k < j; k++) xput(b, s[k], QQ); }
  *i = j;
}

/* dq: 1 = here-document body (like double quotes, but " is ordinary);
 * 2 = assignment value (tilde after = and :) */
static void xword(const char *s, struct xb *b, int dq) {
  int i = 0;
  if (dq != 1 && s[0] == '~') tilde(s, &i, b);
  while (s[i]) {
    char c = s[i];
    if (dq == 1) {
      if (c == '\\' && s[i + 1] && strchr("$`\\\n", s[i + 1])) { if (s[i + 1] != '\n') xput(b, s[i + 1], QQ); i += 2; }
      else if (c == '$') i = do_dollar(s, i, b, QQ, NULL);
      else if (c == '`') i = do_bquote(s, i, b, QQ);
      else xput(b, c, QQ), i++;
      continue;
    }
    if (c == '\'') {
      int j = i + 1;
      while (s[j] && s[j] != '\'') xput(b, s[j++], QQ);
      xput(b, 0, QM);
      i = s[j] ? j + 1 : j;
    } else if (c == '"') {
      int start = b->n, sawat = 0;
      for (i++; s[i] && s[i] != '"';) {
        if (s[i] == '\\' && s[i + 1] && strchr("$`\"\\\n", s[i + 1])) { if (s[i + 1] != '\n') xput(b, s[i + 1], QQ); i += 2; }
        else if (s[i] == '$') i = do_dollar(s, i, b, QQ, &sawat);
        else if (s[i] == '`') i = do_bquote(s, i, b, QQ);
        else xput(b, s[i++], QQ);
      }
      if (s[i]) i++;
      if (b->n == start && !sawat) xput(b, 0, QM);
    } else if (c == '\\') {
      if (s[i + 1] == '\n') i += 2;
      else if (s[i + 1]) { xput(b, s[i + 1], QQ); i += 2; }
      else { xput(b, '\\', QQ); i++; }
    } else if (c == '$') i = do_dollar(s, i, b, QE, NULL);
    else if (c == '`') i = do_bquote(s, i, b, QE);
    else {
      xput(b, c, 0);
      i++;
      if (dq == 2 && (c == ':' || c == '=') && s[i] == '~') tilde(s, &i, b);
    }
  }
}

/* ---- pattern matching: \x is a literal x ---- */
static int utf8len(const char *s) {
  unsigned char c = *s;
  return c < 0xc0 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
}
int pmatch(const char *p, const char *s) {
  for (; *p; p++) {
    switch (*p) {
    case '*':
      while (p[1] == '*') p++;
      if (!p[1]) return 1;
      for (; *s; s++) if (pmatch(p + 1, s)) return 1;
      return pmatch(p + 1, s);
    case '?':
      if (!*s) return 0;
      s += utf8len(s);
      break;
    case '[': {
      const char *q = p + 1;
      int neg = 0, ok = 0;
      if (*q == '!' || *q == '^') neg = 1, q++;
      const char *start = q;
      while (*q && (*q != ']' || q == start)) {
        if (q[0] == '[' && q[1] == ':') { /* [:class:] */
          const char *e = strstr(q + 2, ":]");
          if (e) {
            int n = e - (q + 2), c = (unsigned char)*s, m = 0;
            if (n == 5 && !strncmp(q + 2, "alpha", 5)) m = isalpha(c);
            else if (n == 5 && !strncmp(q + 2, "digit", 5)) m = isdigit(c);
            else if (n == 5 && !strncmp(q + 2, "alnum", 5)) m = isalnum(c);
            else if (n == 5 && !strncmp(q + 2, "upper", 5)) m = isupper(c);
            else if (n == 5 && !strncmp(q + 2, "lower", 5)) m = islower(c);
            else if (n == 5 && !strncmp(q + 2, "space", 5)) m = isspace(c);
            else if (n == 5 && !strncmp(q + 2, "blank", 5)) m = c == ' ' || c == '\t';
            else if (n == 5 && !strncmp(q + 2, "punct", 5)) m = ispunct(c);
            else if (n == 5 && !strncmp(q + 2, "print", 5)) m = isprint(c);
            else if (n == 5 && !strncmp(q + 2, "graph", 5)) m = isgraph(c);
            else if (n == 5 && !strncmp(q + 2, "cntrl", 5)) m = iscntrl(c);
            else if (n == 6 && !strncmp(q + 2, "xdigit", 6)) m = isxdigit(c);
            if (m && c) ok = 1;
            q = e + 2;
            continue;
          }
        }
        char lo = *q;
        if (lo == '\\' && q[1]) lo = *++q;
        char hi = lo;
        if (q[1] == '-' && q[2] && q[2] != ']') { hi = q[2]; q += 2; }
        if ((unsigned char)*s >= (unsigned char)lo && (unsigned char)*s <= (unsigned char)hi) ok = 1;
        q++;
      }
      if (!*q) { if (*s != '[') return 0; s++; break; } /* no closing ]: a literal [ */
      if (!*s || ok == neg) return 0;
      s++;
      p = q;
      break;
    }
    case '\\':
      if (p[1]) p++;
      /* fall through */
    default:
      if (*s != *p) return 0;
      s++;
    }
  }
  return !*s;
}

/* turn characters [from, to) of an expanded word into a pattern string */
static char *to_pattern(struct xb *b, int from, int to, int *has_glob) {
  char *r = malloc(2 * (to - from) + 1), *o = r;
  if (has_glob) *has_glob = 0;
  for (int k = from; k < to; k++) {
    if (b->q[k] & (QB | QM)) continue;
    char c = b->c[k];
    if (c == '\\' || ((b->q[k] & QQ) && strchr("*?[]", c))) *o++ = '\\';
    else if (has_glob && strchr("*?[", c)) *has_glob = 1;
    *o++ = c;
  }
  *o = 0;
  return r;
}
static char *unescape(const char *p) {
  char *r = malloc(strlen(p) + 1), *o = r;
  for (; *p; p++) { if (*p == '\\' && p[1]) p++; *o++ = *p; }
  *o = 0;
  return r;
}

struct sv { char **v; int n, cap; };
static void svpush(struct sv *v, char *s) {
  if (v->n + 2 > v->cap) v->v = realloc(v->v, (v->cap = v->cap ? v->cap * 2 : 16) * sizeof *v->v);
  v->v[v->n++] = s;
  v->v[v->n] = NULL;
}
static int has_meta(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) { if (p[i] == '\\') i++; else if (strchr("*?[", p[i])) return 1; }
  return 0;
}
static int is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }

static void glob_dir(const char *prefix, const char *pat, struct sv *out) {
  const char *slash = strchr(pat, '/');
  size_t cl = slash ? (size_t)(slash - pat) : strlen(pat);
  const char *rest = slash ? slash + 1 : NULL;
  while (rest && *rest == '/') rest++;
  char *comp = strndup(pat, cl);
  if (!has_meta(comp, cl)) {
    char *lit = unescape(comp);
    char *path = malloc(strlen(prefix) + strlen(lit) + 2);
    sprintf(path, "%s%s", prefix, lit);
    struct stat st;
    if (rest && *rest && is_dir(path)) { strcat(path, "/"); glob_dir(path, rest, out); free(path); }
    else if (rest && !*rest && is_dir(path)) { strcat(path, "/"); svpush(out, path); }
    else if (!rest && stat(path, &st) == 0) svpush(out, path);
    else free(path);
    free(lit);
    free(comp);
    return;
  }
  DIR *d = opendir(*prefix ? prefix : ".");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if (e->d_name[0] == '.' && comp[0] != '.') continue;
      if (!pmatch(comp, e->d_name)) continue;
      char *path = malloc(strlen(prefix) + strlen(e->d_name) + 2);
      sprintf(path, "%s%s", prefix, e->d_name);
      if (rest) {
        if (is_dir(path)) { strcat(path, "/"); if (*rest) { glob_dir(path, rest, out); free(path); } else svpush(out, path); }
        else free(path);
      } else svpush(out, path);
    }
    closedir(d);
  }
  free(comp);
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void emit(struct xb *b, int from, int to, int doglob, struct sv *out) {
  int g;
  char *pat = to_pattern(b, from, to, &g);
  if (doglob && g) {
    int before = out->n;
    if (pat[0] == '/') glob_dir("/", pat + 1, out); else glob_dir("", pat, out);
    if (out->n > before) {
      qsort(out->v + before, out->n - before, sizeof(char *), cmpstr);
      free(pat);
      return;
    }
  }
  svpush(out, unescape(pat));
  free(pat);
}

/* field splitting (POSIX 2.6.5): only characters that came from unquoted
 * expansions (QE) split. IFS white space around a field is dropped; any other
 * IFS character ends a field, with the white space next to it */
static void split(struct xb *b, struct sv *out, int doglob) {
  const char *ifs = getvar("IFS");
  if (!ifs) ifs = " \t\n";
  if (opt_f) doglob = 0;
#define IFSCH(k) ((b->q[k] & QE) && *ifs && b->c[k] && strchr(ifs, b->c[k]))
#define IFSWS(k) (IFSCH(k) && isspace((unsigned char)b->c[k]))
  int start = 0, exists = 0;
  for (int k = 0; k < b->n; k++) {
    unsigned char q = b->q[k];
    if (q & QB) { emit(b, start, k, doglob, out); start = k + 1; exists = 0; continue; }
    if (q & QS) { if (exists) emit(b, start, k, doglob, out); start = k + 1; exists = 0; continue; }
    if (q & QM) { exists = 1; continue; }
    if (IFSCH(k)) {
      if (IFSWS(k)) {
        int had = exists;
        if (had) emit(b, start, k, doglob, out);
        while (k + 1 < b->n && IFSWS(k + 1)) k++;
        if (had && k + 1 < b->n && IFSCH(k + 1)) { k++; while (k + 1 < b->n && IFSWS(k + 1)) k++; }
      } else {
        emit(b, start, k, doglob, out);
        while (k + 1 < b->n && IFSWS(k + 1)) k++;
      }
      start = k + 1;
      exists = 0;
      continue;
    }
    exists = 1;
  }
  if (exists) emit(b, start, b->n, doglob, out);
#undef IFSCH
#undef IFSWS
}

int expand_words(char **raw, int n, char ***out) {
  struct sv v = {0};
  svpush(&v, NULL); v.n = 0;
  for (int i = 0; i < n && !expand_error; i++) {
    struct xb b = {0};
    xword(raw[i], &b, 0);
    split(&b, &v, 1);
    xfree(&b);
  }
  *out = v.v;
  return v.n;
}
char *expand_str(const char *raw, int heredoc) {
  struct xb b = {0};
  xword(raw, &b, heredoc ? 1 : 2);
  char *r = malloc(b.n + 1), *o = r;
  for (int k = 0; k < b.n; k++) {
    if (b.q[k] & QM) continue;
    *o++ = (b.q[k] & QB) ? ' ' : b.c[k];
  }
  *o = 0;
  xfree(&b);
  return r;
}
char *expand_pattern(const char *raw, unsigned char **unused) {
  (void)unused;
  struct xb b = {0};
  xword(raw, &b, 0);
  char *r = to_pattern(&b, 0, b.n, NULL);
  xfree(&b);
  return r;
}
void free_argv(char **v) {
  if (!v) return;
  for (char **p = v; *p; p++) free(*p);
  free(v);
}

/* ---- $(( arithmetic )) ---- */
struct ar { const char *s; int err, noeval; };
static long long a_expr(struct ar *a);
static void a_ws(struct ar *a) { while (isspace((unsigned char)*a->s)) a->s++; }
static int a_eat(struct ar *a, const char *op) {
  a_ws(a);
  size_t n = strlen(op);
  if (strncmp(a->s, op, n)) return 0;
  /* do not take "<" out of "<=" or "<<", "&" out of "&&" and so on */
  if (n == 1 && strchr("<>&|=!*+-", op[0]) && (a->s[1] == '=' || (a->s[1] == op[0] && op[0] != '!'))) return 0;
  if (n == 2 && strchr("<>", op[0]) && op[1] == op[0] && a->s[2] == '=') return 0;
  a->s += n;
  return 1;
}
/* a variable in arithmetic: a number, or else an expression of its own */
static long long varval(const char *name) {
  static int depth;
  const char *v = getvar(name);
  if (!v || !*v) return 0;
  char *e;
  long long n = strtoll(v, &e, 0);
  while (isspace((unsigned char)*e)) e++;
  if (!*e || depth > 16) return n;
  depth++;
  int err = 0;
  n = arith(v, &err);
  depth--;
  return n;
}
static void setnum(struct ar *a, const char *name, long long v) {
  if (a->noeval) return;
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%lld", v);
  setvar(name, tmp, 0);
}
static long long a_primary(struct ar *a) {
  a_ws(a);
  if (*a->s == '(') {
    a->s++;
    long long v = a_expr(a);
    a_ws(a);
    if (*a->s == ')') a->s++; else a->err = 1;
    return v;
  }
  if (isdigit((unsigned char)*a->s)) { char *e; long long v = strtoll(a->s, &e, 0); a->s = e; return v; }
  if (isalpha((unsigned char)*a->s) || *a->s == '_') {
    const char *st = a->s;
    while (isalnum((unsigned char)*a->s) || *a->s == '_') a->s++;
    char name[64];
    size_t n = a->s - st < 63 ? (size_t)(a->s - st) : 63;
    memcpy(name, st, n); name[n] = 0;
    long long v = varval(name);
    a_ws(a);
    if (!strncmp(a->s, "++", 2)) { a->s += 2; setnum(a, name, v + 1); }
    else if (!strncmp(a->s, "--", 2)) { a->s += 2; setnum(a, name, v - 1); }
    return v;
  }
  a->err = 1;
  return 0;
}
static long long a_unary(struct ar *a) {
  a_ws(a);
  if (!strncmp(a->s, "++", 2) || !strncmp(a->s, "--", 2)) {
    int inc = a->s[0] == '+';
    a->s += 2;
    a_ws(a);
    const char *st = a->s;
    while (isalnum((unsigned char)*a->s) || *a->s == '_') a->s++;
    char name[64];
    size_t n = a->s - st < 63 ? (size_t)(a->s - st) : 63;
    memcpy(name, st, n); name[n] = 0;
    if (!n) { a->err = 1; return 0; }
    long long v = varval(name) + (inc ? 1 : -1);
    setnum(a, name, v);
    return v;
  }
  if (*a->s == '!') { a->s++; return !a_unary(a); }
  if (*a->s == '~') { a->s++; return ~a_unary(a); }
  if (*a->s == '-') { a->s++; return -a_unary(a); }
  if (*a->s == '+') { a->s++; return a_unary(a); }
  return a_primary(a);
}
static long long a_mul(struct ar *a) {
  long long v = a_unary(a);
  for (;;) {
    if (a_eat(a, "*")) v *= a_unary(a);
    else if (a_eat(a, "/") || a_eat(a, "%")) {
      char op = a->s[-1];
      long long r = a_unary(a);
      if (!r) { if (!a->noeval) { outs(2, "sh: division by zero\n"); a->err = 1; } v = 0; }
      else v = op == '/' ? v / r : v % r;
    } else return v;
  }
}
static long long a_add(struct ar *a) {
  long long v = a_mul(a);
  for (;;) {
    if (a_eat(a, "+")) v += a_mul(a);
    else if (a_eat(a, "-")) v -= a_mul(a);
    else return v;
  }
}
static long long a_shift(struct ar *a) {
  long long v = a_add(a);
  for (;;) {
    if (a_eat(a, "<<")) v <<= a_add(a);
    else if (a_eat(a, ">>")) v >>= a_add(a);
    else return v;
  }
}
static long long a_rel(struct ar *a) {
  long long v = a_shift(a);
  for (;;) {
    if (a_eat(a, "<=")) v = v <= a_shift(a);
    else if (a_eat(a, ">=")) v = v >= a_shift(a);
    else if (a_eat(a, "<")) v = v < a_shift(a);
    else if (a_eat(a, ">")) v = v > a_shift(a);
    else return v;
  }
}
static long long a_eq(struct ar *a) {
  long long v = a_rel(a);
  for (;;) {
    if (a_eat(a, "==")) v = v == a_rel(a);
    else if (a_eat(a, "!=")) v = v != a_rel(a);
    else return v;
  }
}
static long long a_band(struct ar *a) { long long v = a_eq(a); while (a_eat(a, "&")) v &= a_eq(a); return v; }
static long long a_bxor(struct ar *a) { long long v = a_band(a); while (a_eat(a, "^")) v ^= a_band(a); return v; }
static long long a_bor(struct ar *a) { long long v = a_bxor(a); while (a_eat(a, "|")) v |= a_bxor(a); return v; }
static long long a_land(struct ar *a) {
  long long v = a_bor(a);
  while (a_eat(a, "&&")) { if (!v) a->noeval++; long long r = a_bor(a); if (!v) a->noeval--; v = v && r; }
  return v;
}
static long long a_lor(struct ar *a) {
  long long v = a_land(a);
  while (a_eat(a, "||")) { if (v) a->noeval++; long long r = a_land(a); if (v) a->noeval--; v = v || r; }
  return v;
}
static long long a_cond(struct ar *a) {
  long long c = a_lor(a);
  if (!a_eat(a, "?")) return c;
  if (!c) a->noeval++;
  long long x = a_expr(a);
  if (!c) a->noeval--;
  if (!a_eat(a, ":")) { a->err = 1; return 0; }
  if (c) a->noeval++;
  long long y = a_cond(a);
  if (c) a->noeval--;
  return c ? x : y;
}
static long long a_expr(struct ar *a) {
  a_ws(a);
  const char *save = a->s;
  if (isalpha((unsigned char)*a->s) || *a->s == '_') {
    const char *st = a->s;
    while (isalnum((unsigned char)*a->s) || *a->s == '_') a->s++;
    char name[64];
    size_t n = a->s - st < 63 ? (size_t)(a->s - st) : 63;
    memcpy(name, st, n); name[n] = 0;
    a_ws(a);
    static const char *ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|=", 0};
    for (int i = 0; ops[i]; i++) {
      size_t l = strlen(ops[i]);
      if (strncmp(a->s, ops[i], l) || (l == 1 && a->s[1] == '=')) continue;
      a->s += l;
      long long r = a_expr(a), v = varval(name);
      switch (ops[i][0]) {
      case '=': v = r; break;
      case '+': v += r; break;
      case '-': v -= r; break;
      case '*': v *= r; break;
      case '/': case '%':
        if (!r) { if (!a->noeval) { outs(2, "sh: division by zero\n"); a->err = 1; } v = 0; }
        else v = ops[i][0] == '/' ? v / r : v % r;
        break;
      case '<': v <<= r; break;
      case '>': v >>= r; break;
      case '&': v &= r; break;
      case '^': v ^= r; break;
      case '|': v |= r; break;
      }
      setnum(a, name, v);
      return v;
    }
    a->s = save;
  }
  return a_cond(a);
}
long long arith(const char *expr, int *err) {
  struct ar a = {expr, 0, 0};
  long long v = a_expr(&a);
  a_ws(&a);
  if (*a.s && !a.err) a.err = 1;
  if (a.err) { if (err) *err = 1; if (*a.s) outf(2, "sh: arithmetic syntax error: %s\n", expr); }
  return v;
}
