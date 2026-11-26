// SPDX-License-Identifier: GPL-2.0-or-later
/* nl [-p] [-b TYPE] [-h TYPE] [-f TYPE] [-v START] [-i INCR] [-l N] [-n ln|rn|rz]
 *    [-s SEP] [-w WIDTH] [-d CC] [file...]
 * Number lines. TYPE: a all, t non-empty (default for the body), n none,
 * pREGEX lines matching a basic regular expression. Lines made only of the
 * delimiter (\: by default) three, two or one times start a header, body or
 * footer section; a header restarts the numbering unless -p. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>

struct type { int kind; regex_t re; };
static struct type types[3]; /* header, body, footer */
static long start = 1, incr = 1, width = 6, blank_join = 1, num;
static int fmt = 'r', zero_pad, no_restart;
static const char *sep = "\t";
static char delim[3] = "\\:";

static void set_type(struct type *t, const char *s) {
  if (!strcmp(s, "a") || !strcmp(s, "t") || !strcmp(s, "n")) { t->kind = s[0]; return; }
  if (s[0] == 'p') {
    int rc = regcomp(&t->re, s + 1, REG_NOSUB);
    if (rc) { char m[128]; regerror(rc, &t->re, m, sizeof m); fprintf(stderr, "nl: invalid regular expression: %s\n", m); exit(1); }
    t->kind = 'p';
    return;
  }
  fprintf(stderr, "nl: invalid line numbering style: '%s'\n", s);
  exit(1);
}

static int section = 1; /* 0 header, 1 body, 2 footer */
static long blanks;
static void line(const char *s, long n) {
  /* a delimiter line: \:\:\: \:\: \: (the delimiter may be one character) */
  size_t dl = strlen(delim);
  if (dl && n > 0 && n % (long)dl == 0 && n / (long)dl <= 3) {
    int all = 1;
    for (long k = 0; k < n; k += dl) if (memcmp(s + k, delim, dl)) { all = 0; break; }
    if (all) {
      int sec = 3 - n / (long)dl; /* three: header, two: body, one: footer */
      section = sec;
      if (!no_restart) num = start; /* GNU restarts at every section, not only at a header */
      putchar('\n');
      blanks = 0;
      return;
    }
  }
  struct type *t = &types[section];
  int numbered = 0;
  switch (t->kind) {
  case 'a':
    if (n == 0) { blanks++; if (blanks >= blank_join) { numbered = 1; blanks = 0; } }
    else { numbered = 1; blanks = 0; }
    break;
  case 't': numbered = n > 0; break;
  case 'p': {
    char *z = strndup(s, n);
    numbered = regexec(&t->re, z, 0, NULL, 0) == 0;
    free(z);
    break;
  }
  }
  if (numbered) {
    if (fmt == 'l') printf("%-*ld", (int)width, num);
    else if (zero_pad) printf("%0*ld", (int)width, num);
    else printf("%*ld", (int)width, num);
    fputs(sep, stdout);
    num += incr;
  } else printf("%*s", (int)(width + strlen(sep)), "");
  fwrite(s, 1, n, stdout);
  putchar('\n');
}

static long numarg(const char *v, const char *what) {
  char *e;
  long x = strtol(v, &e, 10);
  if (*e || !*v) { fprintf(stderr, "nl: invalid %s: '%s'\n", what, v); exit(1); }
  return x;
}
int main(int argc, char **argv) {
  int i = 1, rc = 0;
  types[0].kind = 'n'; types[1].kind = 't'; types[2].kind = 'n';
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (a[1] == '-') {
      char *eq = strchr(a, '=');
      const char *v = eq ? eq + 1 : (i + 1 < argc ? argv[i + 1] : "");
      size_t kl = eq ? (size_t)(eq - a) : strlen(a);
      int used = !eq;
#define IS(name) (kl == strlen(name) && !strncmp(a, name, kl))
      if (IS("--no-renumber")) { no_restart = 1; continue; }
      if (used) i++;
      if (IS("--body-numbering")) set_type(&types[1], v);
      else if (IS("--header-numbering")) set_type(&types[0], v);
      else if (IS("--footer-numbering")) set_type(&types[2], v);
      else if (IS("--starting-line-number")) start = numarg(v, "starting line number");
      else if (IS("--line-increment")) incr = numarg(v, "line number increment");
      else if (IS("--number-width")) width = numarg(v, "line number field width");
      else if (IS("--number-separator")) sep = v;
      else if (IS("--number-format")) { if (!strcmp(v, "ln")) fmt = 'l'; else if (!strcmp(v, "rn")) fmt = 'r'; else if (!strcmp(v, "rz")) { fmt = 'r'; zero_pad = 1; } else { fprintf(stderr, "nl: invalid line number format: '%s'\n", v); return 1; } }
      else if (IS("--join-blank-lines")) blank_join = numarg(v, "line number of blank lines");
      else if (IS("--section-delimiter")) { strncpy(delim, v, 2); delim[2] = 0; if (strlen(v) == 1) { delim[1] = ':'; } }
      else { fprintf(stderr, "nl: unrecognized option '%s'\n", a); return 1; }
#undef IS
      continue;
    }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'p') { no_restart = 1; continue; }
      if (!strchr("bhfvilnswd", *f)) { fprintf(stderr, "nl: invalid option -- '%c'\nusage: nl [-p] [-b type] [-v start] [-i incr] [-w width] [-n format] [-s sep] [file...]\n", *f); return 1; }
      char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
      if (!v) { fprintf(stderr, "nl: option requires an argument -- '%c'\n", *f); return 1; }
      switch (*f) {
      case 'b': set_type(&types[1], v); break;
      case 'h': set_type(&types[0], v); break;
      case 'f': set_type(&types[2], v); break;
      case 'v': start = numarg(v, "starting line number"); break;
      case 'i': incr = numarg(v, "line number increment"); break;
      case 'l': blank_join = numarg(v, "line number of blank lines"); break;
      case 'w': width = numarg(v, "line number field width"); if (width < 1) { fprintf(stderr, "nl: invalid line number field width: '%s'\n", v); return 1; } break;
      case 's': sep = v; break;
      case 'n': if (!strcmp(v, "ln")) fmt = 'l'; else if (!strcmp(v, "rn")) fmt = 'r'; else if (!strcmp(v, "rz")) { fmt = 'r'; zero_pad = 1; } else { fprintf(stderr, "nl: invalid line number format: '%s'\n", v); return 1; } break;
      case 'd': strncpy(delim, v, 2); delim[2] = 0; if (strlen(v) == 1) delim[1] = ':'; break;
      }
      break;
    }
  }
  num = start;
  setvbuf(stdout, NULL, _IOFBF, 65536);
  char *buf = NULL;
  size_t cap = 0;
  int nf = argc - i;
  for (int k = 0; k < (nf ? nf : 1); k++) {
    const char *name = nf ? argv[i + k] : "-";
    FILE *f = strcmp(name, "-") ? fopen(name, "r") : stdin;
    if (!f) { fprintf(stderr, "nl: %s: %s\n", name, strerror(errno)); rc = 1; continue; }
    long n;
    while ((n = getline(&buf, &cap, f)) > 0) {
      if (buf[n - 1] == '\n') n--;
      line(buf, n);
    }
    if (f != stdin) fclose(f);
  }
  if (fflush(stdout)) rc = 1;
  return rc;
}
