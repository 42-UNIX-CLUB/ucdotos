// SPDX-License-Identifier: GPL-2.0-or-later
/* cut -b LIST | -c LIST | -f LIST [-d DELIM] [-s] [-n] [--complement]
 *     [--output-delimiter=STR] [file...]
 * LIST is N, N-, N-M or -M, separated by commas or blanks. Characters are
 * bytes (there is no locale), so -c is -b. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int mode, only_delim, complement;
static char delim = '\t';
static const char *outdelim;
struct range { long lo, hi; };
static struct range *rs;
static int nr;

static void usage(void) {
  fprintf(stderr, "usage: cut -b list [-n] [file...]\n       cut -c list [file...]\n       cut -f list [-d delim] [-s] [file...]\n");
  exit(1);
}
static void parse_list(const char *s) {
  const char *p = s;
  while (*p) {
    long lo = 1, hi;
    int has_lo = 0;
    if (*p >= '0' && *p <= '9') { lo = strtol(p, (char **)&p, 10); has_lo = 1; }
    if (*p == '-') {
      p++;
      if (*p >= '0' && *p <= '9') hi = strtol(p, (char **)&p, 10);
      else { if (!has_lo) { fprintf(stderr, "cut: invalid range with no endpoint: -\n"); exit(1); } hi = -1; }
    } else {
      if (!has_lo) { fprintf(stderr, "cut: invalid byte, character or field list\n"); exit(1); }
      hi = lo;
    }
    if (lo == 0 || hi == 0) { fprintf(stderr, "cut: fields and positions are numbered from 1\n"); exit(1); }
    if (hi != -1 && hi < lo) { fprintf(stderr, "cut: invalid decreasing range\n"); exit(1); }
    rs = realloc(rs, (nr + 1) * sizeof *rs);
    rs[nr++] = (struct range){lo, hi};
    if (*p == ',' || *p == ' ' || *p == '\t') p++;
    else if (*p) { fprintf(stderr, "cut: invalid byte, character or field list\n"); exit(1); }
  }
  if (!nr) { fprintf(stderr, "cut: invalid byte, character or field list\n"); exit(1); }
}
static int selected(long i) {
  int in = 0;
  for (int k = 0; k < nr && !in; k++) in = i >= rs[k].lo && (rs[k].hi == -1 || i <= rs[k].hi);
  return in != complement;
}

static void cut_line(const char *l, size_t len, FILE *out) {
  if (mode != 'f') {
    int first = 1;
    long prev = -2;
    for (size_t i = 0; i < len; i++) {
      if (!selected(i + 1)) continue;
      /* --output-delimiter separates runs of selected bytes */
      if (outdelim && !first && prev != (long)i - 1) fputs(outdelim, out);
      putc(l[i], out);
      first = 0;
      prev = i;
    }
    putc('\n', out);
    return;
  }
  if (!memchr(l, delim, len)) {
    if (!only_delim) { fwrite(l, 1, len, out); putc('\n', out); }
    return;
  }
  long field = 1;
  int first = 1;
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || l[i] == delim) {
      if (selected(field)) {
        if (!first) { if (outdelim) fputs(outdelim, out); else putc(delim, out); }
        fwrite(l + start, 1, i - start, out);
        first = 0;
      }
      field++;
      start = i + 1;
    }
  }
  putc('\n', out);
}

int main(int argc, char **argv) {
  int i = 1, rc = 0;
  const char *list = NULL;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--complement")) { complement = 1; continue; }
    if (!strcmp(a, "--only-delimited")) { only_delim = 1; continue; }
    if (!strncmp(a, "--output-delimiter=", 19)) { outdelim = a + 19; continue; }
    if (!strcmp(a, "--output-delimiter") && i + 1 < argc) { outdelim = argv[++i]; continue; }
    if (!strncmp(a, "--delimiter=", 12)) { a = a + 12; if (strlen(a) > 1) { fprintf(stderr, "cut: the delimiter must be a single character\n"); return 1; } delim = *a ? *a : '\n'; continue; }
    if (!strncmp(a, "--fields=", 9)) { mode = 'f'; list = a + 9; continue; }
    if (!strncmp(a, "--bytes=", 8)) { mode = 'b'; list = a + 8; continue; }
    if (!strncmp(a, "--characters=", 13)) { mode = 'c'; list = a + 13; continue; }
    if (a[1] == '-') usage();
    for (char *f = a + 1; *f; f++) {
      if (*f == 's') only_delim = 1;
      else if (*f == 'n') ;
      else if (*f == 'b' || *f == 'c' || *f == 'f' || *f == 'd') {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "cut: option requires an argument -- '%c'\n", *f); usage(); }
        if (*f == 'd') {
          if (strlen(v) > 1) { fprintf(stderr, "cut: the delimiter must be a single character\n"); return 1; }
          delim = *v ? *v : '\n';
        } else {
          if (mode && mode != *f) { fprintf(stderr, "cut: only one type of list may be specified\n"); return 1; }
          mode = *f; list = v;
        }
        break;
      } else { fprintf(stderr, "cut: invalid option -- '%c'\n", *f); usage(); }
    }
  }
  if (!mode) { fprintf(stderr, "cut: you must specify a list of bytes, characters, or fields\n"); usage(); }
  if (mode != 'f' && only_delim) { fprintf(stderr, "cut: suppressing non-delimited lines makes sense\n\tonly when operating on fields\n"); return 1; }
  parse_list(list);
  setvbuf(stdout, NULL, _IOFBF, 65536);
  char *line = NULL;
  size_t cap = 0;
  int nfiles = argc - i;
  for (int k = 0; k < (nfiles ? nfiles : 1); k++) {
    const char *name = nfiles ? argv[i + k] : "-";
    FILE *f = strcmp(name, "-") ? fopen(name, "r") : stdin;
    if (!f) { fprintf(stderr, "cut: %s: %s\n", name, strerror(errno)); rc = 1; continue; }
    long n;
    while ((n = getline(&line, &cap, f)) > 0) {
      if (line[n - 1] == '\n') n--;
      cut_line(line, n, stdout);
    }
    if (f != stdin) fclose(f);
    else clearerr(f);
  }
  if (fflush(stdout) != 0) { perror("cut: write error"); rc = 1; }
  return rc;
}
