// SPDX-License-Identifier: GPL-2.0-or-later
/* grep [-EFivnclqrhHs] [-e pattern] pattern [file...]
 * Regular expressions are compiled to a small NFA program and run with
 * Thompson's simulation (no exponential backtracking). Basic syntax by
 * default (. * ^ $ [] \( \) \| \+ \?), extended with -E (+ ? | ()). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

enum { I_CHAR, I_ANY, I_CLASS, I_BOL, I_EOL, I_SPLIT, I_JMP, I_MATCH };
struct inst { int op, x, y; unsigned char *cls; };
struct prog { struct inst *code; int n, cap; };
static int ere, icase, fixed, invert, show_num, count_only, list_only, quiet, recursive, with_name = -1, no_errors;
static const char *re_src;
static int re_pos;

static int emit(struct prog *p, int op, int x, int y) {
  if (p->n == p->cap) p->code = realloc(p->code, (p->cap = p->cap ? p->cap * 2 : 64) * sizeof *p->code);
  p->code[p->n] = (struct inst){op, x, y, NULL};
  return p->n++;
}
/* move instructions [at, n) one slot up to make room for an instruction at `at` */
static void insert_at(struct prog *p, int at, int op, int x, int y) {
  emit(p, I_MATCH, 0, 0);
  memmove(p->code + at + 1, p->code + at, (p->n - 1 - at) * sizeof *p->code);
  p->code[at] = (struct inst){op, x, y, NULL};
}

/* tokens: returns a char, or one of these specials */
enum { T_END = 256, T_LP, T_RP, T_OR, T_STAR, T_PLUS, T_QUEST, T_DOT, T_LB, T_BOL, T_EOL, T_LIT };
static int lit_char;
static int peek_tok(int *len) {
  const char *s = re_src + re_pos;
  *len = 1;
  if (!*s) return T_END;
  if (*s == '\\' && s[1]) {
    *len = 2;
    if (!ere) switch (s[1]) { case '(': return T_LP; case ')': return T_RP; case '|': return T_OR; case '+': return T_PLUS; case '?': return T_QUEST; }
    lit_char = s[1] == 'n' ? '\n' : s[1] == 't' ? '\t' : s[1];
    return T_LIT;
  }
  switch (*s) {
  case '.': return T_DOT;
  case '[': return T_LB;
  case '*': return T_STAR;
  case '^': return T_BOL;
  case '$': return T_EOL;
  }
  if (ere) switch (*s) { case '(': return T_LP; case ')': return T_RP; case '|': return T_OR; case '+': return T_PLUS; case '?': return T_QUEST; }
  lit_char = (unsigned char)*s;
  return T_LIT;
}
static void add_char(unsigned char *cls, int c) {
  cls[c] = 1;
  if (icase) { cls[tolower(c)] = 1; cls[toupper(c)] = 1; }
}
static unsigned char *parse_class(void) {
  unsigned char *cls = calloc(256, 1);
  const char *s = re_src + re_pos + 1;
  int neg = 0;
  if (*s == '^') neg = 1, s++;
  const char *start = s;
  while (*s && (*s != ']' || s == start)) {
    if (*s == '[' && s[1] == ':') { /* [:alpha:] and friends */
      const char *e = strstr(s, ":]");
      if (e) {
        char name[16] = {0};
        memcpy(name, s + 2, e - s - 2 < 15 ? e - s - 2 : 15);
        for (int c = 0; c < 256; c++) {
          int in = !strcmp(name, "alpha") ? isalpha(c) : !strcmp(name, "digit") ? isdigit(c) : !strcmp(name, "alnum") ? isalnum(c) :
                   !strcmp(name, "space") ? isspace(c) : !strcmp(name, "upper") ? isupper(c) : !strcmp(name, "lower") ? islower(c) :
                   !strcmp(name, "punct") ? ispunct(c) : !strcmp(name, "xdigit") ? isxdigit(c) : 0;
          if (in) add_char(cls, c);
        }
        s = e + 2;
        continue;
      }
    }
    unsigned char lo = *s, hi = lo;
    if (s[1] == '-' && s[2] && s[2] != ']') { hi = s[2]; s += 2; }
    for (int c = lo; c <= hi; c++) add_char(cls, c);
    s++;
  }
  if (*s) s++;
  re_pos = s - re_src;
  if (neg) for (int c = 0; c < 256; c++) cls[c] = !cls[c];
  cls['\n'] = 0;
  return cls;
}

static void parse_alt(struct prog *p);
static void parse_seq(struct prog *p) {
  int at_start = 1;
  for (;;) {
    int len, t = peek_tok(&len);
    if (t == T_END || t == T_OR || t == T_RP) return;
    int atom = p->n;
    if (t == T_STAR && at_start && !ere) { t = T_LIT; lit_char = '*'; }
    switch (t) {
    case T_LP: re_pos += len; parse_alt(p); if (peek_tok(&len) == T_RP) re_pos += len; break;
    case T_DOT: re_pos += len; emit(p, I_ANY, 0, 0); break;
    case T_LB: { unsigned char *c = parse_class(); int k = emit(p, I_CLASS, 0, 0); p->code[k].cls = c; break; }
    case T_BOL:
      re_pos += len;
      if (at_start || ere) emit(p, I_BOL, 0, 0); else emit(p, I_CHAR, '^', 0);
      break;
    case T_EOL: {
      re_pos += len;
      int l2, nx = peek_tok(&l2);
      if (ere || nx == T_END || nx == T_OR || nx == T_RP) emit(p, I_EOL, 0, 0); else emit(p, I_CHAR, '$', 0);
      break;
    }
    case T_STAR: case T_PLUS: case T_QUEST: re_pos += len; emit(p, I_CHAR, re_src[re_pos - 1], 0); break; /* nothing to repeat */
    default: re_pos += len; emit(p, I_CHAR, lit_char, 0);
    }
    at_start = 0;
    int alen = p->n - atom;
    for (;;) { /* quantifiers, relative jumps */
      t = peek_tok(&len);
      if (t == T_STAR) { re_pos += len; insert_at(p, atom, I_SPLIT, 1, alen + 2); emit(p, I_JMP, -(alen + 1), 0); }
      else if (t == T_PLUS) { re_pos += len; emit(p, I_SPLIT, -alen, 1); }
      else if (t == T_QUEST) { re_pos += len; insert_at(p, atom, I_SPLIT, 1, alen + 1); }
      else break;
      alen = p->n - atom;
    }
  }
}
static void parse_alt(struct prog *p) {
  int start = p->n;
  parse_seq(p);
  int len;
  while (peek_tok(&len) == T_OR) {
    re_pos += len;
    int l1 = p->n - start;
    insert_at(p, start, I_SPLIT, 1, l1 + 2);
    int j = emit(p, I_JMP, 0, 0);
    parse_seq(p);
    p->code[j].x = p->n - j;
  }
}

/* Thompson simulation */
static int *gen, cur_gen;
static void addthread(struct prog *p, int *list, int *n, int pc, const char *line, const char *s) {
  if (gen[pc] == cur_gen) return;
  gen[pc] = cur_gen;
  struct inst *in = &p->code[pc];
  switch (in->op) {
  case I_JMP: addthread(p, list, n, pc + in->x, line, s); return;
  case I_SPLIT: addthread(p, list, n, pc + in->x, line, s); addthread(p, list, n, pc + in->y, line, s); return;
  case I_BOL: if (s == line) addthread(p, list, n, pc + 1, line, s); return;
  case I_EOL: if (!*s) addthread(p, list, n, pc + 1, line, s); return;
  }
  list[(*n)++] = pc;
}
static int *clist, *nlist;
static int re_match(struct prog *p, const char *line) {
  int nc = 0;
  cur_gen++;
  addthread(p, clist, &nc, 0, line, line);
  for (const char *s = line;; s++) {
    unsigned char c = *s;
    int nn = 0;
    cur_gen++;
    for (int i = 0; i < nc; i++) {
      struct inst *in = &p->code[clist[i]];
      int ok = 0;
      switch (in->op) {
      case I_MATCH: return 1;
      case I_CHAR: ok = c && (icase ? tolower(c) == tolower(in->x) : c == in->x); break;
      case I_ANY: ok = c != 0; break;
      case I_CLASS: ok = c && in->cls[c]; break;
      }
      if (ok) addthread(p, nlist, &nn, clist[i] + 1, line, s + 1);
    }
    if (!c) return 0;
    addthread(p, nlist, &nn, 0, line, s + 1); /* a match may start at any position */
    int *t = clist; clist = nlist; nlist = t;
    nc = nn;
  }
}

static struct prog progs[16];
static char *pats[16];
static int npats;
static int matches(const char *line) {
  for (int k = 0; k < npats; k++) {
    if (fixed) {
      if (icase) {
        size_t pl = strlen(pats[k]);
        for (const char *s = line; *s || !pl; s++) { if (!strncasecmp(s, pats[k], pl)) return 1; if (!*s) break; }
      } else if (strstr(line, pats[k])) return 1;
    } else if (re_match(&progs[k], line)) return 1;
  }
  return 0;
}

static int any_match;
static int grep_fd(int fd, const char *name) {
  size_t cap = 4096, len = 0;
  char *line = malloc(cap);
  long lineno = 0, count = 0;
  static char buf[65536];
  long r, pos = 0, have = 0;
  int done = 0;
  while (!done) {
    if (pos >= have) { have = read(fd, buf, sizeof buf); pos = 0; if (have <= 0) { if (!len) break; done = 1; } }
    int eol = done;
    while (!eol && pos < have) {
      char c = buf[pos++];
      if (c == '\n') { eol = 1; break; }
      if (len + 2 >= cap) line = realloc(line, cap *= 2);
      line[len++] = c;
    }
    if (!eol) continue;
    line[len] = 0;
    lineno++;
    if (matches(line) != invert) {
      count++;
      any_match = 1;
      if (quiet) { free(line); return 1; }
      if (list_only) { printf("%s\n", name); break; }
      if (!count_only) {
        if (with_name) printf("%s:", name);
        if (show_num) printf("%ld:", lineno);
        fwrite(line, 1, len, stdout);
        putchar('\n');
        if (ferror(stdout)) exit(2);
      }
    }
    len = 0;
  }
  (void)r;
  if (count_only) { if (with_name) printf("%s:", name); printf("%ld\n", count); }
  free(line);
  return count > 0;
}
static int grep_path(const char *path, int top) {
  struct stat st;
  if (stat(path, &st) != 0) { if (!no_errors) fprintf(stderr, "grep: %s: %s\n", path, strerror(errno)); return -1; }
  if (S_ISDIR(st.st_mode)) {
    if (!recursive) { if (!no_errors) fprintf(stderr, "grep: %s: Is a directory\n", path); return -1; }
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char *sub = malloc(strlen(path) + strlen(e->d_name) + 2);
      sprintf(sub, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e->d_name);
      grep_path(sub, 0);
      free(sub);
    }
    closedir(d);
    return 0;
  }
  int fd = open(path, O_RDONLY);
  if (fd < 0) { if (!no_errors) fprintf(stderr, "grep: %s: %s\n", path, strerror(errno)); return -1; }
  grep_fd(fd, path);
  close(fd);
  (void)top;
  return 0;
}

int main(int argc, char **argv) {
  int i = 1, err = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "--")) { i++; break; }
    for (char *f = argv[i] + 1; *f; f++) {
      switch (*f) {
      case 'E': ere = 1; break;
      case 'G': ere = 0; break;
      case 'F': fixed = 1; break;
      case 'i': case 'y': icase = 1; break;
      case 'v': invert = 1; break;
      case 'n': show_num = 1; break;
      case 'c': count_only = 1; break;
      case 'l': list_only = 1; break;
      case 'q': quiet = 1; break;
      case 's': no_errors = 1; break;
      case 'r': case 'R': recursive = 1; break;
      case 'h': with_name = 0; break;
      case 'H': with_name = 1; break;
      case 'e':
        if (npats < 16) pats[npats++] = f[1] ? f + 1 : (i + 1 < argc ? argv[++i] : "");
        f += strlen(f) - 1;
        break;
      default: fprintf(stderr, "usage: grep [-EFivnclqrhHs] [-e pattern] pattern [file...]\n"); return 2;
      }
    }
  }
  if (!npats) {
    if (i >= argc) { fprintf(stderr, "usage: grep [-EFivnclqrhHs] [-e pattern] pattern [file...]\n"); return 2; }
    pats[npats++] = argv[i++];
  }
  if (!fixed)
    for (int k = 0; k < npats; k++) {
      re_src = pats[k]; re_pos = 0;
      parse_alt(&progs[k]);
      emit(&progs[k], I_MATCH, 0, 0);
      if (re_src[re_pos]) { fprintf(stderr, "grep: unmatched ) in pattern\n"); return 2; }
    }
  int maxn = 0;
  for (int k = 0; k < npats; k++) if (progs[k].n > maxn) maxn = progs[k].n;
  gen = calloc(maxn + 1, sizeof *gen);
  clist = malloc((maxn + 1) * sizeof *clist);
  nlist = malloc((maxn + 1) * sizeof *nlist);
  if (!isatty(1)) setvbuf(stdout, 0, _IOFBF, 0);
  int nfiles = argc - i;
  if (with_name < 0) with_name = nfiles > 1 || recursive;
  if (!nfiles) {
    if (recursive) err |= grep_path(".", 1) < 0;
    else grep_fd(0, "(standard input)");
  }
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "-")) grep_fd(0, "(standard input)");
    else err |= grep_path(argv[i], 1) < 0;
  }
  fflush(stdout);
  return err && !quiet ? 2 : any_match ? 0 : 1;
}
