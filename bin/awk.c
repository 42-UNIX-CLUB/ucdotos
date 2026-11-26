// SPDX-License-Identifier: GPL-2.0-or-later
/* awk [-F fs] [-v var=value]... [-f progfile | 'program'] [file | var=value]...
 * The POSIX awk language: patterns and actions, BEGIN and END, ranges,
 * fields and records (FS as a blank, a single character or an extended
 * regular expression; RS as a character, "" for paragraphs, or a regular
 * expression), associative arrays, user functions, getline in all its forms,
 * print and printf with > >> and | redirections, close, fflush, system, and
 * the built-in functions length substr index split sub gsub match sprintf
 * sin cos atan2 exp log sqrt int rand srand tolower toupper, plus systime.
 * Regular expressions come from the C library (extended syntax).
 * Not implemented: gawk's other extensions (gensub, strftime, arrays of
 * arrays, @include, BEGINFILE...), and NUL bytes in records. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <regex.h>
#include <sys/wait.h>

/* ---------- helpers ---------- */
static const char *progname = "awk";
static int lineno_src = 1;
static int parsing; /* errors while reading the program exit 1, as in gawk; run-time errors 2 */
static void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fflush(stdout);
  fprintf(stderr, "%s: ", progname);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(parsing ? 1 : 2);
}
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) fatal("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) fatal("out of memory"); return p; }
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xmalloc(n), s, n); }
static char *xstrndup(const char *s, size_t n) { char *d = xmalloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }
struct sbuf { char *s; size_t len, cap; };
static void sb_grow(struct sbuf *b, size_t n) { if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->s = xrealloc(b->s, b->cap); } }
static void sb_add(struct sbuf *b, const char *s, size_t n) { sb_grow(b, n); memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
static void sb_addc(struct sbuf *b, int c) { char ch = c; sb_add(b, &ch, 1); }
static void sb_adds(struct sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

/* ---------- values ---------- */
/* F_NUM and F_STR say which conversions are cached; F_NUMVAL that the value is a
 * number; F_STRNUM that it is input text that looks like a number */
enum { F_NUM = 1, F_STR = 2, F_STRNUM = 4, F_ARR = 8, F_NUMVAL = 16 };
struct array;
typedef struct cell { int flags; double num; char *str; struct array *arr; } Cell;
struct aent { char *key; Cell val; struct aent *next; };
struct array { struct aent **tab; size_t size, n; };

static char *convfmt, *ofmt;
static int looks_numeric(const char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  if (*s == '+' || *s == '-') s++;
  int digits = 0;
  while (isdigit((unsigned char)*s)) s++, digits++;
  if (*s == '.') { s++; while (isdigit((unsigned char)*s)) s++, digits++; }
  if (!digits) return 0;
  if (*s == 'e' || *s == 'E') {
    const char *e = s + 1;
    if (*e == '+' || *e == '-') e++;
    if (isdigit((unsigned char)*e)) { while (isdigit((unsigned char)*e)) e++; s = e; }
  }
  while (*s == ' ' || *s == '\t' || *s == '\n') s++;
  return *s == 0;
}
/* the numeric value of the leading part of s, as awk reads numbers (no hex, no inf) */
static double str2num(const char *s) {
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  const char *st = p;
  if (*p == '+' || *p == '-') p++;
  int digits = 0;
  while (isdigit((unsigned char)*p)) p++, digits++;
  if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++, digits++; }
  if (!digits) return 0;
  if (*p == 'e' || *p == 'E') {
    const char *e = p + 1;
    if (*e == '+' || *e == '-') e++;
    if (isdigit((unsigned char)*e)) { while (isdigit((unsigned char)*e)) e++; p = e; }
  }
  char tmp[64];
  size_t n = p - st;
  if (n < sizeof tmp) { memcpy(tmp, st, n); tmp[n] = 0; return strtod(tmp, NULL); }
  char *big = xstrndup(st, n);
  double d = strtod(big, NULL);
  free(big);
  return d;
}
static char *num2str(double d, const char *fmt) {
  char tmp[512];
  if (d == (double)(long long)d && fabs(d) < 1e16) snprintf(tmp, sizeof tmp, "%lld", (long long)d);
  else if (d == floor(d) && !isinf(d)) { char *big = xmalloc(400); snprintf(big, 400, "%.0f", d); return big; } /* integral values print as integers at any size, as gawk */
  else if (isnan(d)) snprintf(tmp, sizeof tmp, "%snan", signbit(d) ? "-" : "+");
  else if (isinf(d)) snprintf(tmp, sizeof tmp, "%sinf", d < 0 ? "-" : "+");
  else snprintf(tmp, sizeof tmp, fmt, d);
  return xstrdup(tmp);
}
static double getnum(Cell *c) {
  if (c->flags & F_ARR) fatal("attempt to use array in a scalar context");
  if (!(c->flags & F_NUM)) { c->num = c->flags & F_STR ? str2num(c->str) : 0; c->flags |= F_NUM; }
  return c->num;
}
static const char *getstr(Cell *c) {
  if (c->flags & F_ARR) fatal("attempt to use array in a scalar context");
  if (!(c->flags & F_STR)) {
    free(c->str);
    c->str = c->flags & F_NUM ? num2str(c->num, convfmt) : xstrdup("");
    c->flags |= F_STR;
  }
  return c->str;
}
static void cell_free(Cell *c) { free(c->str); c->str = NULL; c->flags = 0; }
static void set_num(Cell *c, double d) { if (c->flags & F_ARR) fatal("attempt to use array in a scalar context"); free(c->str); c->str = NULL; c->num = d; c->flags = F_NUM | F_NUMVAL; }
static void set_str(Cell *c, const char *s) { if (c->flags & F_ARR) fatal("attempt to use array in a scalar context"); char *d = xstrdup(s); free(c->str); c->str = d; c->flags = F_STR; }
static void set_strnum(Cell *c, const char *s) { set_str(c, s); if (looks_numeric(s)) c->flags |= F_STRNUM; }
static void set_cell(Cell *d, Cell *s) {
  if (d == s) return;
  if (s->flags & F_ARR) fatal("attempt to use array in a scalar context");
  char *str = s->flags & F_STR ? xstrdup(s->str) : NULL;
  free(d->str);
  d->str = str;
  d->num = s->num;
  d->flags = s->flags & (F_NUM | F_STR | F_STRNUM | F_NUMVAL);
}
static Cell tmpcell(void) { Cell c = {0, 0, NULL, NULL}; return c; }

/* ---------- arrays ---------- */
static unsigned long hash(const char *s) { unsigned long h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
static struct array *arr_new(void) { struct array *a = xmalloc(sizeof *a); a->size = 16; a->n = 0; a->tab = calloc(a->size, sizeof *a->tab); return a; }
static Cell *arr_get(struct array *a, const char *k, int create) {
  unsigned long h = hash(k);
  for (struct aent *e = a->tab[h % a->size]; e; e = e->next) if (!strcmp(e->key, k)) return &e->val;
  if (!create) return NULL;
  if (a->n >= a->size * 2) {
    size_t ns = a->size * 4;
    struct aent **nt = calloc(ns, sizeof *nt);
    for (size_t i = 0; i < a->size; i++)
      for (struct aent *e = a->tab[i], *nx; e; e = nx) { nx = e->next; unsigned long hh = hash(e->key) % ns; e->next = nt[hh]; nt[hh] = e; }
    free(a->tab); a->tab = nt; a->size = ns;
  }
  struct aent *e = xmalloc(sizeof *e);
  e->key = xstrdup(k);
  e->val = tmpcell();
  e->next = a->tab[h % a->size];
  a->tab[h % a->size] = e;
  a->n++;
  return &e->val;
}
static void arr_del(struct array *a, const char *k) {
  struct aent **pp = &a->tab[hash(k) % a->size];
  for (; *pp; pp = &(*pp)->next)
    if (!strcmp((*pp)->key, k)) { struct aent *e = *pp; *pp = e->next; free(e->key); cell_free(&e->val); free(e); a->n--; return; }
}
static void arr_clear(struct array *a) {
  for (size_t i = 0; i < a->size; i++) for (struct aent *e = a->tab[i], *nx; e; e = nx) { nx = e->next; free(e->key); cell_free(&e->val); free(e); }
  memset(a->tab, 0, a->size * sizeof *a->tab);
  a->n = 0;
}
static struct array *to_array(Cell *c) {
  if (c->flags & F_ARR) return c->arr;
  if (c->flags & (F_NUM | F_STR)) fatal("attempt to use a scalar as an array");
  c->flags = F_ARR;
  c->arr = arr_new();
  return c->arr;
}

/* ---------- lexer ---------- */
enum {
  T_EOF, T_NEWLINE, T_NUMBER, T_STRING, T_ERE, T_NAME, T_FUNC_NAME, T_BUILTIN,
  T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE, T_NEXT, T_NEXTFILE, T_EXIT,
  T_RETURN, T_DELETE, T_IN, T_GETLINE, T_PRINT, T_PRINTF,
  T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_POW, T_NOT, T_GT, T_LT, T_PIPE, T_QUESTION, T_COLON, T_MATCH, T_NOMATCH,
  T_DOLLAR, T_ASSIGN, T_ADD_ASSIGN, T_SUB_ASSIGN, T_MUL_ASSIGN, T_DIV_ASSIGN, T_MOD_ASSIGN, T_POW_ASSIGN,
  T_EQ, T_LE, T_GE, T_NE, T_INCR, T_DECR, T_AND, T_OR, T_APPEND,
};
static const char *src;
static size_t spos, slen;
struct token { int type; char *text; double num; int line; };
static struct token tok;
static int prev_operand;

static const struct { const char *name; int type; } keywords[] = {
  {"BEGIN", T_BEGIN}, {"END", T_END}, {"function", T_FUNCTION}, {"func", T_FUNCTION}, {"if", T_IF}, {"else", T_ELSE},
  {"while", T_WHILE}, {"for", T_FOR}, {"do", T_DO}, {"break", T_BREAK}, {"continue", T_CONTINUE}, {"next", T_NEXT},
  {"nextfile", T_NEXTFILE}, {"exit", T_EXIT}, {"return", T_RETURN}, {"delete", T_DELETE}, {"in", T_IN},
  {"getline", T_GETLINE}, {"print", T_PRINT}, {"printf", T_PRINTF},
};
enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT,
       B_INT, B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH, B_SYSTIME };
static const char *builtins[] = {"length", "substr", "index", "split", "sub", "gsub", "match", "sprintf", "sin", "cos", "atan2", "exp",
                                 "log", "sqrt", "int", "rand", "srand", "tolower", "toupper", "system", "close", "fflush", "systime"};

static int escape_char(const char **pp) {
  const char *p = *pp;
  int c = *p++;
  switch (c) {
  case 'n': c = '\n'; break;
  case 't': c = '\t'; break;
  case 'r': c = '\r'; break;
  case 'f': c = '\f'; break;
  case 'b': c = '\b'; break;
  case 'v': c = '\v'; break;
  case 'a': c = 7; break;
  case '\\': c = '\\'; break;
  case '"': c = '"'; break;
  default:
    if (c >= '0' && c <= '7') {
      int v = c - '0', k = 1;
      while (k < 3 && *p >= '0' && *p <= '7') v = v * 8 + (*p++ - '0'), k++;
      c = v;
    } else { *pp = p; return -1 - c; } /* not an escape: the caller decides */
  }
  *pp = p;
  return c;
}
/* string escapes, as in string literals and -v assignments */
static char *unescape(const char *s, size_t n) {
  struct sbuf b = {0};
  sb_grow(&b, n);
  const char *end = s + n;
  while (s < end) {
    if (*s == '\\' && s + 1 < end) {
      s++;
      int c = escape_char(&s);
      if (c < 0) {
        /* as gawk: the character without the backslash, and a warning */
        fprintf(stderr, "%s: warning: escape sequence `\\%c' treated as plain `%c'\n", progname, -1 - c, -1 - c);
        sb_addc(&b, -1 - c);
      } else sb_addc(&b, c);
    } else sb_addc(&b, *s++);
  }
  sb_add(&b, "", 0); /* terminated even when empty */
  return b.s;
}

static void next_token(void) {
  free(tok.text);
  tok.text = NULL;
again:
  while (spos < slen && (src[spos] == ' ' || src[spos] == '\t' || src[spos] == '\r')) spos++;
  if (spos < slen && src[spos] == '\\' && spos + 1 < slen && src[spos + 1] == '\n') { spos += 2; lineno_src++; goto again; }
  if (spos < slen && src[spos] == '\\' && spos + 2 < slen && src[spos + 1] == '\r' && src[spos + 2] == '\n') { spos += 3; lineno_src++; goto again; }
  if (spos < slen && src[spos] == '#') { while (spos < slen && src[spos] != '\n') spos++; }
  tok.line = lineno_src;
  if (spos >= slen) { tok.type = T_EOF; prev_operand = 0; return; }
  char c = src[spos];
  int operand = 0;
  if (c == '\n') { spos++; lineno_src++; tok.type = T_NEWLINE; prev_operand = 0; return; }
  if (isdigit((unsigned char)c) || (c == '.' && spos + 1 < slen && isdigit((unsigned char)src[spos + 1]))) {
    char *e;
    if (c == '0' && spos + 1 < slen && (src[spos + 1] == 'x' || src[spos + 1] == 'X') && spos + 2 < slen && isxdigit((unsigned char)src[spos + 2])) {
      tok.num = (double)strtoull(src + spos, &e, 16); /* hexadecimal constants in programs, as gawk */
    } else if (c == '0' && spos + 1 < slen && isdigit((unsigned char)src[spos + 1])) {
      /* 010 is octal in programs, as gawk (unless 8 or 9 appear) */
      size_t k = spos;
      int oct = 1;
      while (k < slen && isdigit((unsigned char)src[k])) { if (src[k] > '7') oct = 0; k++; }
      if (oct && !(k < slen && (src[k] == '.' || src[k] == 'e' || src[k] == 'E'))) {
        tok.num = (double)strtoull(src + spos, &e, 8);
        spos = e - src;
        tok.type = T_NUMBER;
        prev_operand = 1;
        return;
      }
      tok.num = strtod(src + spos, &e);
    } else tok.num = strtod(src + spos, &e);
    /* strtod would take "1e" or hex; keep to awk's number syntax */
    size_t k = spos;
    while (k < slen && isdigit((unsigned char)src[k])) k++;
    if (k < slen && src[k] == '.') { k++; while (k < slen && isdigit((unsigned char)src[k])) k++; }
    if (k < slen && (src[k] == 'e' || src[k] == 'E')) {
      size_t j = k + 1;
      if (j < slen && (src[j] == '+' || src[j] == '-')) j++;
      if (j < slen && isdigit((unsigned char)src[j])) { while (j < slen && isdigit((unsigned char)src[j])) j++; k = j; }
    }
    if ((size_t)(e - src) > k && !(c == '0' && (src[spos + 1] == 'x' || src[spos + 1] == 'X'))) {
      char *tmp = xstrndup(src + spos, k - spos);
      tok.num = strtod(tmp, NULL);
      free(tmp);
      e = (char *)src + k;
    }
    spos = e - src;
    tok.type = T_NUMBER;
    prev_operand = 1;
    return;
  }
  if (isalpha((unsigned char)c) || c == '_') {
    size_t st = spos;
    while (spos < slen && (isalnum((unsigned char)src[spos]) || src[spos] == '_')) spos++;
    tok.text = xstrndup(src + st, spos - st);
    for (size_t k = 0; k < sizeof keywords / sizeof *keywords; k++)
      if (!strcmp(tok.text, keywords[k].name)) { tok.type = keywords[k].type; prev_operand = tok.type == T_GETLINE; return; }
    for (size_t k = 0; k < sizeof builtins / sizeof *builtins; k++)
      if (!strcmp(tok.text, builtins[k])) { tok.type = T_BUILTIN; tok.num = k; prev_operand = 1; return; }
    tok.type = spos < slen && src[spos] == '(' ? T_FUNC_NAME : T_NAME;
    prev_operand = 1;
    return;
  }
  if (c == '"') {
    size_t st = ++spos;
    while (spos < slen && src[spos] != '"') {
      if (src[spos] == '\\' && spos + 1 < slen) spos++;
      if (src[spos] == '\n') lineno_src++;
      spos++;
    }
    if (spos >= slen) fatal("source line %d: unterminated string", tok.line);
    tok.text = unescape(src + st, spos - st);
    spos++;
    tok.type = T_STRING;
    prev_operand = 1;
    return;
  }
  if (c == '/' && !prev_operand) {
    /* a regular expression literal: \/ is a slash, other escapes stay for the regex compiler */
    struct sbuf b = {0};
    sb_add(&b, "", 0);
    spos++;
    int in_bracket = 0;
    while (spos < slen && (src[spos] != '/' || in_bracket)) {
      if (src[spos] == '\n') fatal("source line %d: newline in regular expression", tok.line);
      if (src[spos] == '\\' && spos + 1 < slen) {
        if (src[spos + 1] == '/') sb_addc(&b, '/');
        else { sb_addc(&b, '\\'); sb_addc(&b, src[spos + 1]); }
        spos += 2;
        continue;
      }
      if (src[spos] == '[' && !in_bracket) {
        in_bracket = 1;
        sb_addc(&b, src[spos++]);
        if (spos < slen && src[spos] == '^') sb_addc(&b, src[spos++]);
        if (spos < slen && src[spos] == ']') sb_addc(&b, src[spos++]);
        continue;
      }
      if (in_bracket && src[spos] == '[' && spos + 1 < slen && src[spos + 1] == ':') {
        while (spos < slen && !(src[spos] == ':' && spos + 1 < slen && src[spos + 1] == ']')) sb_addc(&b, src[spos++]);
        if (spos < slen) { sb_addc(&b, src[spos++]); sb_addc(&b, src[spos++]); }
        continue;
      }
      if (in_bracket && src[spos] == ']') in_bracket = 0;
      sb_addc(&b, src[spos++]);
    }
    if (spos >= slen) fatal("source line %d: unterminated regular expression", tok.line);
    spos++;
    tok.text = b.s ? b.s : xstrdup("");
    tok.type = T_ERE;
    prev_operand = 1;
    return;
  }
  spos++;
  char n = spos < slen ? src[spos] : 0;
  int t;
#define TWO(ch, tt) if (n == ch) { spos++; t = tt; break; }
  switch (c) {
  case '{': t = T_LBRACE; break;
  case '}': t = T_RBRACE; break;
  case '(': t = T_LPAREN; break;
  case ')': t = T_RPAREN; operand = 1; break;
  case '[': t = T_LBRACKET; break;
  case ']': t = T_RBRACKET; operand = 1; break;
  case ';': t = T_SEMI; break;
  case ',': t = T_COMMA; break;
  case '+': TWO('+', T_INCR) TWO('=', T_ADD_ASSIGN) t = T_PLUS; break;
  case '-': TWO('-', T_DECR) TWO('=', T_SUB_ASSIGN) t = T_MINUS; break;
  case '*':
    if (n == '*') { spos++; if (spos < slen && src[spos] == '=') { spos++; t = T_POW_ASSIGN; } else t = T_POW; break; }
    TWO('=', T_MUL_ASSIGN) t = T_STAR; break;
  case '/': TWO('=', T_DIV_ASSIGN) t = T_SLASH; break;
  case '%': TWO('=', T_MOD_ASSIGN) t = T_PERCENT; break;
  case '^': TWO('=', T_POW_ASSIGN) t = T_POW; break;
  case '!': TWO('=', T_NE) TWO('~', T_NOMATCH) t = T_NOT; break;
  case '>': TWO('=', T_GE) TWO('>', T_APPEND) t = T_GT; break;
  case '<': TWO('=', T_LE) t = T_LT; break;
  case '|': TWO('|', T_OR) t = T_PIPE; break;
  case '&': if (n == '&') { spos++; t = T_AND; break; } fatal("source line %d: syntax error at '&'", tok.line);
  case '?': t = T_QUESTION; break;
  case ':': t = T_COLON; break;
  case '~': t = T_MATCH; break;
  case '$': t = T_DOLLAR; break;
  case '=': TWO('=', T_EQ) t = T_ASSIGN; break;
  default: fatal("source line %d: syntax error at '%c'", tok.line, c);
  }
#undef TWO
  if (t == T_INCR || t == T_DECR) operand = prev_operand; /* x++ / y: still an operand */
  tok.type = t;
  prev_operand = operand;
}

/* ---------- syntax tree ---------- */
enum {
  N_NUM, N_STR, N_ERE, N_VAR, N_LOCAL, N_FIELD, N_INDEX, N_ASSIGN, N_COND, N_OR, N_AND, N_IN, N_MATCH, N_CMP, N_CONCAT,
  N_ARITH, N_NEG, N_PLUS, N_NOT, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_CALL, N_BUILTIN, N_GETLINE, N_GROUP,
  S_EXPR, S_PRINT, S_PRINTF, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BLOCK, S_NEXT, S_NEXTFILE, S_EXIT, S_RETURN, S_BREAK,
  S_CONTINUE, S_DELETE, S_DELETEALL, S_GETLINE,
};
typedef struct node {
  int type, op;
  struct node *a, *b, *c, *d;
  struct node *next;   /* lists: arguments, statements */
  double num;
  char *str;
  regex_t *re;
  int idx;             /* variable or local index, builtin number, function number */
  int nargs;
  int line;
} Node;
static Node *mknode(int type) { Node *n = calloc(1, sizeof *n); n->type = type; n->line = tok.line; return n; }

/* globals */
struct var { char *name; Cell cell; };
static struct var *vars;
static int nvars, capvars;
static int var_index(const char *name) {
  for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) return i;
  if (nvars == capvars) vars = xrealloc(vars, (capvars = capvars * 2 + 64) * sizeof *vars);
  vars[nvars].name = xstrdup(name);
  vars[nvars].cell = tmpcell();
  return nvars++;
}
enum { V_NR, V_NF, V_FNR, V_FS, V_OFS, V_ORS, V_RS, V_FILENAME, V_SUBSEP, V_RSTART, V_RLENGTH, V_CONVFMT, V_OFMT, V_ENVIRON, V_ARGC, V_ARGV };
static const char *special[] = {"NR", "NF", "FNR", "FS", "OFS", "ORS", "RS", "FILENAME", "SUBSEP", "RSTART", "RLENGTH", "CONVFMT", "OFMT", "ENVIRON", "ARGC", "ARGV"};

/* functions */
struct func { char *name; char **params; int nparams; Node *body; int defined; };
static struct func *funcs;
static int nfuncs;
static int func_index(const char *name) {
  for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) return i;
  funcs = xrealloc(funcs, (nfuncs + 1) * sizeof *funcs);
  memset(&funcs[nfuncs], 0, sizeof *funcs);
  funcs[nfuncs].name = xstrdup(name);
  return nfuncs++;
}
static int cur_fi = -1; /* the function whose body is being parsed (an index: the table moves) */
#define cur_func (cur_fi >= 0 ? &funcs[cur_fi] : NULL)

/* rules */
struct rule { Node *pattern, *pattern2, *action; int range_active; };
static struct rule *rules;
static int nrules;
static Node *begin_actions, *end_actions; /* S_BLOCK lists, in order */
static Node **begin_tail = &begin_actions, **end_tail = &end_actions;

/* ---------- parser ---------- */
static void syntax_error(void) __attribute__((noreturn));
static void syntax_error(void) {
  if (tok.type == T_EOF) fatal("source line %d: syntax error at end of program", tok.line);
  if (tok.type == T_NEWLINE) fatal("source line %d: syntax error at newline", tok.line);
  fatal("source line %d: syntax error near '%s'", tok.line, tok.text ? tok.text : "symbol");
}
static void expect(int t) { if (tok.type != t) syntax_error(); next_token(); }
static void opt_newlines(void) { while (tok.type == T_NEWLINE) next_token(); }
static void skip_terminators(void) { while (tok.type == T_NEWLINE || tok.type == T_SEMI) next_token(); }

/* awk's escapes in regular expressions: \n \t \ddd and friends become bytes,
 * \/ and \" their character, \y a word boundary; the rest is left to regcomp */
static char *awk_regex_text(const char *s) {
  struct sbuf b = {0};
  sb_add(&b, "", 0);
  int br = 0;
  while (*s) {
    if (*s == '\\' && s[1]) {
      const char *p = s + 1;
      int c = *p;
      if (c == 'y') { sb_adds(&b, br ? "y" : "\\b"); s += 2; continue; }
      if (strchr("ntrfvab/\"", c) || (c >= '0' && c <= '7')) {
        int v = c == '/' ? (p++, '/') : escape_char(&p);
        if (c == 'b') v = 8;
        if (v >= 0) {
          if (!br && v && strchr(".[]()*+?{}|^$\\", v)) sb_addc(&b, '\\');
          else if (br && (v == ']' || v == '\\' || v == '^' || v == '-')) sb_addc(&b, '\\');
          sb_addc(&b, v);
          s = p;
          continue;
        }
      }
      sb_addc(&b, *s++);
      sb_addc(&b, *s++);
      continue;
    }
    if (*s == '[' && !br) {
      br = 1;
      sb_addc(&b, *s++);
      if (*s == '^') sb_addc(&b, *s++);
      if (*s == ']') sb_addc(&b, *s++);
      continue;
    }
    if (br && s[0] == '[' && s[1] == ':') {
      const char *e = strstr(s + 2, ":]");
      if (e) { sb_add(&b, s, e + 2 - s); s = e + 2; continue; }
    }
    if (br && *s == ']') br = 0;
    sb_addc(&b, *s++);
  }
  return b.s;
}
static regex_t *compile_ere(const char *src_pat) {
  regex_t *re = xmalloc(sizeof *re);
  char *pat = awk_regex_text(src_pat);
  int rc = regcomp(re, pat, REG_EXTENDED | REG_UC_AWK | REG_UC_NOBACKREF);
  free(pat);
  pat = (char *)src_pat;
  if (rc) { char m[128]; regerror(rc, re, m, sizeof m); fatal("regular expression /%s/: %s", pat, m); }
  return re;
}

static Node *expr(int no_gt, int no_in);
static Node *ternary(int no_gt, int no_in);
static int is_lvalue(Node *n) { return n->type == N_VAR || n->type == N_LOCAL || n->type == N_FIELD || n->type == N_INDEX; }

static Node *name_node(const char *name) {
  if (cur_func)
    for (int i = 0; i < cur_func->nparams; i++)
      if (!strcmp(cur_func->params[i], name)) { Node *n = mknode(N_LOCAL); n->idx = i; return n; }
  for (int k = 0; k < nfuncs; k++) if (funcs[k].defined && !strcmp(funcs[k].name, name)) fatal("source line %d: function '%s' used as a variable", tok.line, name);
  Node *n = mknode(N_VAR);
  n->idx = var_index(name);
  return n;
}
/* a subscript list a[i, j]: joined with SUBSEP at run time */
static Node *subscripts(void) {
  Node *first = expr(0, 0), **tail = &first->next;
  int n = 1;
  while (tok.type == T_COMMA) { next_token(); opt_newlines(); *tail = expr(0, 0); tail = &(*tail)->next; n++; }
  first->nargs = n;
  return first;
}
static Node *primary(int no_gt, int no_in);
static Node *getline_tail(Node *g, int no_gt) {
  /* getline [lvalue] [< file] */
  if (tok.type == T_DOLLAR || tok.type == T_NAME) {
    Node *lv = primary(1, 1);
    if (!is_lvalue(lv)) syntax_error();
    g->a = lv;
  }
  if (tok.type == T_LT) { next_token(); g->b = primary(1, 1); g->op = '<'; }
  (void)no_gt;
  return g;
}
static Node *incdec_or_primary(int no_gt, int no_in) {
  if (tok.type == T_INCR || tok.type == T_DECR) {
    int t = tok.type;
    next_token();
    Node *lv = incdec_or_primary(no_gt, no_in);
    if (!is_lvalue(lv)) syntax_error();
    Node *n = mknode(t == T_INCR ? N_PREINC : N_PREDEC);
    n->a = lv;
    return n;
  }
  if (tok.type == T_MINUS || tok.type == T_PLUS || tok.type == T_NOT) {
    int t = tok.type;
    next_token();
    Node *n = mknode(t == T_MINUS ? N_NEG : t == T_PLUS ? N_PLUS : N_NOT);
    n->a = incdec_or_primary(no_gt, no_in);
    return n;
  }
  return primary(no_gt, no_in);
}
static Node *primary(int no_gt, int no_in) {
  Node *n;
  switch (tok.type) {
  case T_NUMBER: n = mknode(N_NUM); n->num = tok.num; next_token(); return n;
  case T_STRING: n = mknode(N_STR); n->str = xstrdup(tok.text); next_token(); return n;
  case T_ERE: n = mknode(N_ERE); n->str = xstrdup(tok.text); n->re = compile_ere(tok.text); next_token(); return n;
  case T_DOLLAR: {
    next_token();
    n = mknode(N_FIELD);
    n->a = incdec_or_primary(1, 1);
    if (n->a->type == N_NEG || n->a->type == N_PLUS || n->a->type == N_NOT) {}
    return n;
  }
  case T_LPAREN: {
    next_token();
    opt_newlines();
    Node *e = expr(0, 0);
    if (tok.type == T_COMMA) {
      /* (a, b) in arr, or a grouped print list */
      Node **tail = &e->next;
      int cnt = 1;
      while (tok.type == T_COMMA) { next_token(); opt_newlines(); *tail = expr(0, 0); tail = &(*tail)->next; cnt++; }
      opt_newlines();
      expect(T_RPAREN);
      if (tok.type == T_IN) {
        next_token();
        if (tok.type != T_NAME) syntax_error();
        Node *in = mknode(N_IN);
        e->nargs = cnt;
        in->a = e;
        in->b = name_node(tok.text);
        next_token();
        return in;
      }
      Node *g = mknode(N_GROUP);
      g->a = e;
      g->nargs = cnt;
      return g;
    }
    opt_newlines();
    expect(T_RPAREN);
    Node *g = mknode(N_GROUP);
    g->a = e;
    g->nargs = 1;
    return g;
  }
  case T_NAME: {
    char *name = xstrdup(tok.text);
    next_token();
    if (tok.type == T_LBRACKET) {
      next_token();
      n = mknode(N_INDEX);
      n->a = name_node(name);
      n->b = subscripts();
      expect(T_RBRACKET);
    } else n = name_node(name);
    free(name);
    return n;
  }
  case T_FUNC_NAME: {
    n = mknode(N_CALL);
    n->idx = func_index(tok.text);
    next_token();
    expect(T_LPAREN);
    opt_newlines();
    Node **tail = &n->a;
    while (tok.type != T_RPAREN) {
      *tail = expr(0, 0);
      tail = &(*tail)->next;
      n->nargs++;
      opt_newlines();
      if (tok.type == T_COMMA) { next_token(); opt_newlines(); }
      else if (tok.type != T_RPAREN) syntax_error();
    }
    next_token();
    return n;
  }
  case T_BUILTIN: {
    n = mknode(N_BUILTIN);
    n->idx = (int)tok.num;
    next_token();
    if (tok.type == T_LPAREN) {
      next_token();
      opt_newlines();
      Node **tail = &n->a;
      while (tok.type != T_RPAREN) {
        *tail = expr(0, 0);
        tail = &(*tail)->next;
        n->nargs++;
        opt_newlines();
        if (tok.type == T_COMMA) { next_token(); opt_newlines(); }
        else if (tok.type != T_RPAREN) syntax_error();
      }
      next_token();
    } else if (n->idx != B_LENGTH) syntax_error();
    return n;
  }
  case T_GETLINE: {
    next_token();
    n = mknode(N_GETLINE);
    return getline_tail(n, no_gt);
  }
  case T_MINUS: case T_PLUS: case T_NOT: case T_INCR: case T_DECR:
    return incdec_or_primary(no_gt, no_in);
  default:
    syntax_error();
  }
}
static Node *postfix(int no_gt, int no_in) {
  Node *n = primary(no_gt, no_in);
  if ((tok.type == T_INCR || tok.type == T_DECR) && is_lvalue(n)) {
    Node *p = mknode(tok.type == T_INCR ? N_POSTINC : N_POSTDEC);
    p->a = n;
    next_token();
    return p;
  }
  return n;
}
static Node *unary(int no_gt, int no_in);
static Node *power(int no_gt, int no_in) {
  Node *n = postfix(no_gt, no_in);
  if (tok.type == T_POW) {
    next_token();
    Node *p = mknode(N_ARITH);
    p->op = '^';
    p->a = n;
    p->b = unary(no_gt, no_in); /* right associative; 2^-1 is allowed */
    return p;
  }
  return n;
}
static Node *unary(int no_gt, int no_in) {
  if (tok.type == T_NOT || tok.type == T_MINUS || tok.type == T_PLUS) {
    int t = tok.type;
    next_token();
    Node *n = mknode(t == T_NOT ? N_NOT : t == T_MINUS ? N_NEG : N_PLUS);
    n->a = unary(no_gt, no_in);
    return n;
  }
  if (tok.type == T_INCR || tok.type == T_DECR) {
    int t = tok.type;
    next_token();
    Node *lv = unary(no_gt, no_in);
    if (!is_lvalue(lv)) syntax_error();
    Node *n = mknode(t == T_INCR ? N_PREINC : N_PREDEC);
    n->a = lv;
    return n;
  }
  return power(no_gt, no_in);
}
static Node *multiplicative(int no_gt, int no_in) {
  Node *n = unary(no_gt, no_in);
  while (tok.type == T_STAR || tok.type == T_SLASH || tok.type == T_PERCENT) {
    Node *b = mknode(N_ARITH);
    b->op = tok.type == T_STAR ? '*' : tok.type == T_SLASH ? '/' : '%';
    next_token();
    b->a = n;
    b->b = unary(no_gt, no_in);
    n = b;
  }
  return n;
}
static Node *additive(int no_gt, int no_in) {
  Node *n = multiplicative(no_gt, no_in);
  while (tok.type == T_PLUS || tok.type == T_MINUS) {
    Node *b = mknode(N_ARITH);
    b->op = tok.type == T_PLUS ? '+' : '-';
    next_token();
    b->a = n;
    b->b = multiplicative(no_gt, no_in);
    n = b;
  }
  return n;
}
static int starts_concat(void) {
  switch (tok.type) {
  case T_NUMBER: case T_STRING: case T_ERE: case T_NAME: case T_FUNC_NAME: case T_BUILTIN: case T_DOLLAR:
  case T_NOT: case T_LPAREN: case T_INCR: case T_DECR: case T_MINUS: case T_PLUS:
    return 1;
  }
  return 0;
}
static Node *concatenation(int no_gt, int no_in) {
  Node *n = additive(no_gt, no_in);
  /* juxtaposition; a getline after "cmd |" is handled by the caller */
  while (starts_concat() && tok.type != T_MINUS && tok.type != T_PLUS) {
    if (tok.type == T_IN) break;
    Node *c = mknode(N_CONCAT);
    c->a = n;
    c->b = additive(no_gt, no_in);
    n = c;
  }
  return n;
}
static Node *pipe_getline(int no_gt, int no_in) {
  Node *n = concatenation(no_gt, no_in);
  while (tok.type == T_PIPE) {
    /* cmd | getline [var]: only when getline follows */
    size_t save_pos = spos;
    int save_line = lineno_src, save_prev = prev_operand;
    struct token save = tok;
    save.text = tok.text ? xstrdup(tok.text) : NULL;
    next_token();
    if (tok.type != T_GETLINE) {
      free(tok.text);
      spos = save_pos; lineno_src = save_line; prev_operand = save_prev;
      tok = save;
      break;
    }
    free(save.text);
    next_token();
    Node *g = mknode(N_GETLINE);
    g->op = '|';
    g->b = n;
    if (tok.type == T_DOLLAR || tok.type == T_NAME) {
      Node *lv = primary(1, 1);
      if (!is_lvalue(lv)) syntax_error();
      g->a = lv;
    }
    n = g;
  }
  return n;
}
static Node *comparison(int no_gt, int no_in) {
  Node *n = pipe_getline(no_gt, no_in);
  for (;;) {
    int t = tok.type, op;
    if (t == T_LT) op = '<';
    else if (t == T_LE) op = 'l';
    else if (t == T_NE) op = '!';
    else if (t == T_EQ) op = '=';
    else if (t == T_GT && !no_gt) op = '>';
    else if (t == T_GE) op = 'g';
    else return n;
    next_token();
    Node *c = mknode(N_CMP);
    c->op = op;
    c->a = n;
    c->b = pipe_getline(no_gt, no_in);
    n = c;
  }
}
static Node *matching(int no_gt, int no_in) {
  Node *n = comparison(no_gt, no_in);
  while (tok.type == T_MATCH || tok.type == T_NOMATCH) {
    Node *m = mknode(N_MATCH);
    m->op = tok.type == T_NOMATCH;
    next_token();
    m->a = n;
    m->b = comparison(no_gt, no_in);
    n = m;
  }
  return n;
}
static Node *in_expr(int no_gt, int no_in) {
  Node *n = matching(no_gt, no_in);
  while (tok.type == T_IN && !no_in) {
    next_token();
    if (tok.type != T_NAME) syntax_error();
    Node *in = mknode(N_IN);
    n->nargs = 1;
    in->a = n;
    in->b = name_node(tok.text);
    next_token();
    n = in;
  }
  return n;
}
static Node *and_expr(int no_gt, int no_in) {
  Node *n = in_expr(no_gt, no_in);
  while (tok.type == T_AND) {
    next_token();
    opt_newlines();
    Node *a = mknode(N_AND);
    a->a = n;
    a->b = in_expr(no_gt, no_in);
    n = a;
  }
  return n;
}
static Node *or_expr(int no_gt, int no_in) {
  Node *n = and_expr(no_gt, no_in);
  while (tok.type == T_OR) {
    next_token();
    opt_newlines();
    Node *o = mknode(N_OR);
    o->a = n;
    o->b = and_expr(no_gt, no_in);
    n = o;
  }
  return n;
}
static Node *ternary(int no_gt, int no_in) {
  Node *n = or_expr(no_gt, no_in);
  if (tok.type == T_QUESTION) {
    next_token();
    opt_newlines();
    Node *c = mknode(N_COND);
    c->a = n;
    c->b = ternary(no_gt, no_in);
    opt_newlines();
    expect(T_COLON);
    opt_newlines();
    c->c = ternary(no_gt, no_in);
    return c;
  }
  return n;
}
static Node *expr(int no_gt, int no_in) {
  Node *n = ternary(no_gt, no_in);
  int t = tok.type;
  if (is_lvalue(n) && (t == T_ASSIGN || t == T_ADD_ASSIGN || t == T_SUB_ASSIGN || t == T_MUL_ASSIGN || t == T_DIV_ASSIGN || t == T_MOD_ASSIGN || t == T_POW_ASSIGN)) {
    next_token();
    opt_newlines();
    Node *a = mknode(N_ASSIGN);
    a->op = t == T_ASSIGN ? '=' : t == T_ADD_ASSIGN ? '+' : t == T_SUB_ASSIGN ? '-' : t == T_MUL_ASSIGN ? '*' : t == T_DIV_ASSIGN ? '/' : t == T_MOD_ASSIGN ? '%' : '^';
    a->a = n;
    a->b = expr(no_gt, no_in);
    return a;
  }
  /* a lone "/=" that the lexer read as an assignment can start a regex: not handled */
  return n;
}

static Node *statement(void);
static Node *block(void) {
  Node *b = mknode(S_BLOCK), **tail = &b->a;
  expect(T_LBRACE);
  skip_terminators();
  while (tok.type != T_RBRACE) {
    if (tok.type == T_EOF) syntax_error();
    Node *s = statement();
    if (s) { *tail = s; while (*tail) tail = &(*tail)->next; }
    skip_terminators();
  }
  next_token();
  return b;
}
static void end_simple(void) {
  if (tok.type == T_SEMI || tok.type == T_NEWLINE) { next_token(); return; }
  if (tok.type == T_RBRACE || tok.type == T_EOF) return;
  syntax_error();
}
static Node *simple_or_block(void) {
  opt_newlines();
  if (tok.type == T_SEMI) { next_token(); return mknode(S_BLOCK); }
  return statement();
}
static Node *print_statement(int type) {
  Node *s = mknode(type);
  next_token();
  Node **tail = &s->a;
  if (tok.type != T_SEMI && tok.type != T_NEWLINE && tok.type != T_RBRACE && tok.type != T_GT && tok.type != T_APPEND && tok.type != T_PIPE && tok.type != T_EOF) {
    Node *e = expr(1, 0);
    if (e->type == N_GROUP && e->nargs > 1 && (tok.type == T_SEMI || tok.type == T_NEWLINE || tok.type == T_RBRACE || tok.type == T_GT || tok.type == T_APPEND || tok.type == T_PIPE || tok.type == T_EOF)) {
      s->a = e->a; /* print (a, b) > "file" */
      for (Node *x = s->a; x; x = x->next) s->nargs++;
    } else {
      *tail = e; tail = &e->next; s->nargs = 1;
      while (tok.type == T_COMMA) { next_token(); opt_newlines(); Node *x = expr(1, 0); *tail = x; tail = &x->next; s->nargs++; }
    }
  }
  if (tok.type == T_GT || tok.type == T_APPEND || tok.type == T_PIPE) {
    s->op = tok.type == T_GT ? '>' : tok.type == T_APPEND ? 'a' : '|';
    next_token();
    s->b = concatenation(1, 0);
  }
  end_simple();
  return s;
}
static Node *statement(void) {
  Node *s;
  switch (tok.type) {
  case T_LBRACE: return block();
  case T_IF: {
    next_token();
    expect(T_LPAREN);
    s = mknode(S_IF);
    s->a = expr(0, 0);
    expect(T_RPAREN);
    s->b = simple_or_block();
    /* an else may follow after newlines or a semicolon */
    size_t save_pos = spos;
    int save_line = lineno_src, save_prev = prev_operand;
    struct token save = tok;
    save.text = tok.text ? xstrdup(tok.text) : NULL;
    while (tok.type == T_NEWLINE || tok.type == T_SEMI) next_token();
    if (tok.type == T_ELSE) {
      free(save.text);
      next_token();
      s->c = simple_or_block();
    } else {
      free(tok.text);
      spos = save_pos; lineno_src = save_line; prev_operand = save_prev;
      tok = save;
    }
    return s;
  }
  case T_WHILE:
    next_token();
    expect(T_LPAREN);
    s = mknode(S_WHILE);
    s->a = expr(0, 0);
    expect(T_RPAREN);
    if (tok.type == T_SEMI) { next_token(); s->b = mknode(S_BLOCK); return s; }
    s->b = simple_or_block();
    return s;
  case T_DO:
    next_token();
    s = mknode(S_DO);
    s->b = simple_or_block();
    skip_terminators();
    expect(T_WHILE);
    expect(T_LPAREN);
    s->a = expr(0, 0);
    expect(T_RPAREN);
    end_simple();
    return s;
  case T_FOR: {
    next_token();
    expect(T_LPAREN);
    /* for (name in array) */
    if (tok.type == T_NAME) {
      size_t save_pos = spos;
      int save_line = lineno_src, save_prev = prev_operand;
      struct token save = tok;
      save.text = xstrdup(tok.text);
      char *name = xstrdup(tok.text);
      next_token();
      if (tok.type == T_IN) {
        next_token();
        if (tok.type == T_NAME) {
          char *arr = xstrdup(tok.text);
          next_token();
          if (tok.type == T_RPAREN) {
            next_token();
            s = mknode(S_FORIN);
            s->a = name_node(name);
            s->b = name_node(arr);
            s->c = simple_or_block();
            free(name); free(arr); free(save.text);
            return s;
          }
          free(arr);
        }
      }
      free(name);
      free(tok.text);
      spos = save_pos; lineno_src = save_line; prev_operand = save_prev;
      tok = save;
    }
    s = mknode(S_FOR);
    if (tok.type != T_SEMI) s->a = expr(0, 0);
    expect(T_SEMI);
    opt_newlines();
    if (tok.type != T_SEMI) s->b = expr(0, 0);
    expect(T_SEMI);
    opt_newlines();
    if (tok.type != T_RPAREN) s->c = expr(0, 0);
    expect(T_RPAREN);
    if (tok.type == T_SEMI) { next_token(); s->d = mknode(S_BLOCK); return s; }
    s->d = simple_or_block();
    return s;
  }
  case T_SEMI: next_token(); return NULL;
  case T_PRINT: return print_statement(S_PRINT);
  case T_PRINTF: return print_statement(S_PRINTF);
  case T_NEXT: next_token(); s = mknode(S_NEXT); end_simple(); return s;
  case T_NEXTFILE: next_token(); s = mknode(S_NEXTFILE); end_simple(); return s;
  case T_BREAK: next_token(); s = mknode(S_BREAK); end_simple(); return s;
  case T_CONTINUE: next_token(); s = mknode(S_CONTINUE); end_simple(); return s;
  case T_EXIT:
    next_token();
    s = mknode(S_EXIT);
    if (tok.type != T_SEMI && tok.type != T_NEWLINE && tok.type != T_RBRACE && tok.type != T_EOF) s->a = expr(0, 0);
    end_simple();
    return s;
  case T_RETURN:
    if (!cur_func) fatal("source line %d: return outside a function", tok.line);
    next_token();
    s = mknode(S_RETURN);
    if (tok.type != T_SEMI && tok.type != T_NEWLINE && tok.type != T_RBRACE && tok.type != T_EOF) s->a = expr(0, 0);
    end_simple();
    return s;
  case T_DELETE:
    next_token();
    if (tok.type != T_NAME) syntax_error();
    {
      char *name = xstrdup(tok.text);
      next_token();
      if (tok.type == T_LBRACKET) {
        next_token();
        s = mknode(S_DELETE);
        s->a = name_node(name);
        s->b = subscripts();
        expect(T_RBRACKET);
      } else { s = mknode(S_DELETEALL); s->a = name_node(name); }
      free(name);
    }
    end_simple();
    return s;
  default:
    s = mknode(S_EXPR);
    s->a = expr(0, 0);
    end_simple();
    return s;
  }
}
static void parse_program(void) {
  parsing = 1;
  next_token();
  skip_terminators();
  while (tok.type != T_EOF) {
    if (tok.type == T_FUNCTION) {
      next_token();
      if (tok.type != T_NAME && tok.type != T_FUNC_NAME) syntax_error();
      int fi = func_index(tok.text);
      struct func *f = &funcs[fi]; /* valid until the body adds functions */
      if (f->defined) fatal("source line %d: function '%s' defined twice", tok.line, tok.text);
      for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, tok.text) && i > V_ARGV) fatal("source line %d: function name '%s' is also a variable", tok.line, tok.text);
      f->defined = 1;
      next_token();
      expect(T_LPAREN);
      while (tok.type != T_RPAREN) {
        if (tok.type != T_NAME) syntax_error();
        f->params = xrealloc(f->params, (f->nparams + 1) * sizeof *f->params);
        f->params[f->nparams++] = xstrdup(tok.text);
        next_token();
        opt_newlines();
        if (tok.type == T_COMMA) { next_token(); opt_newlines(); }
        else if (tok.type != T_RPAREN) syntax_error();
      }
      next_token();
      opt_newlines();
      cur_fi = fi;
      Node *body = block();
      funcs[fi].body = body; /* f may have moved while the body was parsed */
      cur_fi = -1;
    } else if (tok.type == T_BEGIN || tok.type == T_END) {
      int b = tok.type == T_BEGIN;
      next_token();
      opt_newlines();
      if (tok.type != T_LBRACE) syntax_error();
      Node *blk = block();
      if (b) { *begin_tail = blk; begin_tail = &blk->next; } else { *end_tail = blk; end_tail = &blk->next; }
    } else {
      struct rule r = {0};
      if (tok.type != T_LBRACE) {
        r.pattern = expr(0, 0);
        if (tok.type == T_COMMA) { next_token(); opt_newlines(); r.pattern2 = expr(0, 0); }
      }
      if (tok.type == T_LBRACE) r.action = block();
      rules = xrealloc(rules, (nrules + 1) * sizeof *rules);
      rules[nrules++] = r;
    }
    skip_terminators();
  }
  parsing = 0;
  for (int i = 0; i < nfuncs; i++) if (!funcs[i].defined) fatal("function '%s' called but never defined", funcs[i].name);
}

/* ---------- records and fields ---------- */
static struct sbuf record;      /* $0 text */
static Cell *fields;            /* fields[1..nf] */
static int nf, capf, fields_valid = 1, record_valid = 1;
static Cell field0;
static void split_record(void);
static void ensure_fields(int n) {
  if (n + 1 > capf) {
    int nc = (n + 1) * 2;
    fields = xrealloc(fields, nc * sizeof *fields);
    for (int i = capf; i < nc; i++) fields[i] = tmpcell();
    capf = nc;
  }
}
static void set_record(const char *s, size_t n) {
  record.len = 0;
  sb_add(&record, s, n);
  if (!record.s) sb_add(&record, "", 0);
  fields_valid = 0;
  record_valid = 1;
  set_strnum(&field0, record.s);
}
static void rebuild_record(void) {
  struct sbuf b = {0};
  sb_add(&b, "", 0);
  const char *ofs = getstr(&vars[V_OFS].cell);
  for (int i = 1; i <= nf; i++) {
    if (i > 1) sb_adds(&b, ofs);
    sb_adds(&b, getstr(&fields[i]));
  }
  record.len = 0;
  sb_add(&record, b.s, b.len);
  free(b.s);
  set_strnum(&field0, record.s);
  record_valid = 1;
}
static Cell *get_field(int i) {
  if (i < 0) fatal("trying to access out of range field %d", i);
  if (i == 0) { if (!record_valid) rebuild_record(); return &field0; }
  if (!fields_valid) split_record();
  if (i > nf) { ensure_fields(i); static Cell empty; cell_free(&empty); empty = tmpcell(); set_str(&empty, ""); return &empty; }
  return &fields[i];
}
static void set_nf(int n) {
  if (!fields_valid) split_record();
  if (n < 0) fatal("NF set to negative value");
  ensure_fields(n);
  for (int i = nf + 1; i <= n; i++) set_str(&fields[i], "");
  nf = n;
  set_num(&vars[V_NF].cell, n);
  record_valid = 0;
}
/* assignment to $i */
static Cell *field_lvalue(int i) {
  if (i < 0) fatal("trying to access out of range field %d", i);
  if (i == 0) return &field0;
  if (!fields_valid) split_record();
  if (i > nf) set_nf(i);
  record_valid = 0;
  return &fields[i];
}

static regex_t *dyn_regex(const char *s);
/* split s by the field separator fs into a callback */
typedef void (*split_cb)(void *ctx, int idx, const char *s, size_t n);
static int split_with(const char *s, const char *fs, int paragraph, split_cb cb, void *ctx) {
  int n = 0;
  size_t len = strlen(s);
  if (!strcmp(fs, " ")) {
    const char *p = s;
    for (;;) {
      while (*p == ' ' || *p == '\t' || *p == '\n') p++;
      if (!*p) break;
      const char *st = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
      cb(ctx, ++n, st, p - st);
    }
    return n;
  }
  if (len == 0) return 0;
  if (!*fs) { /* gawk: an empty FS splits into characters */
    for (size_t i = 0; i < len; i++) cb(ctx, ++n, s + i, 1);
    return n;
  }
  if (strlen(fs) == 1 && fs[0] != '\\' && !paragraph) {
    char c = fs[0];
    const char *p = s;
    for (;;) {
      const char *e = strchr(p, c);
      if (!e) { cb(ctx, ++n, p, strlen(p)); break; }
      cb(ctx, ++n, p, e - p);
      p = e + 1;
    }
    return n;
  }
  regex_t *re;
  if (paragraph && strlen(fs) == 1) {
    /* paragraph mode: newline always separates fields too */
    char pat[16];
    snprintf(pat, sizeof pat, "[\n%s%c]", fs[0] == '\\' || fs[0] == ']' || fs[0] == '^' ? "\\" : "", fs[0]);
    re = dyn_regex(pat);
  } else if (paragraph) {
    char *pat = xmalloc(strlen(fs) + 8);
    sprintf(pat, "(%s)|\n", fs);
    re = dyn_regex(pat);
    free(pat);
  } else re = dyn_regex(fs);
  size_t pos = 0;
  for (;;) {
    regmatch_t m[1];
    m[0].rm_so = pos; m[0].rm_eo = len;
    if (pos > len || regexec(re, s, 1, m, REG_UC_OFFSET) != 0 || m[0].rm_eo == m[0].rm_so) {
      /* an empty match does not separate (as gawk) */
      if (pos <= len && m[0].rm_eo == m[0].rm_so && pos < len && regexec(re, s, 1, m, REG_UC_OFFSET) == 0) {
        /* look for a non-empty match further on */
        size_t q = m[0].rm_so + 1;
        int found = 0;
        while (q <= len) {
          m[0].rm_so = q; m[0].rm_eo = len;
          if (regexec(re, s, 1, m, REG_UC_OFFSET) != 0) break;
          if (m[0].rm_eo > m[0].rm_so) { found = 1; break; }
          q = m[0].rm_so + 1;
        }
        if (found) { cb(ctx, ++n, s + pos, m[0].rm_so - pos); pos = m[0].rm_eo; continue; }
      }
      cb(ctx, ++n, s + pos, len - pos);
      break;
    }
    cb(ctx, ++n, s + pos, m[0].rm_so - pos);
    pos = m[0].rm_eo;
  }
  return n;
}
static int paragraph_mode(void) { return getstr(&vars[V_RS].cell)[0] == 0; }
static void field_cb(void *ctx, int idx, const char *s, size_t n) {
  (void)ctx;
  ensure_fields(idx);
  char *t = xstrndup(s, n);
  set_strnum(&fields[idx], t);
  free(t);
}
static void split_record(void) {
  fields_valid = 1;
  const char *fs = getstr(&vars[V_FS].cell);
  /* in paragraph mode with the default FS, newlines are already blanks */
  nf = split_with(record.s ? record.s : "", fs, paragraph_mode() && strcmp(fs, " "), field_cb, NULL);
  set_num(&vars[V_NF].cell, nf);
}

/* ---------- input ---------- */
struct input { char *name; FILE *f; int is_pipe; struct sbuf pend; size_t ppos; int eof; };
static struct input *inputs;
static int ninputs;
/* read one record with the current RS; 1 if read */
static int read_record(struct input *in, struct sbuf *out) {
  const char *rs = getstr(&vars[V_RS].cell);
  out->len = 0;
  sb_add(out, "", 0);
  int c;
  if (strlen(rs) <= 1 && rs[0] != 0) {
    int sep = rs[0];
    int any = 0;
    while ((c = getc(in->f)) != EOF) {
      any = 1;
      if (c == sep) return 1;
      sb_addc(out, c);
    }
    return any;
  }
  if (rs[0] == 0) {
    /* paragraph mode: skip leading newlines; a record ends at one or more blank lines */
    while ((c = getc(in->f)) == '\n') {}
    if (c == EOF) return 0;
    sb_addc(out, c);
    while ((c = getc(in->f)) != EOF) {
      if (c == '\n') {
        int d = getc(in->f);
        if (d == '\n') {
          while ((d = getc(in->f)) == '\n') {}
          if (d != EOF) ungetc(d, in->f);
          return 1;
        }
        if (d == EOF) return 1;
        sb_addc(out, '\n');
        ungetc(d, in->f);
        continue;
      }
      sb_addc(out, c);
    }
    return 1;
  }
  /* RS as a regular expression: read what is left of the input and cut at matches */
  if (!in->eof) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in->f)) > 0) sb_add(&in->pend, buf, n);
    in->eof = 1;
  }
  if (!in->pend.s || in->ppos >= in->pend.len) return 0;
  regex_t *re = dyn_regex(rs);
  regmatch_t m[1];
  m[0].rm_so = in->ppos; m[0].rm_eo = in->pend.len;
  if (regexec(re, in->pend.s, 1, m, REG_UC_OFFSET) == 0 && m[0].rm_eo > m[0].rm_so) {
    sb_add(out, in->pend.s + in->ppos, m[0].rm_so - in->ppos);
    in->ppos = m[0].rm_eo;
  } else {
    sb_add(out, in->pend.s + in->ppos, in->pend.len - in->ppos);
    in->ppos = in->pend.len;
  }
  return 1;
}

/* main input: the files named in ARGV, with var=value assignments between them */
static int argi = 1, main_open, main_done, used_file;
static struct input main_in;
static void assign_var_from_cmdline(const char *a);
static int open_next_main(void) {
  for (;;) {
    int argc = (int)getnum(&vars[V_ARGC].cell);
    if (argi >= argc) {
      if (used_file) return 0;
      used_file = 1;
      main_in.f = stdin;
      main_in.name = "-";
      set_str(&vars[V_FILENAME].cell, "");
      return 1;
    }
    char key[32];
    snprintf(key, sizeof key, "%d", argi++);
    Cell *c = arr_get(to_array(&vars[V_ARGV].cell), key, 0);
    if (!c) continue;
    const char *a = getstr(c);
    if (!*a) continue;
    const char *eq = strchr(a, '=');
    if (eq && eq > a && (isalpha((unsigned char)a[0]) || a[0] == '_')) {
      int ok = 1;
      for (const char *p = a; p < eq; p++) if (!isalnum((unsigned char)*p) && *p != '_') ok = 0;
      if (ok) { assign_var_from_cmdline(a); continue; }
    }
    used_file = 1;
    main_in.f = strcmp(a, "-") ? fopen(a, "r") : stdin;
    if (!main_in.f) {
      fflush(stdout);
      fatal("cannot open \"%s\" (%s)", a, strerror(errno)); /* gawk stops here */
    }
    main_in.name = (char *)a;
    set_str(&vars[V_FILENAME].cell, a);
    return 1;
  }
}
static int exit_status;
static int read_main(struct sbuf *out) {
  if (main_done) return 0;
  for (;;) {
    if (!main_open) {
      if (!open_next_main()) { main_done = 1; return 0; }
      main_open = 1;
      set_num(&vars[V_FNR].cell, 0);
      main_in.pend.len = 0; main_in.ppos = 0; main_in.eof = 0;
    }
    if (read_record(&main_in, out)) return 1;
    if (main_in.f != stdin) fclose(main_in.f);
    main_open = 0;
  }
}
static void nextfile(void) {
  if (main_open) { if (main_in.f != stdin) fclose(main_in.f); main_open = 0; }
}

/* ---------- output streams ---------- */
struct ostream { char *name; FILE *f; int kind; }; /* kind: '>' file, '|' pipe */
static struct ostream *outs;
static int nouts;
static FILE *get_out(const char *name, int mode) {
  for (int i = 0; i < nouts; i++) if (outs[i].f && !strcmp(outs[i].name, name) && (outs[i].kind == '|') == (mode == '|')) return outs[i].f;
  FILE *f;
  if (mode == '|') { fflush(stdout); f = popen(name, "w"); }
  else if (!strcmp(name, "/dev/stdout") || !strcmp(name, "-")) f = stdout;
  else if (!strcmp(name, "/dev/stderr")) f = stderr;
  else f = fopen(name, mode == 'a' ? "a" : "w");
  if (!f) fatal("cannot redirect to \"%s\" (%s)", name, strerror(errno));
  outs = xrealloc(outs, (nouts + 1) * sizeof *outs);
  outs[nouts].name = xstrdup(name);
  outs[nouts].f = f;
  outs[nouts].kind = mode == '|' ? '|' : '>';
  nouts++;
  return f;
}
static int close_stream(const char *name) {
  int rc = -1, found = 0;
  for (int i = 0; i < nouts; i++) {
    if (outs[i].f && !strcmp(outs[i].name, name)) {
      found = 1;
      if (outs[i].kind == '|') { int st = pclose(outs[i].f); rc = WIFEXITED(st) ? WEXITSTATUS(st) : 256 + WTERMSIG(st); }
      else if (outs[i].f == stdout || outs[i].f == stderr) rc = fflush(outs[i].f);
      else rc = fclose(outs[i].f);
      outs[i].f = NULL;
    }
  }
  for (int i = 0; i < ninputs; i++) {
    if (inputs[i].f && !strcmp(inputs[i].name, name)) {
      found = 1;
      if (inputs[i].is_pipe) { int st = pclose(inputs[i].f); rc = WIFEXITED(st) ? WEXITSTATUS(st) : 256 + WTERMSIG(st); }
      else rc = inputs[i].f == stdin ? 0 : fclose(inputs[i].f);
      inputs[i].f = NULL;
      free(inputs[i].pend.s);
      inputs[i].pend = (struct sbuf){0};
    }
  }
  return found ? rc : -1;
}
static void flush_all(void) {
  fflush(stdout);
  for (int i = 0; i < nouts; i++) if (outs[i].f) fflush(outs[i].f);
}

/* ---------- regular expressions from strings ---------- */
struct recache { char *src; regex_t *re; };
static struct recache recache[64];
static int recache_next;
static regex_t *dyn_regex(const char *s) {
  for (int i = 0; i < 64; i++) if (recache[i].src && !strcmp(recache[i].src, s)) return recache[i].re;
  struct recache *e = &recache[recache_next];
  recache_next = (recache_next + 1) % 64;
  if (e->src) { free(e->src); regfree(e->re); free(e->re); }
  e->src = xstrdup(s);
  e->re = compile_ere(s);
  return e->re;
}

/* ---------- interpreter ---------- */
enum { R_NORMAL, R_BREAK, R_CONTINUE, R_NEXT, R_NEXTFILE, R_EXIT, R_RETURN };
static int unwinding; /* next or exit seen inside a function called from an expression */
struct frame { Cell **locals; int n; Cell retval; };
static struct frame *frame;

static void eval(Node *n, Cell *out);
static Cell *lvalue(Node *n);
static int exec(Node *s);

static char *subscript_key(Node *list) {
  if (list->nargs <= 1 && !list->next) { Cell c = tmpcell(); eval(list, &c); char *k = xstrdup(getstr(&c)); cell_free(&c); return k; }
  struct sbuf b = {0};
  const char *subsep = getstr(&vars[V_SUBSEP].cell);
  int first = 1;
  for (Node *x = list; x; x = x->next) {
    Cell c = tmpcell();
    eval(x, &c);
    if (!first) sb_adds(&b, subsep);
    sb_adds(&b, getstr(&c));
    cell_free(&c);
    first = 0;
  }
  return b.s ? b.s : xstrdup("");
}
static Cell *var_cell(Node *n) {
  if (n->type == N_LOCAL) return frame->locals[n->idx];
  return &vars[n->idx].cell;
}
static struct array *array_of(Node *n) { return to_array(var_cell(n)); }

/* after writing to a special variable */
static void special_assigned(Cell *c) {
  if (c == &vars[V_NF].cell) set_nf((int)getnum(c));
  else if (c == &vars[V_CONVFMT].cell) { free(convfmt); convfmt = xstrdup(getstr(c)); }
  else if (c == &vars[V_OFMT].cell) { free(ofmt); ofmt = xstrdup(getstr(c)); }
  else if (c == &field0) { set_record(getstr(c), strlen(getstr(c))); }
}
static Cell *lvalue(Node *n) {
  switch (n->type) {
  case N_VAR: case N_LOCAL: {
    Cell *c = var_cell(n);
    if (c == &vars[V_NF].cell && !fields_valid) split_record();
    return c;
  }
  case N_FIELD: { Cell i = tmpcell(); eval(n->a, &i); int k = (int)getnum(&i); cell_free(&i); return field_lvalue(k); }
  case N_INDEX: { char *k = subscript_key(n->b); Cell *c = arr_get(array_of(n->a), k, 1); free(k); return c; }
  }
  fatal("assignment to something that is not a variable");
}
/* numbers, numeric-looking input and uninitialized values compare as numbers */
static int is_numeric(Cell *c) { return (c->flags & (F_NUMVAL | F_STRNUM)) || !(c->flags & (F_NUM | F_STR)); }
static int compare(Cell *a, Cell *b) {
  int na = is_numeric(a), nb = is_numeric(b);
  if (na && nb) { double x = getnum(a), y = getnum(b); return x < y ? -1 : x > y ? 1 : x == y ? 0 : 1; /* NaN is never equal */ }
  return strcmp(getstr(a), getstr(b));
}
static int truth(Cell *c) {
  if (c->flags & (F_NUMVAL | F_STRNUM)) return getnum(c) != 0;
  if (!(c->flags & (F_NUM | F_STR))) return 0;
  return getstr(c)[0] != 0;
}
static int match_cell(Node *ren, Cell *subject, regmatch_t *m, int nm) {
  regex_t *re;
  Cell r = tmpcell();
  if (ren->type == N_ERE) re = ren->re;
  else { eval(ren, &r); re = dyn_regex(getstr(&r)); }
  int ok = regexec(re, getstr(subject), nm, m, 0) == 0;
  cell_free(&r);
  return ok;
}

static double awk_rand_seed, prev_seed = 1; /* gawk reports 1 as the first previous seed */
static unsigned long long rand_state = 0x853c49e6748fea9bULL;
static double awk_rand(void) {
  rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
  return (double)(rand_state >> 11) / 9007199254740992.0;
}
static void awk_srand(double s) { rand_state = (unsigned long long)(long long)s * 2654435761ULL + 0x853c49e6748fea9bULL; awk_rand(); }

/* printf formatting */
static void format(struct sbuf *out, const char *fmt, Node *args) {
  const char *p = fmt;
  while (*p) {
    if (*p != '%') { sb_addc(out, *p++); continue; }
    if (p[1] == '%') { sb_addc(out, '%'); p += 2; continue; }
    char spec[64];
    int k = 0;
    spec[k++] = *p++;
    while (*p && strchr("-+ #0", *p) && k < 40) spec[k++] = *p++;
    while (k < 50 && (isdigit((unsigned char)*p) || *p == '*' || *p == '.')) {
      if (*p == '*') {
        Cell c = tmpcell();
        if (args) { eval(args, &c); args = args->next; }
        k += snprintf(spec + k, sizeof spec - k, "%d", (int)getnum(&c));
        cell_free(&c);
        p++;
      } else spec[k++] = *p++;
    }
    while (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't') p++;
    char conv = *p;
    if (!conv) { spec[k] = 0; sb_adds(out, spec); break; }
    p++;
    if (conv == '%') { sb_addc(out, '%'); continue; }
    Cell c = tmpcell();
    int have = args != NULL;
    if (!have && strchr("diouxXeEfFgGaAcs", conv)) fatal("not enough arguments to satisfy format string");
    if (args) { eval(args, &c); args = args->next; }
    char tmp[512], *big = NULL;
    int n = 0;
    switch (conv) {
    case 'd': case 'i': case 'o': case 'x': case 'X': case 'u': {
      double d = getnum(&c);
      if (isnan(d) || isinf(d)) {
        spec[k] = 0;
        char s2[80];
        snprintf(s2, sizeof s2, "%sf", spec);
        n = snprintf(tmp, sizeof tmp, s2, d);
        break;
      }
      spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv == 'i' ? 'd' : conv; spec[k] = 0;
      long long v = d >= 9.2233720368547758e18 ? 0x7fffffffffffffffLL : d <= -9.2233720368547758e18 ? (-0x7fffffffffffffffLL - 1) : (long long)d;
      if (conv == 'd' || conv == 'i') n = snprintf(tmp, sizeof tmp, spec, v);
      else n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
      break;
    }
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
      spec[k++] = conv; spec[k] = 0;
      n = snprintf(tmp, sizeof tmp, spec, getnum(&c));
      break;
    case 'c': {
      char ch[2] = {0, 0};
      if (have && (c.flags & F_NUMVAL)) ch[0] = (char)(int)getnum(&c);
      else { const char *s = have ? getstr(&c) : ""; ch[0] = s[0]; }
      spec[k++] = 's'; spec[k] = 0;
      if (ch[0] == 0) {
        /* a NUL byte: written as it is */
        int w = atoi(spec + 1);
        int left = strchr(spec, '-') != NULL;
        if (!left) for (int i = 1; i < w; i++) sb_addc(out, ' ');
        sb_add(out, "", 0); out->s[out->len++] = 0; out->s[out->len] = 0;
        if (left) for (int i = 1; i < w; i++) sb_addc(out, ' ');
        n = -1;
      } else n = snprintf(tmp, sizeof tmp, spec, ch);
      break;
    }
    case 's': {
      spec[k++] = 's'; spec[k] = 0;
      const char *s = have ? getstr(&c) : "";
      int need = snprintf(NULL, 0, spec, s);
      if (need >= (int)sizeof tmp) { big = xmalloc(need + 1); snprintf(big, need + 1, spec, s); n = need; }
      else n = snprintf(tmp, sizeof tmp, spec, s);
      break;
    }
    default:
      /* not a conversion: printed as it is */
      spec[k++] = conv; spec[k] = 0;
      sb_adds(out, spec);
      n = -1;
      if (have) { /* the argument was not used */ }
    }
    if (n >= 0) sb_add(out, big ? big : tmp, big ? (size_t)n : (n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1));
    free(big);
    cell_free(&c);
  }
}

static double do_sub(Node *n, int global) {
  Node *re = n->a, *repl = n->a->next, *target = repl ? repl->next : NULL;
  Cell rc = tmpcell();
  eval(repl, &rc);
  const char *r = getstr(&rc);
  Cell tv = tmpcell();
  int assignable = !target || is_lvalue(target);
  if (target && !assignable) eval(target, &tv); /* gawk allows it; the result is dropped */
  Cell *t = !target ? get_field(0) : assignable ? lvalue(target) : &tv;
  char *subject = xstrdup(getstr(t));
  regex_t *rx;
  Cell rec = tmpcell();
  if (re->type == N_ERE) rx = re->re;
  else { eval(re, &rec); rx = dyn_regex(getstr(&rec)); }
  struct sbuf out = {0};
  sb_add(&out, "", 0);
  size_t len = strlen(subject), pos = 0;
  int count = 0;
  long prev_end = -1;
  while (pos <= len) {
    regmatch_t m[1];
    m[0].rm_so = pos; m[0].rm_eo = len;
    if (regexec(rx, subject, 1, m, REG_UC_OFFSET) != 0) break;
    size_t so = m[0].rm_so, eo = m[0].rm_eo;
    if (so == eo && (long)so == prev_end) { /* no empty match right after a match */
      if (so >= len) break;
      sb_addc(&out, subject[so]);
      pos = so + 1;
      continue;
    }
    sb_add(&out, subject + pos, so - pos);
    /* the replacement: & is the match, \& a literal &, \\ a backslash */
    for (const char *q = r; *q; q++) {
      if (*q == '\\' && q[1] == '&') { sb_addc(&out, '&'); q++; }
      else if (*q == '\\' && q[1] == '\\') { sb_addc(&out, '\\'); q++; }
      else if (*q == '&') sb_add(&out, subject + so, eo - so);
      else sb_addc(&out, *q);
    }
    count++;
    prev_end = eo;
    if (so == eo) {
      if (so < len) sb_addc(&out, subject[so]);
      pos = so + 1;
    } else pos = eo;
    if (!global) break;
  }
  if (count && assignable) {
    if (pos < len) sb_add(&out, subject + pos, len - pos);
    Cell *dst = target ? lvalue(target) : field_lvalue(0);
    set_str(dst, out.s);
    if (dst == &field0) special_assigned(dst);
    else if (target && target->type == N_VAR) special_assigned(dst);
  }
  free(out.s);
  free(subject);
  cell_free(&rc);
  cell_free(&rec);
  cell_free(&tv);
  return count;
}

struct split_ctx { struct array *a; };
static void split_arr_cb(void *ctx, int idx, const char *s, size_t n) {
  struct split_ctx *c = ctx;
  char key[32];
  snprintf(key, sizeof key, "%d", idx);
  char *t = xstrndup(s, n);
  set_strnum(arr_get(c->a, key, 1), t);
  free(t);
}

static int getline_into(Node *lv, struct sbuf *rec, int count_nr) {
  if (lv) {
    Cell *c = lvalue(lv);
    set_strnum(c, rec->s);
    special_assigned(c);
  } else {
    set_record(rec->s, rec->len);
  }
  if (count_nr) { set_num(&vars[V_NR].cell, getnum(&vars[V_NR].cell) + 1); set_num(&vars[V_FNR].cell, getnum(&vars[V_FNR].cell) + 1); }
  return 1;
}
static struct input *get_input(const char *name, int is_pipe) {
  for (int i = 0; i < ninputs; i++) if (inputs[i].f && !strcmp(inputs[i].name, name) && inputs[i].is_pipe == is_pipe) return &inputs[i];
  FILE *f;
  if (is_pipe) { flush_all(); f = popen(name, "r"); }
  else f = !strcmp(name, "-") || !strcmp(name, "/dev/stdin") ? stdin : fopen(name, "r");
  if (!f) return NULL;
  inputs = xrealloc(inputs, (ninputs + 1) * sizeof *inputs);
  struct input *in = &inputs[ninputs++];
  memset(in, 0, sizeof *in);
  in->name = xstrdup(name);
  in->f = f;
  in->is_pipe = is_pipe;
  return in;
}

static void call(Node *n, Cell *out);
static void eval(Node *n, Cell *out) {
  cell_free(out);
  switch (n->type) {
  case N_NUM: set_num(out, n->num); return;
  case N_STR: set_str(out, n->str); return;
  case N_ERE: set_num(out, regexec(n->re, getstr(get_field(0)), 0, NULL, 0) == 0); return;
  case N_GROUP: eval(n->a, out); return;
  case N_VAR: case N_LOCAL: {
    Cell *c = var_cell(n);
    if (c == &vars[V_NF].cell && !fields_valid) split_record();
    if (c->flags & F_ARR) fatal("attempt to use array '%s' in a scalar context", n->type == N_VAR ? vars[n->idx].name : "parameter");
    set_cell(out, c);
    return;
  }
  case N_FIELD: { Cell i = tmpcell(); eval(n->a, &i); double d = getnum(&i); cell_free(&i); set_cell(out, get_field((int)d)); return; }
  case N_INDEX: {
    char *k = subscript_key(n->b);
    Cell *c = arr_get(array_of(n->a), k, 1);
    free(k);
    set_cell(out, c);
    return;
  }
  case N_ASSIGN: {
    Cell v = tmpcell();
    eval(n->b, &v);
    Cell *dst = lvalue(n->a);
    if (n->op == '=') set_cell(dst, &v);
    else {
      double x = getnum(dst), y = getnum(&v), r;
      switch (n->op) {
      case '+': r = x + y; break;
      case '-': r = x - y; break;
      case '*': r = x * y; break;
      case '/': if (y == 0) fatal("division by zero in /="); r = x / y; break;
      case '%': if (y == 0) fatal("division by zero in %%="); r = fmod(x, y); break;
      default: r = pow(x, y);
      }
      set_num(dst, r);
    }
    special_assigned(dst);
    set_cell(out, dst);
    cell_free(&v);
    return;
  }
  case N_COND: { Cell c = tmpcell(); eval(n->a, &c); int t = truth(&c); cell_free(&c); eval(t ? n->b : n->c, out); return; }
  case N_OR: case N_AND: {
    Cell c = tmpcell();
    eval(n->a, &c);
    int t = truth(&c);
    if (n->type == N_OR ? !t : t) { eval(n->b, &c); t = truth(&c); }
    cell_free(&c);
    set_num(out, t);
    return;
  }
  case N_IN: {
    char *k = subscript_key(n->a);
    int r = arr_get(array_of(n->b), k, 0) != NULL;
    free(k);
    set_num(out, r);
    return;
  }
  case N_MATCH: {
    Cell s = tmpcell();
    eval(n->a, &s);
    int r = match_cell(n->b, &s, NULL, 0);
    cell_free(&s);
    set_num(out, r != n->op);
    return;
  }
  case N_CMP: {
    Cell a = tmpcell(), b = tmpcell();
    eval(n->a, &a);
    eval(n->b, &b);
    int c = compare(&a, &b), r;
    switch (n->op) {
    case '<': r = c < 0; break;
    case 'l': r = c <= 0; break;
    case '!': r = c != 0; break;
    case '=': r = c == 0; break;
    case '>': r = c > 0; break;
    default: r = c >= 0;
    }
    cell_free(&a); cell_free(&b);
    set_num(out, r);
    return;
  }
  case N_CONCAT: {
    Cell a = tmpcell(), b = tmpcell();
    eval(n->a, &a);
    eval(n->b, &b);
    const char *x = getstr(&a), *y = getstr(&b);
    size_t lx = strlen(x), ly = strlen(y);
    char *s = xmalloc(lx + ly + 1);
    memcpy(s, x, lx); memcpy(s + lx, y, ly + 1);
    free(out->str);
    out->str = s;
    out->flags = F_STR;
    cell_free(&a); cell_free(&b);
    return;
  }
  case N_ARITH: {
    Cell a = tmpcell(), b = tmpcell();
    eval(n->a, &a);
    eval(n->b, &b);
    double x = getnum(&a), y = getnum(&b), r;
    switch (n->op) {
    case '+': r = x + y; break;
    case '-': r = x - y; break;
    case '*': r = x * y; break;
    case '/': if (y == 0) fatal("division by zero"); r = x / y; break;
    case '%': if (y == 0) fatal("division by zero in %%"); r = fmod(x, y); break;
    default: r = pow(x, y);
    }
    cell_free(&a); cell_free(&b);
    set_num(out, r);
    return;
  }
  case N_NEG: { Cell a = tmpcell(); eval(n->a, &a); set_num(out, -getnum(&a)); cell_free(&a); return; }
  case N_PLUS: { Cell a = tmpcell(); eval(n->a, &a); set_num(out, getnum(&a)); cell_free(&a); return; }
  case N_NOT: { Cell a = tmpcell(); eval(n->a, &a); set_num(out, !truth(&a)); cell_free(&a); return; }
  case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: {
    Cell *c = lvalue(n->a);
    double old = getnum(c);
    set_num(c, old + (n->type == N_PREINC || n->type == N_POSTINC ? 1 : -1));
    special_assigned(c);
    set_num(out, n->type == N_PREINC || n->type == N_PREDEC ? getnum(c) : old);
    return;
  }
  case N_CALL: call(n, out); return;
  case N_GETLINE: {
    struct sbuf rec = {0};
    int r;
    if (n->op == '|' || n->op == '<') {
      Cell nm = tmpcell();
      eval(n->b, &nm);
      struct input *in = get_input(getstr(&nm), n->op == '|');
      cell_free(&nm);
      if (!in) r = -1;
      else if (!read_record(in, &rec)) r = 0;
      else r = getline_into(n->a, &rec, n->op == '|' && !n->a ? 1 : n->op == '|');
      if (r == 1 && n->op == '<' && !n->a) { /* getline < file sets $0 and NF only */ }
      if (r == 1 && n->op == '|') { /* NR counted above; FNR too, as gawk */ }
    } else {
      if (!read_main(&rec)) r = 0;
      else r = getline_into(n->a, &rec, 1);
    }
    free(rec.s);
    set_num(out, r);
    return;
  }
  case N_BUILTIN: {
    Node *a = n->a;
    Cell x = tmpcell(), y = tmpcell(), z = tmpcell();
    double r = 0;
    int isstr = 0;
    char *sres = NULL;
    switch (n->idx) {
    case B_LENGTH:
      if (!a) { r = strlen(getstr(get_field(0))); break; }
      if ((a->type == N_VAR || a->type == N_LOCAL) && (var_cell(a)->flags & F_ARR)) { r = var_cell(a)->arr->n; break; }
      eval(a, &x); r = strlen(getstr(&x)); break;
    case B_SUBSTR: {
      if (n->nargs < 2) fatal("substr: needs 2 or 3 arguments");
      eval(a, &x); eval(a->next, &y);
      const char *s = getstr(&x);
      double len = strlen(s), m = getnum(&y), cnt = len;
      if (n->nargs > 2) { eval(a->next->next, &z); cnt = getnum(&z); }
      /* as gawk does here: both numbers are truncated; a start below 1 becomes 1
         without shortening the length */
      if (isnan(m) || isnan(cnt)) { sres = xstrdup(""); isstr = 1; break; }
      double start = m < 0 ? ceil(m) : floor(m), count = cnt < 0 ? ceil(cnt) : floor(cnt);
      if (start < 1) start = 1;
      if (count <= 0 || start > len) sres = xstrdup("");
      else {
        if (count > len - start + 1) count = len - start + 1;
        sres = xstrndup(s + (size_t)start - 1, (size_t)count);
      }
      isstr = 1;
      break;
    }
    case B_INDEX: {
      eval(a, &x); eval(a->next, &y);
      const char *p = strstr(getstr(&x), getstr(&y));
      r = p ? p - x.str + 1 : 0;
      break;
    }
    case B_SPLIT: {
      if (n->nargs < 2) fatal("split: needs 2 or 3 arguments");
      eval(a, &x);
      char *s = xstrdup(getstr(&x));
      Node *an = a->next;
      if (an->type != N_VAR && an->type != N_LOCAL) fatal("split: second argument is not an array");
      struct array *arr = array_of(an);
      const char *fs;
      Cell fsc = tmpcell();
      if (n->nargs > 2) {
        Node *sep = an->next;
        if (sep->type == N_ERE) fs = sep->str;
        else { eval(sep, &fsc); fs = getstr(&fsc); }
      } else fs = getstr(&vars[V_FS].cell);
      char *fsd = xstrdup(fs);
      arr_clear(arr);
      struct split_ctx ctx = {arr};
      if (n->nargs > 2 && an->next->type == N_ERE) {
        /* /re/ as a separator is always a regular expression, even / / or /x/ */
        char *pat = xmalloc(strlen(fsd) + 3);
        sprintf(pat, "(%s)", fsd);
        r = split_with(s, pat, 0, split_arr_cb, &ctx);
        free(pat);
      } else r = split_with(s, fsd, 0, split_arr_cb, &ctx);
      free(fsd); free(s); cell_free(&fsc);
      break;
    }
    case B_SUB: case B_GSUB:
      if (n->nargs < 2) fatal("%s: needs 2 or 3 arguments", n->idx == B_SUB ? "sub" : "gsub");
      if (n->nargs == 3 && !is_lvalue(a->next->next)) {
        /* gawk: a non-lvalue target is allowed (the result is lost) */
      }
      r = do_sub(n, n->idx == B_GSUB);
      break;
    case B_MATCH: {
      eval(a, &x);
      regmatch_t m[1];
      int ok = match_cell(a->next, &x, m, 1);
      r = ok ? m[0].rm_so + 1 : 0;
      set_num(&vars[V_RSTART].cell, r);
      set_num(&vars[V_RLENGTH].cell, ok ? m[0].rm_eo - m[0].rm_so : -1);
      break;
    }
    case B_SPRINTF: {
      if (!a) fatal("sprintf: no format");
      eval(a, &x);
      struct sbuf b = {0};
      sb_add(&b, "", 0);
      char *f = xstrdup(getstr(&x));
      format(&b, f, a->next);
      free(f);
      sres = b.s;
      isstr = 1;
      break;
    }
    case B_SIN: eval(a, &x); r = sin(getnum(&x)); break;
    case B_COS: eval(a, &x); r = cos(getnum(&x)); break;
    case B_ATAN2: eval(a, &x); eval(a->next, &y); r = atan2(getnum(&x), getnum(&y)); break;
    case B_EXP: eval(a, &x); r = exp(getnum(&x)); break;
    case B_LOG: eval(a, &x); if (getnum(&x) < 0) fprintf(stderr, "%s: warning: log: received negative argument %g\n", progname, getnum(&x)); r = log(getnum(&x)); break;
    case B_SQRT: eval(a, &x); if (getnum(&x) < 0) fprintf(stderr, "%s: warning: sqrt: called with negative argument %g\n", progname, getnum(&x)); r = sqrt(getnum(&x)); break;
    case B_INT: eval(a, &x); { double d = getnum(&x); r = d < 0 ? ceil(d) : floor(d); } break;
    case B_RAND: r = awk_rand(); break;
    case B_SRAND:
      r = prev_seed;
      if (a) { eval(a, &x); awk_rand_seed = getnum(&x); } else awk_rand_seed = (double)time(NULL);
      prev_seed = awk_rand_seed;
      awk_srand(awk_rand_seed);
      break;
    case B_TOLOWER: case B_TOUPPER: {
      eval(a, &x);
      sres = xstrdup(getstr(&x));
      for (char *p = sres; *p; p++) *p = n->idx == B_TOLOWER ? tolower((unsigned char)*p) : toupper((unsigned char)*p);
      isstr = 1;
      break;
    }
    case B_SYSTEM: {
      eval(a, &x);
      flush_all();
      int st = system(getstr(&x));
      r = st == -1 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : 256 + WTERMSIG(st);
      break;
    }
    case B_CLOSE: eval(a, &x); r = close_stream(getstr(&x)); break;
    case B_FFLUSH:
      if (!a) flush_all();
      else {
        eval(a, &x);
        const char *nm = getstr(&x);
        r = -1;
        for (int i = 0; i < nouts; i++) if (outs[i].f && !strcmp(outs[i].name, nm)) { fflush(outs[i].f); r = 0; }
        if (!*nm) { flush_all(); r = 0; }
      }
      break;
    case B_SYSTIME: r = (double)time(NULL); break;
    }
    cell_free(&x); cell_free(&y); cell_free(&z);
    if (isstr) { free(out->str); out->str = sres; out->flags = F_STR; }
    else set_num(out, r);
    return;
  }
  }
  fatal("internal error: cannot evaluate node %d", n->type);
}

static int call_depth;
static char *stack_base;
#ifdef __wasm__
#define STACK_ROOM (768 * 1024) /* programs get a 1 MiB stack */
#else
#define STACK_ROOM (4 * 1024 * 1024)
#endif
static void call(Node *n, Cell *out) {
  struct func *f = &funcs[n->idx];
  if (n->nargs > f->nparams) fatal("function '%s' called with %d arguments, defined with %d", f->name, n->nargs, f->nparams);
  char here;
  long used = stack_base - &here;
  if (used < 0) used = -used;
  if (used > STACK_ROOM) fatal("function call nesting too deep (%d calls)", call_depth);
  call_depth++;
  struct frame fr;
  fr.n = f->nparams;
  fr.locals = xmalloc((fr.n + 1) * sizeof *fr.locals);
  fr.retval = tmpcell();
  char *owned = calloc(fr.n + 1, 1);
  Node *a = n->a;
  for (int i = 0; i < fr.n; i++, a = a ? a->next : NULL) {
    if (a && (a->type == N_VAR || a->type == N_LOCAL)) {
      Cell *c = var_cell(a);
      if (c->flags & F_ARR || !(c->flags & (F_NUM | F_STR))) {
        /* an array (or an untyped name that may become one) is passed by reference */
        if (c->flags & F_ARR) { fr.locals[i] = c; continue; }
        if (!(c->flags & (F_NUM | F_STR))) { fr.locals[i] = c; owned[i] = 2; continue; }
      }
    }
    fr.locals[i] = xmalloc(sizeof(Cell));
    *fr.locals[i] = tmpcell();
    owned[i] = 1;
    if (a) eval(a, fr.locals[i]);
  }
  struct frame *saved = frame;
  frame = &fr;
  int r = exec(f->body);
  frame = saved;
  if (r == R_EXIT || r == R_NEXT || r == R_NEXTFILE) unwinding = r;
  for (int i = 0; i < fr.n; i++) {
    if (owned[i] == 1) {
      if (fr.locals[i]->flags & F_ARR) { arr_clear(fr.locals[i]->arr); free(fr.locals[i]->arr->tab); free(fr.locals[i]->arr); }
      else cell_free(fr.locals[i]);
      free(fr.locals[i]);
    } else if (owned[i] == 2 && !(fr.locals[i]->flags & F_ARR)) {
      /* an untyped caller variable used as a scalar inside: scalars pass by value,
         so the caller's variable goes back to untyped */
      cell_free(fr.locals[i]);
    }
  }
  free(owned);
  free(fr.locals);
  *out = fr.retval;
  call_depth--;
}

static void print_cell(FILE *f, Cell *c) {
  if (c->flags & F_NUMVAL) {
    char *s = num2str(c->num, ofmt);
    fputs(s, f);
    free(s);
  } else fputs(getstr(c), f);
}
static FILE *redirect(Node *s) {
  if (!s->op) return stdout;
  Cell nm = tmpcell();
  eval(s->b, &nm);
  FILE *f = get_out(getstr(&nm), s->op);
  cell_free(&nm);
  return f;
}

/* a loop that makes no system call would freeze the browser tab: make a cheap one now and then */
static void breathe(void) {
#ifdef __wasm__
  static unsigned n;
  if ((++n & 0xffff) == 0) getpid();
#endif
}
static int exec_list(Node *s) {
  for (; s; s = s->next) {
    int r = exec(s);
    if (r != R_NORMAL) return r;
  }
  return R_NORMAL;
}
static Cell exit_cell;
static int exec(Node *s) {
  if (!s) return R_NORMAL;
  int r;
  switch (s->type) {
  case S_BLOCK: return exec_list(s->a);
  case S_EXPR: { Cell c = tmpcell(); eval(s->a, &c); cell_free(&c); break; }
  case S_PRINT: {
    FILE *f;
    Cell *vals = calloc(s->nargs + 1, sizeof *vals);
    int i = 0;
    for (Node *a = s->a; a; a = a->next, i++) eval(a, &vals[i]);
    f = redirect(s);
    if (!s->a) fputs(getstr(get_field(0)), f);
    const char *ofs = getstr(&vars[V_OFS].cell);
    for (i = 0; i < s->nargs; i++) {
      if (i) fputs(ofs, f);
      print_cell(f, &vals[i]);
      cell_free(&vals[i]);
    }
    free(vals);
    fputs(getstr(&vars[V_ORS].cell), f);
    if (ferror(f)) fatal("write error: %s", strerror(errno));
    if (f != stdout || isatty(1)) fflush(f);
    break;
  }
  case S_PRINTF: {
    if (!s->a) fatal("printf: no format");
    Cell fc = tmpcell();
    eval(s->a, &fc);
    struct sbuf b = {0};
    sb_add(&b, "", 0);
    char *fmt = xstrdup(getstr(&fc));
    format(&b, fmt, s->a->next);
    free(fmt);
    FILE *f = redirect(s);
    fwrite(b.s, 1, b.len, f);
    if (f != stdout) fflush(f);
    free(b.s);
    cell_free(&fc);
    break;
  }
  case S_IF: {
    Cell c = tmpcell();
    eval(s->a, &c);
    int t = truth(&c);
    cell_free(&c);
    if (unwinding) return unwinding;
    return exec(t ? s->b : s->c);
  }
  case S_WHILE:
    for (;;) {
      breathe();
      Cell c = tmpcell();
      eval(s->a, &c);
      int t = truth(&c);
      cell_free(&c);
      if (unwinding) return unwinding;
      if (!t) break;
      r = exec(s->b);
      if (r == R_BREAK) break;
      if (r == R_CONTINUE) continue;
      if (r != R_NORMAL) return r;
    }
    break;
  case S_DO:
    for (;;) {
      breathe();
      r = exec(s->b);
      if (r == R_BREAK) break;
      if (r != R_NORMAL && r != R_CONTINUE) return r;
      Cell c = tmpcell();
      eval(s->a, &c);
      int t = truth(&c);
      cell_free(&c);
      if (unwinding) return unwinding;
      if (!t) break;
    }
    break;
  case S_FOR:
    if (s->a) { Cell c = tmpcell(); eval(s->a, &c); cell_free(&c); }
    for (;;) {
      breathe();
      if (s->b) {
        Cell c = tmpcell();
        eval(s->b, &c);
        int t = truth(&c);
        cell_free(&c);
        if (unwinding) return unwinding;
        if (!t) break;
      }
      r = exec(s->d);
      if (r == R_BREAK) break;
      if (r != R_NORMAL && r != R_CONTINUE) return r;
      if (s->c) { Cell c = tmpcell(); eval(s->c, &c); cell_free(&c); }
      if (unwinding) return unwinding;
    }
    break;
  case S_FORIN: {
    struct array *a = array_of(s->b);
    /* iterate over a snapshot of the keys: the body may change the array */
    size_t n = 0, cap = a->n + 1;
    char **keys = xmalloc(cap * sizeof *keys);
    for (size_t i = 0; i < a->size; i++) for (struct aent *e = a->tab[i]; e; e = e->next) keys[n++] = xstrdup(e->key);
    r = R_NORMAL;
    for (size_t i = 0; i < n; i++) {
      if (!arr_get(a, keys[i], 0)) continue;
      Cell *v = lvalue(s->a);
      set_strnum(v, keys[i]);
      special_assigned(v);
      r = exec(s->c);
      if (r == R_BREAK) { r = R_NORMAL; break; }
      if (r == R_CONTINUE) { r = R_NORMAL; continue; }
      if (r != R_NORMAL) break;
    }
    for (size_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
    return r;
  }
  case S_NEXT: return R_NEXT;
  case S_NEXTFILE: return R_NEXTFILE;
  case S_BREAK: return R_BREAK;
  case S_CONTINUE: return R_CONTINUE;
  case S_EXIT:
    if (s->a) { eval(s->a, &exit_cell); exit_status = (int)getnum(&exit_cell); }
    return R_EXIT;
  case S_RETURN:
    if (s->a) eval(s->a, &frame->retval);
    return R_RETURN;
  case S_DELETE: {
    char *k = subscript_key(s->b);
    arr_del(array_of(s->a), k);
    free(k);
    break;
  }
  case S_DELETEALL: arr_clear(array_of(s->a)); break;
  default: fatal("internal error: cannot execute node %d", s->type);
  }
  if (unwinding) return unwinding;
  return R_NORMAL;
}

/* ---------- main ---------- */
static void assign_var_from_cmdline(const char *a) {
  const char *eq = strchr(a, '=');
  char *name = xstrndup(a, eq - a);
  for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) fatal("cannot assign to function name '%s'", name);
  int idx = var_index(name);
  char *val = unescape(eq + 1, strlen(eq + 1));
  set_strnum(&vars[idx].cell, val);
  special_assigned(&vars[idx].cell);
  free(val);
  free(name);
}

static void run_end(void);
static void finish(int code) __attribute__((noreturn));
static void finish(int code) {
  flush_all();
  for (int i = 0; i < nouts; i++) if (outs[i].f && outs[i].kind == '|') pclose(outs[i].f); else if (outs[i].f && outs[i].f != stdout && outs[i].f != stderr) fclose(outs[i].f);
  if (fflush(stdout) != 0 && errno != EPIPE) { fprintf(stderr, "%s: write error: %s\n", progname, strerror(errno)); exit(2); }
  exit(code);
}

int main(int argc, char **argv, char **envp) {
  char base;
  stack_base = &base;
  struct sbuf prog = {0};
  int i = 1, have_prog = 0;
  const char *fs_opt = NULL;
  char **assigns = NULL;
  int nassigns = 0;
  for (int k = 0; k < (int)(sizeof special / sizeof *special); k++) var_index(special[k]);
  set_str(&vars[V_FS].cell, " ");
  set_str(&vars[V_OFS].cell, " ");
  set_str(&vars[V_ORS].cell, "\n");
  set_str(&vars[V_RS].cell, "\n");
  set_str(&vars[V_SUBSEP].cell, "\034");
  set_str(&vars[V_CONVFMT].cell, "%.6g");
  set_str(&vars[V_OFMT].cell, "%.6g");
  set_num(&vars[V_NR].cell, 0);
  set_num(&vars[V_NF].cell, 0);
  set_num(&vars[V_FNR].cell, 0);
  set_num(&vars[V_RSTART].cell, 0);
  set_num(&vars[V_RLENGTH].cell, -1);
  set_str(&vars[V_FILENAME].cell, "");
  convfmt = xstrdup("%.6g");
  ofmt = xstrdup("%.6g");
  set_record("", 0);
  struct array *env = to_array(&vars[V_ENVIRON].cell);
  for (char **e = envp; e && *e; e++) {
    char *eq = strchr(*e, '=');
    if (!eq) continue;
    char *k = xstrndup(*e, eq - *e);
    set_strnum(arr_get(env, k, 1), eq + 1);
    free(k);
  }
  for (; i < argc; i++) {
    char *a = argv[i];
    if (a[0] != '-' || !a[1]) break;
    if (!strcmp(a, "--")) { i++; break; }
    if (!strcmp(a, "--version")) { puts("awk (UC.OS)"); return 0; }
    if (a[1] == 'F' || a[1] == 'v' || a[1] == 'f') {
      char *v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
      if (!v) fatal("option requires an argument -- %c", a[1]);
      if (a[1] == 'F') fs_opt = v;
      else if (a[1] == 'v') {
        if (!strchr(v, '=') || v[0] == '=') fatal("invalid -v argument '%s'", v);
        assigns = xrealloc(assigns, (nassigns + 1) * sizeof *assigns);
        assigns[nassigns++] = v;
      } else {
        FILE *f = strcmp(v, "-") ? fopen(v, "r") : stdin;
        if (!f) fatal("cannot open source file '%s' (%s)", v, strerror(errno));
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_add(&prog, buf, n);
        sb_addc(&prog, '\n');
        if (f != stdin) fclose(f);
        have_prog = 1;
      }
      continue;
    }
    if (!strcmp(a, "--posix") || !strcmp(a, "--traditional") || !strcmp(a, "-P") || !strcmp(a, "--re-interval")) continue;
    fatal("unknown option '%s'", a);
  }
  if (!have_prog) {
    if (i >= argc) { fprintf(stderr, "usage: awk [-F fs] [-v var=value] [-f progfile | 'program'] [file ...]\n"); return 2; }
    sb_adds(&prog, argv[i++]);
  }
  if (fs_opt) {
    char *v = unescape(fs_opt, strlen(fs_opt)); /* gawk takes -Ft as the letter t */
    set_str(&vars[V_FS].cell, v);
    free(v);
  }
  /* ARGV: the program name, then the operands */
  struct array *av = to_array(&vars[V_ARGV].cell);
  set_str(arr_get(av, "0", 1), "awk");
  int n = 1;
  for (; i < argc; i++, n++) { char key[32]; snprintf(key, sizeof key, "%d", n); set_strnum(arr_get(av, key, 1), argv[i]); }
  set_num(&vars[V_ARGC].cell, n);
  src = prog.s ? prog.s : "";
  slen = prog.len;
  parse_program();
  for (int k = 0; k < nassigns; k++) assign_var_from_cmdline(assigns[k]);
  if (!isatty(1)) setvbuf(stdout, NULL, _IOFBF, 65536);

  int r = R_NORMAL;
  for (Node *b = begin_actions; b; b = b->next) {
    r = exec(b);
    if (r == R_EXIT) break;
    unwinding = 0;
  }
  if (r == R_EXIT) { run_end(); finish(exit_status); }
  unwinding = 0;
  if (nrules || end_actions) {
    struct sbuf rec = {0};
    while (read_main(&rec)) {
      set_num(&vars[V_NR].cell, getnum(&vars[V_NR].cell) + 1); /* the user may have assigned NR */
      set_num(&vars[V_FNR].cell, getnum(&vars[V_FNR].cell) + 1);
      set_record(rec.s, rec.len);
      for (int k = 0; k < nrules; k++) {
        struct rule *ru = &rules[k];
        int hit;
        if (!ru->pattern) hit = 1;
        else if (ru->pattern2) {
          if (!ru->range_active) {
            Cell c = tmpcell(); eval(ru->pattern, &c); hit = truth(&c); cell_free(&c);
            if (hit) { Cell d = tmpcell(); eval(ru->pattern2, &d); ru->range_active = !truth(&d); cell_free(&d); }
          } else {
            hit = 1;
            Cell d = tmpcell(); eval(ru->pattern2, &d); if (truth(&d)) ru->range_active = 0; cell_free(&d);
          }
        } else { Cell c = tmpcell(); eval(ru->pattern, &c); hit = truth(&c); cell_free(&c); }
        if (unwinding) { r = unwinding; unwinding = 0; }
        else if (!hit) continue;
        else if (!ru->action) { fputs(getstr(get_field(0)), stdout); fputs(getstr(&vars[V_ORS].cell), stdout); r = R_NORMAL; }
        else { r = exec(ru->action); unwinding = 0; }
        if (r == R_NEXT) { r = R_NORMAL; break; }
        if (r == R_NEXTFILE) { nextfile(); r = R_NORMAL; break; }
        if (r == R_EXIT) break;
      }
      if (r == R_EXIT) break;
    }
    free(rec.s);
  }
  run_end();
  finish(exit_status);
}
static void run_end(void) {
  static int ran;
  if (ran) return;
  ran = 1;
  unwinding = 0;
  for (Node *b = end_actions; b; b = b->next) {
    int r = exec(b);
    unwinding = 0;
    if (r == R_EXIT) break;
  }
}
