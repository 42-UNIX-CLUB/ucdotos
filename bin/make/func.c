// SPDX-License-Identifier: GPL-2.0-or-later
/* make: the built-in functions, $(subst ...), $(foreach ...), $(shell ...) and the rest. */
#include "make.h"

typedef void (*fn_t)(buf *out, char **argv, int argc, const char *fname);
struct func { const char *name; int minargs, maxargs, expand; fn_t fn; };

static void each_word(const char *s, void (*cb)(buf *, const char *, size_t, void *), buf *out, void *ctx) {
  const char *p = s;
  size_t len;
  const char *w;
  while ((w = next_word(&p, &len))) cb(out, w, len, ctx);
}
static void sep(buf *out, size_t start) { if (out->n > start) bputc(out, ' '); }

static void f_subst(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *from = a[0], *to = a[1], *t = a[2];
  size_t fl = strlen(from), tl = strlen(to);
  if (!fl) { bputs(out, t); bput(out, to, tl); return; }
  const char *p;
  while ((p = strstr(t, from))) { bput(out, t, p - t); bput(out, to, tl); t = p + fl; }
  bputs(out, t);
}
void patsubst_into(buf *out, const char *pat, const char *rep, const char *text) {
  const char *pp, *rp;
  char *upat = unescape_percent(pat, &pp);
  char *urep = unescape_percent(rep, &rp);
  if (!pp) rp = 0; /* without % in the pattern, the replacement is literal */
  const char *p = text;
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) {
    sep(out, start);
    size_t so, sl;
    if (pattern_match(upat, pp, w, len, &so, &sl)) {
      if (rp) pattern_subst(out, urep, rp, w + so, sl);
      else bputs(out, urep);
    } else bput(out, w, len);
  }
  free(upat); free(urep);
}
static void f_patsubst(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; patsubst_into(out, a[0], a[1], a[2]); }
static void f_strip(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; strip_into(out, a[0], strlen(a[0])); }
static void f_findstring(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; if (strstr(a[1], a[0])) bputs(out, a[0]); }

struct pats { int n; char **p; const char **pct; };
static void pats_make(struct pats *ps, const char *s) {
  ps->n = 0; ps->p = 0; ps->pct = 0;
  const char *p = s;
  size_t len;
  const char *w;
  int cap = 0;
  while ((w = next_word(&p, &len))) {
    if (ps->n >= cap) { cap = cap ? cap * 2 : 8; ps->p = xrealloc(ps->p, cap * sizeof *ps->p); ps->pct = xrealloc(ps->pct, cap * sizeof *ps->pct); }
    char *t = xstrndup(w, len);
    ps->p[ps->n] = unescape_percent(t, &ps->pct[ps->n]);
    free(t);
    ps->n++;
  }
}
static int pats_match(struct pats *ps, const char *w, size_t len) {
  for (int i = 0; i < ps->n; i++) if (pattern_match(ps->p[i], ps->pct[i], w, len, 0, 0)) return 1;
  return 0;
}
static void pats_free(struct pats *ps) {
  for (int i = 0; i < ps->n; i++) free(ps->p[i]);
  free(ps->p); free(ps->pct);
}
static void filter_common(buf *out, char **a, int neg) {
  struct pats ps;
  pats_make(&ps, a[0]);
  const char *p = a[1];
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) {
    if (pats_match(&ps, w, len) != neg) { sep(out, start); bput(out, w, len); }
  }
  pats_free(&ps);
}
static void f_filter(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; filter_common(out, a, 0); }
static void f_filter_out(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; filter_common(out, a, 1); }

static void f_sort(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  strlist l = {0};
  const char *p = a[0];
  size_t len;
  const char *w;
  while ((w = next_word(&p, &len))) sl_add(&l, xstrndup(w, len));
  if (l.n > 1) qsort(l.v, l.n, sizeof *l.v, str_cmp);
  size_t start = out->n;
  for (int i = 0; i < l.n; i++) {
    if (i && !strcmp(l.v[i], l.v[i - 1])) continue;
    sep(out, start);
    bputs(out, l.v[i]);
  }
  sl_free(&l);
}
static long get_num(const char *s, const char *fname, const char *which) {
  char *t = xstrdup(s);
  buf b = {0};
  strip_into(&b, t, strlen(t));
  free(t);
  const char *v = bstr(&b);
  const char *q = v;
  if (*q == '-' || *q == '+') q++;
  int ok = *q != 0;
  for (; *q; q++) if (!isdigit((unsigned char)*q)) ok = 0;
  if (!ok) fatal_at(read_file_name, read_line, "%s %s argument to '%s' function: '%s'",
                    strcmp(fname, "intcmp") ? "invalid" : "non-numeric", which, fname, v);
  long r = strtol(v, 0, 10);
  bfree(&b);
  return r;
}
static int count_words(const char *s) {
  int c = 0;
  size_t len;
  while (next_word(&s, &len)) c++;
  return c;
}
static void f_word(buf *out, char **a, int n, const char *fn) {
  (void)n;
  long k = get_num(a[0], fn, "first");
  if (k <= 0) fatal_at(read_file_name, read_line, "first argument to 'word' function must be greater than 0");
  const char *p = a[1];
  size_t len;
  const char *w;
  while ((w = next_word(&p, &len))) if (--k == 0) { bput(out, w, len); return; }
}
static void f_wordlist(buf *out, char **a, int n, const char *fn) {
  (void)n;
  long s = get_num(a[0], fn, "first");
  long e = get_num(a[1], fn, "second");
  if (s <= 0) fatal_at(read_file_name, read_line, "invalid first argument to 'wordlist' function: '%ld'", s);
  if (e < 0) fatal_at(read_file_name, read_line, "invalid second argument to 'wordlist' function: '%ld'", e);
  const char *p = a[2];
  size_t len, start = out->n;
  const char *w;
  long i = 0;
  while ((w = next_word(&p, &len))) {
    i++;
    if (i >= s && i <= e) { sep(out, start); bput(out, w, len); }
  }
}
static void f_words(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  char t[24];
  snprintf(t, sizeof t, "%d", count_words(a[0]));
  bputs(out, t);
}
static void f_firstword(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[0];
  size_t len;
  const char *w = next_word(&p, &len);
  if (w) bput(out, w, len);
}
static void f_lastword(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[0], *last = 0;
  size_t len, ll = 0;
  const char *w;
  while ((w = next_word(&p, &len))) { last = w; ll = len; }
  if (last) bput(out, last, ll);
}
static void cb_dir(buf *out, const char *w, size_t n, void *ctx) { sep(out, (size_t)ctx); path_dir(out, w, n); }
static void cb_notdir(buf *out, const char *w, size_t n, void *ctx) {
  size_t st = (size_t)ctx;
  size_t i = n;
  while (i > 0 && w[i - 1] != '/') i--;
  sep(out, st);
  bput(out, w + i, n - i);
}
static long suffix_pos(const char *w, size_t n) {
  for (size_t i = n; i > 0; i--) {
    if (w[i - 1] == '/') return -1;
    if (w[i - 1] == '.') return i - 1;
  }
  return -1;
}
static void cb_suffix(buf *out, const char *w, size_t n, void *ctx) {
  long d = suffix_pos(w, n);
  if (d >= 0) { sep(out, (size_t)ctx); bput(out, w + d, n - d); }
}
static void cb_basename(buf *out, const char *w, size_t n, void *ctx) {
  long d = suffix_pos(w, n);
  sep(out, (size_t)ctx);
  bput(out, w, d >= 0 ? (size_t)d : n);
}
static void f_dir(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; each_word(a[0], cb_dir, out, (void *)out->n); }
static void f_notdir(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; each_word(a[0], cb_notdir, out, (void *)out->n); }
static void f_suffix(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; each_word(a[0], cb_suffix, out, (void *)out->n); }
static void f_basename(buf *out, char **a, int n, const char *fn) { (void)n; (void)fn; each_word(a[0], cb_basename, out, (void *)out->n); }
static void f_addsuffix(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[1];
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) { sep(out, start); bput(out, w, len); bputs(out, a[0]); }
}
static void f_addprefix(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[1];
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) { sep(out, start); bputs(out, a[0]); bput(out, w, len); }
}
static void f_join(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[0], *q = a[1];
  size_t l1, l2, start = out->n;
  for (;;) {
    const char *w1 = next_word(&p, &l1), *w2 = next_word(&q, &l2);
    if (!w1 && !w2) break;
    sep(out, start);
    if (w1) bput(out, w1, l1);
    if (w2) bput(out, w2, l2);
  }
}
static void f_wildcard(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[0];
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) {
    char *pat = xstrndup(w, len);
    strlist l = {0};
    glob_expand(pat, &l);
    for (int i = 0; i < l.n; i++) { sep(out, start); bputs(out, l.v[i]); }
    sl_free(&l);
    free(pat);
  }
}
static void f_realpath(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[0];
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) {
    char *path = xstrndup(w, len);
    char *ab = abspath_of(path, make_cwd);
    sysstat st;
    if (sys_stat(ab, &st) == 0 && st.exists) { sep(out, start); bputs(out, ab); }
    free(ab); free(path);
  }
}
static void f_abspath(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  const char *p = a[0];
  size_t len, start = out->n;
  const char *w;
  while ((w = next_word(&p, &len))) {
    char *path = xstrndup(w, len);
    char *ab = abspath_of(path, make_cwd);
    sep(out, start);
    bputs(out, ab);
    free(ab); free(path);
  }
}

/* lazy functions get their arguments unexpanded */
static char *strip_dup(const char *s) {
  buf b = {0};
  const char *e = s + strlen(s);
  while (is_space(*s)) s++;
  while (e > s && is_space(e[-1])) e--;
  bput(&b, s, e - s);
  char *r = bdetach(&b);
  return r ? r : xstrdup("");
}
static void f_if(buf *out, char **a, int n, const char *fn) {
  (void)fn;
  char *c = expand(a[0]);
  char *s = strip_dup(c);
  int t = *s != 0;
  free(c); free(s);
  if (t) expand_into(out, a[1], strlen(a[1]));
  else if (n > 2) expand_into(out, a[2], strlen(a[2]));
}
static void f_or(buf *out, char **a, int n, const char *fn) {
  (void)fn;
  for (int i = 0; i < n; i++) {
    char *e = expand(a[i]);
    char *s = strip_dup(e);
    free(e);
    if (*s) { bputs(out, s); free(s); return; }
    free(s);
  }
}
static void f_and(buf *out, char **a, int n, const char *fn) {
  (void)fn;
  char *last = 0;
  for (int i = 0; i < n; i++) {
    free(last);
    char *e = expand(a[i]);
    last = strip_dup(e);
    free(e);
    if (!*last) { free(last); return; }
  }
  if (last) bputs(out, last);
  free(last);
}
static void f_intcmp(buf *out, char **a, int n, const char *fn) {
  (void)fn;
  char *l = expand(a[0]), *r = expand(a[1]);
  long long x = get_num(l, "intcmp", "first"), y = get_num(r, "intcmp", "second");
  free(l); free(r);
  int c = x < y ? -1 : x > y ? 1 : 0;
  if (n == 2) { if (c == 0) { char t[32]; snprintf(t, sizeof t, "%lld", x); bputs(out, t); } return; }
  int idx;
  if (c < 0) idx = 2;
  else if (c == 0) idx = n > 3 ? 3 : -1;
  else idx = n > 4 ? 4 : n > 3 ? 3 : -1;
  if (idx < 0) return;
  expand_into(out, a[idx], strlen(a[idx]));
}
static void push_tmp(const char *name, const char *value) {
  tmpvar *t = xcalloc(1, sizeof *t);
  t->name = xstrdup(name);
  t->nlen = strlen(name);
  t->v.name = (char *)t->name;
  t->v.value = xstrdup(value);
  t->v.vlen = strlen(value);
  t->v.flavor = F_SIMPLE;
  t->v.origin = O_AUTOMATIC;
  t->next = tmpvars;
  tmpvars = t;
}
static void pop_tmp(void) {
  tmpvar *t = tmpvars;
  tmpvars = t->next;
  free((char *)t->name);
  free(t->v.value);
  free(t);
}
static void f_foreach(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  char *vn = expand(a[0]);
  char *vname = strip_dup(vn);
  free(vn);
  char *list = expand(a[1]);
  const char *p = list;
  size_t len, start = out->n;
  const char *w;
  push_tmp(vname, "");
  tmpvar *t = tmpvars;
  int iter = 0;
  (void)start;
  while ((w = next_word(&p, &len))) {
    free(t->v.value);
    t->v.value = xstrndup(w, len);
    t->v.vlen = len;
    if (iter++) bputc(out, ' '); /* even after an empty result, as GNU Make does */
    expand_into(out, a[2], strlen(a[2]));
  }
  pop_tmp();
  free(vname);
  free(list);
}
static void f_let(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  char *names = expand(a[0]);
  char *list = expand(a[1]);
  const char *pn = names, *pl = list;
  size_t nl, ll;
  const char *nw, *lw;
  int pushed = 0;
  while ((nw = next_word(&pn, &nl))) {
    char *nm = xstrndup(nw, nl);
    const char *save = pn;
    size_t dummy;
    int last = next_word(&save, &dummy) == 0;
    if (last) {
      const char *rest = skip_ws(pl);
      size_t rl = strlen(rest);
      while (rl && is_space(rest[rl - 1])) rl--;
      char *r = xstrndup(rest, rl);
      push_tmp(nm, r);
      free(r);
    } else {
      lw = next_word(&pl, &ll);
      char *v = lw ? xstrndup(lw, ll) : xstrdup("");
      push_tmp(nm, v);
      free(v);
    }
    free(nm);
    pushed++;
  }
  expand_into(out, a[2], strlen(a[2]));
  while (pushed--) pop_tmp();
  free(names); free(list);
}
static void f_call(buf *out, char **a, int n, const char *fn) {
  (void)fn;
  char *name = strip_dup(a[0]);
  if (!*name) { free(name); return; }
  size_t nl = strlen(name);
  if (is_function_name(name, nl)) {
    /* $(call info,...) calls the function directly */
    buf args = {0};
    for (int i = 1; i < n; i++) {
      if (i > 1) bputc(&args, ',');
      for (const char *q = a[i]; *q; q++) { if (*q == '$') bputc(&args, '$'); bputc(&args, *q); }
    }
    call_function(out, name, nl, bstr(&args), args.n, '(');
    bfree(&args);
    free(name);
    return;
  }
  var *v = var_lookup(name, nl);
  if (!v) { free(name); return; }
  tmpvar *frame = xcalloc(1, sizeof *frame);
  frame->is_call = 1;
  frame->nargs = n - 1;
  frame->next = tmpvars;
  tmpvars = frame;
  char num[16];
  push_tmp("0", name);
  int pushed = 1;
  for (int i = 1; i < n; i++) {
    snprintf(num, sizeof num, "%d", i);
    push_tmp(num, a[i]);
    pushed++;
  }
  /* the body: $(name) with the numbered variables visible */
  if (v->flavor == F_SIMPLE) bputs(out, v->value);
  else {
    if (v->expanding) fatal_at(read_file_name, read_line, "Recursive variable '%s' references itself (eventually)", v->name);
    v->expanding = 1;
    char *val = xstrdup(v->value);
    expand_into(out, val, strlen(val));
    free(val);
    v->expanding = 0;
  }
  while (pushed--) pop_tmp();
  tmpvars = frame->next;
  free(frame);
  free(name);
}
static void f_value(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  var *v = var_lookup(a[0], strlen(a[0]));
  if (v) bputs(out, v->value);
}
static void f_eval(buf *out, char **a, int n, const char *fn) {
  (void)out; (void)n; (void)fn;
  read_text(a[0], read_file_name, read_line);
}
static void f_origin(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  var *v = var_lookup(a[0], strlen(a[0]));
  static const char *names[] = {"default", "environment", "file", "environment override", "command line", "override", "automatic"};
  bputs(out, v ? names[v->origin] : "undefined");
}
static void f_flavor(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  var *v = var_lookup(a[0], strlen(a[0]));
  bputs(out, !v ? "undefined" : v->flavor == F_SIMPLE ? "simple" : "recursive");
}
static void f_shell(buf *out, char **a, int n, const char *fn) {
  (void)n; (void)fn;
  int st;
  char *r = run_shell_capture(a[0], &st, 0);
  bputs(out, r);
  free(r);
  char sb[16];
  snprintf(sb, sizeof sb, "%d", st);
  var_set(&globals, ".SHELLSTATUS", sb, F_SIMPLE, O_FILE);
}
static void f_error(buf *out, char **a, int n, const char *fn) {
  (void)out; (void)n; (void)fn;
  fatal_at(read_file_name, read_line, "%s", a[0]);
}
static void f_warning(buf *out, char **a, int n, const char *fn) {
  (void)out; (void)n; (void)fn;
  warn_at(read_file_name, read_line, "%s", a[0]);
}
static void f_info(buf *out, char **a, int n, const char *fn) {
  (void)out; (void)n; (void)fn;
  fputs(a[0], stdout);
  fputc('\n', stdout);
  fflush(stdout);
}
static void f_file(buf *out, char **a, int n, const char *fn) {
  (void)fn;
  const char *op = skip_ws(a[0]);
  if (op[0] == '>') {
    int app = op[1] == '>';
    const char *nm = skip_ws(op + 1 + app);
    char *name = strip_dup(nm);
    if (!*name) fatal_at(read_file_name, read_line, "file: missing filename");
    FILE *f = fopen(name, app ? "a" : "w");
    if (!f) fatal_at(read_file_name, read_line, "open: %s: %s", name, strerror(errno));
    if (n > 1) {
      size_t l = strlen(a[1]);
      fwrite(a[1], 1, l, f);
      if (!l || a[1][l - 1] != '\n') fputc('\n', f);
    }
    fclose(f);
    stat_forget(name);
    free(name);
  } else if (op[0] == '<') {
    char *name = strip_dup(op + 1);
    if (n > 1) fatal_at(read_file_name, read_line, "file: too many arguments");
    FILE *f = fopen(name, "r");
    if (f) {
      buf b = {0};
      char tmp[4096];
      size_t r;
      while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) bput(&b, tmp, r);
      fclose(f);
      if (b.n && b.s[b.n - 1] == '\n') b.n--;
      bput(out, b.s ? b.s : "", b.n);
      bfree(&b);
    }
    free(name);
  } else fatal_at(read_file_name, read_line, "file: invalid file operation: %s", op);
}

static const struct func funcs[] = {
  {"abspath", 0, 1, 1, f_abspath},
  {"addprefix", 2, 2, 1, f_addprefix},
  {"addsuffix", 2, 2, 1, f_addsuffix},
  {"and", 1, 0, 0, f_and},
  {"basename", 0, 1, 1, f_basename},
  {"call", 1, 0, 1, f_call},
  {"dir", 0, 1, 1, f_dir},
  {"error", 0, 1, 1, f_error},
  {"eval", 0, 1, 1, f_eval},
  {"file", 1, 2, 1, f_file},
  {"filter", 2, 2, 1, f_filter},
  {"filter-out", 2, 2, 1, f_filter_out},
  {"findstring", 2, 2, 1, f_findstring},
  {"firstword", 0, 1, 1, f_firstword},
  {"flavor", 0, 1, 1, f_flavor},
  {"foreach", 3, 3, 0, f_foreach},
  {"if", 2, 3, 0, f_if},
  {"info", 0, 1, 1, f_info},
  {"intcmp", 2, 5, 0, f_intcmp},
  {"join", 2, 2, 1, f_join},
  {"lastword", 0, 1, 1, f_lastword},
  {"let", 3, 3, 0, f_let},
  {"notdir", 0, 1, 1, f_notdir},
  {"or", 1, 0, 0, f_or},
  {"origin", 0, 1, 1, f_origin},
  {"patsubst", 3, 3, 1, f_patsubst},
  {"realpath", 0, 1, 1, f_realpath},
  {"shell", 0, 1, 1, f_shell},
  {"sort", 0, 1, 1, f_sort},
  {"strip", 0, 1, 1, f_strip},
  {"subst", 3, 3, 1, f_subst},
  {"suffix", 0, 1, 1, f_suffix},
  {"value", 0, 1, 1, f_value},
  {"warning", 0, 1, 1, f_warning},
  {"wildcard", 0, 1, 1, f_wildcard},
  {"word", 2, 2, 1, f_word},
  {"wordlist", 3, 3, 1, f_wordlist},
  {"words", 0, 1, 1, f_words},
};
#define NFUNCS (int)(sizeof funcs / sizeof funcs[0])

static const struct func *find_func(const char *s, size_t n) {
  int lo = 0, hi = NFUNCS - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int c = strncmp(funcs[mid].name, s, n);
    if (c == 0 && funcs[mid].name[n]) c = 1;
    if (c == 0) return &funcs[mid];
    if (c < 0) lo = mid + 1; else hi = mid - 1;
  }
  return 0;
}
int is_function_name(const char *s, size_t n) { return find_func(s, n) != 0; }

int call_function(buf *out, const char *name, size_t nlen, const char *args, size_t alen, char open) {
  const struct func *f = find_func(name, nlen);
  if (!f) return 0;
  char close = open == '(' ? ')' : '}';
  /* split at top-level commas; the last argument takes the rest */
  char *argv[64];
  int argc = 0;
  int max = f->maxargs ? f->maxargs : 64;
  size_t start = 0;
  int depth = 0;
  if (alen == 0 && f->maxargs != 0 && f->minargs == 0) {
    argv[argc++] = xstrdup("");
  } else {
    for (size_t i = 0; i <= alen; i++) {
      if (i == alen) { argv[argc++] = xstrndup(args + start, i - start); break; }
      char c = args[i];
      if (c == open) depth++;
      else if (c == close) depth--;
      else if (c == ',' && depth == 0 && argc < max - 1) {
        argv[argc++] = xstrndup(args + start, i - start);
        start = i + 1;
      }
    }
  }
  if (argc < f->minargs)
    fatal_at(read_file_name, read_line, "insufficient number of arguments (%d) to function '%s'", argc == 1 && !*argv[0] ? 0 : argc, f->name);
  if (f->expand) {
    for (int i = 0; i < argc; i++) {
      char *e = expand(argv[i]);
      free(argv[i]);
      argv[i] = e;
    }
  }
  f->fn(out, argv, argc, f->name);
  for (int i = 0; i < argc; i++) free(argv[i]);
  return 1;
}

/* Run SHELL -c CMD and return its output with newlines folded to spaces and
 * trailing newlines removed. */
char *run_shell_capture(const char *cmd, int *status, int one_newline) {
  char *shell = 0;
  var *sv = var_lookup("SHELL", 5);
  shell = sv ? var_value(sv) : xstrdup("/bin/sh");
  if (!*shell) { free(shell); shell = xstrdup("/bin/sh"); }
  char *flags = 0;
  var *fv = var_lookup(".SHELLFLAGS", 11);
  flags = fv ? var_value(fv) : xstrdup("-c");
  char *argv[16];
  int ac = 0;
  argv[ac++] = shell;
  const char *p = flags;
  size_t len;
  const char *w;
  strlist fl = {0};
  while ((w = next_word(&p, &len)) && ac < 14) { sl_add(&fl, xstrndup(w, len)); argv[ac++] = fl.v[fl.n - 1]; }
  argv[ac++] = (char *)cmd;
  argv[ac] = 0;
  int fds[2];
  if (sys_pipe(fds) < 0) fatal("pipe: %s", strerror(errno));
  fflush(stdout);
  fflush(stderr);
  char **env = build_env(0);
  int pid = sys_spawn(argv, env, fds[1]);
  sys_close(fds[1]);
  buf b = {0};
  if (pid < 0) {
    errmsg("%s: %s", shell, strerror(-pid));
    *status = 127;
  } else {
    char tmp[4096];
    int r;
    while ((r = sys_read(fds[0], tmp, sizeof tmp)) > 0) bput(&b, tmp, r);
    *status = 0;
    int st = 0;
    if (sys_waitpid(pid, &st) > 0) {
      int ex, code, sig;
      sys_wait_status(st, &ex, &code, &sig);
      *status = ex ? code : 128 + sig;
    }
  }
  sys_close(fds[0]);
  for (char **e = env; *e; e++) free(*e);
  free(env);
  sl_free(&fl);
  free(shell); free(flags);
  /* fold */
  if (one_newline) { if (b.n && b.s[b.n - 1] == '\n') b.n--; }
  else while (b.n && b.s[b.n - 1] == '\n') { b.n--; if (b.n && b.s[b.n - 1] == '\r') b.n--; }
  for (size_t i = 0; i < b.n; i++) if (b.s[i] == '\n') b.s[i] = ' ';
  char *r = bdetach(&b);
  return r ? r : xstrdup("");
}
