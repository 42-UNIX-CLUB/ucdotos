// SPDX-License-Identifier: GPL-2.0-or-later
/* make: variables, scopes and expansion of $(...) references. */
#include "make.h"

vset globals;
scope *cur_scope;
file *auto_file;
int auto_2nd;
tmpvar *tmpvars;

var *vset_lookup(vset *s, const char *name, size_t n) { return hget(&s->h, name, n); }

var *var_set(vset *s, const char *name, const char *value, int flavor, int origin) {
  int isnew;
  size_t n = strlen(name);
  void **slot = hput(&s->h, name, n, &isnew);
  var *v = *slot;
  if (!v) {
    v = xcalloc(1, sizeof *v);
    v->name = xstrndup(name, n);
    *slot = v;
  } else free(v->value);
  v->vlen = strlen(value);
  v->vcap = 0;
  v->value = xstrndup(value, v->vlen);
  v->flavor = flavor;
  v->origin = origin;
  v->file = read_file_name;
  v->line = read_line;
  return v;
}

int var_is_auto(const char *name, size_t n) {
  if (n == 1) return strchr("@<^+?*|%", name[0]) != 0;
  if (n == 2 && (name[1] == 'D' || name[1] == 'F')) return strchr("@<^+?*|%", name[0]) != 0;
  return 0;
}

/* automatic variables are computed on demand from auto_file */
static void deps_words(buf *out, file *f, int kind) {
  /* kind: '^' unique normal, '+' all normal, '|' order-only, '?' newer, '<' first */
  int first = 1;
  htab seen = {0};
  mtime_t tm = 0;
  if (kind == '?') tm = (f->mtime == MT_UNKNOWN) ? file_mtime(f) : f->mtime;
  for (dep *d = f->deps; d; d = d->next) {
    if (!d->f) continue;
    if (kind == '|' ? !d->oo : d->oo) continue;
    if (kind == '|') {
      int also_normal = 0;
      for (dep *e = f->deps; e; e = e->next) if (!e->oo && e->f == d->f) also_normal = 1;
      if (also_normal) continue;
    }
    const char *nm = d->f->vpath ? d->f->vpath : d->f->name;
    char *member = 0;
    {
      size_t l = strlen(nm);
      const char *lp = strchr(nm, '(');
      if (lp && lp != nm && l && nm[l - 1] == ')') { member = xstrndup(lp + 1, nm + l - 1 - (lp + 1)); nm = member; }
    }
    if (kind == '^' || kind == '|' || kind == '?') {
      int isnew;
      hput(&seen, nm, strlen(nm), &isnew);
      if (!isnew) continue;
    }
    if (kind == '?') {
      mtime_t dm = d->f->mtime;
      if (dm == MT_UNKNOWN) dm = file_mtime(d->f);
      int newer = f->phony || tm <= MT_MISSING || d->f->phony || dm > tm || dm <= MT_MISSING || d->f->changed;
      if (!newer) continue;
    }
    if (!first) bputc(out, ' ');
    bputs(out, nm);
    first = 0;
    free(member);
    if (kind == '<') break;
  }
  for (unsigned i = 0; i < seen.cap; i++) free(seen.t[i].key);
  free(seen.t);
}
static void auto_value(buf *out, char c) {
  file *f = auto_file;
  switch (c) {
  case '@': {
    const char *nm = f->name;
    const char *lp = strchr(nm, '(');
    if (lp && nm[strlen(nm) - 1] == ')' && lp != nm) { bput(out, nm, lp - nm); break; }
    bputs(out, f->vpath && !f->ran ? f->vpath : nm);
    break;
  }
  case '%': {
    const char *nm = f->name;
    const char *lp = strchr(nm, '(');
    size_t l = strlen(nm);
    if (lp && l && nm[l - 1] == ')' && lp != nm) bput(out, lp + 1, nm + l - 1 - (lp + 1));
    break;
  }
  case '*':
    if (f->stem) bputs(out, f->stem);
    else { char *s = suffix_stem(f->name); if (s) { bputs(out, s); free(s); } }
    break;
  default:
    deps_words(out, f, c);
  }
}
static var autov[8];
static int autoi;
static var *auto_var(const char *name, size_t n) {
  buf b = {0};
  if (n == 1) auto_value(&b, name[0]);
  else {
    buf w = {0};
    auto_value(&w, name[0]);
    bstr(&w);
    const char *p = w.s;
    size_t len;
    const char *t;
    int first = 1;
    while ((t = next_word(&p, &len))) {
      if (!first) bputc(&b, ' ');
      first = 0;
      if (name[1] == 'D') {
        size_t i = len;
        while (i > 0 && t[i - 1] != '/') i--;
        if (i == 0) bputc(&b, '.');
        else if (i == 1) bputc(&b, '/');
        else bput(&b, t, i - 1);
      } else path_notdir(&b, t, len);
    }
    bfree(&w);
  }
  var *v = &autov[autoi++ & 7];
  free(v->value);
  v->vlen = b.n;
  v->value = bdetach(&b);
  if (!v->value) v->value = xstrdup("");
  v->name = "auto";
  v->flavor = F_SIMPLE;
  v->origin = O_AUTOMATIC;
  return v;
}

static var empty_var = {.name = "", .value = "", .flavor = F_SIMPLE, .origin = O_AUTOMATIC};

static int all_digits(const char *s, size_t n) {
  if (!n) return 0;
  for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
  return 1;
}

/* Find NAME starting at scope START (and the globals after it). *after receives the
 * scope chain that follows the definition found, for target-specific +=. */
static var *lookup_chain(const char *name, size_t n, scope *start, scope **after, int *in_globals) {
  for (scope *s = start; s; s = s->next) {
    var *v = vset_lookup(s->set, name, n);
    if (v && (!v->priv || s->priv_ok)) {
      if (after) *after = s->next;
      if (in_globals) *in_globals = 0;
      return v;
    }
  }
  if (after) *after = 0;
  if (in_globals) *in_globals = 1;
  return vset_lookup(&globals, name, n);
}

var *var_lookup_global(const char *name, size_t n) { return vset_lookup(&globals, name, n); }

var *var_lookup(const char *name, size_t n) {
  int digits = -1;
  for (tmpvar *t = tmpvars; t; t = t->next) {
    if (t->is_call) {
      if (digits < 0) digits = all_digits(name, n);
      if (digits) return &empty_var;
      continue;
    }
    if (t->nlen == n && !memcmp(t->name, name, n)) return &t->v;
  }
  if (auto_file && var_is_auto(name, n)) {
    if (!auto_2nd || name[0] != '%') return auto_var(name, n);
  }
  return lookup_chain(name, n, cur_scope, 0, 0);
}

static const char *cur_file(void) { return read_file_name; }
static int var_depth;
static var *cur_var; /* innermost variable being expanded */
static const char *outer_file;
static int outer_line;

static void expand_var_value(buf *out, var *v, const char *name, size_t n, scope *after, int in_globals);

static void expand_found(buf *out, var *v, const char *name, size_t n, scope *after, int in_globals) {
  if (v->conditional && !in_globals) {
    /* target-specific ?=: an outer definition wins */
    scope *a2; int g2;
    var *o = lookup_chain(name, n, after, &a2, &g2);
    if (o) { expand_found(out, o, name, n, a2, g2); return; }
  }
  if (v->append && !in_globals) {
    scope *a2; int g2;
    var *o = lookup_chain(name, n, after, &a2, &g2);
    size_t before = out->n;
    if (o) expand_found(out, o, name, n, a2, g2);
    if (out->n > before && *v->value) bputc(out, ' ');
  }
  expand_var_value(out, v, name, n, after, in_globals);
}

static void expand_var_value(buf *out, var *v, const char *name, size_t n, scope *after, int in_globals) {
  (void)after; (void)in_globals; (void)name; (void)n;
  if (v->flavor == F_SIMPLE) { bput(out, v->value, v->vlen); return; }
  if (v->expanding) fatal_at(v->file ? v->file : cur_file(), v->file ? v->line : read_line, "Recursive variable '%s' references itself (eventually)", v->name);
  v->expanding = 1;
  const char *sf = read_file_name;
  int sl = read_line;
  if (var_depth++ == 0) { outer_file = sf; outer_line = sl; }
  var *save_var = cur_var;
  cur_var = v;
  expand_into(out, v->value, v->vlen);
  cur_var = save_var;
  var_depth--;
  read_file_name = sf;
  read_line = sl;
  v->expanding = 0;
}

static void expand_ref(buf *out, const char *name, size_t n) {
  /* tmpvars and automatic variables first */
  int digits = -1;
  for (tmpvar *t = tmpvars; t; t = t->next) {
    if (t->is_call) {
      if (digits < 0) digits = all_digits(name, n);
      if (digits) return;
      continue;
    }
    if (t->nlen == n && !memcmp(t->name, name, n)) { expand_var_value(out, &t->v, name, n, 0, 1); return; }
  }
  if (auto_file && var_is_auto(name, n) && (!auto_2nd || name[0] != '%')) {
    bputs(out, auto_var(name, n)->value);
    return;
  }
  scope *after; int ing;
  var *v = lookup_chain(name, n, cur_scope, &after, &ing);
  if (!v) {
    if (opt_warn_undef) warn_at(var_depth ? outer_file : read_file_name, var_depth ? outer_line : read_line, "warning: undefined variable '%.*s'", (int)n, name);
    return;
  }
  expand_found(out, v, name, n, after, ing);
}

char *var_value(var *v) {
  buf b = {0};
  if (v->flavor == F_SIMPLE) bputs(&b, v->value);
  else expand_found(&b, v, v->name, strlen(v->name), 0, 1);
  char *s = bdetach(&b);
  return s ? s : xstrdup("");
}

long find_close(const char *s, long i, char open) {
  char close = open == '(' ? ')' : '}';
  int depth = 1;
  for (; s[i]; i++) {
    if (s[i] == open) depth++;
    else if (s[i] == close && --depth == 0) return i;
  }
  return -1;
}
static long find_closen(const char *s, long i, long n, char open) {
  char close = open == '(' ? ')' : '}';
  int depth = 1;
  for (; i < n; i++) {
    if (s[i] == open) depth++;
    else if (s[i] == close && --depth == 0) return i;
  }
  return -1;
}

/* $(var:a=b) */
static void subst_ref(buf *out, const char *name, size_t nn, const char *from, const char *to) {
  buf val = {0};
  expand_ref(&val, name, nn);
  bstr(&val);
  const char *pf = find_percent(from);
  char *pfrom = 0, *pto = 0;
  const char *pct_from = 0, *pct_to = 0;
  if (pf) {
    pfrom = unescape_percent(from, &pct_from);
    pto = unescape_percent(to, &pct_to);
  }
  size_t fl = strlen(from);
  const char *p = val.s;
  size_t len;
  const char *w;
  int first = 1;
  while ((w = next_word(&p, &len))) {
    if (!first) bputc(out, ' ');
    first = 0;
    if (pf) {
      size_t so, sl;
      if (pattern_match(pfrom, pct_from, w, len, &so, &sl)) pattern_subst(out, pto, pct_to, w + so, sl);
      else bput(out, w, len);
    } else if (len >= fl && !memcmp(w + len - fl, from, fl)) {
      bput(out, w, len - fl);
      bputs(out, to);
    } else bput(out, w, len);
  }
  free(pfrom); free(pto);
  bfree(&val);
}

void expand_into(buf *out, const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    const char *d = memchr(s + i, '$', n - i);
    if (!d) { bput(out, s + i, n - i); return; }
    size_t j = d - s;
    bput(out, s + i, j - i);
    if (j + 1 >= n) return; /* a lone $ at the end disappears */
    char c = s[j + 1];
    if (c == '$') { bputc(out, '$'); i = j + 2; continue; }
    if (c != '(' && c != '{') {
      expand_ref(out, &s[j + 1], 1);
      i = j + 2;
      continue;
    }
    long close = find_closen(s, j + 2, n, c);
    if (close < 0) {
      if (cur_var && cur_var->file) fatal_at(cur_var->file, cur_var->line, "unterminated variable reference");
      fatal_at(read_file_name, read_line, "unterminated variable reference");
    }
    const char *in = s + j + 2;
    size_t inl = close - (j + 2);
    i = close + 1;
    /* a function call? */
    size_t k = 0;
    while (k < inl && (islower((unsigned char)in[k]) || in[k] == '-')) k++;
    if (k > 0 && (k == inl || is_blank(in[k]) || in[k] == '\n') && is_function_name(in, k)) {
      size_t a = k;
      while (a < inl && is_space(in[a])) a++;
      if (call_function(out, in, k, in + a, inl - a, c)) continue;
    }
    /* substitution reference: a colon at the top level, then = */
    long colon = -1;
    {
      int depth = 0;
      for (size_t q = 0; q < inl; q++) {
        char ch = in[q];
        if (ch == '$' && q + 1 < inl && (in[q + 1] == '(' || in[q + 1] == '{')) {
          long cl = find_closen(in, q + 2, inl, in[q + 1]);
          if (cl < 0) break;
          q = cl;
          continue;
        }
        if (ch == '(' || ch == '{') depth++;
        else if (ch == ')' || ch == '}') depth--;
        else if (ch == ':' && depth == 0) { colon = q; break; }
      }
    }
    if (colon >= 0) {
      long eq = -1;
      int depth = 0;
      for (size_t q = colon + 1; q < inl; q++) {
        char ch = in[q];
        if (ch == '$' && q + 1 < inl && (in[q + 1] == '(' || in[q + 1] == '{')) {
          long cl = find_closen(in, q + 2, inl, in[q + 1]);
          if (cl < 0) break;
          q = cl;
          continue;
        }
        if (ch == '(' || ch == '{') depth++;
        else if (ch == ')' || ch == '}') depth--;
        else if (ch == '=' && depth == 0) { eq = q; break; }
      }
      if (eq >= 0) {
        char *nm = expandn(in, colon);
        char *from = expandn(in + colon + 1, eq - colon - 1);
        char *to = expandn(in + eq + 1, inl - eq - 1);
        subst_ref(out, nm, strlen(nm), from, to);
        free(nm); free(from); free(to);
        continue;
      }
    }
    if (memchr(in, '$', inl)) {
      char *nm = expandn(in, inl);
      expand_ref(out, nm, strlen(nm));
      free(nm);
    } else expand_ref(out, in, inl);
  }
}

char *expand_var_named(const char *name) {
  buf b = {0};
  expand_ref(&b, name, strlen(name));
  char *r = bdetach(&b);
  return r ? r : xstrdup("");
}

char *expandn(const char *s, size_t n) {
  buf b = {0};
  expand_into(&b, s, n);
  char *r = bdetach(&b);
  return r ? r : xstrdup("");
}
char *expand(const char *s) { return expandn(s, strlen(s)); }

/* Priority: a definition is ignored when the existing variable came from a
 * stronger origin (command line beats the makefile, unless 'override'). */
static int stronger(int existing, int incoming) {
  if (existing == O_AUTOMATIC) return 0;
  if (opt_e && existing == O_ENV && incoming == O_FILE) return 1;
  if (incoming == O_OVERRIDE) return 0;
  return existing > incoming;
}

var *var_define(const char *name, const char *value, int flavor, int origin) {
  var *v = vset_lookup(&globals, name, strlen(name));
  if (v && stronger(v->origin, origin)) return v;
  return var_set(&globals, name, value, flavor, origin);
}

static char *escape_dollars(const char *s) {
  buf b = {0};
  for (; *s; s++) { if (*s == '$') bputc(&b, '$'); bputc(&b, *s); }
  char *r = bdetach(&b);
  return r ? r : xstrdup("");
}

var *var_assign(vset *s, const char *name, const char *raw, int op, int origin, int is_target) {
  size_t nl = strlen(name);
  var *old = vset_lookup(s, name, nl);
  int is_global = (s == &globals);
  if (old && is_global && stronger(old->origin, origin)) {
    /* the value is ignored; under -e the variable is now an override */
    if (opt_e && old->origin == O_ENV) old->origin = O_ENV_OVERRIDE;
    return old;
  }
  if (is_target && origin != O_OVERRIDE) {
    /* a command line value beats a target-specific one */
    var *g = vset_lookup(&globals, name, nl);
    if (g && (g->origin == O_COMMAND || g->origin == O_ENV_OVERRIDE)) {
      var *v = var_set(s, name, g->value, g->flavor, g->origin);
      v->append = 0;
      return v;
    }
  }
  char *val = 0;
  int flavor = F_RECURSIVE;
  switch (op) {
  case '=': val = xstrdup(raw); break;
  case ':': val = expand(raw); flavor = F_SIMPLE; break;
  case '3': { char *e = expand(raw); val = escape_dollars(e); free(e); break; }
  case '!': {
    char *cmd = expand(raw);
    int st;
    char *outp = run_shell_capture(cmd, &st, 1);
    free(cmd);
    /* != removes one trailing newline; run_shell_capture already folded them */
    val = outp;
    char sb[16];
    snprintf(sb, sizeof sb, "%d", st);
    var_set(&globals, ".SHELLSTATUS", sb, F_SIMPLE, O_FILE);
    break;
  }
  case '?':
    if (is_target) {
      var *v = var_set(s, name, raw, F_RECURSIVE, origin);
      v->conditional = 1;
      return v;
    }
    if (old) return old;
    val = xstrdup(raw);
    break;
  case '+':
    if (!old) {
      if (is_target) {
        var *v = var_set(s, name, raw, F_RECURSIVE, origin);
        v->append = 1;
        /* if the global is simple, the appended text is expanded now */
        var *g = vset_lookup(&globals, name, nl);
        if (g && g->flavor == F_SIMPLE) { char *e = expand(raw); free(v->value); v->value = e; v->vlen = strlen(e); v->vcap = 0; v->flavor = F_SIMPLE; }
        return v;
      }
      val = xstrdup(raw);
      break;
    } else {
      /* append in place: makefiles often grow one variable thousands of times */
      char *e = old->flavor == F_SIMPLE ? expand(raw) : 0;
      const char *add = e ? e : raw;
      size_t al = strlen(add);
      if (al) {
        size_t need = old->vlen + 1 + al + 1;
        if (need > old->vcap) {
          size_t cap = old->vcap ? old->vcap : old->vlen + 1;
          while (cap < need) cap = cap * 2 + 16;
          old->value = xrealloc(old->value, cap);
          old->vcap = cap;
        }
        if (old->vlen) old->value[old->vlen++] = ' ';
        memcpy(old->value + old->vlen, add, al);
        old->vlen += al;
        old->value[old->vlen] = 0;
      }
      free(e);
      if (old->origin < origin || origin == O_OVERRIDE) old->origin = origin;
      old->file = read_file_name;
      old->line = read_line;
      return old;
    }
  }
  var *v = var_set(s, name, val, flavor, origin);
  v->append = 0;
  v->conditional = 0;
  free(val);
  return v;
}

void var_undefine(const char *name, int origin) {
  size_t n = strlen(name);
  var *v = vset_lookup(&globals, name, n);
  if (!v) return;
  if (stronger(v->origin, origin)) return;
  /* the key stays in the table with no value: lookups then miss it */
  void **slot = hput(&globals.h, name, n, 0);
  *slot = 0;
}
