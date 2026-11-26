// SPDX-License-Identifier: GPL-2.0-or-later
/* sed [-nEsuz] [-i[SUFFIX]] [-l N] [-e script]... [-f file]... [script] [file...]
 * The POSIX stream editor with the GNU extensions that scripts use: -i, -E,
 * -s, -z, addresses 0,/re/ first~step addr,+N addr,~N, the I and M flags,
 * commands F Q R T W z v, \n \t \xHH and friends in scripts, and \L \U \l \u \E
 * in replacements. Regular expressions come from the C library (regcomp).
 * Not implemented: the e command and the e flag of s (they run a shell).
 *
 * -i writes each file to a temporary file in the same directory and renames
 * it over the original. A SUFFIX keeps the original under a new name: without
 * a '*' the suffix is appended; with '*', each '*' is replaced by the file's
 * name, and a result without '/' is placed in the file's directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <regex.h>
#include <sys/stat.h>

/* ---- small helpers ---- */
static void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) { fprintf(stderr, "sed: out of memory\n"); exit(4); }
  return p;
}
static char *xstrndup(const char *s, size_t n) {
  char *d = xrealloc(NULL, n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}
struct buf { char *s; size_t len, cap; };
static void bgrow(struct buf *b, size_t need) {
  if (need + 1 > b->cap) { b->cap = (need + 1) * 2; b->s = xrealloc(b->s, b->cap); }
}
static void bset(struct buf *b, const char *s, size_t n) { bgrow(b, n); memmove(b->s, s, n); b->len = n; b->s[n] = 0; }
static void bcat(struct buf *b, const char *s, size_t n) { bgrow(b, b->len + n); memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
static void bputc(struct buf *b, int c) { char ch = c; bcat(b, &ch, 1); }

/* ---- options ---- */
static int quiet, ere, separate, unbuffered, zero, inplace, posix_mode, line_wrap = 70;
static const char *suffix;
static char eol = '\n';

/* ---- outputs ---- */
struct out { char *name; FILE *f; int missing_nl; };
static struct out *outs;
static int nouts;
static struct out stdout_out, *cur_out;

static int get_out(const char *name) {
  for (int i = 0; i < nouts; i++) if (!strcmp(outs[i].name, name)) return i;
  outs = xrealloc(outs, (nouts + 1) * sizeof *outs);
  struct out *o = &outs[nouts];
  o->name = strdup(name);
  o->missing_nl = 0;
  if (!strcmp(name, "/dev/stdout")) o->f = NULL; /* the current output, resolved when writing */
  else if (!strcmp(name, "/dev/stderr")) o->f = stderr;
  else if (!(o->f = fopen(name, "w"))) { fprintf(stderr, "sed: couldn't open file %s: %s\n", name, strerror(errno)); exit(4); }
  return nouts++;
}
static struct out *resolve(int idx) {
  if (outs[idx].f == NULL) return &stdout_out;
  return &outs[idx];
}
static void out_write(struct out *o, const char *s, size_t n, int nl) {
  if (o->missing_nl) { putc(eol, o->f); o->missing_nl = 0; }
  fwrite(s, 1, n, o->f);
  if (nl) putc(eol, o->f); else o->missing_nl = 1;
  if (ferror(o->f)) { fprintf(stderr, "sed: couldn't write: %s\n", strerror(errno)); exit(4); }
  if (unbuffered) fflush(o->f);
}
static void out_text(struct out *o, const char *s, size_t n) { /* text that carries its own newlines */
  if (o->missing_nl) { putc(eol, o->f); o->missing_nl = 0; }
  fwrite(s, 1, n, o->f);
  if (unbuffered) fflush(o->f);
}

/* ---- program ---- */
enum { A_NONE, A_LINE, A_LAST, A_RE, A_STEP, A_PLUS, A_TILDE, A_ZERO };
struct addr { int type; long n, step; regex_t *re; };
struct rpart { int kind; char *s; size_t n; int group; }; /* kind 0 text, 1 group, 2 case op (s[0] = L U l u E) */
struct cmd {
  struct addr a1, a2;
  int naddr, neg;
  int active; long end_line;
  char c;
  char *text; size_t tlen;   /* a i c; file name for r R; label for : b t T */
  int jump;                  /* b t T: target index; {: index of matching } */
  regex_t *re;               /* s */
  struct rpart *rp; int nrp;
  int global, occurrence, print, wfile, eval;
  long arg;                  /* q Q exit code, l width */
  unsigned char ymap[256];
};
static struct cmd *prog;
static int nprog;
static regex_t *last_re;

/* the script text, and where we are in it */
static char *script;
static size_t slen, spos;
static void bad(const char *msg) {
  fprintf(stderr, "sed: -e expression #1, char %zu: %s\n", spos, msg);
  exit(1);
}
static int sc(void) { return spos < slen ? (unsigned char)script[spos] : -1; }
static void skip_ws(void) { while (spos < slen && (script[spos] == ' ' || script[spos] == '\t')) spos++; }

/* text between delimiters: \delim becomes delim, other backslash pairs stay;
 * a backslash followed by a newline becomes a newline */
static char *read_delimited(int delim, size_t *outlen, int is_regex) {
  struct buf b = {0};
  bgrow(&b, 16);
  for (;;) {
    int c = sc();
    if (c < 0) return NULL;
    spos++;
    if (c == delim) break;
    if (c == '\n' && is_regex) { free(b.s); return NULL; }
    if (c == '\\') {
      int d = sc();
      if (d < 0) { free(b.s); return NULL; }
      spos++;
      if (d == delim) bputc(&b, d);
      else if (d == '\n') bputc(&b, '\n');
      else if (d == 'n' && is_regex) bputc(&b, '\n');
      else { bputc(&b, '\\'); bputc(&b, d); }
      continue;
    }
    bputc(&b, c);
  }
  *outlen = b.len;
  return b.s;
}

/* \dNNN \oNNN \xHH \cX: returns the byte, or -1 if not such an escape; *p points after the letter */
static int num_escape(int letter, const char **pp, const char *end) {
  const char *p = *pp;
  int base = letter == 'd' ? 10 : letter == 'o' ? 8 : letter == 'x' ? 16 : 0, maxd = letter == 'x' ? 2 : 3, v = 0, k = 0;
  if (letter == 'c') {
    if (p >= end) return -1;
    *pp = p + 1;
    return toupper((unsigned char)*p) ^ 0x40;
  }
  if (!base) return -1;
  while (k < maxd && p < end) {
    int c = (unsigned char)*p, d = isdigit(c) ? c - '0' : isxdigit(c) ? tolower(c) - 'a' + 10 : 99;
    if (d >= base) break;
    v = v * base + d; p++; k++;
  }
  if (!k) return -1;
  *pp = p;
  return v & 255;
}
static int simple_escape(int c) {
  switch (c) {
  case 'a': return 7; case 'f': return 12; case 'v': return 11; case 'r': return 13; case 't': return 9; case 'n': return 10;
  }
  return -1;
}

/* the regex text after read_delimited: turn \t \xHH and friends into bytes */
static char *convert_re(const char *s, size_t n) {
  struct buf b = {0};
  bgrow(&b, n + 1);
  const char *end = s + n;
  const char *special = ere ? ".[]*^$\\+?(){}|/" : ".[]*^$\\/";
  int in_bracket = 0;
  for (const char *p = s; p < end;) {
    int c = (unsigned char)*p++;
    if (in_bracket) {
      if (c == '[' && p < end && (*p == ':' || *p == '.' || *p == '=')) { /* [:class:] */
        int k = *p;
        bputc(&b, c);
        while (p < end && !(p[0] == k && p + 1 < end && p[1] == ']')) bputc(&b, *p++);
        if (p < end) { bputc(&b, *p++); bputc(&b, *p++); }
        in_bracket = 2;
        continue;
      }
      if (c == ']' && in_bracket > 1) { in_bracket = 0; bputc(&b, c); continue; }
      if (c == '\\' && p < end && simple_escape((unsigned char)*p) >= 0) { bputc(&b, simple_escape((unsigned char)*p++)); in_bracket = 2; continue; }
      bputc(&b, c);
      in_bracket = 2;
      continue;
    }
    if (c == '[') {
      bputc(&b, c);
      in_bracket = 1;
      if (p < end && *p == '^') bputc(&b, *p++);
      if (p < end && *p == ']') { bputc(&b, *p++); in_bracket = 2; }
      continue;
    }
    if (c == '\\' && p < end) {
      int d = (unsigned char)*p++, v = simple_escape(d);
      if (v < 0 && (d == 'd' || d == 'o' || d == 'x' || d == 'c')) v = num_escape(d, &p, end);
      if (v >= 0) {
        if (v == 0) bad("a NUL byte in a regular expression is not supported");
        if (strchr(special, v)) bputc(&b, '\\');
        bputc(&b, v);
      } else { bputc(&b, '\\'); bputc(&b, d); }
      continue;
    }
    bputc(&b, c);
  }
  return b.s;
}

static regex_t *compile_re(const char *raw, size_t n, int flags) {
  if (n == 0) return NULL; /* the empty regex: the last one used */
  char *pat = convert_re(raw, n);
  regex_t *re = xrealloc(NULL, sizeof *re);
  int cf = (ere ? REG_EXTENDED : 0) | ((flags & 1) ? REG_ICASE : 0) | ((flags & 2) ? REG_NEWLINE : 0);
  int rc = regcomp(re, pat, cf);
  free(pat);
  if (rc) {
    char msg[128];
    regerror(rc, re, msg, sizeof msg);
    bad(msg);
  }
  return re;
}

static long read_num(void) {
  long v = 0;
  while (isdigit(sc())) { v = v * 10 + (script[spos] - '0'); spos++; if (v > 100000000) v = 100000000; }
  return v;
}

static int parse_addr(struct addr *a) {
  int c = sc();
  a->type = A_NONE;
  if (isdigit(c)) {
    long n = read_num();
    if (sc() == '~') {
      spos++;
      a->type = A_STEP; a->n = n; a->step = read_num();
      return 1;
    }
    a->type = n == 0 ? A_ZERO : A_LINE;
    a->n = n;
    return 1;
  }
  if (c == '$') { spos++; a->type = A_LAST; return 1; }
  if (c == '/' || c == '\\') {
    spos++;
    int delim = '/';
    if (c == '\\') { delim = sc(); if (delim < 0 || delim == '\n' || delim == '\\') bad("unexpected `,'"); spos++; }
    size_t n;
    char *raw = read_delimited(delim, &n, 1);
    if (!raw) bad("unterminated address regex");
    int flags = 0;
    for (;;) {
      if (sc() == 'I') { flags |= 1; spos++; }
      else if (sc() == 'M') { flags |= 2; spos++; }
      else break;
    }
    a->type = A_RE;
    a->re = compile_re(raw, n, flags);
    free(raw);
    return 1;
  }
  return 0;
}

/* text of a i c: one-liner or the classic a\ form */
static void parse_text(struct cmd *cm) {
  struct buf b = {0};
  bgrow(&b, 16);
  skip_ws();
  if (sc() == '\\') {
    spos++;
    if (sc() == '\n') spos++;
  } else {
    if (posix_mode) bad("expected \\ after `a', `c' or `i'");
    if (sc() < 0 || sc() == '\n') bad("expected \\ after `a', `c' or `i'");
  }
  int cont = 0; /* the last byte came from a backslash-newline */
  for (;;) {
    int c = sc();
    if (c < 0) break;
    spos++;
    cont = 0;
    if (c == '\\') {
      int d = sc();
      if (d < 0) break;
      spos++;
      cont = d == '\n';
      int v = d == 'n' || d == 't' || d == 'a' || d == 'f' || d == 'v' || d == 'r' ? simple_escape(d) : d;
      bputc(&b, v); /* a backslash-newline keeps the newline and the text goes on */
      continue;
    }
    if (c == '\n') break;
    bputc(&b, c);
  }
  if (sc() >= 0 || (b.len && !cont)) bputc(&b, '\n'); /* GNU: nothing more at the very end of the script */
  cm->text = b.s;
  cm->tlen = b.len;
}

/* rest of the line: file names for r R w W and the w flag */
static char *read_filename(void) {
  skip_ws();
  size_t st = spos;
  while (spos < slen && script[spos] != '\n') spos++;
  char *name = xstrndup(script + st, spos - st);
  if (!*name) bad("missing filename in r/R/w/W commands");
  return name;
}
static char *read_label(void) {
  skip_ws();
  size_t st = spos;
  while (spos < slen && script[spos] != '\n' && script[spos] != ';') spos++;
  size_t e = spos;
  while (e > st && (script[e - 1] == ' ' || script[e - 1] == '\t')) e--;
  return xstrndup(script + st, e - st);
}

static void parse_replacement(struct cmd *cm, const char *s, size_t n) {
  struct buf lit = {0};
  bgrow(&lit, 16);
  const char *end = s + n;
#define FLUSH() do { if (lit.len) { cm->rp = xrealloc(cm->rp, (cm->nrp + 1) * sizeof *cm->rp); cm->rp[cm->nrp++] = (struct rpart){0, xstrndup(lit.s, lit.len), lit.len, 0}; lit.len = 0; } } while (0)
  for (const char *p = s; p < end;) {
    int c = (unsigned char)*p++;
    if (c == '&') {
      FLUSH();
      cm->rp = xrealloc(cm->rp, (cm->nrp + 1) * sizeof *cm->rp);
      cm->rp[cm->nrp++] = (struct rpart){1, NULL, 0, 0};
      continue;
    }
    if (c == '\\' && p < end) {
      int d = (unsigned char)*p++;
      if (isdigit(d)) {
        if (d - '0' > (int)(cm->re ? cm->re->re_nsub : 9)) bad("invalid reference on `s' command's RHS");
        FLUSH();
        cm->rp = xrealloc(cm->rp, (cm->nrp + 1) * sizeof *cm->rp);
        cm->rp[cm->nrp++] = (struct rpart){1, NULL, 0, d - '0'};
        continue;
      }
      if (strchr("LUluE", d) && !posix_mode) {
        FLUSH();
        cm->rp = xrealloc(cm->rp, (cm->nrp + 1) * sizeof *cm->rp);
        cm->rp[cm->nrp++] = (struct rpart){2, xstrndup((char *)&d, 1), 1, 0};
        continue;
      }
      int v = simple_escape(d);
      if (v < 0 && (d == 'd' || d == 'o' || d == 'x' || d == 'c')) v = num_escape(d, &p, end);
      if (v >= 0) bputc(&lit, v);
      else bputc(&lit, d); /* \& \\ \/ and anything else: the character itself */
      continue;
    }
    bputc(&lit, c);
  }
  FLUSH();
#undef FLUSH
  free(lit.s);
}

static void end_of_cmd(void) {
  skip_ws();
  int c = sc();
  if (c < 0 || c == '\n' || c == ';') { if (c >= 0) spos++; return; }
  if (c == '}' || c == '#') return;
  bad("extra characters after command");
}

static void parse_script(void) {
  int *open = NULL, nopen = 0;
  spos = 0;
  if (slen >= 2 && script[0] == '#' && script[1] == 'n' && (slen == 2 || script[2] == '\n')) quiet = 1;
  for (;;) {
    while (spos < slen && (isspace((unsigned char)script[spos]) || script[spos] == ';')) spos++;
    if (spos >= slen) break;
    prog = xrealloc(prog, (nprog + 1) * sizeof *prog);
    struct cmd *cm = &prog[nprog];
    memset(cm, 0, sizeof *cm);
    cm->wfile = -1;
    if (parse_addr(&cm->a1)) {
      cm->naddr = 1;
      skip_ws();
      if (sc() == ',') {
        spos++;
        skip_ws();
        if (sc() == '+' || sc() == '~') {
          int k = sc();
          spos++;
          if (!isdigit(sc())) bad("expected number");
          cm->a2.type = k == '+' ? A_PLUS : A_TILDE;
          cm->a2.n = read_num();
        } else if (!parse_addr(&cm->a2)) bad("unexpected `,'");
        if (cm->a2.type == A_ZERO) bad("invalid usage of line address 0");
        cm->naddr = 2;
      }
      if (cm->a1.type == A_ZERO && (cm->naddr != 2 || cm->a2.type != A_RE)) bad("invalid usage of line address 0");
      if (cm->a1.type == A_ZERO) cm->active = 1;
    }
    skip_ws();
    while (sc() == '!') { cm->neg = 1; spos++; skip_ws(); }
    int c = sc();
    if (c < 0) bad("missing command");
    spos++;
    cm->c = c;
    switch (c) {
    case '{':
      open = xrealloc(open, (nopen + 1) * sizeof *open);
      open[nopen++] = nprog;
      break;
    case '}':
      if (!nopen) bad("unexpected `}'");
      if (cm->naddr) bad("} doesn't want any addresses");
      prog[open[--nopen]].jump = nprog;
      end_of_cmd();
      break;
    case '#':
      if (cm->naddr) bad("comments don't accept any addresses");
      while (spos < slen && script[spos] != '\n') spos++;
      break;
    case ':': {
      if (cm->naddr) bad(": doesn't want any addresses");
      cm->text = read_label();
      if (!*cm->text) bad("\":\" lacks a label");
      if (sc() == ';') spos++;
      break;
    }
    case 'a': case 'i': case 'c':
      parse_text(cm);
      break;
    case 'b': case 't': case 'T':
      cm->text = read_label();
      if (sc() == ';') spos++;
      break;
    case 'r': case 'R':
      cm->text = read_filename();
      break;
    case 'w': case 'W': {
      char *name = read_filename();
      cm->wfile = get_out(name);
      free(name);
      break;
    }
    case 'q': case 'Q': case 'l': case 'L':
      if ((c == 'q' || c == 'Q') && cm->naddr > 1) bad("command only uses one address");
      skip_ws();
      cm->arg = isdigit(sc()) ? read_num() : (c == 'l' ? -1 : 0);
      end_of_cmd();
      break;
    case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'n': case 'N':
    case 'p': case 'P': case 'x': case 'z': case 'F':
      end_of_cmd();
      break;
    case 'v':
      read_label();
      if (sc() == ';') spos++;
      break;
    case 'e':
      bad("the e command is not supported");
      break;
    case 's': {
      int delim = sc();
      if (delim < 0 || delim == '\n' || delim == '\\') bad("unterminated `s' command");
      spos++;
      size_t rn, pn;
      char *raw = read_delimited(delim, &rn, 1);
      if (!raw) bad("unterminated `s' command");
      char *rep = read_delimited(delim, &pn, 0);
      if (!rep) bad("unterminated `s' command");
      int flags = 0;
      for (;;) {
        int f = sc();
        if (f == 'g') { if (cm->global) bad("multiple `g' options to `s' command"); cm->global = 1; spos++; }
        else if (f == 'p') { if (cm->print) bad("multiple `p' options to `s' command"); cm->print = 1; spos++; }
        else if (f == 'i' || f == 'I') { flags |= 1; spos++; }
        else if (f == 'm' || f == 'M') { flags |= 2; spos++; }
        else if (f == 'e') bad("the e flag of s is not supported");
        else if (isdigit(f)) {
          if (cm->occurrence) bad("multiple number options to `s' command");
          long v = read_num();
          if (v == 0) bad("number option to `s' command may not be zero");
          cm->occurrence = v;
        } else if (f == 'w') {
          spos++;
          char *name = read_filename();
          cm->wfile = get_out(name);
          free(name);
          break;
        } else break;
      }
      cm->re = compile_re(raw, rn, flags);
      parse_replacement(cm, rep, pn);
      free(raw);
      free(rep);
      if (cm->wfile < 0) end_of_cmd();
      break;
    }
    case 'y': {
      int delim = sc();
      if (delim < 0 || delim == '\n' || delim == '\\') bad("unterminated `y' command");
      spos++;
      size_t n1, n2;
      char *a = read_delimited(delim, &n1, 0), *b = a ? read_delimited(delim, &n2, 0) : NULL;
      if (!a || !b) bad("unterminated `y' command");
      /* unescape: \\ is a backslash, \n a newline */
      char *x[2] = {a, b};
      size_t len[2] = {n1, n2};
      for (int k = 0; k < 2; k++) {
        size_t o = 0;
        for (size_t i = 0; i < len[k]; i++) {
          if (x[k][i] == '\\' && i + 1 < len[k]) {
            int d = x[k][++i], v = simple_escape(d);
            x[k][o++] = v >= 0 ? v : d == '\\' ? '\\' : (bad("unknown option to `y'"), 0);
          } else x[k][o++] = x[k][i];
        }
        len[k] = o;
      }
      if (len[0] != len[1]) bad("strings for `y' command are different lengths");
      for (int k = 0; k < 256; k++) cm->ymap[k] = k;
      for (size_t i = 0; i < len[0]; i++) cm->ymap[(unsigned char)a[i]] = b[i];
      free(a); free(b);
      end_of_cmd();
      break;
    }
    default: {
      char msg[40];
      snprintf(msg, sizeof msg, "unknown command: `%c'", c);
      bad(msg);
    }
    }
    nprog++;
  }
  if (nopen) bad("unmatched `{'");
  free(open);
  /* resolve labels */
  for (int i = 0; i < nprog; i++) {
    struct cmd *cm = &prog[i];
    if (cm->c != 'b' && cm->c != 't' && cm->c != 'T') continue;
    cm->jump = nprog;
    if (!*cm->text) continue;
    int found = 0;
    for (int k = 0; k < nprog; k++)
      if (prog[k].c == ':' && !strcmp(prog[k].text, cm->text)) { cm->jump = k; found = 1; break; }
    if (!found) { fprintf(stderr, "sed: can't find label for jump to `%s'\n", cm->text); exit(4); }
  }
}

/* ---- input ---- */
static char **files;
static int nfiles, fileidx = -1;
static FILE *in;
static const char *in_name;
static struct buf ahead;
static int have_ahead, ahead_nl, input_done;
static const char *ahead_name, *cur_name = "-";
static long lineno;
static int exit_code;
/* in-place state */
static char *tmpname;
static FILE *tmpf;

static void finish_inplace(void) {
  if (!tmpf) return;
  if (fclose(tmpf) != 0) { fprintf(stderr, "sed: couldn't close %s: %s\n", tmpname, strerror(errno)); exit(4); }
  tmpf = NULL;
  if (suffix && *suffix) {
    /* as GNU sed: each '*' in the suffix becomes the file name as given */
    struct buf bak = {0};
    if (strchr(suffix, '*')) {
      for (const char *p = suffix; *p; p++) {
        if (*p == '*') bcat(&bak, in_name, strlen(in_name));
        else bputc(&bak, *p);
      }
    } else { bcat(&bak, in_name, strlen(in_name)); bcat(&bak, suffix, strlen(suffix)); }
    if (rename(in_name, bak.s) != 0) {
      fprintf(stderr, "sed: cannot rename %s to %s: %s\n", in_name, bak.s, strerror(errno));
      unlink(tmpname); /* the original stays as it was */
      exit(4);
    }
    free(bak.s);
  }
  if (rename(tmpname, in_name) != 0) { fprintf(stderr, "sed: cannot rename %s: %s\n", tmpname, strerror(errno)); exit(4); }
  free(tmpname);
  tmpname = NULL;
  stdout_out.f = stdout;
  stdout_out.missing_nl = 0;
}

static int start_inplace(const char *name, FILE *f) {
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
    fprintf(stderr, "sed: couldn't edit %s: not a regular file\n", name);
    return -1;
  }
  const char *slash = strrchr(name, '/');
  int dirlen = slash ? (int)(slash - name + 1) : 0;
  static unsigned counter;
  for (int tries = 0; tries < 100; tries++) {
    tmpname = xrealloc(tmpname, dirlen + 32);
    snprintf(tmpname, dirlen + 32, "%.*ssed%05u%04u", dirlen, name, (unsigned)getpid() % 100000, counter++ % 10000);
    int fd = open(tmpname, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
    if (fd >= 0) {
      tmpf = fdopen(fd, "w");
      stdout_out.f = tmpf;
      stdout_out.missing_nl = 0;
      return 0;
    }
    if (errno != EEXIST) break;
  }
  fprintf(stderr, "sed: couldn't open temporary file %s: %s\n", tmpname, strerror(errno));
  exit(4);
}

/* open the next input file; returns 0 when there are no more */
static int next_file(void) {
  for (;;) {
    if (in && in != stdin) fclose(in);
    in = NULL;
    if (inplace) finish_inplace();
    if (++fileidx >= nfiles) return 0;
    in_name = files[fileidx];
    if (!strcmp(in_name, "-") && !inplace) { in = stdin; clearerr(stdin); }
    else if (!(in = fopen(in_name, "r"))) {
      fprintf(stderr, "sed: can't read %s: %s\n", in_name, strerror(errno));
      exit_code = 2;
      continue;
    }
    if (inplace && start_inplace(in_name, in) < 0) { fclose(in); in = NULL; exit_code = 4; continue; }
    if (separate) lineno = 0;
    return 1;
  }
}

/* read one raw line from the current input into b; 1 if a line was read */
static int raw_line(struct buf *b, int *had_nl) {
  b->len = 0;
  int c, any = 0;
  while ((c = getc(in)) != EOF) {
    any = 1;
    if (c == eol) { *had_nl = 1; bgrow(b, b->len); b->s[b->len] = 0; return 1; }
    bputc(b, c);
  }
  if (ferror(in)) { fprintf(stderr, "sed: read error on %s: %s\n", in_name, strerror(errno)); exit_code = 2; }
  *had_nl = 0;
  if (any) bgrow(b, b->len), b->s[b->len] = 0;
  return any;
}
/* fill the look-ahead line, crossing file boundaries unless files are separate */
static void fill_ahead(void) {
  have_ahead = 0;
  for (;;) {
    if (in && raw_line(&ahead, &ahead_nl)) { have_ahead = 1; ahead_name = in == stdin ? "-" : in_name; return; }
    if (separate) return; /* the end of each file is a last line: read_line opens the next */
    if (!next_file()) { input_done = 1; return; }
  }
}
/* the next input line: 1 if there was one */
static struct buf ps, hs;
static int ps_nl = 1, hs_nl = 1;
static int read_line(struct buf *dst, int append) {
  while (!have_ahead) {
    if (!separate || input_done) return 0;
    if (!next_file()) { input_done = 1; return 0; }
    fill_ahead();
  }
  if (append) { bputc(dst, '\n'); bcat(dst, ahead.s, ahead.len); }
  else bset(dst, ahead.s, ahead.len);
  ps_nl = ahead_nl;
  cur_name = ahead_name;
  lineno++;
  fill_ahead();
  return 1;
}
/* $: the last line of the input, or of each file with -s and -i */
static int is_last(void) { return !have_ahead; }

/* ---- execution ---- */
struct pending { int kind; char *s; size_t n; }; /* 0 text, 1 file (r), 2 line already read (R) */
static struct pending *queue;
static int nqueue;
static struct { char *name; FILE *f; } *rfiles;
static int nrfiles;
static int replaced;

static void queue_add(int kind, const char *s, size_t n) {
  queue = xrealloc(queue, (nqueue + 1) * sizeof *queue);
  queue[nqueue++] = (struct pending){kind, xstrndup(s, n), n};
}
static void flush_queue(void) {
  for (int i = 0; i < nqueue; i++) {
    struct pending *q = &queue[i];
    if (q->kind == 1) {
      FILE *f = !strcmp(q->s, "/dev/stdin") ? stdin : fopen(q->s, "r");
      if (f) {
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) out_text(&stdout_out, tmp, n);
        if (f != stdin) fclose(f);
      }
    } else out_text(&stdout_out, q->s, q->n);
    free(q->s);
  }
  nqueue = 0;
}

static int match1(struct addr *a) {
  switch (a->type) {
  case A_LINE: return lineno == a->n;
  case A_LAST: return is_last();
  case A_STEP: return a->step <= 0 ? lineno == a->n : lineno >= a->n && (lineno - a->n) % a->step == 0;
  case A_ZERO: return 0;
  case A_RE: {
    regex_t *re = a->re ? a->re : last_re;
    if (!re) { fprintf(stderr, "sed: no previous regular expression\n"); exit(1); }
    last_re = re;
    regmatch_t m[1] = {{0, (regoff_t)ps.len}};
    return regexec(re, ps.s, 1, m, REG_STARTEND) == 0;
  }
  }
  return 0;
}
static int matches(struct cmd *cm) {
  if (cm->naddr == 0) return 1;
  if (cm->naddr == 1) return match1(&cm->a1);
  if (cm->active) {
    switch (cm->a2.type) {
    case A_LINE: case A_PLUS: case A_TILDE:
      if (lineno > cm->end_line) { cm->active = 0; return match1(&cm->a1) ? matches(cm) : 0; }
      if (lineno == cm->end_line) cm->active = 0;
      return 1;
    case A_LAST: if (is_last()) cm->active = 0; return 1;
    default: if (match1(&cm->a2)) cm->active = 0; return 1;
    }
  }
  if (!match1(&cm->a1)) return 0;
  switch (cm->a2.type) {
  case A_LINE: if (cm->a2.n > lineno) { cm->active = 1; cm->end_line = cm->a2.n; } break;
  case A_PLUS: if (cm->a2.n > 0) { cm->active = 1; cm->end_line = lineno + cm->a2.n; } break;
  case A_TILDE: if (cm->a2.n > 0) { cm->active = 1; cm->end_line = (lineno / cm->a2.n + 1) * cm->a2.n; } break;
  case A_LAST: if (!is_last()) cm->active = 1; break;
  default: cm->active = 1; break; /* a regex is first checked on the next line */
  }
  return 1;
}

static void put_case(struct buf *b, const char *s, size_t n, int *mode, int *once) {
  for (size_t i = 0; i < n; i++) {
    int c = (unsigned char)s[i];
    if (*once == 'u') { c = toupper(c); *once = 0; }
    else if (*once == 'l') { c = tolower(c); *once = 0; }
    else if (*mode == 'U') c = toupper(c);
    else if (*mode == 'L') c = tolower(c);
    bputc(b, c);
  }
}

static int do_subst(struct cmd *cm) {
  regex_t *re = cm->re ? cm->re : last_re;
  if (!re) { fprintf(stderr, "sed: no previous regular expression\n"); exit(1); }
  last_re = re;
  size_t nm = re->re_nsub + 1 > 10 ? 10 : re->re_nsub + 1;
  regmatch_t m[10];
  struct buf res = {0};
  bgrow(&res, ps.len + 16);
  size_t pos = 0, copied = 0;
  long count = 0;
  int did = 0;
  long prev_end = -1;
  while (pos <= ps.len) {
    m[0].rm_so = pos; m[0].rm_eo = ps.len;
    if (regexec(re, ps.s, nm, m, REG_UC_OFFSET) != 0) break;
    size_t so = m[0].rm_so, eo = m[0].rm_eo;
    if (so == eo && (long)so == prev_end) { /* no empty match right after a match */
      if (so >= ps.len) break;
      pos = so + 1;
      continue;
    }
    count++;
    prev_end = eo;
    if (count >= (cm->occurrence ? cm->occurrence : 1)) {
      bcat(&res, ps.s + copied, so - copied);
      int mode = 0, once = 0;
      for (int k = 0; k < cm->nrp; k++) {
        struct rpart *r = &cm->rp[k];
        if (r->kind == 0) put_case(&res, r->s, r->n, &mode, &once);
        else if (r->kind == 1) {
          if (r->group < (int)nm && m[r->group].rm_so >= 0)
            put_case(&res, ps.s + m[r->group].rm_so, m[r->group].rm_eo - m[r->group].rm_so, &mode, &once);
        } else {
          int op = r->s[0];
          if (op == 'u' || op == 'l') once = op;
          else if (op == 'E') { mode = 0; once = 0; }
          else mode = op;
        }
      }
      copied = eo;
      did = 1;
      if (!cm->global) break;
    }
    if (eo == so) {
      if (so >= ps.len) break;
      pos = so + 1;
    } else pos = eo;
  }
  if (!did) { free(res.s); return 0; }
  bcat(&res, ps.s + copied, ps.len - copied);
  free(ps.s);
  ps = res;
  return 1;
}

static void do_list(struct cmd *cm) {
  long width = cm->arg >= 0 ? cm->arg : line_wrap;
  struct buf o = {0};
  bgrow(&o, ps.len * 2 + 4);
  long col = 0;
  for (size_t i = 0; i < ps.len; i++) {
    unsigned char c = ps.s[i];
    char tmp[8];
    if (c == '\\') strcpy(tmp, "\\\\");
    else if (c == '\a') strcpy(tmp, "\\a");
    else if (c == '\b') strcpy(tmp, "\\b");
    else if (c == '\f') strcpy(tmp, "\\f");
    else if (c == '\n') strcpy(tmp, "\\n");
    else if (c == '\r') strcpy(tmp, "\\r");
    else if (c == '\t') strcpy(tmp, "\\t");
    else if (c == '\v') strcpy(tmp, "\\v");
    else if (c < 32 || c >= 127) snprintf(tmp, sizeof tmp, "\\%03o", c);
    else { tmp[0] = c; tmp[1] = 0; }
    long w = strlen(tmp);
    if (width > 0 && col + w > width - 1) { bcat(&o, "\\\n", 2); col = 0; }
    bcat(&o, tmp, w);
    col += w;
  }
  bcat(&o, "$\n", 2);
  out_text(&stdout_out, o.s, o.len);
  free(o.s);
}

static FILE *rfile(const char *name) {
  for (int i = 0; i < nrfiles; i++) if (!strcmp(rfiles[i].name, name)) return rfiles[i].f;
  rfiles = xrealloc(rfiles, (nrfiles + 1) * sizeof *rfiles);
  rfiles[nrfiles].name = strdup(name);
  rfiles[nrfiles].f = !strcmp(name, "/dev/stdin") ? stdin : fopen(name, "r");
  return rfiles[nrfiles++].f;
}

static void emit_ps(void) { out_write(&stdout_out, ps.s, ps.len, ps_nl); }

static void run(void) {
  fill_ahead();
  int restart = 0; /* D: start the next cycle without reading */
  for (;;) {
    if (!restart) {
      if (!read_line(&ps, 0)) break;
      replaced = 0;
    }
    restart = 0;
    int pc = 0, quit = -1, eof_quit = 0, autoprint = !quiet;
    while (pc < nprog) {
#ifdef __wasm__
      static unsigned steps;
      if ((++steps & 0xffff) == 0) getpid(); /* a branch loop must not freeze the browser tab */
#endif
      struct cmd *cm = &prog[pc];
      int hit = matches(cm);
      if (cm->neg) hit = !hit;
      if (!hit) {
        if (cm->c == '{') { pc = cm->jump + 1; continue; }
        pc++;
        continue;
      }
      switch (cm->c) {
      case '{': case '}': case ':': case '#': case 'v': break;
      case '=': { char tmp[32]; int n = snprintf(tmp, sizeof tmp, "%ld\n", lineno); out_text(&stdout_out, tmp, n); break; }
      case 'a': queue_add(0, cm->text, cm->tlen); break;
      case 'i': out_text(&stdout_out, cm->text, cm->tlen); break;
      case 'c':
        if (cm->naddr < 2 || !cm->active || cm->neg) out_text(&stdout_out, cm->text, cm->tlen);
        autoprint = 0;
        pc = nprog;
        continue;
      case 'b': pc = cm->jump; continue;
      case 't': if (replaced) { replaced = 0; pc = cm->jump; continue; } break;
      case 'T': if (!replaced) { pc = cm->jump; continue; } replaced = 0; break;
      case 'd': autoprint = 0; pc = nprog; continue;
      case 'D': {
        char *nl = memchr(ps.s, '\n', ps.len);
        if (!nl) { autoprint = 0; pc = nprog; continue; }
        size_t skip = nl - ps.s + 1;
        memmove(ps.s, ps.s + skip, ps.len - skip);
        ps.len -= skip;
        ps.s[ps.len] = 0;
        autoprint = 0;
        restart = 1;
        pc = nprog;
        continue;
      }
      case 'F': out_text(&stdout_out, cur_name, strlen(cur_name)); out_text(&stdout_out, "\n", 1); break;
      /* each buffer remembers whether its line ended with a newline, as in GNU sed */
      case 'g': bset(&ps, hs.s, hs.len); ps_nl = hs_nl; break;
      case 'G': bputc(&ps, '\n'); bcat(&ps, hs.s, hs.len); ps_nl = hs_nl; break;
      case 'h': bset(&hs, ps.s, ps.len); hs_nl = ps_nl; break;
      case 'H': bputc(&hs, '\n'); bcat(&hs, ps.s, ps.len); hs_nl = ps_nl; break;
      case 'x': { struct buf t = ps; ps = hs; hs = t; int k = ps_nl; ps_nl = hs_nl; hs_nl = k; break; }
      case 'l': do_list(cm); break;
      case 'n':
        if (!have_ahead && (input_done || !separate)) { if (posix_mode) autoprint = 0; eof_quit = 1; pc = nprog; continue; }
        if (!quiet) emit_ps();
        flush_queue();
        if (!read_line(&ps, 0)) { autoprint = 0; eof_quit = 1; pc = nprog; continue; }
        break;
      case 'N':
        if (!have_ahead && (input_done || !separate)) { if (posix_mode) autoprint = 0; eof_quit = 1; pc = nprog; continue; }
        flush_queue();
        if (!read_line(&ps, 1)) { eof_quit = 1; pc = nprog; continue; }
        break;
      case 'p': emit_ps(); break;
      case 'P': { char *nl = memchr(ps.s, '\n', ps.len); out_write(&stdout_out, ps.s, nl ? (size_t)(nl - ps.s) : ps.len, 1); break; }
      case 'q': quit = cm->arg; pc = nprog; continue;
      case 'Q': flush_queue(); exit_code = exit_code ? exit_code : cm->arg; goto done;
      case 'r': queue_add(1, cm->text, strlen(cm->text)); break;
      case 'R': {
        FILE *f = rfile(cm->text);
        if (f) {
          struct buf l = {0};
          int c;
          while ((c = getc(f)) != EOF) { bputc(&l, c); if (c == '\n') break; }
          if (l.len) { if (l.s[l.len - 1] != '\n') bputc(&l, '\n'); queue_add(2, l.s, l.len); }
          free(l.s);
        }
        break;
      }
      case 's':
        if (do_subst(cm)) {
          replaced = 1;
          if (cm->print) emit_ps();
          if (cm->wfile >= 0) out_write(resolve(cm->wfile), ps.s, ps.len, 1);
        }
        break;
      case 'w': out_write(resolve(cm->wfile), ps.s, ps.len, 1); break;
      case 'W': { char *nl = memchr(ps.s, '\n', ps.len); out_write(resolve(cm->wfile), ps.s, nl ? (size_t)(nl - ps.s) : ps.len, 1); break; }
      case 'y': for (size_t k = 0; k < ps.len; k++) ps.s[k] = cm->ymap[(unsigned char)ps.s[k]]; break;
      case 'z': ps.len = 0; ps.s[0] = 0; break;
      }
      pc++;
    }
    if (autoprint) emit_ps();
    flush_queue();
    if (eof_quit) break;
    if (quit >= 0) {
      if (stdout_out.missing_nl) { putc(eol, stdout_out.f); stdout_out.missing_nl = 0; } /* GNU does this on q */
      exit_code = exit_code ? exit_code : quit;
      break;
    }
  }
done:
  if (inplace) {
    /* q stops early: what was written so far replaces the file */
    if (tmpf) finish_inplace();
  }
}

static void add_script(const char *s, size_t n) {
  script = xrealloc(script, slen + n + 2);
  memcpy(script + slen, s, n);
  slen += n;
  script[slen++] = '\n';
  script[slen] = 0;
}
static void add_script_file(const char *name) {
  FILE *f = strcmp(name, "-") ? fopen(name, "r") : stdin;
  if (!f) { fprintf(stderr, "sed: couldn't open file %s: %s\n", name, strerror(errno)); exit(4); }
  struct buf b = {0};
  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) bcat(&b, tmp, n);
  if (f != stdin) fclose(f);
  if (b.len && b.s[b.len - 1] == '\n') b.len--;
  add_script(b.s ? b.s : "", b.len);
  free(b.s);
}

static void usage(void) {
  fprintf(stderr, "usage: sed [-nEsuz] [-i[SUFFIX]] [-e script] [-f file] [script] [file...]\n");
  exit(1);
}

int main(int argc, char **argv) {
  int i = 1, have_script = 0, nops = 0, dashdash = 0;
  char **ops = xrealloc(NULL, argc * sizeof *ops);
  /* options may follow operands, as with GNU getopt */
  for (; i < argc; i++) {
    char *a = argv[i];
    if (dashdash || a[0] != '-' || !a[1]) { ops[nops++] = a; continue; }
    if (!strcmp(a, "--")) { dashdash = 1; continue; }
    if (a[1] == '-') {
      if (!strcmp(a, "--quiet") || !strcmp(a, "--silent")) quiet = 1;
      else if (!strcmp(a, "--regexp-extended")) ere = 1;
      else if (!strcmp(a, "--separate")) separate = 1;
      else if (!strcmp(a, "--unbuffered")) unbuffered = 1;
      else if (!strcmp(a, "--null-data")) zero = 1;
      else if (!strcmp(a, "--posix")) posix_mode = 1;
      else if (!strcmp(a, "--debug") || !strcmp(a, "--sandbox") || !strcmp(a, "--follow-symlinks")) ;
      else if (!strcmp(a, "--in-place")) { inplace = 1; separate = 1; }
      else if (!strncmp(a, "--in-place=", 11)) { inplace = 1; separate = 1; suffix = a + 11; }
      else if (!strncmp(a, "--expression=", 13)) { add_script(a + 13, strlen(a + 13)); have_script = 1; }
      else if (!strncmp(a, "--file=", 7)) { add_script_file(a + 7); have_script = 1; }
      else if (!strncmp(a, "--line-length=", 14)) line_wrap = atoi(a + 14);
      else if (!strcmp(a, "--expression") && i + 1 < argc) { add_script(argv[i + 1], strlen(argv[i + 1])); i++; have_script = 1; }
      else if (!strcmp(a, "--file") && i + 1 < argc) { add_script_file(argv[++i]); have_script = 1; }
      else if (!strcmp(a, "--version")) { puts("sed (UC.OS)"); return 0; }
      else if (!strcmp(a, "--help")) { usage(); }
      else { fprintf(stderr, "sed: unknown option -- '%s'\n", a + 2); usage(); }
      continue;
    }
    for (char *f = a + 1; *f; f++) {
      if (*f == 'n') quiet = 1;
      else if (*f == 'E' || *f == 'r') ere = 1;
      else if (*f == 's') separate = 1;
      else if (*f == 'u') unbuffered = 1;
      else if (*f == 'z') zero = 1;
      else if (*f == 'i') { inplace = 1; separate = 1; if (f[1]) suffix = f + 1; break; }
      else if (*f == 'e' || *f == 'f' || *f == 'l') {
        char *v = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : NULL);
        if (!v) { fprintf(stderr, "sed: option requires an argument -- '%c'\n", *f); usage(); }
        if (*f == 'e') { add_script(v, strlen(v)); have_script = 1; }
        else if (*f == 'f') { add_script_file(v); have_script = 1; }
        else line_wrap = atoi(v);
        break;
      } else { fprintf(stderr, "sed: invalid option -- '%c'\n", *f); usage(); }
    }
  }
  int first = 0;
  if (!have_script) {
    if (!nops) usage();
    add_script(ops[0], strlen(ops[0]));
    first = 1;
  }
  if (zero) eol = 0;
  stdout_out.f = stdout;
  cur_out = &stdout_out;
  setvbuf(stdout, NULL, _IOFBF, 65536);
  parse_script();
  static char *dash[] = {"-"};
  nops -= first;
  files = nops ? ops + first : dash;
  nfiles = nops ? nops : 1;
  if (inplace && !nops) { fprintf(stderr, "sed: no input files\n"); return 4; }
  bset(&ps, "", 0);
  bset(&hs, "", 0);
  if (!separate) { if (!next_file()) input_done = 1; }
  else if (!next_file()) input_done = 1;
  run();
  if (fflush(stdout) != 0) { fprintf(stderr, "sed: couldn't flush stdout: %s\n", strerror(errno)); return 4; }
  for (int k = 0; k < nouts; k++) if (outs[k].f && outs[k].f != stderr) fclose(outs[k].f);
  return exit_code;
}
