// SPDX-License-Identifier: GPL-2.0-or-later
/* make: reading makefiles. Logical lines, comments, conditionals, define,
 * include, variable assignments, rules and recipes. */
#include "make.h"

const char *read_file_name;
int read_line;
strlist makefiles_read;
incl *missing_incs;
int nmissing_incs;
int all_silent, all_ignore;
int second_expansion, oneshell, posix_mode, export_all, notparallel, delete_on_error, all_secondary, all_notint;
int recipe_prefix = '\t';
extern int reading_builtin;
char **suffixes;
int nsuffixes;

typedef struct reader {
  const char *name;
  const char *text;
  size_t len, pos;
  int line;
} reader;

/* the rule whose recipe lines are being collected */
typedef struct pending {
  int active;        /* a rule line was seen; tab lines belong to it */
  int no_targets;    /* the rule had no targets: ignore its recipe */
  file **t; int nt;  /* explicit targets (or double-colon entries) */
  dep **mark;        /* for each target, its first new dep (to reorder) */
  dep **oldhead;
  rule *pr;          /* pattern rule */
  cmds *c;
  const char *file;
  int line;
  int grouped;
} pending;
static pending cur;

/* conditionals */
typedef struct cond { int state; int seen_else; } cond; /* state: 0 active, 1 not yet taken, 2 done */
static cond conds[256];
static int nconds;
static int ignoring(void) {
  for (int i = 0; i < nconds; i++) if (conds[i].state != 0) return 1;
  return 0;
}

void set_suffixes_default(void) {
  static const char *def = ".out .a .ln .o .c .cc .C .cpp .p .f .F .m .r .y .l .ym .yl .s .S .mod .sym .def .h .info .dvi .tex .texinfo .texi .txinfo .w .ch .web .sh .elc .el";
  const char *p = def;
  size_t len;
  const char *w;
  while ((w = next_word(&p, &len))) {
    suffixes = xrealloc(suffixes, (nsuffixes + 1) * sizeof *suffixes);
    suffixes[nsuffixes++] = xstrndup(w, len);
  }
  var_define("SUFFIXES", def, F_RECURSIVE, O_DEFAULT);
  file *s = file_enter(".SUFFIXES");
  for (int i = 0; i < nsuffixes; i++) dep_append(s, dep_new(file_enter(suffixes[i])));
}

void record_default_goal(const char *name) {
  var *v = var_lookup_global(".DEFAULT_GOAL", 13);
  if (v && *v->value) return;
  var_set(&globals, ".DEFAULT_GOAL", name, F_RECURSIVE, O_FILE);
}

/* ---- logical lines ---- */
static int raw_line(reader *r, buf *b, int *lineno) {
  b->n = 0;
  if (r->pos >= r->len) return 0;
  *lineno = r->line + 1;
  for (;;) {
    const char *s = r->text + r->pos;
    const char *nl = memchr(s, '\n', r->len - r->pos);
    size_t n = nl ? (size_t)(nl - s) : r->len - r->pos;
    r->pos += n + (nl ? 1 : 0);
    r->line++;
    size_t m = n;
    if (m && s[m - 1] == '\r') m--; /* tolerate CRLF */
    bput(b, s, m);
    size_t k = 0;
    while (k < m && s[m - 1 - k] == '\\') k++;
    if ((k & 1) && nl && r->pos <= r->len) {
      bputc(b, '\n'); /* keep "\\\n"; the caller decides what it means */
      if (r->pos >= r->len) break;
      continue;
    }
    break;
  }
  bstr(b);
  return 1;
}

/* Outside recipes, backslash-newline and the whitespace around it become one space. */
static void collapse(buf *b) {
  char *s = b->s;
  size_t n = b->n, o = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') {
      while (o > 0 && is_blank(s[o - 1])) o--;
      i += 2;
      while (i < n) {
        if (is_blank(s[i])) { i++; continue; }
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') { i += 2; continue; }
        break;
      }
      s[o++] = ' ';
      i--;
      continue;
    }
    s[o++] = s[i];
  }
  b->n = o;
  bstr(b);
}

/* Remove a comment: a # outside $(...) that is not escaped. \# becomes #. */
static void strip_comment(buf *b) {
  char *s = b->s;
  size_t n = b->n, o = 0;
  int depth = 0;
  char stack[64];
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{')) {
      if (depth < 64) stack[depth] = s[i + 1];
      depth++;
      s[o++] = c; s[o++] = s[++i];
      continue;
    }
    if (depth > 0 && (c == ')' || c == '}')) {
      char open = depth <= 64 ? stack[depth - 1] : '(';
      if ((open == '(' && c == ')') || (open == '{' && c == '}')) depth--;
      s[o++] = c;
      continue;
    }
    if (depth > 0 && (c == '(' || c == '{')) {
      if (depth < 64) stack[depth] = c == '(' ? 1 : 2; /* plain parens: track loosely */
      s[o++] = c;
      continue;
    }
    if (c == '#' && depth == 0) {
      size_t k = 0;
      while (k < o && s[o - 1 - k] == '\\') k++;
      if (k & 1) { /* escaped: drop one backslash per pair plus the escaping one */
        size_t keep = k / 2;
        o -= k;
        for (size_t j = 0; j < keep; j++) s[o++] = '\\';
        s[o++] = '#';
        continue;
      }
      if (k) { o -= k; for (size_t j = 0; j < k / 2; j++) s[o++] = '\\'; }
      break;
    }
    s[o++] = c;
  }
  b->n = o;
  bstr(b);
}

/* ---- assignments ---- */
static long skip_ref(const char *s, long i) {
  /* s[i] is '$' and s[i+1] an opening paren: return the index of the close */
  long c = find_close(s, i + 2, s[i + 1]);
  return c < 0 ? (long)strlen(s) - 1 : c;
}
int find_assign(const char *s, size_t *name_end, size_t *val_start, int *op) {
  for (long i = 0; s[i]; i++) {
    char c = s[i];
    if (c == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) { i = skip_ref(s, i); continue; }
    if (c == '=') {
      long ne = i;
      int o = '=';
      if (i > 0 && (s[i - 1] == '+' || s[i - 1] == '?' || s[i - 1] == '!')) { o = s[i - 1]; ne = i - 1; }
      *name_end = ne;
      long v = i + 1;
      while (is_blank(s[v])) v++;
      *val_start = v;
      *op = o;
      return 1;
    }
    if (c == ':') {
      if (s[i + 1] == '=') { *name_end = i; *op = ':'; long v = i + 2; while (is_blank(s[v])) v++; *val_start = v; return 1; }
      if (s[i + 1] == ':' && s[i + 2] == '=') { *name_end = i; *op = ':'; long v = i + 3; while (is_blank(s[v])) v++; *val_start = v; return 1; }
      if (s[i + 1] == ':' && s[i + 2] == ':' && s[i + 3] == '=') { *name_end = i; *op = '3'; long v = i + 4; while (is_blank(s[v])) v++; *val_start = v; return 1; }
      return 0;
    }
    if (c == ';' || c == '|') {
      /* not before an assignment operator in a variable name; keep scanning */
    }
  }
  return 0;
}

static void check_special_var(const char *name) {
  if (!strcmp(name, ".RECIPEPREFIX")) {
    var *v = var_lookup_global(name, strlen(name));
    char *val = v ? var_value(v) : xstrdup("");
    recipe_prefix = *val ? *val : '\t';
    free(val);
  }
}

/* parse "[override] [export|unexport] [private] NAME OP VALUE" */
static int word_is(const char *s, const char *w, const char **after) {
  size_t n = strlen(w);
  if (strncmp(s, w, n)) return 0;
  if (s[n] && !is_blank(s[n])) return 0;
  *after = skip_ws(s + n);
  return 1;
}

static void do_assign(const char *s, vset *set, int origin, int exp, int priv, int is_target) {
  size_t ne, vs;
  int op;
  if (!find_assign(s, &ne, &vs, &op)) return;
  size_t e = ne;
  while (e > 0 && is_blank(s[e - 1])) e--;
  char *rawname = xstrndup(s, e);
  char *name = expand(rawname);
  free(rawname);
  if (!*name) fatal_at(read_file_name, read_line, "empty variable name");
  /* the value keeps trailing blanks, as GNU Make does */
  var *v = var_assign(set, name, s + vs, op, origin, is_target);
  if (v) {
    if (exp) v->exp = exp;
    if (priv) v->priv = 1;
  }
  if (set == &globals) check_special_var(name);
  free(name);
}

void parse_var_line(const char *s, vset *set, int origin, int is_target) {
  int exp = 0, priv = 0;
  const char *a;
  for (;;) {
    if (word_is(s, "override", &a)) { origin = O_OVERRIDE; s = a; continue; }
    if (word_is(s, "export", &a)) { exp = EXP_YES; s = a; continue; }
    if (word_is(s, "unexport", &a)) { exp = EXP_NO; s = a; continue; }
    if (word_is(s, "private", &a)) { priv = 1; s = a; continue; }
    break;
  }
  do_assign(s, set, origin, exp, priv, is_target);
}

/* ---- rules ---- */
static void finish_rule(void) {
  if (!cur.active) return;
  cmds *c = cur.c;
  if (cur.pr) {
    cur.pr->cmds = c;
    rule_add(cur.pr, !reading_builtin); /* built-in rules never replace the user's */
  }
  for (int i = 0; i < cur.nt; i++) {
    file *f = cur.t[i];
    if (!c) continue;
    if (f->cmds && f->cmds != c && !f->dc && !f->dchead) {
      if (!f->cmds->file || strcmp(f->cmds->file, "<builtin>")) {
        warn_at(c->file, c->line, "warning: overriding recipe for target '%s'", f->name);
        warn_at(f->cmds->file, f->cmds->line, "warning: ignoring old recipe for target '%s'", f->name);
      }
    }
    f->cmds = c;
    /* the prerequisites of the rule with the recipe come first */
    if (cur.mark && cur.mark[i] && cur.oldhead[i] && cur.oldhead[i] != cur.mark[i]) {
      dep *first_new = cur.mark[i];
      dep *old = cur.oldhead[i];
      dep *p = old;
      while (p && p->next != first_new) p = p->next;
      if (p) {
        dep *last = first_new;
        while (last->next) last = last->next;
        p->next = 0;
        last->next = old;
        f->deps = first_new;
        f->deptail = &p->next;
      }
    }
  }
  free(cur.t); free(cur.mark); free(cur.oldhead);
  memset(&cur, 0, sizeof cur);
}

static void add_recipe_line(const char *text, int lineno) {
  if (cur.no_targets) return;
  if (!cur.c) {
    cur.c = xcalloc(1, sizeof *cur.c);
    cur.c->file = cur.file;
    cur.c->line = lineno;
    cur.c->recipe_prefix = recipe_prefix;
  }
  cmds *c = cur.c;
  c->lines = xrealloc(c->lines, (c->n + 1) * sizeof *c->lines);
  c->lineno = xrealloc(c->lineno, (c->n + 1) * sizeof *c->lineno);
  c->lines[c->n] = xstrdup(text);
  c->lineno[c->n] = lineno;
  c->n++;
}

/* split a list of file names, joining archive references "lib(a b)" and
 * expanding wildcards */
static void split_names(const char *s, strlist *out, int glob) {
  const char *p = s;
  size_t len;
  const char *w;
  while ((w = next_word(&p, &len))) {
    char *word = xstrndup(w, len);
    const char *lp = strchr(word, '(');
    if (lp && lp != word && !strchr(lp, ')')) {
      /* collect until the closing paren */
      buf b = {0};
      bputs(&b, word);
      const char *w2;
      size_t l2;
      while ((w2 = next_word(&p, &l2))) {
        bputc(&b, ' ');
        bput(&b, w2, l2);
        if (memchr(w2, ')', l2)) break;
      }
      free(word);
      word = bdetach(&b);
      lp = strchr(word, '(');
    }
    if (lp && lp != word && word[strlen(word) - 1] == ')' && strchr(lp + 1, ' ')) {
      size_t an = lp - word;
      const char *q = lp + 1;
      const char *mw;
      size_t ml;
      char *inner = xstrndup(q, strlen(q) - 1);
      const char *ip = inner;
      while ((mw = next_word(&ip, &ml))) {
        buf b = {0};
        bput(&b, word, an);
        bputc(&b, '(');
        bput(&b, mw, ml);
        bputc(&b, ')');
        sl_add(out, bdetach(&b));
      }
      free(inner);
      free(word);
      continue;
    }
    if (glob && has_glob_chars(word) && !find_percent(word)) {
      strlist g = {0};
      if (glob_expand(word, &g) > 0) {
        for (int i = 0; i < g.n; i++) sl_add(out, g.v[i]);
        free(g.v);
        free(word);
        continue;
      }
      sl_free(&g);
    }
    sl_add(out, word);
  }
}

static int is_special(const char *n) {
  return n[0] == '.' && (!strcmp(n, ".PHONY") || !strcmp(n, ".SUFFIXES") || !strcmp(n, ".DEFAULT") ||
      !strcmp(n, ".PRECIOUS") || !strcmp(n, ".INTERMEDIATE") || !strcmp(n, ".SECONDARY") ||
      !strcmp(n, ".SECONDEXPANSION") || !strcmp(n, ".DELETE_ON_ERROR") || !strcmp(n, ".IGNORE") ||
      !strcmp(n, ".LOW_RESOLUTION_TIME") || !strcmp(n, ".SILENT") || !strcmp(n, ".EXPORT_ALL_VARIABLES") ||
      !strcmp(n, ".NOTPARALLEL") || !strcmp(n, ".ONESHELL") || !strcmp(n, ".POSIX") || !strcmp(n, ".NOTINTERMEDIATE"));
}

static void special_target(const char *name, strlist *deps) {
  if (!strcmp(name, ".PHONY")) for (int i = 0; i < deps->n; i++) file_enter(deps->v[i])->phony = 1;
  else if (!strcmp(name, ".SUFFIXES")) {
    if (!deps->n) {
      for (int i = 0; i < nsuffixes; i++) free(suffixes[i]);
      nsuffixes = 0;
      file *s = file_enter(".SUFFIXES");
      s->deps = 0; s->deptail = &s->deps;
    } else for (int i = 0; i < deps->n; i++) {
      suffixes = xrealloc(suffixes, (nsuffixes + 1) * sizeof *suffixes);
      suffixes[nsuffixes++] = xstrdup(deps->v[i]);
    }
  } else if (!strcmp(name, ".PRECIOUS")) for (int i = 0; i < deps->n; i++) { if (!find_percent(deps->v[i])) file_enter(deps->v[i])->precious = 1; }
  else if (!strcmp(name, ".INTERMEDIATE")) for (int i = 0; i < deps->n; i++) file_enter(deps->v[i])->intermediate = 1;
  else if (!strcmp(name, ".SECONDARY")) {
    if (!deps->n) all_secondary = 1;
    for (int i = 0; i < deps->n; i++) { file *f = file_enter(deps->v[i]); f->secondary = 1; f->intermediate = 1; }
  } else if (!strcmp(name, ".NOTINTERMEDIATE")) {
    if (!deps->n) all_notint = 1;
    for (int i = 0; i < deps->n; i++) if (!find_percent(deps->v[i])) file_enter(deps->v[i])->notintermediate = 1;
  } else if (!strcmp(name, ".SILENT")) {
    if (!deps->n) all_silent = 1;
    for (int i = 0; i < deps->n; i++) file_enter(deps->v[i])->silent = 1;
  } else if (!strcmp(name, ".IGNORE")) {
    if (!deps->n) all_ignore = 1;
    for (int i = 0; i < deps->n; i++) file_enter(deps->v[i])->ignore = 1;
  } else if (!strcmp(name, ".SECONDEXPANSION")) second_expansion = 1;
  else if (!strcmp(name, ".ONESHELL")) oneshell = 1;
  else if (!strcmp(name, ".POSIX")) {
    posix_mode = 1;
    var_define(".SHELLFLAGS", "-ec", F_RECURSIVE, O_DEFAULT);
  }
  else if (!strcmp(name, ".NOTPARALLEL")) { if (!deps->n) notparallel = 1; }
  else if (!strcmp(name, ".DELETE_ON_ERROR")) delete_on_error = 1;
  else if (!strcmp(name, ".EXPORT_ALL_VARIABLES")) export_all = 1;
}

static int is_suffix_rule_name(const char *n) {
  if (n[0] != '.') return 0;
  size_t l = strlen(n);
  if (is_suffix(n, l)) return 1;
  for (size_t i = 1; i < l; i++)
    if (n[i] == '.' && is_suffix(n, i) && is_suffix(n + i, l - i)) return 1;
  return 0;
}

static int default_goal_ok(const char *n) {
  if (find_percent(n)) return 0;
  if (n[0] == '.' && !strchr(n, '/')) return 0;
  return 1;
}

/* deps text -> dep list for one target. stem: for static pattern rules. */
static dep *make_deps(const char *text, const char *pat, const char *pct, const char *stem, size_t sl, int need2) {
  dep *head = 0, **tail = &head;
  if (need2) {
    /* keep the text: it is expanded again when the target is considered */
    const char *bar = 0;
    int depth = 0;
    for (const char *q = text; *q; q++) {
      if (*q == '$' && (q[1] == '(' || q[1] == '{')) { depth++; q++; continue; }
      if (depth && (*q == ')' || *q == '}')) { depth--; continue; }
      if (!depth && *q == '|') { bar = q; break; }
    }
    char *normal = bar ? xstrndup(text, bar - text) : xstrdup(text);
    if (*skip_ws(normal)) {
      dep *d = dep_new(0);
      d->name = normal; d->need2 = 1;
      *tail = d; tail = &d->next;
    } else free(normal);
    if (bar && *skip_ws(bar + 1)) {
      dep *d = dep_new(0);
      d->name = xstrdup(bar + 1); d->need2 = 1; d->oo = 1;
      *tail = d; tail = &d->next;
    }
    (void)pat; (void)pct; (void)stem; (void)sl;
    return head;
  }
  int oo = 0;
  int wait = 0;
  const char *p = text;
  strlist names = {0};
  /* split at | first */
  const char *bar = strchr(text, '|');
  char *normal = bar ? xstrndup(text, bar - text) : xstrdup(text);
  for (int part = 0; part < 2; part++) {
    names.n = 0;
    if (part == 0) split_names(normal, &names, 1);
    else if (bar) split_names(bar + 1, &names, 1);
    oo = part;
    for (int i = 0; i < names.n; i++) {
      char *nm = names.v[i];
      if (!strcmp(nm, ".WAIT")) { wait = 1; free(nm); continue; }
      if (pat) {
        const char *dpct;
        char *u = unescape_percent(nm, &dpct);
        buf b = {0};
        if (dpct) pattern_subst(&b, u, dpct, stem, sl);
        else bputs(&b, u);
        free(u);
        free(nm);
        nm = bdetach(&b);
        if (!nm) nm = xstrdup("");
      }
      file *f = file_enter(nm);
      f->mentioned = 1;
      dep *d = dep_new(f);
      d->oo = oo;
      d->wait = wait;
      wait = 0;
      *tail = d; tail = &d->next;
      free(nm);
    }
  }
  (void)p;
  free(names.v);
  free(normal);
  return head;
}

static file *dc_entry(file *f) {
  /* a further double-colon rule for F */
  file *e = xcalloc(1, sizeof *e);
  e->name = f->name;
  e->deptail = &e->deps;
  e->mtime = MT_UNKNOWN;
  e->dchead = f;
  e->dc = 1;
  e->is_target = 1;
  file **pp = &f->dcnext;
  while (*pp) pp = &(*pp)->dcnext;
  *pp = e;
  return e;
}

static void parse_rule(const char *line, size_t colon, const char *recipe, int lineno, const char *fname) {
  /* line: text before ';' with comments removed; colon: index of the first ':' */
  int dc = 0, grouped = 0;
  size_t tend = colon;
  const char *rest = line + colon + 1;
  if (*rest == ':') { dc = 1; rest++; }
  if (tend > 0 && line[tend - 1] == '&' && !dc) { grouped = 1; tend--; }
  char *traw = xstrndup(line, tend);
  char *texp = expand(traw);
  free(traw);
  strlist targets = {0};
  split_names(texp, &targets, 1);
  free(texp);
  /* target-specific variable? */
  size_t ne, vs;
  int op;
  const char *rs = skip_ws(rest);
  {
    const char *t = rs, *a;
    for (;;) {
      if (word_is(t, "override", &a) || word_is(t, "export", &a) || word_is(t, "unexport", &a) || word_is(t, "private", &a)) { t = a; continue; }
      break;
    }
    if (find_assign(t, &ne, &vs, &op)) {
      finish_rule();
      if (recipe) {
        /* "t: X = a ; b": the ; belongs to the value */
        buf b = {0};
        bputs(&b, rs);
        bputc(&b, ';');
        bputs(&b, recipe);
        rs = bdetach(&b);
      }
      for (int i = 0; i < targets.n; i++) {
        const char *tn = targets.v[i];
        if (find_percent(tn)) {
          int origin = O_FILE, exp = 0, priv = 0;
          const char *s = rs, *a2;
          for (;;) {
            if (word_is(s, "override", &a2)) { origin = O_OVERRIDE; s = a2; continue; }
            if (word_is(s, "export", &a2)) { exp = EXP_YES; s = a2; continue; }
            if (word_is(s, "unexport", &a2)) { exp = EXP_NO; s = a2; continue; }
            if (word_is(s, "private", &a2)) { priv = 1; s = a2; continue; }
            break;
          }
          pattern_var_add(tn, (char *)s, origin, exp, priv);
        } else {
          file *f = file_enter(tn);
          if (!f->vars) f->vars = xcalloc(1, sizeof *f->vars);
          parse_var_line(rs, f->vars, O_FILE, 1);
        }
      }
      sl_free(&targets);
      return;
    }
  }
  finish_rule();
  cur.active = 1;
  cur.file = fname;
  cur.line = lineno;
  if (!targets.n) {
    cur.no_targets = 1;
    sl_free(&targets);
    return;
  }
  /* static pattern rule? */
  char *rexp = expand(rs);
  char *pat = 0;
  const char *deptext = rexp;
  {
    char *c2 = 0;
    for (char *q = rexp; *q; q++) if (*q == ':') { c2 = q; break; }
    if (c2) {
      *c2 = 0;
      buf pb = {0};
      strip_into(&pb, rexp, strlen(rexp));
      pat = bdetach(&pb);
      if (!pat) pat = xstrdup("");
      deptext = c2 + 1;
    }
  }
  int need2 = second_expansion && strchr(deptext, '$');
  int any_pct = 0, all_pct = 1;
  for (int i = 0; i < targets.n; i++) { if (find_percent(targets.v[i])) any_pct = 1; else all_pct = 0; }
  if (!pat && any_pct) {
    int mixed = !all_pct;
    if (mixed) {
      fflush(stdout);
      fprintf(stderr, "%s:%d: *** mixed implicit and normal rules: deprecated syntax\n", fname, lineno);
      fflush(stderr);
    }
    rule *r = xcalloc(1, sizeof *r);
    r->targets = xmalloc(targets.n * sizeof *r->targets);
    r->pcts = xmalloc(targets.n * sizeof *r->pcts);
    for (int i = 0; i < targets.n; i++) if (find_percent(targets.v[i])) { r->targets[r->nt] = unescape_percent(targets.v[i], &r->pcts[r->nt]); r->nt++; }
    r->terminal = dc;
    r->need2 = need2;
    /* pattern deps keep their % */
    dep *head = 0, **tail = &head;
    const char *bar = need2 ? 0 : strchr(deptext, '|');
    char *normal = bar ? xstrndup(deptext, bar - deptext) : xstrdup(deptext);
    for (int part = 0; part < 2; part++) {
      const char *src = part ? (bar ? bar + 1 : "") : normal;
      if (need2) {
        dep *d = dep_new(0);
        if (!*skip_ws(src)) continue;
        d->name = xstrdup(src); d->need2 = 1;
        *tail = d; tail = &d->next;
        continue;
      }
      strlist dn = {0};
      split_names(src, &dn, 0);
      for (int i = 0; i < dn.n; i++) {
        if (!strcmp(dn.v[i], ".WAIT")) continue;
        dep *d = dep_new(0);
        d->name = xstrdup(dn.v[i]);
        d->oo = part;
        *tail = d; tail = &d->next;
        if (!find_percent(dn.v[i])) file_enter(dn.v[i])->mentioned = 1;
      }
      sl_free(&dn);
    }
    free(normal);
    r->deps = head;
    cur.pr = r;
    if (!mixed) {
      if (recipe) add_recipe_line(recipe, lineno);
      free(rexp);
      sl_free(&targets);
      return;
    }
    int j = 0;
    for (int i = 0; i < targets.n; i++) { if (find_percent(targets.v[i])) free(targets.v[i]); else targets.v[j++] = targets.v[i]; }
    targets.n = j;
  }
  if (!pat && targets.n == 1 && *skip_ws(deptext) && is_suffix_rule_name(targets.v[0])) {
    warn_at(fname, lineno, "warning: ignoring prerequisites on suffix rule definition");
    deptext = "";
  }
  const char *ppct = 0;
  char *upat = pat ? unescape_percent(pat, &ppct) : 0;
  if (pat && !ppct) fatal_at(fname, lineno, "target pattern contains no '%%'");
  cur.t = xcalloc(targets.n, sizeof *cur.t);
  cur.mark = xcalloc(targets.n, sizeof *cur.mark);
  cur.oldhead = xcalloc(targets.n, sizeof *cur.oldhead);
  cur.grouped = grouped;
  for (int i = 0; i < targets.n; i++) {
    const char *tn = targets.v[i];
    if (is_special(tn)) {
      strlist dl = {0};
      split_names(deptext, &dl, 1);
      special_target(tn, &dl);
      sl_free(&dl);
    }
    file *f = file_enter(tn);
    if (f->is_target && f->dc != dc && !f->dchead && strcmp(tn, ".SUFFIXES") && !is_special(tn))
      fatal_at(fname, lineno, "target file '%s' has both : and :: entries", tn);
    file *t = f;
    if (dc) {
      if (f->dc && f->is_target) t = dc_entry(f);
      f->dc = 1;
    }
    f->is_target = 1;
    size_t so = 0, sl = 0;
    const char *stem = 0;
    if (upat) {
      if (!pattern_match(upat, ppct, tn, strlen(tn), &so, &sl)) {
        warn_at(fname, lineno, "target '%s' doesn't match the target pattern", tn);
        stem = 0;
      } else {
        stem = tn + so;
        free(t->stem);
        t->stem = xstrndup(stem, sl);
      }
    }
    dep *nd;
    if (upat && !stem) nd = 0;
    else nd = make_deps(deptext, upat, ppct, stem, sl, need2);
    if (nd && need2 && upat) for (dep *d = nd; d; d = d->next) d->wait = 2; /* static pattern: % after 2nd expansion */
    cur.oldhead[i] = t->deps;
    cur.mark[i] = nd;
    if (nd) dep_append(t, nd);
    if (need2) f->needs2 = 1;
    cur.t[cur.nt++] = t;
    if (i == 0 || 1) {
      if (default_goal_ok(tn)) {
        var *dg = var_lookup_global(".DEFAULT_GOAL", 13);
        if (!dg || !*dg->value) record_default_goal(tn);
      }
    }
  }
  if (grouped && cur.nt > 1) {
    for (int i = 0; i < cur.nt; i++) {
      file *f = cur.t[i];
      f->grouped = 1;
      f->also = xcalloc(cur.nt, sizeof *f->also);
      f->nalso = 0;
      for (int j = 0; j < cur.nt; j++) if (j != i) f->also[f->nalso++] = cur.t[j];
    }
  }
  free(upat);
  free(pat);
  free(rexp);
  sl_free(&targets);
  if (recipe) add_recipe_line(recipe, lineno);
}

/* ---- conditionals ---- */
static int eval_cond(const char *kw, const char *s, const char *fname, int lineno) {
  if (!strcmp(kw, "ifdef") || !strcmp(kw, "ifndef")) {
    char *e = expand(s);
    buf b = {0};
    strip_into(&b, e, strlen(e));
    free(e);
    var *v = var_lookup(bstr(&b), b.n);
    int def = v && *v->value;
    bfree(&b);
    return kw[2] == 'd' ? def : !def;
  }
  int eq = !strcmp(kw, "ifeq");
  char *a1, *a2;
  const char *p = skip_ws(s);
  if (*p == '(') {
    p++;
    int depth = 0;
    const char *q = p;
    for (; *q; q++) {
      if (*q == '(') depth++;
      else if (*q == ')') { if (depth == 0) break; depth--; }
      else if (*q == ',' && depth == 0) break;
    }
    if (*q != ',') fatal_at(fname, lineno, "invalid syntax in conditional");
    const char *e1 = q;
    while (e1 > p && is_blank(e1[-1])) e1--;
    a1 = xstrndup(p, e1 - p);
    q = skip_ws(q + 1);
    const char *b2 = q;
    depth = 0;
    for (; *q; q++) {
      if (*q == '(') depth++;
      else if (*q == ')') { if (depth == 0) break; depth--; }
    }
    if (*q != ')') fatal_at(fname, lineno, "invalid syntax in conditional");
    a2 = xstrndup(b2, q - b2);
    const char *after = skip_ws(q + 1);
    if (*after) warn_at(fname, lineno, "extraneous text after '%s' directive", kw);
  } else if (*p == '"' || *p == '\'') {
    char qc = *p++;
    const char *e = strchr(p, qc);
    if (!e) fatal_at(fname, lineno, "invalid syntax in conditional");
    a1 = xstrndup(p, e - p);
    p = skip_ws(e + 1);
    if (*p != '"' && *p != '\'') { free(a1); fatal_at(fname, lineno, "invalid syntax in conditional"); }
    qc = *p++;
    e = strchr(p, qc);
    if (!e) { free(a1); fatal_at(fname, lineno, "invalid syntax in conditional"); }
    a2 = xstrndup(p, e - p);
    const char *after = skip_ws(e + 1);
    if (*after) warn_at(fname, lineno, "extraneous text after '%s' directive", kw);
  } else fatal_at(fname, lineno, "invalid syntax in conditional");
  char *x1 = expand(a1), *x2 = expand(a2);
  int same = !strcmp(x1, x2);
  free(a1); free(a2); free(x1); free(x2);
  return eq ? same : !same;
}

static int cond_keyword(const char *s, char *kw, const char **rest) {
  static const char *kws[] = {"ifdef", "ifndef", "ifeq", "ifneq", "else", "endif", 0};
  for (int i = 0; kws[i]; i++) {
    size_t n = strlen(kws[i]);
    if (!strncmp(s, kws[i], n) && (!s[n] || is_space(s[n]) || ((i == 2 || i == 3) && (s[n] == '(' || s[n] == '"' || s[n] == '\'')))) {
      strcpy(kw, kws[i]);
      *rest = skip_ws(s + n);
      return 1;
    }
  }
  return 0;
}

static void handle_cond(const char *kw, const char *rest, const char *fname, int lineno) {
  if (!strcmp(kw, "endif")) {
    if (!nconds) fatal_at(fname, lineno, "extraneous '%s'", "endif");
    if (*rest) warn_at(fname, lineno, "extraneous text after '%s' directive", "endif");
    nconds--;
    return;
  }
  if (!strcmp(kw, "else")) {
    if (!nconds) fatal_at(fname, lineno, "extraneous '%s'", "else");
    cond *c = &conds[nconds - 1];
    if (c->seen_else) fatal_at(fname, lineno, "only one 'else' per conditional");
    char kw2[16];
    const char *r2;
    int outer_ignoring = 0;
    for (int i = 0; i < nconds - 1; i++) if (conds[i].state) outer_ignoring = 1;
    if (*rest && cond_keyword(rest, kw2, &r2) && strcmp(kw2, "else") && strcmp(kw2, "endif")) {
      if (c->state == 1 && !outer_ignoring) c->state = eval_cond(kw2, r2, fname, lineno) ? 0 : 1;
      else c->state = 2;
      return;
    }
    if (*rest) warn_at(fname, lineno, "extraneous text after '%s' directive", "else");
    c->seen_else = 1;
    c->state = c->state == 1 ? 0 : 2;
    return;
  }
  if (nconds >= 256) fatal_at(fname, lineno, "conditionals nested too deeply");
  cond *c = &conds[nconds++];
  c->seen_else = 0;
  int outer = 0;
  for (int i = 0; i < nconds - 1; i++) if (conds[i].state) outer = 1;
  if (outer) c->state = 2;
  else c->state = eval_cond(kw, rest, fname, lineno) ? 0 : 1;
}

/* ---- define ---- */
static void read_define(reader *r, const char *rest, int origin, int exp, int priv, const char *fname, int lineno) {
  /* rest: "NAME [op]" */
  char *spec = xstrdup(rest);
  /* trailing blanks */
  char *e = spec + strlen(spec);
  while (e > spec && is_space(e[-1])) *--e = 0;
  int op = '=';
  size_t sl = strlen(spec);
  if (sl >= 4 && !strcmp(spec + sl - 4, ":::=")) { op = '3'; spec[sl - 4] = 0; }
  else if (sl >= 3 && !strcmp(spec + sl - 3, "::=")) { op = ':'; spec[sl - 3] = 0; }
  else if (sl >= 2 && !strcmp(spec + sl - 2, ":=")) { op = ':'; spec[sl - 2] = 0; }
  else if (sl >= 2 && (!strcmp(spec + sl - 2, "+=") || !strcmp(spec + sl - 2, "?=") || !strcmp(spec + sl - 2, "!="))) { op = spec[sl - 2]; spec[sl - 2] = 0; }
  else if (sl >= 1 && spec[sl - 1] == '=') { spec[sl - 1] = 0; }
  e = spec + strlen(spec);
  while (e > spec && is_space(e[-1])) *--e = 0;
  if (!*spec) fatal_at(fname, lineno, "empty variable name");
  char *name = expand(spec);
  free(spec);
  buf body = {0};
  int depth = 1, first = 1;
  buf ln = {0};
  int lno;
  for (;;) {
    if (!raw_line(r, &ln, &lno)) fatal_at(fname, lineno, "missing 'endef', unterminated 'define'");
    const char *s = skip_ws(ln.s);
    char kw[16];
    (void)kw;
    if (!strncmp(s, "endef", 5) && (!s[5] || is_space(s[5]) || s[5] == '#')) {
      if (--depth == 0) {
        const char *after = skip_ws(s + 5);
        if (*after && *after != '#') warn_at(fname, lno, "extraneous text after 'endef' directive");
        break;
      }
    } else if (!strncmp(s, "define", 6) && (!s[6] || is_space(s[6]))) depth++;
    else {
      const char *t = s;
      const char *a;
      int prefixed = 0;
      while (word_is(t, "override", &a) || word_is(t, "export", &a) || word_is(t, "private", &a)) { t = a; prefixed = 1; }
      if (prefixed && !strncmp(t, "define", 6) && (!t[6] || is_space(t[6]))) depth++;
    }
    if (!first) bputc(&body, '\n');
    first = 0;
    collapse(&ln); /* backslash-newline inside define joins lines too */
    bput(&body, ln.s, ln.n);
  }
  bfree(&ln);
  if (ignoring()) { bfree(&body); free(name); return; }
  var *v = var_assign(&globals, name, body.s ? bstr(&body) : "", op, origin, 0);
  if (v) {
    if (exp) v->exp = exp;
    if (priv) v->priv = 1;
  }
  check_special_var(name);
  bfree(&body);
  free(name);
}

/* ---- include ---- */
static int try_read(const char *path, int flags);
static void do_include(const char *rest, int optional, const char *fname, int lineno) {
  char *e = expand(rest);
  strlist names = {0};
  split_names(e, &names, 1);
  free(e);
  for (int i = 0; i < names.n; i++) {
    const char *nm = names.v[i];
    if (try_read(nm, 0)) continue;
    int found = 0;
    if (nm[0] != '/') {
      for (int k = 0; k < include_dirs.n && !found; k++) {
        char *p = xconcat(include_dirs.v[k], "/", nm);
        if (try_read(p, 0)) found = 1;
        free(p);
      }
      static const char *defdirs[] = {"/usr/local/include", "/usr/include", 0};
      for (int k = 0; defdirs[k] && !found; k++) {
        char *p = xconcat(defdirs[k], "/", nm);
        if (try_read(p, 0)) found = 1;
        free(p);
      }
    }
    if (found) continue;
    missing_incs = xrealloc(missing_incs, (nmissing_incs + 1) * sizeof *missing_incs);
    incl *m = &missing_incs[nmissing_incs++];
    m->name = xstrdup(nm);
    m->from = fname;
    m->line = lineno;
    m->optional = optional;
    file_enter(nm)->is_makefile = 1;
  }
  sl_free(&names);
}

/* ---- the line dispatcher ---- */
static void read_lines(reader *r) {
  buf b = {0};
  int lineno;
  int base = nconds;
  const char *save_name = read_file_name;
  int save_line = read_line;
  while (raw_line(r, &b, &lineno)) {
    read_file_name = r->name;
    read_line = lineno;
    int started_with_prefix = b.n > 0 && b.s[0] == recipe_prefix;
    /* recipe line */
    if (started_with_prefix && cur.active) {
      if (ignoring()) continue;
      /* drop the prefix, and the prefix after each backslash-newline */
      buf rl = {0};
      for (size_t i = 1; i < b.n; i++) {
        bputc(&rl, b.s[i]);
        if (b.s[i] == '\n' && i > 0 && b.s[i - 1] == '\\' && i + 1 < b.n && b.s[i + 1] == recipe_prefix) i++;
      }
      add_recipe_line(rl.s ? bstr(&rl) : "", lineno);
      bfree(&rl);
      continue;
    }
    collapse(&b);
    strip_comment(&b);
    const char *s = skip_ws(b.s);
    if (!*s) continue;
    char kw[16];
    const char *rest;
    if (cond_keyword(s, kw, &rest)) {
      handle_cond(kw, rest, r->name, lineno);
      continue;
    }
    if (ignoring()) {
      /* skip define bodies whole, so their lines are not mistaken for directives */
      const char *t = s, *a;
      while (word_is(t, "override", &a) || word_is(t, "export", &a) || word_is(t, "private", &a)) t = a;
      if (!strncmp(t, "define", 6) && (!t[6] || is_space(t[6]))) {
        int depth = 1;
        buf ln = {0};
        int lno;
        while (depth && raw_line(r, &ln, &lno)) {
          const char *q = skip_ws(ln.s);
          if (!strncmp(q, "endef", 5) && (!q[5] || is_space(q[5]))) depth--;
          else if (!strncmp(q, "define", 6) && (!q[6] || is_space(q[6]))) depth++;
        }
        bfree(&ln);
      }
      continue;
    }
    /* directives */
    int origin = reading_builtin ? O_DEFAULT : O_FILE, exp = 0, priv = 0, saw_prefix = 0;
    const char *a;
    const char *t = s;
    for (;;) {
      if (word_is(t, "override", &a)) { origin = O_OVERRIDE; t = a; saw_prefix = 1; continue; }
      if (word_is(t, "export", &a)) { exp = EXP_YES; t = a; saw_prefix = 1; continue; }
      if (word_is(t, "unexport", &a)) { exp = EXP_NO; t = a; saw_prefix = 1; continue; }
      if (word_is(t, "private", &a)) { priv = 1; t = a; saw_prefix = 1; continue; }
      break;
    }
    if (word_is(t, "define", &a)) {
      finish_rule();
      read_define(r, a, origin, exp, priv, r->name, lineno);
      continue;
    }
    if (word_is(t, "endef", &a) && !saw_prefix) fatal_at(r->name, lineno, "missing separator");
    if (word_is(t, "undefine", &a)) {
      finish_rule();
      char *e = expand(a);
      strlist nl = {0};
      split_names(e, &nl, 0);
      for (int i = 0; i < nl.n; i++) var_undefine(nl.v[i], origin);
      sl_free(&nl);
      free(e);
      continue;
    }
    if (!saw_prefix) {
      int inc = 0, opt = 0;
      if (word_is(t, "include", &a)) inc = 1;
      else if (word_is(t, "-include", &a) || word_is(t, "sinclude", &a)) { inc = 1; opt = 1; }
      if (inc) {
        finish_rule();
        do_include(a, opt, r->name, lineno);
        continue;
      }
      if (word_is(t, "vpath", &a)) {
        finish_rule();
        char *e = expand(a);
        const char *p = e;
        size_t len;
        const char *w = next_word(&p, &len);
        if (!w) vpath_add(0, 0);
        else {
          char *pat = xstrndup(w, len);
          vpath_add(pat, skip_ws(p));
          free(pat);
        }
        free(e);
        continue;
      }
      if (word_is(t, "load", &a) || word_is(t, "-load", &a)) {
        fatal_at(r->name, lineno, "the 'load' directive is not supported");
      }
    }
    size_t ne, vs;
    int op;
    if (find_assign(t, &ne, &vs, &op)) {
      finish_rule();
      do_assign(t, &globals, origin, exp, priv, 0);
      continue;
    }
    if (saw_prefix && (exp || priv) && origin != O_OVERRIDE) {
      /* export NAMES / unexport NAMES / export alone */
      finish_rule();
      if (!*t) { export_all = exp == EXP_YES; continue; }
      char *e = expand(t);
      strlist nl = {0};
      split_names(e, &nl, 0);
      for (int i = 0; i < nl.n; i++) {
        var *v = var_lookup_global(nl.v[i], strlen(nl.v[i]));
        if (!v) v = var_set(&globals, nl.v[i], "", F_RECURSIVE, O_FILE), v->defined_in_define = 2;
        v->exp = exp;
      }
      sl_free(&nl);
      free(e);
      continue;
    }
    if (saw_prefix && origin == O_OVERRIDE && !*t) fatal_at(r->name, lineno, "missing separator");
    /* a rule? find ';' and ':' at the top level */
    long semi = -1, colon = -1;
    for (long i = 0; s[i]; i++) {
      if (s[i] == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) { i = skip_ref(s, i); continue; }
      if (s[i] == ';' && semi < 0) { semi = i; break; }
      if (s[i] == ':' && colon < 0) colon = i;
    }
    if (colon >= 0 && (semi < 0 || colon < semi)) {
      /* the recipe after ';' is taken from the raw line: comments there go to the shell */
      char *head = xstrndup(s, semi >= 0 ? (size_t)semi : strlen(s));
      const char *recipe = 0;
      char *rbuf = 0;
      if (semi >= 0) {
        /* re-read the raw line text after the first top-level ';' */
        rbuf = xstrdup(s + semi + 1);
        recipe = skip_ws(rbuf);
      }
      parse_rule(head, colon, recipe, lineno, r->name);
      free(head);
      free(rbuf);
      continue;
    }
    /* something else: expand it; empty is fine ($(eval ...) lines) */
    char *e = expand(s);
    const char *es = skip_ws(e);
    if (!*es) { free(e); continue; }
    if (strchr(es, ':')) {
      /* the expansion is a rule */
      long c2 = strchr(es, ':') - es;
      long sm = strchr(es, ';') ? strchr(es, ';') - es : -1;
      char *head = xstrndup(es, sm >= 0 && sm > c2 ? (size_t)sm : strlen(es));
      parse_rule(head, c2, sm > c2 ? skip_ws(es + sm + 1) : 0, lineno, r->name);
      free(head);
      free(e);
      continue;
    }
    free(e);
    if (started_with_prefix) fatal_at(r->name, lineno, "recipe commences before first target");
    if (!strncmp(b.s, "        ", 8) && recipe_prefix == '\t') fatal_at(r->name, lineno, "missing separator (did you mean TAB instead of 8 spaces?)");
    fatal_at(r->name, lineno, "missing separator");
  }
  bfree(&b);
  if (nconds > base) fatal_at(r->name, r->line + 1, "missing 'endif'");
  read_file_name = save_name;
  read_line = save_line;
}

static void read_buffer(const char *name, const char *text, size_t len, int line0) {
  reader r = {name, text, len, 0, line0};
  pending save = cur;
  memset(&cur, 0, sizeof cur);
  read_lines(&r);
  finish_rule();
  cur = save;
}

void read_text(const char *text, const char *name, int line) {
  read_buffer(name ? name : "", text, strlen(text), line > 0 ? line - 1 : 0);
}

static char *slurp(const char *path, size_t *len) {
  FILE *f = strcmp(path, "-") ? fopen(path, "r") : stdin;
  if (!f) return 0;
  buf b = {0};
  char tmp[8192];
  size_t r;
  while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) bput(&b, tmp, r);
  if (f != stdin) fclose(f);
  *len = b.n;
  char *s = bdetach(&b);
  return s ? s : xstrdup("");
}

static int try_read(const char *path, int flags) {
  (void)flags;
  if (strcmp(path, "-") && sys_isdir(path)) return 0;
  size_t len;
  char *text = slurp(path, &len);
  if (!text) return 0;
  char *name = xstrdup(path);
  sl_add(&makefiles_read, name);
  file_enter(name)->is_makefile = 1;
  /* MAKEFILE_LIST */
  var *ml = var_lookup_global("MAKEFILE_LIST", 13);
  buf b = {0};
  if (ml && *ml->value) { bputs(&b, ml->value); bputc(&b, ' '); }
  bputs(&b, name);
  var_set(&globals, "MAKEFILE_LIST", bstr(&b), F_SIMPLE, O_FILE);
  bfree(&b);
  read_buffer(name, text, len, 0);
  /* the text stays allocated: recipe lines and names point into copies only */
  free(text);
  return 1;
}

void read_makefile(const char *name, int flags) {
  if (!try_read(name, flags)) {
    missing_incs = xrealloc(missing_incs, (nmissing_incs + 1) * sizeof *missing_incs);
    incl *m = &missing_incs[nmissing_incs++];
    m->name = xstrdup(name);
    m->from = 0;
    m->line = 0;
    m->optional = flags;
    file_enter(name)->is_makefile = 1;
  }
}
