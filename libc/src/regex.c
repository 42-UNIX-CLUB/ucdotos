// SPDX-License-Identifier: GPL-2.0-or-later
/* POSIX regular expressions (regcomp, regexec, regerror, regfree).
 *
 * The pattern is parsed into a tree, then compiled to a small program for a
 * virtual machine (CHAR, SET, SPLIT, JMP, SAVE, assertions, MATCH).
 *
 * Two matchers run that program:
 * - Without back-references: a Pike VM (Thompson's simulation that carries
 *   submatch positions per thread). Time is O(program size x text length),
 *   never exponential. Among matches it keeps the leftmost start, then the
 *   longest end; between paths with the same end it keeps the one that
 *   prefers more repetitions and earlier alternatives.
 * - With back-references (which no automaton can match): a depth-first
 *   backtracking search that tries every path from the leftmost start and
 *   keeps the longest. It can take exponential time on patterns such as
 *   \(a*\)*\1, so it stops after a step budget and regexec reports REG_ESPACE.
 *
 * Counted repetitions {m,n} are expanded into copies, capped at 65536
 * instructions. Bytes, not characters: there is no locale. */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef __wasm__
#include <sys/syscall.h>
#endif

enum { O_CHAR, O_ANY, O_ANYNL, O_SET, O_BOL, O_EOL, O_WORDB, O_NWORDB, O_WBEG, O_WEND, O_BUFBEG, O_BUFEND,
       O_BACKREF, O_SPLIT, O_JMP, O_SAVE, O_MARK, O_NONEMPTY, O_MATCH };
struct inst { unsigned char op; int x, y; };
struct prog {
  struct inst *code;
  int n, cap;
  unsigned char (*sets)[32];
  int nsets, setcap;
  int nsub, nloops, backrefs, icase, anchored, newline;
  unsigned char first[256]; /* bytes that can begin a match; all ones if unknown */
  int first_any;
};

/* ---- parse tree ---- */
enum { N_EMPTY, N_CHAR, N_ANY, N_SET, N_ASSERT, N_BACKREF, N_CAT, N_ALT, N_GROUP, N_REPEAT };
struct node { int type, a, b, min, max; }; /* CAT/ALT: a, b children. GROUP: a = child, min = index. REPEAT: a child */
struct parser {
  const unsigned char *s;
  int pos, flags, ere, err, nsub, depth;
  struct node *nodes;
  int nn, ncap;
  struct prog *p;
  int open_groups[256], nopen; /* groups not yet closed: a back-reference to them is invalid */
  int closed[10];
};

static int newnode(struct parser *ps, int type, int a, int b) {
  if (ps->err) return 0;
  if (ps->nn == ps->ncap) {
    int nc = ps->ncap ? ps->ncap * 2 : 64;
    struct node *n = realloc(ps->nodes, nc * sizeof *n);
    if (!n) { ps->err = REG_ESPACE; return 0; }
    ps->nodes = n; ps->ncap = nc;
  }
  ps->nodes[ps->nn] = (struct node){type, a, b, 0, 0};
  return ps->nn++;
}
static int newset(struct parser *ps) {
  struct prog *p = ps->p;
  if (p->nsets == p->setcap) {
    int nc = p->setcap ? p->setcap * 2 : 8;
    void *n = realloc(p->sets, nc * 32);
    if (!n) { ps->err = REG_ESPACE; return 0; }
    p->sets = n; p->setcap = nc;
  }
  memset(p->sets[p->nsets], 0, 32);
  return p->nsets++;
}
#define SETHAS(set, c) ((set)[(c) >> 3] & (1 << ((c) & 7)))
#define SETADD(set, c) ((set)[(c) >> 3] |= (unsigned char)(1 << ((c) & 7)))

static int peekc(struct parser *ps) { return ps->s[ps->pos]; }

static int class_has(const char *name, int c) {
  if (!strcmp(name, "alpha")) return isalpha(c);
  if (!strcmp(name, "digit")) return isdigit(c);
  if (!strcmp(name, "alnum")) return isalnum(c);
  if (!strcmp(name, "upper")) return isupper(c);
  if (!strcmp(name, "lower")) return islower(c);
  if (!strcmp(name, "space")) return isspace(c);
  if (!strcmp(name, "blank")) return c == ' ' || c == '\t';
  if (!strcmp(name, "punct")) return ispunct(c);
  if (!strcmp(name, "print")) return isprint(c);
  if (!strcmp(name, "graph")) return isgraph(c);
  if (!strcmp(name, "cntrl")) return iscntrl(c);
  if (!strcmp(name, "xdigit")) return isxdigit(c);
  return -1;
}
static int valid_class(const char *name) { return class_has(name, 'a') >= 0; }

static void set_add(struct parser *ps, unsigned char *set, int c) {
  SETADD(set, c);
  if (ps->flags & REG_ICASE) { SETADD(set, tolower(c)); SETADD(set, toupper(c)); }
}

/* one element of a bracket expression: returns a byte, or -2 for a class (already added), -1 on error */
static int bracket_elem(struct parser *ps, unsigned char *set, int *is_class) {
  const unsigned char *s = ps->s;
  *is_class = 0;
  if (s[ps->pos] == '[' && (s[ps->pos + 1] == ':' || s[ps->pos + 1] == '=' || s[ps->pos + 1] == '.')) {
    int kind = s[ps->pos + 1], start = ps->pos + 2, e = start;
    while (s[e] && !(s[e] == kind && s[e + 1] == ']')) e++;
    if (!s[e]) { ps->err = REG_EBRACK; return -1; }
    int len = e - start;
    ps->pos = e + 2;
    if (kind == ':') {
      char name[16];
      if (len >= (int)sizeof name) { ps->err = REG_ECTYPE; return -1; }
      memcpy(name, s + start, len); name[len] = 0;
      if (!valid_class(name)) { ps->err = REG_ECTYPE; return -1; }
      for (int c = 0; c < 256; c++) {
        if (class_has(name, c)) set_add(ps, set, c);
      }
      if (ps->flags & REG_ICASE && (!strcmp(name, "upper") || !strcmp(name, "lower")))
        for (int c = 'a'; c <= 'z'; c++) { SETADD(set, c); SETADD(set, c - 32); }
      *is_class = 1;
      return -2;
    }
    if (len != 1) { ps->err = REG_ECOLLATE; return -1; } /* only single-byte collating elements */
    if (kind == '=') { set_add(ps, set, s[start]); *is_class = 1; return -2; }
    return s[start];
  }
  if ((ps->flags & REG_UC_AWK) && s[ps->pos] == '\\' && s[ps->pos + 1]) { ps->pos += 2; return s[ps->pos - 1]; }
  return s[ps->pos++];
}

static int parse_bracket(struct parser *ps) {
  int k = newset(ps);
  if (ps->err) return 0;
  unsigned char set[32] = {0};
  int neg = 0;
  if (peekc(ps) == '^') { neg = 1; ps->pos++; }
  int first = 1;
  for (;;) {
    int c = peekc(ps);
    if (!c) { ps->err = REG_EBRACK; return 0; }
    if (c == ']' && !first) { ps->pos++; break; }
    first = 0;
    int cls, lo = bracket_elem(ps, set, &cls);
    if (ps->err) return 0;
    if (cls) {
      if (peekc(ps) == '-' && ps->s[ps->pos + 1] != ']' && ps->s[ps->pos + 1]) { ps->err = REG_ERANGE; return 0; }
      continue;
    }
    if (peekc(ps) == '-' && ps->s[ps->pos + 1] && ps->s[ps->pos + 1] != ']') {
      ps->pos++;
      int hi = bracket_elem(ps, set, &cls);
      if (ps->err) return 0;
      if (cls || hi < lo) { ps->err = REG_ERANGE; return 0; }
      for (int x = lo; x <= hi; x++) set_add(ps, set, x);
    } else set_add(ps, set, lo);
  }
  if (neg) {
    for (int i = 0; i < 32; i++) set[i] = ~set[i];
    if (ps->flags & REG_NEWLINE) set['\n' >> 3] &= ~(1 << ('\n' & 7));
  }
  memcpy(ps->p->sets[k], set, 32);
  int n = newnode(ps, N_SET, k, 0);
  return n;
}

static int lit(struct parser *ps, int c) {
  if ((ps->flags & REG_ICASE) && isalpha(c)) {
    int k = newset(ps);
    if (ps->err) return 0;
    SETADD(ps->p->sets[k], tolower(c));
    SETADD(ps->p->sets[k], toupper(c));
    return newnode(ps, N_SET, k, 0);
  }
  return newnode(ps, N_CHAR, c, 0);
}
static int wordset(struct parser *ps, int neg, int space) {
  int k = newset(ps);
  if (ps->err) return 0;
  for (int c = 0; c < 256; c++) {
    int in = space ? !!isspace(c) : (isalnum(c) || c == '_');
    if (in != neg) SETADD(ps->p->sets[k], c);
  }
  return newnode(ps, N_SET, k, 0);
}
static int cat(struct parser *ps, int a, int b) {
  if (a < 0) return b;
  return newnode(ps, N_CAT, a, b);
}

static int parse_alt(struct parser *ps);

/* {m,n} after the opening brace; returns 0 if the text is not an interval (ERE: then it is literal) */
static int parse_interval(struct parser *ps, int *min, int *max) {
  const unsigned char *s = ps->s;
  int p = ps->pos, m = -1, n;
  if (isdigit(s[p])) { m = 0; while (isdigit(s[p])) { m = m * 10 + s[p++] - '0'; if (m > RE_DUP_MAX) m = RE_DUP_MAX + 1; } }
  if (s[p] == ',') {
    p++;
    n = -1;
    if (isdigit(s[p])) { n = 0; while (isdigit(s[p])) { n = n * 10 + s[p++] - '0'; if (n > RE_DUP_MAX) n = RE_DUP_MAX + 1; } }
    if (m < 0) m = 0;
  } else n = m;
  if (ps->ere) { if (s[p] != '}') return 0; p++; }
  else { if (s[p] != '\\' || s[p + 1] != '}') { ps->err = s[p] ? REG_BADBR : REG_EBRACE; return 0; } p += 2; }
  if (m < 0) { if (ps->ere) return 0; ps->err = REG_BADBR; return 0; }
  if (m > RE_DUP_MAX || n > RE_DUP_MAX || (n >= 0 && n < m)) { ps->err = REG_BADBR; return 0; }
  ps->pos = p;
  *min = m; *max = n;
  return 1;
}

/* a quantifier at the current position? Returns 1 and the bounds. */
static int quantifier(struct parser *ps, int *min, int *max) {
  const unsigned char *s = ps->s + ps->pos;
  if (*s == '*') { ps->pos++; *min = 0; *max = -1; return 1; }
  if (ps->ere) {
    if (*s == '+') { ps->pos++; *min = 1; *max = -1; return 1; }
    if (*s == '?') { ps->pos++; *min = 0; *max = 1; return 1; }
    if (*s == '{') {
      int save = ps->pos;
      ps->pos++;
      if (parse_interval(ps, min, max)) return 1;
      ps->pos = save;
      return 0;
    }
    return 0;
  }
  if (*s == '\\') {
    if (s[1] == '+') { ps->pos += 2; *min = 1; *max = -1; return 1; }
    if (s[1] == '?') { ps->pos += 2; *min = 0; *max = 1; return 1; }
    if (s[1] == '{') { ps->pos += 2; if (parse_interval(ps, min, max)) return 1; if (!ps->err) ps->err = REG_BADBR; return 0; }
  }
  return 0;
}

/* end of a concatenation? */
static int at_cat_end(struct parser *ps) {
  const unsigned char *s = ps->s + ps->pos;
  if (!*s) return 1;
  if (ps->ere) return *s == '|' || (*s == ')' && ps->depth > 0);
  return *s == '\\' && (s[1] == '|' || (s[1] == ')' && ps->depth > 0));
}

static int parse_cat(struct parser *ps) {
  int seq = -1, start = 1;
  while (!ps->err && !at_cat_end(ps)) {
    const unsigned char *s = ps->s + ps->pos;
    int atom = -1, c = *s;
    if (ps->ere && c == ')' && ps->depth == 0) { ps->pos++; atom = lit(ps, ')'); }
    else if (c == '[') { ps->pos++; atom = parse_bracket(ps); }
    else if (c == '.') { ps->pos++; atom = newnode(ps, N_ANY, 0, 0); }
    else if (c == '^' && (ps->ere || start)) { ps->pos++; atom = newnode(ps, N_ASSERT, O_BOL, 0); seq = cat(ps, seq, atom); continue; }
    else if (c == '$') {
      ps->pos++;
      if (ps->ere || at_cat_end(ps)) atom = newnode(ps, N_ASSERT, O_EOL, 0);
      else atom = lit(ps, '$');
    } else if (ps->ere && (c == '*' || c == '+' || c == '?' || c == '{')) { ps->err = REG_BADRPT; break; } /* nothing to repeat */
    else if (c == '*' && start) { ps->pos++; atom = lit(ps, '*'); }
    else if (ps->ere && c == '(') {
      ps->pos++;
      int idx = ++ps->nsub;
      if (ps->nopen < 256) ps->open_groups[ps->nopen++] = idx;
      ps->depth++;
      int inner = parse_alt(ps);
      ps->depth--;
      if (ps->err) break;
      if (peekc(ps) != ')') { ps->err = REG_EPAREN; break; }
      ps->pos++;
      ps->nopen--;
      if (idx < 10) ps->closed[idx] = 1;
      atom = newnode(ps, N_GROUP, inner, 0);
      if (!ps->err) ps->nodes[atom].min = idx;
    } else if (c == '\\') {
      int d = s[1];
      if (!d) { ps->err = REG_EESCAPE; break; }
      ps->pos += 2;
      if (!ps->ere && d == '(') {
        int idx = ++ps->nsub;
        if (ps->nopen < 256) ps->open_groups[ps->nopen++] = idx;
        ps->depth++;
        int inner = parse_alt(ps);
        ps->depth--;
        if (ps->err) break;
        if (peekc(ps) != '\\' || ps->s[ps->pos + 1] != ')') { ps->err = REG_EPAREN; break; }
        ps->pos += 2;
        ps->nopen--;
        if (idx < 10) ps->closed[idx] = 1;
        atom = newnode(ps, N_GROUP, inner, 0);
        if (!ps->err) ps->nodes[atom].min = idx;
      } else if (!ps->ere && d == ')') { ps->err = REG_EPAREN; break; }
      else if (!ps->ere && d == '{') { if (start) { atom = lit(ps, '{'); } else { ps->err = REG_BADRPT; break; } }
      else if (!ps->ere && d == '}') atom = lit(ps, '}');
      else if (d >= '1' && d <= '9' && !(ps->flags & REG_UC_NOBACKREF)) {
        int n = d - '0';
        if (n > ps->nsub || !ps->closed[n]) { ps->err = REG_ESUBREG; break; }
        atom = newnode(ps, N_BACKREF, n, 0);
        ps->p->backrefs = 1;
      } else if (d == 'w' || d == 'W') atom = wordset(ps, d == 'W', 0);
      else if (d == 's' || d == 'S') atom = wordset(ps, d == 'S', 1);
      else if (d == 'b') atom = newnode(ps, N_ASSERT, O_WORDB, 0);
      else if (d == 'B') atom = newnode(ps, N_ASSERT, O_NWORDB, 0);
      else if (d == '<') atom = newnode(ps, N_ASSERT, O_WBEG, 0);
      else if (d == '>') atom = newnode(ps, N_ASSERT, O_WEND, 0);
      else if (d == '`') atom = newnode(ps, N_ASSERT, O_BUFBEG, 0);
      else if (d == '\'') atom = newnode(ps, N_ASSERT, O_BUFEND, 0);
      else atom = lit(ps, d);
    } else { ps->pos++; atom = lit(ps, c); }
    if (ps->err) break;
    start = 0;
    int min, max;
    while (!ps->err && quantifier(ps, &min, &max)) {
      int r = newnode(ps, N_REPEAT, atom, 0);
      if (ps->err) break;
      ps->nodes[r].min = min; ps->nodes[r].max = max;
      atom = r;
    }
    seq = cat(ps, seq, atom);
  }
  if (seq < 0) seq = newnode(ps, N_EMPTY, 0, 0);
  return seq;
}

static int parse_alt(struct parser *ps) {
  if (ps->depth > 200) { ps->err = REG_ESPACE; return 0; }
  int left = parse_cat(ps);
  for (;;) {
    if (ps->err) return left;
    const unsigned char *s = ps->s + ps->pos;
    if (ps->ere && *s == '|') ps->pos++;
    else if (!ps->ere && s[0] == '\\' && s[1] == '|') ps->pos += 2;
    else return left;
    int right = parse_cat(ps);
    left = newnode(ps, N_ALT, left, right);
  }
}

/* ---- code generation ---- */
static int nullable(struct parser *ps, int ni) {
  struct node *n = &ps->nodes[ni];
  switch (n->type) {
  case N_CHAR: case N_ANY: case N_SET: return 0;
  case N_CAT: return nullable(ps, n->a) && nullable(ps, n->b);
  case N_ALT: return nullable(ps, n->a) || nullable(ps, n->b);
  case N_GROUP: return nullable(ps, n->a);
  case N_REPEAT: return n->min == 0 || nullable(ps, n->a);
  }
  return 1; /* empty, assertions, back-references */
}
#define MAXPROG 65536
static int emit(struct parser *ps, int op, int x, int y) {
  struct prog *p = ps->p;
  if (ps->err) return 0;
  if (p->n >= MAXPROG) { ps->err = REG_ESPACE; return 0; }
  if (p->n == p->cap) {
    int nc = p->cap ? p->cap * 2 : 64;
    struct inst *c = realloc(p->code, nc * sizeof *c);
    if (!c) { ps->err = REG_ESPACE; return 0; }
    p->code = c; p->cap = nc;
  }
  p->code[p->n] = (struct inst){(unsigned char)op, x, y};
  return p->n++;
}
static void gen(struct parser *ps, int ni, int depth) {
  if (ps->err) return;
  if (depth > 400) { ps->err = REG_ESPACE; return; }
  struct node *nd = &ps->nodes[ni];
  struct node n = *nd;
  switch (n.type) {
  case N_EMPTY: break;
  case N_CHAR: emit(ps, O_CHAR, n.a, 0); break;
  case N_ANY: emit(ps, (ps->flags & REG_NEWLINE) ? O_ANYNL : O_ANY, 0, 0); break;
  case N_SET: emit(ps, O_SET, n.a, 0); break;
  case N_ASSERT: emit(ps, n.a, 0, 0); break;
  case N_BACKREF: emit(ps, O_BACKREF, n.a, 0); break;
  case N_CAT: {
    /* concatenations are left-deep chains: collect them without recursing on the left */
    int len = 0, cur = ni;
    while (ps->nodes[cur].type == N_CAT) { len++; cur = ps->nodes[cur].a; }
    int *right = malloc((len + 1) * sizeof *right), k = len;
    if (!right) { ps->err = REG_ESPACE; return; }
    for (cur = ni; ps->nodes[cur].type == N_CAT; cur = ps->nodes[cur].a) right[--k] = ps->nodes[cur].b;
    gen(ps, cur, depth + 1);
    for (k = 0; k < len && !ps->err; k++) gen(ps, right[k], depth + 1);
    free(right);
    break;
  }
  case N_ALT: {
    int sp = emit(ps, O_SPLIT, 0, 0);
    gen(ps, n.a, depth + 1);
    int j = emit(ps, O_JMP, 0, 0);
    if (ps->err) return;
    ps->p->code[sp].x = sp + 1;
    ps->p->code[sp].y = ps->p->n;
    gen(ps, n.b, depth + 1);
    if (ps->err) return;
    ps->p->code[j].x = ps->p->n;
    break;
  }
  case N_GROUP:
    emit(ps, O_SAVE, 2 * n.min, 0);
    gen(ps, n.a, depth + 1);
    emit(ps, O_SAVE, 2 * n.min + 1, 0);
    break;
  case N_REPEAT: {
    for (int i = 0; i < n.min && !ps->err; i++) gen(ps, n.a, depth + 1);
    if (n.max < 0 && !nullable(ps, n.a)) {
      /* every iteration consumes input: a plain loop */
      int l1 = emit(ps, O_SPLIT, 0, 0);
      gen(ps, n.a, depth + 1);
      emit(ps, O_JMP, l1, 0);
      if (ps->err) return;
      ps->p->code[l1].x = l1 + 1;
      ps->p->code[l1].y = ps->p->n;
    } else if (n.max < 0) {
      /* the body can match the empty string. As in glibc, only a first iteration may
         be empty: it still sets its captures. Later iterations must make progress
         (MARK remembers where an iteration began, NONEMPTY checks it), which also
         keeps the backtracker out of endless empty loops. */
      int skip = -1, reg = ps->p->nloops++;
      if (n.min == 0) { skip = emit(ps, O_SPLIT, 0, 0); gen(ps, n.a, depth + 1); }
      int l = emit(ps, O_SPLIT, 0, 0);
      emit(ps, O_MARK, reg, 0);
      gen(ps, n.a, depth + 1);
      emit(ps, O_NONEMPTY, reg, 0);
      emit(ps, O_JMP, l, 0);
      if (ps->err) return;
      ps->p->code[l].x = l + 1;
      ps->p->code[l].y = ps->p->n;
      if (skip >= 0) { ps->p->code[skip].x = skip + 1; ps->p->code[skip].y = ps->p->n; }
    } else {
      int opt = n.max - n.min, *splits = malloc((opt + 1) * sizeof *splits);
      if (!splits) { ps->err = REG_ESPACE; return; }
      for (int i = 0; i < opt && !ps->err; i++) {
        splits[i] = emit(ps, O_SPLIT, 0, 0);
        gen(ps, n.a, depth + 1);
      }
      if (!ps->err) for (int i = 0; i < opt; i++) { ps->p->code[splits[i]].x = splits[i] + 1; ps->p->code[splits[i]].y = ps->p->n; }
      free(splits);
    }
    break;
  }
  }
}

/* bytes that can start a match: walk the epsilon closure of the start */
static void compute_first(struct prog *p) {
  unsigned char *seen = calloc(p->n, 1);
  int *stack = malloc(p->n * 2 * sizeof *stack), sp = 0;
  memset(p->first, 0, 256);
  p->first_any = 0;
  if (!seen || !stack) { p->first_any = 1; free(seen); free(stack); return; }
  stack[sp++] = 0;
  while (sp && !p->first_any) {
    int pc = stack[--sp];
    if (seen[pc]) continue;
    seen[pc] = 1;
    struct inst *in = &p->code[pc];
    switch (in->op) {
    case O_CHAR: p->first[in->x] = 1; break;
    case O_SET: for (int c = 0; c < 256; c++) if (SETHAS(p->sets[in->x], c)) p->first[c] = 1; break;
    case O_SPLIT: stack[sp++] = in->y; stack[sp++] = in->x; break;
    case O_JMP: stack[sp++] = in->x; break;
    case O_ANY: case O_ANYNL: case O_MATCH: case O_BACKREF: p->first_any = 1; break;
    default: stack[sp++] = pc + 1; break; /* SAVE, MARK, NONEMPTY, assertions */
    }
  }
  free(seen);
  free(stack);
}

int regcomp(regex_t *re, const char *pattern, int flags) {
  struct prog *p = calloc(1, sizeof *p);
  if (!p) return REG_ESPACE;
  struct parser ps = {0};
  ps.s = (const unsigned char *)pattern;
  ps.flags = flags;
  ps.ere = flags & REG_EXTENDED;
  ps.p = p;
  p->icase = flags & REG_ICASE;
  p->newline = (flags & REG_NEWLINE) != 0;
  int root = parse_alt(&ps);
  if (!ps.err && ps.s[ps.pos]) ps.err = REG_EPAREN;
  if (!ps.err) {
    emit(&ps, O_SAVE, 0, 0);
    gen(&ps, root, 0);
    emit(&ps, O_SAVE, 1, 0);
    emit(&ps, O_MATCH, 0, 0);
  }
  free(ps.nodes);
  if (ps.err) {
    free(p->code); free(p->sets); free(p);
    re->__impl = 0;
    return ps.err;
  }
  p->nsub = ps.nsub;
  p->anchored = p->code[1].op == O_BOL && !(flags & REG_NEWLINE);
  compute_first(p);
  re->re_nsub = ps.nsub;
  re->__impl = p;
  return 0;
}

void regfree(regex_t *re) {
  struct prog *p = re->__impl;
  if (!p) return;
  free(p->code); free(p->sets); free(p);
  re->__impl = 0;
}

static const char *const messages[] = {
  "Success", "No match", "Invalid regular expression", "Invalid collation character",
  "Invalid character class name", "Trailing backslash", "Invalid back reference",
  "Unmatched [, [^, [:, [., or [=", "Unmatched ( or \\(", "Unmatched \\{",
  "Invalid content of \\{\\}", "Invalid range end", "Memory exhausted or expression too big",
  "Invalid preceding regular expression",
};
size_t regerror(int err, const regex_t *re, char *buf, size_t size) {
  (void)re;
  const char *m = err >= 0 && err <= REG_BADRPT ? messages[err] : "Unknown error";
  size_t n = strlen(m) + 1;
  if (size) { size_t k = n < size ? n : size; memcpy(buf, m, k - 1); buf[k - 1] = 0; }
  return n;
}

/* ---- matching ---- */
struct subject {
  const unsigned char *s;
  int base, end; /* ^ matches at base; text exists in [0, end) */
  int notbol, noteol, newline;
};
static int isword(int c) { return isalnum(c) || c == '_'; }
static int assert_ok(const struct subject *sj, int op, int pos) {
  const unsigned char *s = sj->s;
  int before = pos > sj->base ? s[pos - 1] : -1; /* REG_STARTEND: nothing exists before the subject */
  int after = pos < sj->end ? s[pos] : -1;
  int wb = before >= 0 && isword(before), wa = after >= 0 && isword(after);
  switch (op) {
  case O_BOL: return (pos == sj->base && !sj->notbol) || (sj->newline && pos > sj->base && s[pos - 1] == '\n');
  case O_EOL: return (pos == sj->end && !sj->noteol) || (sj->newline && pos < sj->end && s[pos] == '\n');
  case O_WORDB: return wb != wa;
  case O_NWORDB: return wb == wa;
  case O_WBEG: return !wb && wa;
  case O_WEND: return wb && !wa;
  case O_BUFBEG: return pos == sj->base;
  case O_BUFEND: return pos == sj->end;
  }
  return 0;
}

static long re_steps;
static void breathe(void) {
#ifdef __wasm__
  /* a long match would otherwise make no system call and freeze the browser tab */
  if ((++re_steps & 0x3ffff) == 0) __syscall0(SYS_sched_yield);
#else
  ++re_steps;
#endif
}

struct tlist { int n; int *pc; int *caps; };
struct pike {
  struct prog *p;
  const struct subject *sj;
  int ncap, capn; /* thread vector: capn capture slots, then one register per MARK */
  unsigned *mark;
  unsigned gen;
  int *stack; /* pairs: pc (>=0) or -1-slot, value */
  int *cur;
};
static void addthread(struct pike *vm, struct tlist *l, int pc0, const int *caps, int pos) {
  struct prog *p = vm->p;
  int *cur = vm->cur, *st = vm->stack, sp = 0, ncap = vm->ncap;
  memcpy(cur, caps, ncap * sizeof *cur);
  st[sp++] = pc0; st[sp++] = 0;
  while (sp) {
    int val = st[--sp], pc = st[--sp];
    if (pc < 0) { cur[-1 - pc] = val; continue; }
    if (vm->mark[pc] == vm->gen) continue;
    vm->mark[pc] = vm->gen;
    struct inst *in = &p->code[pc];
    switch (in->op) {
    case O_JMP: st[sp++] = in->x; st[sp++] = 0; break;
    case O_SPLIT: st[sp++] = in->y; st[sp++] = 0; st[sp++] = in->x; st[sp++] = 0; break;
    case O_SAVE:
      if (in->x < vm->capn) { st[sp++] = -1 - in->x; st[sp++] = cur[in->x]; cur[in->x] = pos; }
      st[sp++] = pc + 1; st[sp++] = 0;
      break;
    case O_MARK: {
      int r = vm->capn + in->x;
      st[sp++] = -1 - r; st[sp++] = cur[r]; cur[r] = pos;
      st[sp++] = pc + 1; st[sp++] = 0;
      break;
    }
    case O_NONEMPTY: if (cur[vm->capn + in->x] != pos) { st[sp++] = pc + 1; st[sp++] = 0; } break;
    case O_BOL: case O_EOL: case O_WORDB: case O_NWORDB: case O_WBEG: case O_WEND: case O_BUFBEG: case O_BUFEND:
      if (assert_ok(vm->sj, in->op, pos)) { st[sp++] = pc + 1; st[sp++] = 0; }
      break;
    default:
      l->pc[l->n] = pc;
      memcpy(l->caps + l->n * ncap, cur, ncap * sizeof *cur);
      l->n++;
    }
  }
}

static int pike_run(struct prog *p, const struct subject *sj, int start, int capn, int *best) {
  struct pike vm;
  int n = p->n, rc = REG_ESPACE, matched = 0, ncap = capn + p->nloops;
  vm.p = p; vm.sj = sj; vm.ncap = ncap; vm.capn = capn; vm.gen = 0;
  vm.mark = calloc(n, sizeof *vm.mark);
  vm.stack = malloc((size_t)n * 4 * sizeof(int) + 64);
  vm.cur = malloc(ncap * sizeof(int));
  struct tlist a = {0, malloc(n * sizeof(int)), malloc((size_t)n * ncap * sizeof(int))};
  struct tlist b = {0, malloc(n * sizeof(int)), malloc((size_t)n * ncap * sizeof(int))};
  int *init = malloc(ncap * sizeof(int));
  if (!vm.mark || !vm.stack || !vm.cur || !a.pc || !a.caps || !b.pc || !b.caps || !init) goto out;
  struct tlist *cl = &a, *nl = &b;
  vm.gen = 1;
  for (int pos = start;; pos++) {
    if (!matched && (!p->anchored || pos == sj->base)) {
      if (cl->n == 0 && !p->first_any) { /* skip ahead to a byte that can start a match */
        while (pos < sj->end && !p->first[sj->s[pos]]) pos++;
        if (pos >= sj->end) { /* only an empty match could remain, and first_any says there is none */ break; }
      }
      if (cl->n == 0) vm.gen++; /* nothing alive: forget the marks of the last step */
      for (int i = 0; i < ncap; i++) init[i] = -1;
      init[0] = pos;
      addthread(&vm, cl, 0, init, pos);
    }
    if (cl->n == 0) { if (matched || p->anchored || pos >= sj->end) break; continue; }
    int c = pos < sj->end ? sj->s[pos] : -1;
    vm.gen++;
    nl->n = 0;
    for (int i = 0; i < cl->n; i++) {
      int *caps = cl->caps + i * ncap;
      if (matched && caps[0] > best[0]) continue;
      struct inst *in = &p->code[cl->pc[i]];
      int ok = 0;
      breathe();
      switch (in->op) {
      case O_MATCH:
        if (!matched || caps[0] < best[0] || (caps[0] == best[0] && pos > best[1])) {
          memcpy(best, caps, capn * sizeof *best);
          best[1] = pos;
          matched = 1;
        }
        break;
      case O_CHAR: ok = c == in->x; break;
      case O_ANY: ok = c >= 0; break;
      case O_ANYNL: ok = c >= 0 && c != '\n'; break;
      case O_SET: ok = c >= 0 && SETHAS(p->sets[in->x], c); break;
      }
      if (ok) addthread(&vm, nl, cl->pc[i] + 1, caps, pos + 1);
    }
    struct tlist *t = cl; cl = nl; nl = t;
    if (pos >= sj->end) break;
  }
  rc = matched ? 0 : REG_NOMATCH;
out:
  free(vm.mark); free(vm.stack); free(vm.cur); free(a.pc); free(a.caps); free(b.pc); free(b.caps); free(init);
  return rc;
}

/* ---- backtracking, for patterns with back-references ---- */
#ifndef RE_STEP_BUDGET
#define RE_STEP_BUDGET 20000000L
#endif
long __re_step_budget = RE_STEP_BUDGET;
long __re_budget_hits;

static int bt_run(struct prog *p, const struct subject *sj, int start, int ncap, int *best) {
  int nregs = p->nloops, nslots = 2 * (p->nsub + 1);
  int *caps = malloc((nslots + nregs + 1) * sizeof(int)); /* captures, then loop registers */
  int *bcaps = malloc(nslots * sizeof(int));
  size_t cap = 256, sp = 0;
  int *st = malloc(cap * 3 * sizeof(int)); /* triples: kind (0 choice, 1 undo), a, b */
  long steps = 0;
  int rc = REG_NOMATCH;
  if (!caps || !bcaps || !st) { rc = REG_ESPACE; goto out; }
  for (int s0 = start; s0 <= sj->end; s0++) {
    if (p->anchored && s0 != sj->base) break;
    if (!p->first_any && (s0 >= sj->end || !p->first[sj->s[s0]])) continue;
    int found = 0, bestend = -1;
    for (int i = 0; i < nslots + nregs; i++) caps[i] = -1;
    sp = 0;
    st[0] = 0; st[1] = 0; st[2] = s0; sp = 1;
    while (sp) {
      sp--;
      int kind = st[sp * 3], a = st[sp * 3 + 1], b = st[sp * 3 + 2];
      if (kind == 1) { caps[a] = b; continue; }
      int pc = a, pos = b;
      for (;;) {
        if (++steps > __re_step_budget) { __re_budget_hits++; rc = REG_ESPACE; goto out; }
        breathe();
        struct inst *in = &p->code[pc];
        int fail = 0;
        switch (in->op) {
        case O_CHAR: if (pos < sj->end && sj->s[pos] == in->x) { pos++; pc++; } else fail = 1; break;
        case O_ANY: if (pos < sj->end) { pos++; pc++; } else fail = 1; break;
        case O_ANYNL: if (pos < sj->end && sj->s[pos] != '\n') { pos++; pc++; } else fail = 1; break;
        case O_SET: if (pos < sj->end && SETHAS(p->sets[in->x], sj->s[pos])) { pos++; pc++; } else fail = 1; break;
        case O_JMP: pc = in->x; break;
        case O_NONEMPTY: if (caps[nslots + in->x] == pos) fail = 1; else pc++; break;
        case O_SPLIT:
        case O_SAVE:
        case O_MARK: {
          if (sp + 2 >= cap) {
            int *n2 = realloc(st, cap * 2 * 3 * sizeof(int));
            if (!n2) { rc = REG_ESPACE; goto out; }
            st = n2; cap *= 2;
          }
          if (in->op == O_SPLIT) { st[sp * 3] = 0; st[sp * 3 + 1] = in->y; st[sp * 3 + 2] = pos; sp++; pc = in->x; }
          else if (in->op == O_SAVE) { st[sp * 3] = 1; st[sp * 3 + 1] = in->x; st[sp * 3 + 2] = caps[in->x]; sp++; caps[in->x] = pos; pc++; }
          else {
            int r = nslots + in->x;
            st[sp * 3] = 1; st[sp * 3 + 1] = r; st[sp * 3 + 2] = caps[r]; sp++; caps[r] = pos; pc++;
          }
          break;
        }
        case O_BACKREF: {
          int so = caps[2 * in->x], eo = caps[2 * in->x + 1];
          if (so < 0 || eo < 0) { fail = 1; break; }
          int len = eo - so;
          if (pos + len > sj->end) { fail = 1; break; }
          for (int k = 0; k < len; k++) {
            int x = sj->s[so + k], y = sj->s[pos + k];
            if (x != y && !(p->icase && tolower(x) == tolower(y))) { fail = 1; break; }
          }
          if (!fail) { pos += len; pc++; }
          break;
        }
        case O_MATCH:
          if (pos > bestend) {
            bestend = pos; found = 1;
            memcpy(bcaps, caps, nslots * sizeof(int));
            bcaps[1] = pos;
          }
          fail = 1;
          if (pos == sj->end) sp = 0; /* nothing can be longer */
          break;
        default:
          if (assert_ok(sj, in->op, pos)) pc++; else fail = 1;
        }
        if (fail) break;
      }
    }
    if (found) {
      memcpy(best, bcaps, (ncap < nslots ? ncap : nslots) * sizeof(int));
      for (int i = nslots; i < ncap; i++) best[i] = -1;
      rc = 0;
      goto out;
    }
  }
out:
  free(caps); free(bcaps); free(st);
  return rc;
}

int regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t *pm, int eflags) {
  struct prog *p = re->__impl;
  if (!p) return REG_BADPAT;
  struct subject sj;
  sj.s = (const unsigned char *)string;
  sj.notbol = eflags & REG_NOTBOL;
  sj.noteol = eflags & REG_NOTEOL;
  sj.newline = 0;
  int start = 0;
  if (eflags & (REG_STARTEND | REG_UC_OFFSET)) {
    if (!pm) return REG_BADPAT;
    start = pm[0].rm_so;
    sj.end = pm[0].rm_eo;
    sj.base = (eflags & REG_STARTEND) ? start : 0;
  } else { sj.base = 0; sj.end = strlen(string); }
  if (start < 0 || start > sj.end) return REG_NOMATCH;
  sj.newline = p->newline; /* a compile flag, but the anchors need it while matching */
  if (re->re_nsub != (size_t)p->nsub) return REG_BADPAT;
  int nslots = 2 * (p->nsub + 1);
  int want = (int)nmatch;
  if (want > p->nsub + 1) want = p->nsub + 1;
  if (want < 1) want = 1;
  int ncap = p->backrefs ? nslots : 2 * want;
  int bestbuf[20], *best = ncap <= 20 ? bestbuf : malloc(ncap * sizeof(int));
  if (!best) return REG_ESPACE;
  int rc = p->backrefs ? bt_run(p, &sj, start, ncap, best) : pike_run(p, &sj, start, ncap, best);
  if (rc == 0 && pm && nmatch) {
    for (size_t i = 0; i < nmatch; i++) {
      if ((int)(2 * i + 1) < ncap && (int)i <= p->nsub && best[2 * i] >= 0 && best[2 * i + 1] >= 0) {
        pm[i].rm_so = best[2 * i]; pm[i].rm_eo = best[2 * i + 1];
      } else pm[i].rm_so = pm[i].rm_eo = -1;
    }
  }
  if (best != bestbuf) free(best);
  return rc;
}
