// SPDX-License-Identifier: GPL-2.0-or-later
/* make: the file database, time stamps, directory search, pattern rules,
 * suffix rules and the implicit rule search. */
#include "make.h"

htab files;
rule **rules;
int nrules;
static int rule_seq;

file *file_lookupn(const char *name, size_t n) { return hget(&files, name, n); }
file *file_lookup(const char *name) { return file_lookupn(name, strlen(name)); }

/* "./foo" and "foo" are the same file, as in GNU Make */
static void norm_name(const char **name, size_t *n) {
  while (*n > 2 && (*name)[0] == '.' && (*name)[1] == '/') {
    *name += 2; *n -= 2;
    while (*n > 1 && **name == '/') { (*name)++; (*n)--; }
  }
}
file *file_entern(const char *name, size_t n) {
  norm_name(&name, &n);
  int isnew;
  void **slot = hput(&files, name, n, &isnew);
  if (!isnew && *slot) return *slot;
  file *f = xcalloc(1, sizeof *f);
  f->name = xstrndup(name, n);
  f->deptail = &f->deps;
  f->mtime = MT_UNKNOWN;
  f->in_db = 1;
  *slot = f;
  return f;
}
file *file_enter(const char *name) { return file_entern(name, strlen(name)); }

dep *dep_new(file *f) {
  dep *d = xcalloc(1, sizeof *d);
  d->f = f;
  return d;
}
void dep_append(file *t, dep *d) {
  *t->deptail = d;
  while (d->next) d = d->next;
  t->deptail = &d->next;
}

/* ---- time stamps ---- */
static htab statcache;
typedef struct { mtime_t mt; } stent;
mtime_t stat_mtime(const char *path) {
  size_t n = strlen(path);
  stent *e = hget(&statcache, path, n);
  if (e) return e->mt;
  sysstat st;
  mtime_t mt = MT_MISSING;
  if (sys_stat(path, &st) == 0 && st.exists) mt = st.mtime < MT_OLD + 1 ? MT_OLD + 1 : st.mtime;
  void **slot = hput(&statcache, path, n, 0);
  if (!*slot) *slot = xmalloc(sizeof(stent));
  ((stent *)*slot)->mt = mt;
  return mt;
}
void stat_forget(const char *path) {
  void **slot;
  size_t n = strlen(path);
  if (hget(&statcache, path, n)) { slot = hput(&statcache, path, n, 0); free(*slot); *slot = 0; }
}

/* ar archives: lib.a(member) time stamps, read from the member headers */
static mtime_t ar_member_mtime(const char *arname, const char *mem) {
  FILE *f = fopen(arname, "rb");
  if (!f) return MT_MISSING;
  char magic[8];
  mtime_t r = MT_MISSING;
  if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "!<arch>\n", 8)) { fclose(f); return MT_MISSING; }
  char *names = 0; long nameslen = 0;
  char hdr[60];
  const char *base = strrchr(mem, '/');
  base = base ? base + 1 : mem;
  while (fread(hdr, 1, 60, f) == 60) {
    char sz[11]; memcpy(sz, hdr + 48, 10); sz[10] = 0;
    long size = strtol(sz, 0, 10);
    char nm[17]; memcpy(nm, hdr, 16); nm[16] = 0;
    char date[13]; memcpy(date, hdr + 16, 12); date[12] = 0;
    char full[256];
    if (!strncmp(nm, "//", 2)) {
      free(names);
      names = xmalloc(size + 1);
      if (fread(names, 1, size, f) != (size_t)size) break;
      names[size] = 0; nameslen = size;
      if (size & 1) fgetc(f);
      continue;
    }
    if (nm[0] == '/' && isdigit((unsigned char)nm[1]) && names) {
      long off = strtol(nm + 1, 0, 10);
      int k = 0;
      while (off + k < nameslen && names[off + k] != '/' && names[off + k] != '\n' && k < 255) { full[k] = names[off + k]; k++; }
      full[k] = 0;
    } else {
      int k = 0;
      while (k < 16 && nm[k] != '/' && nm[k] != ' ') { full[k] = nm[k]; k++; }
      full[k] = 0;
    }
    if (full[0] && !strcmp(full, base)) {
      long long t = strtoll(date, 0, 10);
      r = (mtime_t)t * 1000000000LL;
      if (r <= MT_OLD) r = MT_OLD + 1;
      break;
    }
    fseek(f, size + (size & 1), SEEK_CUR);
  }
  free(names);
  fclose(f);
  return r;
}
static int is_archive_member(const char *name, size_t *lp) {
  size_t n = strlen(name);
  if (n < 3 || name[n - 1] != ')') return 0;
  const char *p = strchr(name, '(');
  if (!p || p == name) return 0;
  if (lp) *lp = p - name;
  return 1;
}

mtime_t file_mtime(file *f) {
  if (f->mtime != MT_UNKNOWN) return f->mtime;
  if (f->dchead) return f->mtime = file_mtime(f->dchead);
  mtime_t mt;
  size_t lp;
  if (is_archive_member(f->name, &lp)) {
    char *ar = xstrndup(f->name, lp);
    char *mem = xstrndup(f->name + lp + 1, strlen(f->name) - lp - 2);
    mt = ar_member_mtime(ar, mem);
    free(ar); free(mem);
  } else {
    mt = stat_mtime(f->name);
    if (mt == MT_MISSING && f->name[0] != '/' && !f->vpath && !f->phony) {
      char *vp = vpath_search(f->name);
      if (vp) { f->vpath = vp; mt = stat_mtime(vp); }
    } else if (f->vpath) mt = stat_mtime(f->vpath);
  }
  if (f->oldfile && mt != MT_MISSING) mt = MT_OLD;
  if (f->newfile) mt = MT_NEW - 1;
  f->mtime = mt;
  return mt;
}
int file_exists_or_vpath(file *f) { return file_mtime(f) != MT_MISSING; }

/* ---- directory search ---- */
typedef struct vpath_ent { char *pat; const char *pct; strlist dirs; } vpath_ent;
static vpath_ent *vpaths;
static int nvpaths;
static void split_dirs(strlist *out, const char *s) {
  const char *p = s;
  while (*p) {
    while (*p == ':' || is_space(*p)) p++;
    const char *e = p;
    while (*e && *e != ':' && !is_space(*e)) e++;
    if (e > p) {
      size_t n = e - p;
      while (n > 1 && p[n - 1] == '/') n--;
      sl_add(out, xstrndup(p, n));
    }
    p = e;
  }
}
void vpath_add(const char *pattern, const char *dirs) {
  if (!pattern) { /* vpath alone: forget everything */
    for (int i = 0; i < nvpaths; i++) { free(vpaths[i].pat); sl_free(&vpaths[i].dirs); }
    nvpaths = 0;
    return;
  }
  if (!dirs || !*skip_ws(dirs)) { /* vpath PATTERN: forget that pattern */
    int j = 0;
    for (int i = 0; i < nvpaths; i++) {
      if (!strcmp(vpaths[i].pat, pattern)) { free(vpaths[i].pat); sl_free(&vpaths[i].dirs); continue; }
      vpaths[j++] = vpaths[i];
    }
    nvpaths = j;
    return;
  }
  vpaths = xrealloc(vpaths, (nvpaths + 1) * sizeof *vpaths);
  vpath_ent *e = &vpaths[nvpaths++];
  memset(e, 0, sizeof *e);
  e->pat = unescape_percent(pattern, &e->pct);
  split_dirs(&e->dirs, dirs);
}
static char *try_dirs(strlist *dirs, const char *name) {
  for (int i = 0; i < dirs->n; i++) {
    char *p = xconcat(dirs->v[i], "/", name);
    if (stat_mtime(p) != MT_MISSING) return p;
    free(p);
  }
  return 0;
}
char *vpath_search(const char *name) {
  size_t n = strlen(name);
  for (int i = 0; i < nvpaths; i++) {
    if (!pattern_match(vpaths[i].pat, vpaths[i].pct, name, n, 0, 0)) continue;
    char *r = try_dirs(&vpaths[i].dirs, name);
    if (r) return r;
  }
  var *v = var_lookup_global("VPATH", 5);
  if (v && *v->value) {
    char *val = var_value(v);
    strlist dirs = {0};
    split_dirs(&dirs, val);
    char *r = try_dirs(&dirs, name);
    sl_free(&dirs);
    free(val);
    if (r) return r;
  }
  return 0;
}

/* ---- suffixes ---- */
int is_suffix(const char *s, size_t n) {
  for (int i = 0; i < nsuffixes; i++) if (strlen(suffixes[i]) == n && !memcmp(suffixes[i], s, n)) return 1;
  return 0;
}
char *suffix_stem(const char *name) {
  size_t n = strlen(name);
  for (int i = 0; i < nsuffixes; i++) {
    size_t l = strlen(suffixes[i]);
    if (l < n && !memcmp(name + n - l, suffixes[i], l)) return xstrndup(name, n - l);
  }
  return 0;
}

/* ---- pattern rules ---- */
static int same_rule(rule *a, rule *b) {
  if (a->nt != b->nt) return 0;
  for (int i = 0; i < a->nt; i++) if (strcmp(a->targets[i], b->targets[i])) return 0;
  dep *x = a->deps, *y = b->deps;
  for (; x && y; x = x->next, y = y->next) if (strcmp(x->name, y->name) || x->oo != y->oo) return 0;
  return !x && !y;
}
void rule_add(rule *r, int override) {
  r->seq = rule_seq++;
  for (int i = 0; i < nrules; i++) {
    if (!same_rule(rules[i], r)) continue;
    if (!override) return; /* built-in rules never replace */
    /* replace; a rule with no recipe stays as a marker that cancels built-ins */
    memmove(&rules[i], &rules[i + 1], (nrules - i - 1) * sizeof *rules);
    nrules--;
    break;
  }
  rules = xrealloc(rules, (nrules + 1) * sizeof *rules);
  rules[nrules++] = r;
}
static rule *rule_simple(const char *tpat, const char *dpat, cmds *c) {
  rule *r = xcalloc(1, sizeof *r);
  r->nt = 1;
  r->targets = xmalloc(sizeof(char *));
  r->pcts = xmalloc(sizeof(char *));
  r->targets[0] = unescape_percent(tpat, &r->pcts[0]);
  if (dpat) {
    dep *d = dep_new(0);
    d->name = xstrdup(dpat);
    r->deps = d;
  }
  r->cmds = c;
  return r;
}

/* Turn .c.o: and .c: into %.o: %.c and %: %.c, in the order GNU Make uses. */
void rules_finish(void) {
  for (int i = 0; i < nsuffixes; i++) {
    const char *s = suffixes[i];
    char *tp = xconcat("%", s, 0);
    rule_add(rule_simple(tp, 0, 0), 0); /* a "specific type" marker */
    free(tp);
    file *f = file_lookup(s);
    if (f && f->cmds && !f->deps) {
      char *dp = xconcat("%", s, 0);
      rule *r = rule_simple("%", dp, f->cmds);
      r->from_suffix = 1;
      rule_add(r, 0);
      free(dp);
    }
    for (int j = 0; j < nsuffixes; j++) {
      const char *t = suffixes[j];
      char *nm = xconcat(s, t, 0);
      file *g = file_lookup(nm);
      free(nm);
      if (!g || !g->cmds || g->deps) continue;
      char *dp = xconcat("%", s, 0);
      char *tp2;
      if (!strcmp(t, ".a")) {
        tp2 = xstrdup("(%.o)");
        rule *ra = rule_simple(tp2, dp, g->cmds);
        ra->from_suffix = 1;
        rule_add(ra, 0);
        free(tp2);
      }
      tp2 = xconcat("%", t, 0);
      rule *r = rule_simple(tp2, dp, g->cmds);
      r->from_suffix = 1;
      rule_add(r, 0);
      free(dp); free(tp2);
    }
  }
}

/* ---- pattern-specific variables ---- */
typedef struct pvar { char *pat; const char *pct; vset set; } pvar;
static pvar *pvars;
static int npvars;
void pattern_var_add(const char *pattern, char *line_rest, int origin, int export_kind, int priv) {
  pvar *p = 0;
  for (int i = 0; i < npvars; i++) if (!strcmp(pvars[i].pat, pattern)) { p = &pvars[i]; break; }
  if (!p) {
    pvars = xrealloc(pvars, (npvars + 1) * sizeof *pvars);
    p = &pvars[npvars++];
    memset(p, 0, sizeof *p);
    char *u = unescape_percent(pattern, &p->pct);
    p->pat = u;
  }
  size_t ne, vs;
  int op;
  if (!find_assign(line_rest, &ne, &vs, &op)) return;
  char *name = xstrndup(line_rest, ne);
  /* trim */
  char *e = name + strlen(name);
  while (e > name && is_space(e[-1])) *--e = 0;
  char *nm = expand(name);
  var *v = var_assign(&p->set, nm, line_rest + vs, op, origin, 1);
  if (v) {
    if (export_kind) v->exp = export_kind;
    if (priv) v->priv = 1;
  }
  free(nm);
  free(name);
}

static scope *scope_node(vset *s, int priv_ok, scope *next) {
  scope *n = xmalloc(sizeof *n);
  n->set = s; n->priv_ok = priv_ok; n->next = next;
  return n;
}
void build_scope(file *f) {
  /* parent's chain, copied with private variables hidden */
  scope *tail = 0, **tp = &tail;
  if (f->parent && f->parent->sc) {
    for (scope *s = f->parent->sc; s; s = s->next) {
      *tp = scope_node(s->set, 0, 0);
      tp = &(*tp)->next;
    }
  }
  /* pattern-specific: longer stems first in the chain end, so shorter ones win */
  int nm = 0;
  int *idx = 0;
  size_t *stl = 0;
  size_t fl = strlen(f->name);
  for (int i = 0; i < npvars; i++) {
    size_t so, sl;
    if (!pattern_match(pvars[i].pat, pvars[i].pct, f->name, fl, &so, &sl)) continue;
    idx = xrealloc(idx, (nm + 1) * sizeof *idx);
    stl = xrealloc(stl, (nm + 1) * sizeof *stl);
    idx[nm] = i; stl[nm] = sl; nm++;
  }
  /* insertion sort by stem length, descending, stable */
  for (int i = 1; i < nm; i++) {
    int j = i;
    while (j > 0 && stl[j - 1] < stl[j]) {
      int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
      size_t ts = stl[j]; stl[j] = stl[j - 1]; stl[j - 1] = ts;
      j--;
    }
  }
  scope *head = tail;
  for (int i = 0; i < nm; i++) head = scope_node(&pvars[idx[i]].set, 1, head);
  free(idx); free(stl);
  file *own = f->dchead ? f->dchead : f;
  if (own->vars) head = scope_node(own->vars, 1, head);
  f->sc = head;
}

/* ---- implicit rule search ---- */
typedef struct plan {
  rule *r;
  int ti;
  char *stem;      /* with the directory */
  strlist deps;    /* names */
  unsigned char *oo;
  struct plan **sub; /* plans for intermediate deps, or NULL */
} plan;

static void plan_free(plan *p) {
  if (!p) return;
  for (int i = 0; i < p->deps.n; i++) if (p->sub) plan_free(p->sub[i]);
  free(p->sub);
  free(p->oo);
  sl_free(&p->deps);
  free(p->stem);
  free(p);
}

static int is_explicit_dep_of(file *t, const char *name) {
  if (!t) return 0;
  for (dep *d = t->deps; d; d = d->next) if (d->f && !d->implicit && !strcmp(d->f->name, name)) return 1;
  return 0;
}
static int ought_to_exist(const char *name, file *t, int compat) {
  file *f = file_lookup(name);
  if (f && (f->is_target || f->phony)) return 1;
  if (compat) return f && f->mentioned;
  return is_explicit_dep_of(t, name);
}
static int exists_now(const char *name) {
  file *f = file_lookup(name);
  if (f) return file_mtime(f) != MT_MISSING;
  if (stat_mtime(name) != MT_MISSING) return 1;
  if (name[0] != '/') {
    char *vp = vpath_search(name);
    if (vp) { free(vp); return 1; }
  }
  return 0;
}

typedef struct cand { rule *r; int ti; char *stem; size_t sl, full; } cand;

static void expand_rule_deps(plan *p, rule *r, const char *dirpart, size_t dl, const char *nstem, size_t nsl, int slash, file *t) {
  for (dep *d = r->deps; d; d = d->next) {
    const char *pct;
    char *pat = unescape_percent(d->name, &pct);
    buf b = {0};
    if (pct) {
      if (!slash) bput(&b, dirpart, dl);
      pattern_subst(&b, pat, pct, nstem, nsl);
    } else bputs(&b, pat);
    free(pat);
    char *nm = bdetach(&b);
    if (!nm) nm = xstrdup("");
    if (d->need2 || r->need2) {
      /* second expansion of a pattern rule's prerequisites */
      file *save = auto_file;
      int s2 = auto_2nd;
      file tmpf;
      if (t) {
        auto_file = t;
        auto_2nd = 1;
        char *saved_stem = t->stem;
        buf sb = {0};
        bput(&sb, dirpart, slash ? 0 : dl);
        bput(&sb, nstem, nsl);
        t->stem = bdetach(&sb);
        char *e = expand(nm);
        free(t->stem);
        t->stem = saved_stem;
        free(nm);
        nm = e;
      }
      (void)tmpf;
      auto_file = save;
      auto_2nd = s2;
      const char *q = nm;
      size_t len;
      const char *w;
      int any = 0;
      while ((w = next_word(&q, &len))) {
        sl_add(&p->deps, xstrndup(w, len));
        p->oo = xrealloc(p->oo, p->deps.n);
        p->oo[p->deps.n - 1] = d->oo;
        any = 1;
      }
      (void)any;
      free(nm);
      continue;
    }
    sl_add(&p->deps, nm);
    p->oo = xrealloc(p->oo, p->deps.n);
    p->oo[p->deps.n - 1] = d->oo;
  }
}

static plan *search(const char *name, int depth, file *t);

static plan *search_pass(cand *cs, int nc, const char *name, int depth, file *t, int compat, int pass2) {
  size_t n = strlen(name);
  const char *slash = strrchr(name, '/');
  size_t dl = slash ? (size_t)(slash - name + 1) : 0;
  for (int i = 0; i < nc; i++) {
    rule *r = cs[i].r;
    if (pass2 && r->terminal) continue;
    if (r->in_use) continue;
    int tslash = strchr(r->targets[cs[i].ti], '/') != 0;
    plan *p = xcalloc(1, sizeof *p);
    p->r = r;
    p->ti = cs[i].ti;
    /* the stem as $* sees it: directory plus the matched part */
    const char *ns = cs[i].stem;
    size_t nsl = cs[i].sl;
    buf sb = {0};
    if (!tslash) bput(&sb, name, dl);
    bput(&sb, ns, nsl);
    p->stem = bdetach(&sb);
    if (!p->stem) p->stem = xstrdup("");
    expand_rule_deps(p, r, name, dl, ns, nsl, tslash, t);
    int ok = 1;
    p->sub = xcalloc(p->deps.n + 1, sizeof *p->sub);
    for (int k = 0; k < p->deps.n && ok; k++) {
      const char *dn = p->deps.v[k];
      if (!strcmp(dn, name)) { ok = 0; break; }
      if (exists_now(dn) || ought_to_exist(dn, t, compat)) continue;
      if (!pass2 || r->terminal) { ok = 0; break; }
      r->in_use = 1;
      plan *sub = search(dn, depth + 1, 0);
      r->in_use = 0;
      if (!sub) { ok = 0; break; }
      p->sub[k] = sub;
    }
    (void)n;
    if (ok) return p;
    plan_free(p);
  }
  return 0;
}

static plan *search(const char *name, int depth, file *t) {
  size_t n = strlen(name);
  const char *slash = strrchr(name, '/');
  const char *nd = slash ? slash + 1 : name;
  size_t ndl = n - (nd - name);
  cand *cs = 0;
  int nc = 0, specific = 0;
  if (depth > 32) return 0;
  for (int i = 0; i < nrules; i++) {
    rule *r = rules[i];
    for (int ti = 0; ti < r->nt; ti++) {
      const char *tp = r->targets[ti];
      const char *pct = r->pcts[ti];
      size_t so, sl;
      int ok;
      const char *subj;
      if (strchr(tp, '/')) { ok = pattern_match(tp, pct, name, n, &so, &sl); subj = name; }
      else { ok = pattern_match(tp, pct, nd, ndl, &so, &sl); subj = nd; }
      if (!ok || sl == 0) continue;
      int anything = tp[0] == '%' && tp[1] == 0;
      if (!anything) specific = 1;
      cs = xrealloc(cs, (nc + 1) * sizeof *cs);
      cs[nc].r = r; cs[nc].ti = ti; cs[nc].stem = (char *)subj + so; cs[nc].sl = sl;
      cs[nc].full = sl + (subj == nd ? (size_t)(nd - name) : 0);
      nc++;
      break;
    }
  }
  /* drop non-terminal match-anything rules when something more specific matched,
   * or when looking for a prerequisite of an implicit rule; drop rules with no recipe */
  int j = 0;
  for (int i = 0; i < nc; i++) {
    rule *r = cs[i].r;
    const char *tp = r->targets[cs[i].ti];
    int anything = tp[0] == '%' && tp[1] == 0;
    if (anything && !r->terminal && (specific || depth > 0)) continue;
    if (!r->cmds) continue;
    cs[j++] = cs[i];
  }
  nc = j;
  /* shortest stem first, stable */
  for (int i = 1; i < nc; i++) {
    cand c = cs[i];
    int k = i;
    while (k > 0 && cs[k - 1].full > c.full) { cs[k] = cs[k - 1]; k--; }
    cs[k] = c;
  }
  plan *p = 0;
  for (int compat = 0; compat < 2 && !p; compat++) {
    p = search_pass(cs, nc, name, depth, t, compat, 0);
    if (!p) p = search_pass(cs, nc, name, depth, t, compat, 1);
  }
  free(cs);
  return p;
}

static int is_notint_pattern(rule *r) {
  file *ni = file_lookup(".NOTINTERMEDIATE");
  if (!ni) return 0;
  for (dep *d = ni->deps; d; d = d->next)
    for (int i = 0; i < r->nt; i++) if (d->f && !strcmp(d->f->name, r->targets[i])) return 1;
  return 0;
}

/* Apply a plan to FILE: recipe, stem, prerequisites, and intermediate files. */
static void apply_plan(file *f, plan *p) {
  rule *r = p->r;
  f->cmds = r->cmds;
  f->irule = r;
  f->stem = xstrdup(p->stem);
  /* the implicit rule's prerequisites come first */
  dep *head = 0, **tail = &head;
  for (int k = 0; k < p->deps.n; k++) {
    const char *dn = p->deps.v[k];
    file *df = file_enter(dn);
    if (p->sub && p->sub[k]) {
      if (!df->is_target && !df->mentioned && !df->notintermediate && !all_notint && !is_notint_pattern(p->sub[k]->r)) {
        df->intermediate = 1;
      }
      df->created_by_chain = 1;
      df->tried_impl = 1;
      apply_plan(df, p->sub[k]);
    }
    dep *d = dep_new(df);
    d->oo = p->oo ? p->oo[k] : 0;
    d->implicit = 1;
    *tail = d;
    tail = &d->next;
  }
  *tail = f->deps;
  if (head) {
    if (!f->deps) f->deptail = tail;
    f->deps = head;
  }
  /* the other targets of the rule are made by the same recipe */
  if (r->nt > 1) {
    f->also = xcalloc(r->nt, sizeof *f->also);
    f->nalso = 0;
    const char *slash = strrchr(f->name, '/');
    size_t dl = slash ? (size_t)(slash - f->name + 1) : 0;
    const char *nstem = p->stem + (strchr(r->targets[p->ti], '/') ? 0 : dl);
    for (int i = 0; i < r->nt; i++) {
      if (i == p->ti) continue;
      buf b = {0};
      if (!strchr(r->targets[i], '/')) bput(&b, f->name, dl);
      pattern_subst(&b, r->targets[i], r->pcts[i], nstem, strlen(nstem));
      file *o = file_enter(bstr(&b));
      bfree(&b);
      f->also[f->nalso++] = o;
    }
  }
}

int implicit_search(file *f, int depth) {
  (void)depth;
  const char *name = f->name;
  plan *p = search(name, 0, f);
  size_t lp;
  if (!p && is_archive_member(name, &lp)) {
    char *mem = xstrdup(name + lp);
    p = search(mem, 0, f);
    if (p) { /* the stem is the member's */ }
    free(mem);
  }
  if (!p) return 0;
  apply_plan(f, p);
  plan_free(p);
  return 1;
}
