// SPDX-License-Identifier: GPL-2.0-or-later
/* Dependency resolution as a satisfiability problem.
 *
 * Every package version that could matter (the requests, what is installed,
 * and everything their Depends, Pre-Depends and, if wanted, Recommends can
 * reach) becomes a boolean variable: "this version is installed afterwards".
 * The rules become clauses:
 *   Depends/Pre-Depends   v -> (a1 | a2 | ...)  every version or provider that satisfies the group
 *   Conflicts/Breaks      not (v and c)          for each version or provider c it names
 *   one version per name  not (v1 and v2)        (Multi-Arch: same versions of other architectures excepted)
 *   requests              install: one of the matching versions; remove: none of them
 *   Essential             an installed essential package stays (unless allowed)
 * A conflict-driven clause-learning search (unit propagation with two watched
 * literals, first-UIP learning, non-chronological backjumping) finds an
 * assignment. Which assignment is found is decided by the order of decisions,
 * chosen to behave like apt: satisfy open dependencies with the first
 * alternative that works (installed version, then candidate, then providers by
 * priority), keep or upgrade installed packages, then add Recommends, and
 * leave everything else out. Learning makes it complete: if no solution
 * exists, it says so instead of guessing. */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int *l; int n; int learned; pkgver *a, *b; int kind; } clause;
enum { C_DEP, C_CONFLICT, C_AMO, C_JOB, C_ESSENTIAL, C_LEARNED, C_REMOVE };
typedef struct { int *v; int n, cap; } ivec;
static void iv_push(ivec *v, int x) { if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 4; v->v = xrealloc(v->v, v->cap * sizeof *v->v); } v->v[v->n++] = x; }

typedef struct {
  pkgindex *ix;
  const solve_opts *o;
  pkgver **var; int nvar, capvar;      /* var 1..nvar */
  ivec *deps;                          /* per var: dependency clause indexes (for decisions) */
  ivec *recs;                          /* per var: recommends groups as clause-like literal lists stored in recl */
  ivec recl;                           /* flattened: count, lits... */
  ivec positive;                       /* clause indexes that are all-positive (jobs, essentials) */
  clause *cl; int ncl, capcl;
  ivec *watch;                         /* per literal */
  signed char *val;                    /* per var: 0, 1 true, -1 false */
  int *level, *reason, *trail, ntrail, qhead;
  ivec lim;                            /* trail length at the start of each level */
  ivec queue;                          /* closure work list */
  pkgname **removing; int nremoving;
  int conflicts, decisions, props;
  sbuf *why;
} S;

#define VAR(l) ((l) >> 1)
#define NEG(l) ((l) ^ 1)
#define POS(v) ((v) << 1)
#define NOT(v) (((v) << 1) | 1)
static int lval(S *s, int l) { int x = s->val[VAR(l)]; return (l & 1) ? -x : x; }

static int var_of(S *s, pkgver *v) {
  if (v->solver > 0 && v->solver <= s->nvar && s->var[v->solver] == v) return v->solver;
  if (s->nvar + 1 >= s->capvar) {
    int nc = s->capvar ? s->capvar * 2 : 256;
    s->var = xrealloc(s->var, nc * sizeof *s->var);
    s->deps = xrealloc(s->deps, nc * sizeof *s->deps);
    s->recs = xrealloc(s->recs, nc * sizeof *s->recs);
    memset(s->deps + s->capvar, 0, (nc - s->capvar) * sizeof *s->deps);
    memset(s->recs + s->capvar, 0, (nc - s->capvar) * sizeof *s->recs);
    s->capvar = nc;
  }
  v->solver = ++s->nvar;
  s->var[v->solver] = v;
  iv_push(&s->queue, v->solver);
  return v->solver;
}

static int add_clause(S *s, int *lits, int n, int kind, pkgver *a, pkgver *b) {
  if (s->ncl == s->capcl) { s->capcl = s->capcl ? s->capcl * 2 : 1024; s->cl = xrealloc(s->cl, s->capcl * sizeof *s->cl); }
  clause *c = &s->cl[s->ncl];
  c->l = xmalloc((n ? n : 1) * sizeof *c->l);
  memcpy(c->l, lits, n * sizeof *lits);
  c->n = n; c->learned = kind == C_LEARNED; c->a = a; c->b = b; c->kind = kind;
  return s->ncl++;
}

/* ---- matching dependency atoms ---- */
static int ma_is(const pkgver *v, const char *what) { return v->multiarch && !strcmp(v->multiarch, what); }
static int arch_fits(const dep_atom *a, const pkgver *w, const char *deparch) {
  const char *wa = w->arch ? w->arch : "all";
  if (!arch_ok_native(wa)) return 0;
  if (!deparch || !strcmp(deparch, "all")) deparch = pkg_cfg.arch;
  if (a->archq) {
    if (!strcmp(a->archq, "any")) return ma_is(w, "allowed") || ma_is(w, "foreign") || !strcmp(wa, "all") || !strcmp(wa, deparch);
    if (!strcmp(a->archq, "native")) return !strcmp(wa, pkg_cfg.arch) || !strcmp(wa, "all");
    return !strcmp(wa, a->archq);
  }
  return !strcmp(wa, "all") || !strcmp(wa, deparch) || ma_is(w, "foreign");
}
int atom_matches(const dep_atom *a, const pkgver *w, const char *deparch, int via_provides_ok) {
  if (!strcmp(w->pn->name, a->name)) return arch_fits(a, w, deparch) && ver_satisfies(w->ver, a->op, a->ver);
  if (!via_provides_ok || !w->provides) return 0;
  dep_list l;
  if (dep_parse(w->provides, DEP_PROVIDES, &l, 0) < 0) return 0;
  int ok = 0;
  for (int i = 0; i < l.n && !ok; i++) {
    const dep_atom *p = &l.o[i].a[0];
    if (strcmp(p->name, a->name)) continue;
    if (a->op && !(p->op == REL_EQ && ver_satisfies(p->ver, a->op, a->ver))) continue;
    ok = arch_fits(a, w, deparch);
  }
  dep_free(&l);
  return ok;
}
static int prov_cmp(const void *x, const void *y) {
  const pkgver *a = *(pkgver *const *)x, *b = *(pkgver *const *)y;
  if (a->installed != b->installed) return b->installed - a->installed;
  if (a->prio != b->prio) return b->prio - a->prio;
  int c = strcmp(a->pn->name, b->pn->name);
  return c ? c : ver_cmp(b->ver, a->ver);
}
/* Versions satisfying the atom, in preference order: the named package
 * (installed version, candidate, then the rest, highest first), then providers. */
int dep_candidates(pkgindex *ix, const dep_atom *a, const char *deparch, pkgver ***out) {
  int n = 0, cap = 8;
  pkgver **r = xmalloc(cap * sizeof *r);
  pkgname *pn = index_name(ix, a->name, 0);
  if (pn) {
    pkgver *inst = index_installed(pn), *cand = index_candidate(ix, pn);
    pkgver *first[2] = {inst, cand};
    for (int k = 0; k < 2; k++) {
      pkgver *v = first[k];
      if (!v || !atom_matches(a, v, deparch, 0)) continue;
      int dup = 0;
      for (int i = 0; i < n; i++) if (r[i] == v) dup = 1;
      if (!dup) r[n++] = v;
    }
    for (pkgver *v = pn->vers; v; v = v->next) {
      if (!atom_matches(a, v, deparch, 0)) continue;
      int dup = 0;
      for (int i = 0; i < n; i++) if (r[i] == v) dup = 1;
      if (dup) continue;
      if (n == cap) { cap *= 2; r = xrealloc(r, cap * sizeof *r); }
      r[n++] = v;
    }
    int start = n;
    for (uint32_t i = 0; i < pn->nprov; i++) {
      pkgver *v = pn->prov[i];
      if (v->pn == pn || !atom_matches(a, v, deparch, 1)) continue;
      if (n == cap) { cap *= 2; r = xrealloc(r, cap * sizeof *r); }
      r[n++] = v;
    }
    xqsort(r + start, n - start, sizeof *r, prov_cmp);
  }
  *out = r;
  return n;
}

/* ---- building the problem ---- */
static void dep_group_clause(S *s, int vid, pkgver *v, const dep_or *o, int is_rec) {
  ivec lits = {0};
  if (!is_rec) iv_push(&lits, NOT(vid));
  for (int j = 0; j < o->n; j++) {
    pkgver **c;
    int n = dep_candidates(s->ix, &o->a[j], v->arch, &c);
    for (int k = 0; k < n; k++) {
      int w = var_of(s, c[k]), l = POS(w), dup = 0;
      for (int m = 0; m < lits.n; m++) if (lits.v[m] == l) dup = 1;
      if (!dup) iv_push(&lits, l);
    }
    free(c);
  }
  if (is_rec) {
    if (lits.n) { iv_push(&s->recs[vid], s->recl.n); iv_push(&s->recl, lits.n); for (int i = 0; i < lits.n; i++) iv_push(&s->recl, lits.v[i]); }
  } else {
    int ci = add_clause(s, lits.v, lits.n, C_DEP, v, 0);
    iv_push(&s->deps[vid], ci);
  }
  free(lits.v);
}
static void expand(S *s, int vid) {
  pkgver *v = s->var[vid];
  const char *f[3] = {v->predepends, v->depends, s->o->recommends ? v->recommends : 0};
  for (int k = 0; k < 3; k++) {
    if (!f[k]) continue;
    dep_list l;
    if (dep_parse(f[k], DEP_ALT, &l, 0) < 0) {
      /* an unparsable field makes the version uninstallable */
      if (k < 2) { int lit = NOT(vid); add_clause(s, &lit, 1, C_DEP, v, 0); }
      continue;
    }
    for (int i = 0; i < l.n; i++) dep_group_clause(s, vid, v, &l.o[i], k == 2);
    dep_free(&l);
  }
}
static void conflicts_of(S *s, int vid) {
  pkgver *v = s->var[vid];
  const char *f[2] = {v->conflicts, v->breaks};
  for (int k = 0; k < 2; k++) {
    if (!f[k]) continue;
    dep_list l;
    if (dep_parse(f[k], 0, &l, 0) < 0) continue;
    for (int i = 0; i < l.n; i++) {
      pkgver **c;
      int n = dep_candidates(s->ix, &l.o[i].a[0], v->arch, &c);
      for (int j = 0; j < n; j++) {
        if (c[j]->pn == v->pn) continue;  /* never against itself or its own other versions */
        if (c[j]->solver <= 0 || c[j]->solver > s->nvar || s->var[c[j]->solver] != c[j]) continue;
        int lits[2] = {NOT(vid), NOT(c[j]->solver)};
        add_clause(s, lits, 2, C_CONFLICT, v, c[j]);
      }
      free(c);
    }
    dep_free(&l);
  }
}

/* ---- search ---- */
static void watch_clause(S *s, int ci) {
  clause *c = &s->cl[ci];
  if (c->n >= 2) { iv_push(&s->watch[NEG(c->l[0])], ci); iv_push(&s->watch[NEG(c->l[1])], ci); }
}
static void assign(S *s, int lit, int reason) {
  int v = VAR(lit);
  s->val[v] = (lit & 1) ? -1 : 1;
  s->level[v] = s->lim.n;
  s->reason[v] = reason;
  s->trail[s->ntrail++] = lit;
}
/* Returns a conflicting clause index or -1. watch[x] holds clauses watching a literal whose negation is x:
 * when literal x becomes true, those clauses lost a watched literal. */
static int propagate(S *s) {
  while (s->qhead < s->ntrail) {
    int p = s->trail[s->qhead++];   /* p is now true; clauses watching NEG(p) are in watch[p] */
    ivec *ws = &s->watch[p];
    int i = 0, j = 0;
    s->props++;
    while (i < ws->n) {
      int ci = ws->v[i++];
      clause *c = &s->cl[ci];
      int falselit = NEG(p);
      if (c->l[0] == falselit) { c->l[0] = c->l[1]; c->l[1] = falselit; }
      if (lval(s, c->l[0]) == 1) { ws->v[j++] = ci; continue; }
      int found = 0;
      for (int k = 2; k < c->n; k++)
        if (lval(s, c->l[k]) != -1) {
          c->l[1] = c->l[k]; c->l[k] = falselit;
          iv_push(&s->watch[NEG(c->l[1])], ci);
          found = 1;
          break;
        }
      if (found) continue;
      ws->v[j++] = ci;
      if (lval(s, c->l[0]) == -1) { while (i < ws->n) ws->v[j++] = ws->v[i++]; ws->n = j; return ci; }
      assign(s, c->l[0], ci);
    }
    ws->n = j;
  }
  return -1;
}
static void backjump(S *s, int lvl) {
  if (s->lim.n <= lvl) return;
  int stop = s->lim.v[lvl];
  for (int i = s->ntrail - 1; i >= stop; i--) { int v = VAR(s->trail[i]); s->val[v] = 0; s->reason[v] = -1; }
  s->ntrail = stop;
  s->qhead = stop;
  s->lim.n = lvl;
}
/* First-UIP learning. Returns the learned clause index and sets *btlevel. */
static int analyze(S *s, int confl, int *btlevel) {
  static char *seen; static int seencap;
  if (seencap <= s->nvar) { seencap = s->nvar + 1; seen = xrealloc(seen, seencap); }
  memset(seen, 0, s->nvar + 1);
  ivec out = {0};
  iv_push(&out, 0);
  int counter = 0, p = -1, idx = s->ntrail - 1, cur = s->lim.n;
  do {
    clause *c = &s->cl[confl];
    for (int k = 0; k < c->n; k++) {
      int q = c->l[k];
      if (p >= 0 && q == p) continue;
      int v = VAR(q);
      if (seen[v] || s->level[v] == 0) continue;
      seen[v] = 1;
      if (s->level[v] >= cur) counter++;
      else iv_push(&out, q);
    }
    while (!seen[VAR(s->trail[idx])]) idx--;
    p = s->trail[idx];
    confl = s->reason[VAR(p)];
    seen[VAR(p)] = 0;
    idx--;
    counter--;
  } while (counter > 0);
  out.v[0] = NEG(p);
  int bt = 0, bi = 1;
  for (int k = 1; k < out.n; k++) if (s->level[VAR(out.v[k])] > bt) { bt = s->level[VAR(out.v[k])]; bi = k; }
  if (out.n > 1) { int t = out.v[1]; out.v[1] = out.v[bi]; out.v[bi] = t; }
  *btlevel = bt;
  int ci = add_clause(s, out.v, out.n, C_LEARNED, 0, 0);
  free(out.v);
  return ci;
}
static int clause_satisfied(S *s, const clause *c) { for (int k = 0; k < c->n; k++) if (lval(s, c->l[k]) == 1) return 1; return 0; }
static int pick_in(S *s, const int *lits, int n) {
  for (int k = 0; k < n; k++) if (lval(s, lits[k]) == 1) return 0;   /* already satisfied */
  for (int k = 0; k < n; k++) if (lval(s, lits[k]) == 0 && !(lits[k] & 1)) return lits[k];
  return 0;
}
/* The next decision literal, or 0 when every variable can be set false. */
static int decide(S *s, int phase_rec) {
  for (int i = 0; i < s->positive.n; i++) {
    clause *c = &s->cl[s->positive.v[i]];
    int l = pick_in(s, c->l, c->n);
    if (l) return l;
  }
  for (int t = 0; t < s->ntrail; t++) {
    int lit = s->trail[t];
    if (lit & 1) continue;
    int v = VAR(lit);
    for (int k = 0; k < s->deps[v].n; k++) {
      clause *c = &s->cl[s->deps[v].v[k]];
      int l = pick_in(s, c->l + 1, c->n - 1);
      if (l) return l;
    }
  }
  /* installed packages: keep (or upgrade) unless something forbids it */
  for (int v = 1; v <= s->nvar; v++) {
    pkgver *pv = s->var[v];
    if (!pv->installed) continue;
    pkgname *pn = pv->pn;
    int any_true = 0;
    for (pkgver *w = pn->vers; w; w = w->next) if (w->solver > 0 && w->solver <= s->nvar && s->var[w->solver] == w && s->val[w->solver] == 1) any_true = 1;
    if (any_true) continue;
    int removing = 0;
    for (int r = 0; r < s->nremoving; r++) if (s->removing[r] == pn) removing = 1;
    if (removing) continue;
    pkgver *cand = index_candidate(s->ix, pn);
    pkgver *order[3] = {(s->o->upgrade_all || s->o->dist_upgrade) ? cand : pv, (s->o->upgrade_all || s->o->dist_upgrade) ? pv : cand, 0};
    for (int k = 0; k < 2; k++)
      if (order[k] && order[k]->solver > 0 && order[k]->solver <= s->nvar && s->var[order[k]->solver] == order[k] && s->val[order[k]->solver] == 0)
        return POS(order[k]->solver);
    /* any other version before giving the package up */
    for (pkgver *w = pn->vers; w; w = w->next) if (w->solver > 0 && w->solver <= s->nvar && s->var[w->solver] == w && s->val[w->solver] == 0) return POS(w->solver);
  }
  if (phase_rec)
    for (int t = 0; t < s->ntrail; t++) {
      int lit = s->trail[t];
      if (lit & 1) continue;
      int v = VAR(lit);
      if (s->var[v]->installed) continue;   /* only for what this run installs or upgrades */
      for (int k = 0; k < s->recs[v].n; k++) {
        int at = s->recs[v].v[k];
        int l = pick_in(s, s->recl.v + at + 1, s->recl.v[at]);
        if (l) return l;
      }
    }
  return 0;
}

static void describe_clause(S *s, const clause *c, sbuf *out) {
  switch (c->kind) {
  case C_DEP: {
    if (c->n == 1) { sb_printf(out, "  %s %s cannot be installed: a dependency cannot be satisfied by any available package\n", c->a->pn->name, c->a->ver); break; }
    sb_printf(out, "  %s %s depends on one of:", c->a->pn->name, c->a->ver);
    for (int k = 0; k < c->n; k++) if (!(c->l[k] & 1)) { pkgver *w = s->var[VAR(c->l[k])]; sb_printf(out, " %s (%s)", w->pn->name, w->ver); }
    sb_putc(out, '\n');
    break;
  }
  case C_CONFLICT: sb_printf(out, "  %s %s conflicts with or breaks %s %s\n", c->a->pn->name, c->a->ver, c->b->pn->name, c->b->ver); break;
  case C_AMO: sb_printf(out, "  only one version of %s can be installed\n", c->a->pn->name); break;
  case C_JOB: sb_printf(out, "  you asked for %s\n", c->a ? c->a->pn->name : "a package"); break;
  case C_ESSENTIAL: sb_printf(out, "  %s is essential and must stay installed\n", c->a->pn->name); break;
  case C_REMOVE: sb_printf(out, "  you asked to remove %s\n", c->a->pn->name); break;
  default: break;
  }
}
static void explain(S *s, int confl) {
  /* the original clauses behind the final contradiction, following reasons at level 0 */
  char *seen = xcalloc(s->nvar + 1, 1);
  char *cseen = xcalloc(s->ncl, 1);
  ivec stack = {0};
  iv_push(&stack, confl);
  int shown = 0;
  while (stack.n && shown < 12) {
    int ci = stack.v[--stack.n];
    if (ci < 0 || cseen[ci]) continue;
    cseen[ci] = 1;
    clause *c = &s->cl[ci];
    if (!c->learned) { describe_clause(s, c, s->why); shown++; }
    for (int k = 0; k < c->n; k++) {
      int v = VAR(c->l[k]);
      if (seen[v]) continue;
      seen[v] = 1;
      if (s->reason[v] >= 0) iv_push(&stack, s->reason[v]);
    }
  }
  free(stack.v); free(seen); free(cseen);
}

static void job_versions(S *s, const solve_job *j, ivec *lits) {
  if (j->ver) { iv_push(lits, POS(var_of(s, j->ver))); return; }
  for (pkgver *v = j->pn->vers; v; v = v->next) {
    if (!arch_ok_native(v->arch)) continue;
    if (j->verstr && !ver_satisfies(v->ver, j->op ? j->op : REL_EQ, j->verstr)) continue;
    iv_push(lits, POS(var_of(s, v)));
  }
}

int solve(pkgindex *ix, pkgdb *db, const solve_job *jobs, int njobs, const solve_opts *o, solve_result *r) {
  (void)db;
  memset(r, 0, sizeof *r);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  S s = {0};
  s.ix = ix; s.o = o; s.why = &r->why;
  /* reset scratch numbers */
  for (uint32_t h = 0; h < ix->tabcap; h++) for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash) for (pkgver *v = pn->vers; v; v = v->next) v->solver = 0;
  /* seeds: installed versions and requested packages */
  for (uint32_t h = 0; h < ix->tabcap; h++) for (pkgname *pn = ix->tab[h]; pn; pn = pn->next_hash) for (pkgver *v = pn->vers; v; v = v->next) {
    if (!v->installed) continue;
    var_of(&s, v);
    if (o->upgrade_all || o->dist_upgrade) { pkgver *c = index_candidate(ix, pn); if (c) var_of(&s, c); }
  }
  ivec tmp = {0};
  for (int i = 0; i < njobs; i++) {
    const solve_job *j = &jobs[i];
    if (j->kind == JOB_INSTALL) {
      tmp.n = 0;
      job_versions(&s, j, &tmp);
    } else if (j->kind == JOB_REMOVE || j->kind == JOB_PURGE) {
      s.removing = xrealloc(s.removing, (s.nremoving + 1) * sizeof *s.removing);
      s.removing[s.nremoving++] = j->pn;
      for (pkgver *v = j->pn->vers; v; v = v->next) if (v->installed) var_of(&s, v);
    }
  }
  /* closure */
  for (int qi = 0; qi < s.queue.n; qi++) expand(&s, s.queue.v[qi]);
  int nv = s.nvar;
  s.val = xcalloc(nv + 1, 1);
  s.level = xcalloc(nv + 1, sizeof *s.level);
  s.reason = xmalloc((nv + 1) * sizeof *s.reason);
  for (int i = 0; i <= nv; i++) s.reason[i] = -1;
  s.trail = xmalloc((nv + 1) * sizeof *s.trail);
  s.watch = xcalloc(2 * (nv + 1), sizeof *s.watch);
  for (int v = 1; v <= nv; v++) conflicts_of(&s, v);
  /* one version per package name */
  for (int v = 1; v <= nv; v++) {
    pkgver *a = s.var[v];
    for (pkgver *b = a->next; b; b = b->next) {
      if (b->solver <= 0 || b->solver > nv || s.var[b->solver] != b) continue;
      if (ma_is(a, "same") && ma_is(b, "same") && strcmp(a->arch, b->arch)) continue;
      int lits[2] = {NOT(v), NOT(b->solver)};
      add_clause(&s, lits, 2, C_AMO, a, b);
    }
  }
  int unsat = 0;
  for (int i = 0; i < njobs; i++) {
    const solve_job *j = &jobs[i];
    tmp.n = 0;
    if (j->kind == JOB_INSTALL) {
      job_versions(&s, j, &tmp);
      if (!tmp.n) { sb_printf(&r->why, "  no version of %s matches%s%s\n", j->pn->name, j->verstr ? " " : "", j->verstr ? j->verstr : ""); unsat = 1; continue; }
      int ci = add_clause(&s, tmp.v, tmp.n, C_JOB, j->ver ? j->ver : j->pn->vers, 0);
      iv_push(&s.positive, ci);
    } else if (j->kind == JOB_REMOVE || j->kind == JOB_PURGE) {
      for (pkgver *v = j->pn->vers; v; v = v->next) if (v->solver > 0 && v->solver <= nv && s.var[v->solver] == v) {
        int l = NOT(v->solver);
        add_clause(&s, &l, 1, C_REMOVE, v, 0);
      }
    }
  }
  /* installed essential packages stay */
  if (!o->allow_essential_remove)
    for (int v = 1; v <= nv; v++) {
      pkgver *a = s.var[v];
      if (!a->installed || !(a->essential || a->important)) continue;
      int removing = 0;
      for (int k = 0; k < s.nremoving; k++) if (s.removing[k] == a->pn) removing = 1;
      if (removing) { sb_printf(&r->why, "  %s is %s; removing it needs the essential-removal override\n", a->pn->name, a->essential ? "essential" : "important"); unsat = 1; continue; }
      tmp.n = 0;
      for (pkgver *b = a->pn->vers; b; b = b->next) if (b->solver > 0 && b->solver <= nv && s.var[b->solver] == b) iv_push(&tmp, POS(b->solver));
      int ci = add_clause(&s, tmp.v, tmp.n, C_ESSENTIAL, a, 0);
      iv_push(&s.positive, ci);
    }
  free(tmp.v);
  /* unit clauses at level 0, watches for the rest */
  for (int ci = 0; ci < s.ncl && !unsat; ci++) {
    clause *c = &s.cl[ci];
    if (c->n == 0) { unsat = 1; describe_clause(&s, c, &r->why); }
    else if (c->n == 1) {
      int lv = lval(&s, c->l[0]);
      if (lv == -1) {
        unsat = 1;
        describe_clause(&s, c, &r->why);
        int rs = s.reason[VAR(c->l[0])];
        if (rs >= 0) describe_clause(&s, &s.cl[rs], &r->why);
      }
      else if (lv == 0) assign(&s, c->l[0], ci);
    } else watch_clause(&s, ci);
  }
  int confl = unsat ? -2 : propagate(&s);
  if (confl >= 0) { explain(&s, confl); unsat = 1; }
  while (!unsat) {
    int lit = decide(&s, o->recommends);
    if (!lit) break;
    s.decisions++;
    iv_push(&s.lim, s.ntrail);
    assign(&s, lit, -1);
    for (;;) {
      confl = propagate(&s);
      if (confl < 0) break;
      s.conflicts++;
      if (s.lim.n == 0) { explain(&s, confl); unsat = 1; break; }
      int bt;
      int ci = analyze(&s, confl, &bt);
      backjump(&s, bt);
      clause *c = &s.cl[ci];
      if (c->n == 1) assign(&s, c->l[0], ci);
      else { watch_clause(&s, ci); assign(&s, c->l[0], ci); }
    }
  }
  if (!unsat) {
    for (int v = 1; v <= nv; v++) if (!s.val[v]) s.val[v] = -1;
    for (int ci = 0; ci < s.ncl; ci++) if (!clause_satisfied(&s, &s.cl[ci])) { sb_puts(&r->why, "  internal error: a clause is not satisfied\n"); describe_clause(&s, &s.cl[ci], &r->why); unsat = 1; break; }
  }
  if (!unsat) {
    /* steps: new versions, then removals */
    for (int v = 1; v <= nv; v++) {
      pkgver *a = s.var[v];
      if (s.val[v] != 1 || a->installed) continue;
      pkgver *old = index_installed(a->pn);
      r->steps = xrealloc(r->steps, (r->n + 1) * sizeof *r->steps);
      solve_step *st = &r->steps[r->n++];
      st->v = a; st->old = old; st->action = 1; st->auto_inst = 1;
      for (int i = 0; i < njobs; i++) if (jobs[i].kind == JOB_INSTALL && jobs[i].pn == a->pn) st->auto_inst = 0;
      if (old) { if (ver_cmp(a->ver, old->ver) < 0) r->n_downgrade++; else r->n_upgrade++; }
      else r->n_install++;
      r->dl_bytes += a->size;
      r->inst_kib += a->isize;
      if (old) r->freed_kib += old->isize;
    }
    for (int v = 1; v <= nv; v++) {
      pkgver *a = s.var[v];
      if (!a->installed || s.val[v] == 1) continue;
      int replaced = 0;
      for (pkgver *b = a->pn->vers; b; b = b->next) if (b->solver > 0 && b->solver <= nv && s.var[b->solver] == b && s.val[b->solver] == 1) replaced = 1;
      if (replaced) continue;
      r->steps = xrealloc(r->steps, (r->n + 1) * sizeof *r->steps);
      solve_step *st = &r->steps[r->n++];
      st->v = a; st->old = a; st->action = 2;
      for (int i = 0; i < njobs; i++) if (jobs[i].kind == JOB_PURGE && jobs[i].pn == a->pn) st->action = 3;
      r->n_remove++;
      r->freed_kib += a->isize;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  r->ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
  r->vars = nv; r->clauses = s.ncl; r->conflicts = s.conflicts; r->decisions = s.decisions;
  /* the variable numbers stay in pkgver.solver for solve_order; free the rest */
  for (int ci = 0; ci < s.ncl; ci++) free(s.cl[ci].l);
  free(s.cl);
  for (int v = 0; v < s.capvar; v++) { if (v <= nv || v < s.capvar) { free(s.deps[v].v); free(s.recs[v].v); } }
  free(s.deps); free(s.recs); free(s.recl.v); free(s.positive.v);
  if (s.watch) for (int l = 0; l < 2 * (nv + 1); l++) free(s.watch[l].v);
  free(s.watch); free(s.val); free(s.level); free(s.reason); free(s.trail); free(s.lim.v); free(s.queue.v); free(s.removing);
  free(s.var);
  return unsat ? -1 : 0;
}
void solve_result_free(solve_result *r) { free(r->steps); sb_free(&r->why); memset(r, 0, sizeof *r); }

/* ---- order: Tarjan's strongly connected components over "depends on" ---- */
typedef struct { solve_result *r; int *index, *low, *onstack, *stack, sp, counter; order_step *out; int nout; sbuf *notes; pkgindex *ix; } T;
static int step_of(solve_result *r, pkgver *v) { for (int i = 0; i < r->n; i++) if (r->steps[i].v == v && r->steps[i].action == 1) return i; return -1; }
static void edges(T *t, int i, ivec *out) {
  pkgver *v = t->r->steps[i].v;
  const char *f[2] = {v->predepends, v->depends};
  for (int k = 0; k < 2; k++) {
    dep_list l;
    if (!f[k] || dep_parse(f[k], DEP_ALT, &l, 0) < 0) continue;
    for (int g = 0; g < l.n; g++) {
      /* the satisfier this solution chose: first candidate that is being installed */
      for (int a = 0; a < l.o[g].n; a++) {
        pkgver **c;
        int n = dep_candidates(t->ix, &l.o[g].a[a], v->arch, &c), done = 0;
        for (int j = 0; j < n && !done; j++) { int si = step_of(t->r, c[j]); if (si >= 0 && si != i) { iv_push(out, si * 2 + (k == 0)); done = 1; } }
        free(c);
        if (done) break;
      }
    }
    dep_free(&l);
  }
}
static void strong(T *t, int i) {
  t->index[i] = t->low[i] = ++t->counter;
  t->stack[t->sp++] = i;
  t->onstack[i] = 1;
  ivec e = {0};
  edges(t, i, &e);
  for (int k = 0; k < e.n; k++) {
    int j = e.v[k] / 2;
    if (!t->index[j]) { strong(t, j); if (t->low[j] < t->low[i]) t->low[i] = t->low[j]; }
    else if (t->onstack[j] && t->index[j] < t->low[i]) t->low[i] = t->index[j];
  }
  if (t->low[i] == t->index[i]) {
    int start = t->sp;
    do { start--; } while (t->stack[start] != i);
    int size = t->sp - start;
    if (size > 1) {
      /* a cycle: dpkg tolerates Depends cycles (unpack all, then configure all), not Pre-Depends ones */
      for (int a = start; a < t->sp; a++) {
        ivec e2 = {0};
        edges(t, t->stack[a], &e2);
        for (int k = 0; k < e2.n; k++)
          if ((e2.v[k] & 1) && t->onstack[e2.v[k] / 2]) {
            int in = 0;
            for (int b = start; b < t->sp; b++) if (t->stack[b] == e2.v[k] / 2) in = 1;
            if (in) sb_printf(t->notes, "Pre-Depends cycle: %s and %s; dpkg may refuse this order\n", t->r->steps[t->stack[a]].v->pn->name, t->r->steps[e2.v[k] / 2].v->pn->name);
          }
        free(e2.v);
      }
    }
    for (int a = start; a < t->sp; a++) { t->out[t->nout].v = t->r->steps[t->stack[a]].v; t->out[t->nout++].op = 'u'; }
    for (int a = start; a < t->sp; a++) { t->out[t->nout].v = t->r->steps[t->stack[a]].v; t->out[t->nout++].op = 'c'; t->onstack[t->stack[a]] = 0; }
    t->sp = start;
  }
  free(e.v);
}
int solve_order(pkgindex *ix, solve_result *r, order_step **out, int *n, sbuf *notes) {
  T t = {0};
  t.r = r; t.ix = ix; t.notes = notes;
  t.index = xcalloc(r->n + 1, sizeof(int)); t.low = xcalloc(r->n + 1, sizeof(int));
  t.onstack = xcalloc(r->n + 1, sizeof(int)); t.stack = xcalloc(r->n + 1, sizeof(int));
  t.out = xcalloc(2 * r->n + 1, sizeof *t.out);
  /* removals first, so that conflicting packages are gone before replacements arrive */
  for (int i = 0; i < r->n; i++) if (r->steps[i].action >= 2) { t.out[t.nout].v = r->steps[i].v; t.out[t.nout++].op = r->steps[i].action == 3 ? 'p' : 'r'; }
  for (int i = 0; i < r->n; i++) if (r->steps[i].action == 1 && !t.index[i]) strong(&t, i);
  free(t.index); free(t.low); free(t.onstack); free(t.stack);
  *out = t.out;
  *n = t.nout;
  return 0;
}
