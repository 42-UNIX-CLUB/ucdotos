// SPDX-License-Identifier: GPL-2.0-or-later
/* Relationship fields (Debian Policy 7.1, deb-control(5)):
 *   Depends: a (>= 1.0), b | c:any (<< 2~), d [amd64 !i386] <!nocheck>
 * Entries are separated by commas, alternatives by "|". An entry is a package
 * name, an optional ":arch" qualifier, an optional "(op version)" with op one
 * of << <= = >= >> (the obsolete < and > mean <= and >=), an optional
 * "[arch ...]" restriction list and optional "<profile ...>" lists. Spaces and
 * folded lines may appear between any two parts. */
#include "pkg.h"
#include <stdlib.h>
#include <string.h>

static void skipws(const char **p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }
static int namech(int c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || (c >= 'A' && c <= 'Z'); }
static int seterr(char **err, const char *msg, const char *at) {
  if (err) *err = xasprintf("%s near '%.20s'", msg, at);
  return -1;
}

static int parse_atom(const char **pp, int flags, dep_atom *a, char **err) {
  const char *p = *pp;
  memset(a, 0, sizeof *a);
  skipws(&p);
  const char *s = p;
  while (namech((unsigned char)*p)) p++;
  if (p == s) return seterr(err, "missing package name", s);
  a->name = xstrndup(s, p - s);
  if (!((a->name[0] >= 'a' && a->name[0] <= 'z') || (a->name[0] >= '0' && a->name[0] <= '9')))
    return seterr(err, "package name must start with an alphanumeric character", s);
  for (char *c = a->name; *c; c++) if (*c >= 'A' && *c <= 'Z') return seterr(err, "package names are lowercase", s);
  if (*p == ':') {
    p++;
    s = p;
    while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') p++;
    if (p == s) return seterr(err, "missing architecture qualifier", s);
    a->archq = xstrndup(s, p - s);
  }
  skipws(&p);
  if (*p == '(') {
    p++;
    skipws(&p);
    if (p[0] == '<' && p[1] == '<') { a->op = REL_LT; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { a->op = REL_LE; p += 2; }
    else if (p[0] == '>' && p[1] == '>') { a->op = REL_GT; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { a->op = REL_GE; p += 2; }
    else if (p[0] == '=') { a->op = REL_EQ; p++; }
    else if (p[0] == '<') { a->op = REL_LE; p++; }   /* obsolete forms */
    else if (p[0] == '>') { a->op = REL_GE; p++; }
    else a->op = REL_EQ;   /* "(1.0)": an old spelling of "= 1.0" */
    if ((flags & DEP_PROVIDES) && a->op != REL_EQ) return seterr(err, "only exact versions may be provided", p);
    skipws(&p);
    s = p;
    while (*p && *p != ')' && *p != ' ' && *p != '\t' && *p != '\n') p++;
    if (p == s) return seterr(err, "missing version", s);
    a->ver = xstrndup(s, p - s);
    skipws(&p);
    if (*p != ')') return seterr(err, "missing ')'", p);
    p++;
    const char *why = ver_check(a->ver);
    if (why && strcmp(why, "version number does not start with digit")) return seterr(err, why, s);
    skipws(&p);
  }
  if (*p == '[') {
    if (!(flags & DEP_RESTRICT)) return seterr(err, "architecture restrictions are only allowed in source fields", p);
    p++;
    int neg_seen = -1;
    for (;;) {
      skipws(&p);
      if (*p == ']') { p++; break; }
      int neg = 0;
      if (*p == '!') { neg = 1; p++; }
      if (neg_seen >= 0 && neg != neg_seen) return seterr(err, "mixed negated and plain architectures", p);
      neg_seen = neg;
      s = p;
      while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') p++;
      if (p == s) return seterr(err, "bad architecture list", s);
      char *arch = xstrndup(s, p - s);
      sl_add(&a->arches, arch);
      free(arch);
    }
    a->arch_neg = neg_seen == 1;
    skipws(&p);
  }
  if (*p == '<') {
    if (!(flags & DEP_RESTRICT)) return seterr(err, "build profiles are only allowed in source fields", p);
    sbuf b = {0};
    while (*p == '<') {
      s = p;
      while (*p && *p != '>') p++;
      if (*p != '>') { sb_free(&b); return seterr(err, "missing '>'", s); }
      p++;
      if (b.n) sb_putc(&b, ' ');
      sb_add(&b, s, p - s);
      skipws(&p);
    }
    a->profiles = b.s;
  }
  *pp = p;
  return 0;
}

static void atom_free(dep_atom *a) { free(a->name); free(a->archq); free(a->ver); sl_free(&a->arches); free(a->profiles); }
void dep_free(dep_list *l) {
  for (int i = 0; i < l->n; i++) { for (int j = 0; j < l->o[i].n; j++) atom_free(&l->o[i].a[j]); free(l->o[i].a); }
  free(l->o);
  l->o = 0; l->n = 0;
}

int dep_parse(const char *text, int flags, dep_list *out, char **err) {
  memset(out, 0, sizeof *out);
  if (err) *err = 0;
  if (!text) return 0;
  const char *p = text;
  int cap = 0;
  for (;;) {
    skipws(&p);
    if (!*p) break;
    if (*p == ',') { p++; continue; }   /* empty entries, as dpkg accepts a trailing comma */
    dep_or o = {0};
    int acap = 0;
    for (;;) {
      if (o.n == acap) { acap = acap ? acap * 2 : 2; o.a = xrealloc(o.a, acap * sizeof *o.a); }
      if (parse_atom(&p, flags, &o.a[o.n], err) < 0) { atom_free(&o.a[o.n]); for (int j = 0; j < o.n; j++) atom_free(&o.a[j]); free(o.a); dep_free(out); return -1; }
      o.n++;
      skipws(&p);
      if (*p == '|') {
        if (!(flags & DEP_ALT)) { for (int j = 0; j < o.n; j++) atom_free(&o.a[j]); free(o.a); dep_free(out); return seterr(err, "alternatives are not allowed in this field", p); }
        p++;
        continue;
      }
      break;
    }
    if (out->n == cap) { cap = cap ? cap * 2 : 4; out->o = xrealloc(out->o, cap * sizeof *out->o); }
    out->o[out->n++] = o;
    skipws(&p);
    if (*p == ',') { p++; continue; }
    if (*p) { dep_free(out); return seterr(err, "expected ',' or '|'", p); }
  }
  return 0;
}

void dep_atom_format(sbuf *b, const dep_atom *a) {
  sb_puts(b, a->name);
  if (a->archq) sb_printf(b, ":%s", a->archq);
  if (a->op) sb_printf(b, " (%s %s)", rel_str(a->op), a->ver);
  if (a->arches.n) {
    sb_puts(b, " [");
    for (int i = 0; i < a->arches.n; i++) sb_printf(b, "%s%s%s", i ? " " : "", a->arch_neg ? "!" : "", a->arches.v[i]);
    sb_putc(b, ']');
  }
  if (a->profiles) sb_printf(b, " %s", a->profiles);
}
void dep_format(sbuf *b, const dep_list *l) {
  for (int i = 0; i < l->n; i++) {
    if (i) sb_puts(b, ", ");
    for (int j = 0; j < l->o[i].n; j++) { if (j) sb_puts(b, " | "); dep_atom_format(b, &l->o[i].a[j]); }
  }
}

/* Architecture wildcards (Policy 11.1): "any", "OS-any", "any-CPU", or exact.
 * Every Debian architecture name here runs Linux, so OS is "linux". */
int arch_matches(const char *pat, const char *arch) {
  if (!strcmp(pat, arch) || !strcmp(pat, "any")) return 1;
  if (!strcmp(pat, "linux-any")) return strcmp(arch, "all") != 0;
  if (!strncmp(pat, "any-", 4)) return !strcmp(pat + 4, arch);
  if (!strncmp(pat, "linux-", 6)) return !strcmp(pat + 6, arch);
  return 0;
}
void dep_reduce_arch(dep_list *l, const char *arch) {
  for (int i = 0; i < l->n; i++) {
    dep_or *o = &l->o[i];
    int k = 0;
    for (int j = 0; j < o->n; j++) {
      dep_atom *a = &o->a[j];
      int keep = 1;
      if (a->arches.n) {
        int m = 0;
        for (int x = 0; x < a->arches.n; x++) if (arch_matches(a->arches.v[x], arch)) m = 1;
        keep = a->arch_neg ? !m : m;
      }
      if (keep) o->a[k++] = *a; else atom_free(a);
    }
    o->n = k;
  }
  int k = 0;
  for (int i = 0; i < l->n; i++) { if (l->o[i].n) l->o[k++] = l->o[i]; else free(l->o[i].a); }
  l->n = k;
}
