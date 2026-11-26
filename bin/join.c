// SPDX-License-Identifier: GPL-2.0-or-later
/* join [-i] [-a 1|2] [-v 1|2] [-e EMPTY] [-o LIST|auto] [-t CHAR] [-1 F] [-2 F] [-j F]
 *      [--header] [--check-order|--nocheck-order] [-z] file1 file2
 * Join the lines of two files sorted on a common field. Without -t, fields
 * are separated by runs of blanks and output fields by one space. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

struct line { char *text; long len; char **f; long *fl; int nf; };
static int tab = -1, icase, eol = '\n', header, check = 1;
static int field[2] = {0, 0}, print_unpaired[2], only_unpaired;
static const char *empty;
struct ospec { int file, field; }; /* file 0: the join field */
static struct ospec *ospec;
static int nospec = -1, oauto;

static void split(struct line *l) {
  l->nf = 0;
  int cap = 8;
  l->f = malloc(cap * sizeof *l->f);
  l->fl = malloc(cap * sizeof *l->fl);
  char *s = l->text, *end = l->text + l->len;
  if (tab < 0) {
    while (s < end) {
      while (s < end && (*s == ' ' || *s == '\t')) s++;
      if (s >= end) break;
      char *b = s;
      while (s < end && *s != ' ' && *s != '\t') s++;
      if (l->nf == cap) { cap *= 2; l->f = realloc(l->f, cap * sizeof *l->f); l->fl = realloc(l->fl, cap * sizeof *l->fl); }
      l->f[l->nf] = b; l->fl[l->nf++] = s - b;
    }
  } else {
    if (l->len == 0) return;
    for (;;) {
      char *b = s;
      while (s < end && *s != tab) s++;
      if (l->nf == cap) { cap *= 2; l->f = realloc(l->f, cap * sizeof *l->f); l->fl = realloc(l->fl, cap * sizeof *l->fl); }
      l->f[l->nf] = b; l->fl[l->nf++] = s - b;
      if (s >= end) break;
      s++;
    }
  }
}
static FILE *in[2];
static const char *names[2];
static long lineno[2];
static struct line prevl[2];
static int have_prev[2], disorder;
static int keycmp(const struct line *a, int fa, const struct line *b, int fb) {
  const char *x = fa < a->nf ? a->f[fa] : "", *y = fb < b->nf ? b->f[fb] : "";
  long nx = fa < a->nf ? a->fl[fa] : 0, ny = fb < b->nf ? b->fl[fb] : 0;
  long m = nx < ny ? nx : ny;
  int c = icase ? strncasecmp(x, y, m) : memcmp(x, y, m);
  if (c) return c;
  return (nx > ny) - (nx < ny);
}
static int readl(int k, struct line *l) {
  char *buf = NULL;
  size_t cap = 0;
  long n = getdelim(&buf, &cap, eol, in[k]);
  if (n < 0) { free(buf); return 0; }
  if (n > 0 && buf[n - 1] == eol) n--;
  l->text = buf; l->len = n;
  split(l);
  lineno[k]++;
  if (check && have_prev[k] && keycmp(&prevl[k], field[k], l, field[k]) > 0 && !(disorder & (1 << k))) {
    disorder |= 1 << k;
    fflush(stdout);
    fprintf(stderr, "join: %s:%ld: is not sorted: %.*s\n", names[k], lineno[k], (int)n, buf);
    if (check == 2) exit(1);
  }
  if (have_prev[k]) { free(prevl[k].text); free(prevl[k].f); free(prevl[k].fl); }
  prevl[k] = *l;
  prevl[k].text = malloc(n + 1); memcpy(prevl[k].text, buf, n);
  prevl[k].f = malloc((l->nf + 1) * sizeof *l->f); prevl[k].fl = malloc((l->nf + 1) * sizeof *l->fl);
  for (int i = 0; i < l->nf; i++) { prevl[k].f[i] = prevl[k].text + (l->f[i] - buf); prevl[k].fl[i] = l->fl[i]; }
  have_prev[k] = 1;
  return 1;
}
static void freel(struct line *l) { free(l->text); free(l->f); free(l->fl); }

static int first_out;
static void out_field(const char *s, long n) {
  if (!first_out) putchar(tab < 0 ? ' ' : tab);
  first_out = 0;
  if (n == 0 && empty) fputs(empty, stdout);
  else fwrite(s, 1, n, stdout);
}
static int auto_nf[2];
static void output(const struct line *a, const struct line *b) {
  first_out = 1;
  const struct line *ls[2] = {a, b};
  if (nospec >= 0) {
    for (int k = 0; k < nospec; k++) {
      int fi = ospec[k].file, fn = ospec[k].field;
      if (fi == 0) {
        const struct line *l = a ? a : b;
        int j = a ? field[0] : field[1];
        out_field(j < l->nf ? l->f[j] : "", j < l->nf ? l->fl[j] : 0);
      } else {
        const struct line *l = ls[fi - 1];
        if (l && fn < l->nf) out_field(l->f[fn], l->fl[fn]);
        else out_field("", 0);
      }
    }
  } else {
    const struct line *l = a ? a : b;
    int j = a ? field[0] : field[1];
    out_field(j < l->nf ? l->f[j] : "", j < l->nf ? l->fl[j] : 0);
    for (int k = 0; k < 2; k++) {
      const struct line *m = ls[k];
      int nf = m ? m->nf : 0;
      if (oauto) nf = auto_nf[k];
      for (int i = 0; i < nf; i++) {
        if (i == field[k]) continue;
        if (m && i < m->nf) out_field(m->f[i], m->fl[i]);
        else out_field("", 0);
      }
    }
  }
  putchar(eol);
}

static int parse_field(const char *s) {
  char *e;
  long v = strtol(s, &e, 10);
  if (*e || v < 1) { fprintf(stderr, "join: invalid field number: '%s'\n", s); exit(1); }
  return v - 1;
}
static void parse_o(const char *s) {
  if (!strcmp(s, "auto")) { oauto = 1; return; }
  if (nospec < 0) nospec = 0;
  char *copy = strdup(s), *save, *tok;
  for (tok = strtok_r(copy, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
    ospec = realloc(ospec, (nospec + 1) * sizeof *ospec);
    if (!strcmp(tok, "0")) ospec[nospec++] = (struct ospec){0, 0};
    else if ((tok[0] == '1' || tok[0] == '2') && tok[1] == '.') ospec[nospec++] = (struct ospec){tok[0] - '0', parse_field(tok + 2)};
    else { fprintf(stderr, "join: invalid field specifier: '%s'\n", tok); exit(1); }
  }
  free(copy);
}

int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--ignore-case")) { icase = 1; continue; }
    if (!strcmp(a, "--header")) { header = 1; continue; }
    if (!strcmp(a, "--check-order")) { check = 2; continue; }
    if (!strcmp(a, "--nocheck-order")) { check = 0; continue; }
    if (!strcmp(a, "--zero-terminated")) { eol = 0; continue; }
    if (a[1] == '-') { fprintf(stderr, "join: unrecognized option '%s'\n", a); return 1; }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'i') { icase = 1; continue; }
      if (*f == 'z') { eol = 0; continue; }
      if (!strchr("aevot12j", *f)) { fprintf(stderr, "join: invalid option -- '%c'\nusage: join [options] file1 file2\n", *f); return 1; }
      char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
      if (!v) { fprintf(stderr, "join: option requires an argument -- '%c'\n", *f); return 1; }
      switch (*f) {
      case 'a': case 'v':
        if (strcmp(v, "1") && strcmp(v, "2")) { fprintf(stderr, "join: invalid field number: '%s'\n", v); return 1; }
        print_unpaired[v[0] - '1'] = 1;
        if (*f == 'v') only_unpaired = 1;
        break;
      case 'e': empty = v; break;
      case 'o': parse_o(v); break;
      case 't':
        if (strlen(v) > 1) { fprintf(stderr, "join: multi-character tab '%s'\n", v); return 1; }
        tab = v[0] ? (unsigned char)v[0] : '\n';
        break;
      case '1': field[0] = parse_field(v); break;
      case '2': field[1] = parse_field(v); break;
      case 'j': field[0] = field[1] = parse_field(v); break;
      }
      break;
    }
  }
  if (argc - i != 2) { fprintf(stderr, argc - i < 2 ? "join: missing operand\n" : "join: extra operand '%s'\n", argc - i > 2 ? argv[i + 2] : ""); return 1; }
  for (int k = 0; k < 2; k++) {
    names[k] = argv[i + k];
    in[k] = strcmp(names[k], "-") ? fopen(names[k], "r") : stdin;
    if (!in[k]) { fprintf(stderr, "join: %s: %s\n", names[k], strerror(errno)); return 1; }
  }
  setvbuf(stdout, NULL, _IOFBF, 65536);
  struct line cur[2];
  int have[2];
  have[0] = readl(0, &cur[0]);
  have[1] = readl(1, &cur[1]);
  if (oauto) { auto_nf[0] = have[0] ? cur[0].nf : 0; auto_nf[1] = have[1] ? cur[1].nf : 0; }
  if (header && (have[0] || have[1])) {
    output(have[0] ? &cur[0] : NULL, have[1] ? &cur[1] : NULL);
    have_prev[0] = have_prev[1] = 0; /* the header is not part of the sort order */
    if (have[0]) { freel(&cur[0]); have[0] = readl(0, &cur[0]); }
    if (have[1]) { freel(&cur[1]); have[1] = readl(1, &cur[1]); }
  }
  /* groups of lines with equal keys from file 2 are collected to pair them all */
  while (have[0] || have[1]) {
    int c = !have[0] ? 1 : !have[1] ? -1 : keycmp(&cur[0], field[0], &cur[1], field[1]);
    if (c < 0) {
      if (print_unpaired[0]) output(&cur[0], NULL);
      freel(&cur[0]); have[0] = readl(0, &cur[0]);
    } else if (c > 0) {
      if (print_unpaired[1]) output(NULL, &cur[1]);
      freel(&cur[1]); have[1] = readl(1, &cur[1]);
    } else {
      struct line *g = NULL;
      int ng = 0;
      struct line key = cur[1];
      do {
        g = realloc(g, (ng + 1) * sizeof *g);
        g[ng++] = cur[1];
        have[1] = readl(1, &cur[1]);
      } while (have[1] && keycmp(&cur[1], field[1], &key, field[1]) == 0);
      do {
        if (!only_unpaired) for (int k = 0; k < ng; k++) output(&cur[0], &g[k]);
        freel(&cur[0]);
        have[0] = readl(0, &cur[0]);
      } while (have[0] && keycmp(&cur[0], field[0], &g[0], field[1]) == 0);
      for (int k = 0; k < ng; k++) freel(&g[k]);
      free(g);
    }
  }
  if (fflush(stdout)) return 1;
  if (disorder) { fprintf(stderr, "join: input is not in sorted order\n"); return 1; }
  return 0;
}
