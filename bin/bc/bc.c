// SPDX-License-Identifier: GPL-2.0-or-later
/* bc: the language. Statements are parsed into a tree and run one top-level
 * statement at a time, so piped and interactive input behave the same. */
#include "bc.h"
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

/* ---- lexer ---- */
enum { T_EOF, T_NL, T_NUM, T_NAME, T_STR, T_OP, T_KW };
static const char *kws[] = {"if", "else", "while", "for", "break", "continue", "return", "define", "auto",
                            "quit", "halt", "print", "length", "scale", "sqrt", "ibase", "obase", "last", "limits", "read", 0};
enum { K_IF, K_ELSE, K_WHILE, K_FOR, K_BREAK, K_CONTINUE, K_RETURN, K_DEFINE, K_AUTO, K_QUIT, K_HALT, K_PRINT,
       K_LENGTH, K_SCALE, K_SQRT, K_IBASE, K_OBASE, K_LAST, K_LIMITS, K_READ };
static const char *src;
static int pos, tok, tkw, incomplete, perr;
static char tbuf[4096], top[4];

static void lex(void) {
  for (;;) {
    while (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r') pos++;
    if (src[pos] == '\\' && src[pos + 1] == '\n') { pos += 2; continue; }
    if (src[pos] == '/' && src[pos + 1] == '*') {
      const char *e = strstr(src + pos + 2, "*/");
      if (!e) { incomplete = 1; tok = T_EOF; return; }
      pos = e - src + 2;
      continue;
    }
    if (src[pos] == '#') { while (src[pos] && src[pos] != '\n') pos++; continue; }
    break;
  }
  char c = src[pos];
  if (!c) { tok = T_EOF; return; }
  if (c == '\n') { pos++; tok = T_NL; return; }
  if (isdigit((unsigned char)c) || (c >= 'A' && c <= 'F') || (c == '.' && (isdigit((unsigned char)src[pos + 1]) || (src[pos + 1] >= 'A' && src[pos + 1] <= 'F')))) {
    int n = 0, dot = 0;
    for (;;) {
      char d = src[pos];
      if (d == '\\' && src[pos + 1] == '\n') { pos += 2; continue; }
      if (isdigit((unsigned char)d) || (d >= 'A' && d <= 'F') || (d == '.' && !dot)) { if (d == '.') dot = 1; if (n < 4095) tbuf[n++] = d; pos++; }
      else break;
    }
    tbuf[n] = 0;
    tok = T_NUM;
    return;
  }
  if (c == '.' ) { pos++; tok = T_KW; tkw = K_LAST; return; }
  if (islower((unsigned char)c) || c == '_') {
    int n = 0;
    while ((islower((unsigned char)src[pos]) || isdigit((unsigned char)src[pos]) || src[pos] == '_') && n < 4095) tbuf[n++] = src[pos++];
    tbuf[n] = 0;
    for (int i = 0; kws[i]; i++) if (!strcmp(kws[i], tbuf)) { tok = T_KW; tkw = i; return; }
    tok = T_NAME;
    return;
  }
  if (c == '"') {
    int n = 0;
    pos++;
    while (src[pos] && src[pos] != '"') { if (n < 4095) tbuf[n++] = src[pos]; pos++; }
    if (!src[pos]) { incomplete = 1; tok = T_EOF; return; }
    pos++;
    tbuf[n] = 0;
    tok = T_STR;
    return;
  }
  static const char *ops[] = {"^=", "*=", "/=", "%=", "+=", "-=", "++", "--", "==", "!=", "<=", ">=", "&&", "||", 0};
  for (int i = 0; ops[i]; i++)
    if (src[pos] == ops[i][0] && src[pos + 1] == ops[i][1]) { strcpy(top, ops[i]); pos += 2; tok = T_OP; return; }
  top[0] = c; top[1] = 0;
  pos++;
  tok = T_OP;
}
static int isop(const char *o) { return tok == T_OP && !strcmp(top, o); }
static int iskw(int k) { return tok == T_KW && tkw == k; }
static void syntax(void) {
  if (perr || incomplete) return;
  if (tok == T_EOF) { incomplete = 1; return; }
  perr = 1;
}
static int expect(const char *o) { if (isop(o)) { lex(); return 1; } syntax(); return 0; }
static void skip_nl(void) { while (tok == T_NL) lex(); }

/* ---- tree ---- */
enum { E_NUM, E_VAR, E_ARR, E_SCALE, E_IBASE, E_OBASE, E_LAST, E_BIN, E_NEG, E_NOT, E_ASSIGN, E_PREINC, E_POSTINC,
       E_CALL, E_LENGTH, E_SCALEF, E_SQRT, E_AND, E_OR, E_READ,
       S_EXPR, S_STR, S_PRINT, S_BLOCK, S_IF, S_WHILE, S_FOR, S_BREAK, S_CONT, S_RETURN, S_HALT, S_DEFINE, S_LIMITS };
typedef struct node {
  int type;
  char op[4];
  num *val;
  char *name, *str;
  struct node *a, *b, *c, *d;
  struct node **list;
  int nlist;
  /* function definitions */
  char **params, **autos;
  int nparams, nautos, *param_arr, *auto_arr;
} node;
static node *mk(int t) { node *n = calloc(1, sizeof *n); n->type = t; return n; }
static void push(node *n, node *x) { n->list = realloc(n->list, (n->nlist + 1) * sizeof *n->list); n->list[n->nlist++] = x; }

static node *expr(void);
static node *statement(void);

static int is_lvalue(node *n) { return n && (n->type == E_VAR || n->type == E_ARR || n->type == E_SCALE || n->type == E_IBASE || n->type == E_OBASE || n->type == E_LAST); }

static node *primary(void) {
  node *n = NULL;
  if (tok == T_NUM) { n = mk(E_NUM); n->str = strdup(tbuf); lex(); return n; }
  if (isop("(")) { lex(); n = expr(); expect(")"); return n; }
  if (tok == T_NAME) {
    char *name = strdup(tbuf);
    lex();
    if (isop("(")) {
      lex();
      n = mk(E_CALL);
      n->name = name;
      while (!isop(")") && !perr && !incomplete) {
        if (tok == T_NAME) { /* an array argument: name[] */
          int save = pos;
          char nm[4096];
          strcpy(nm, tbuf);
          lex();
          if (isop("[")) {
            int save2 = pos;
            lex();
            if (isop("]")) { lex(); node *x = mk(E_ARR); x->name = strdup(nm); push(n, x); goto next; }
            (void)save2;
          }
          pos = save; tok = T_NAME; strcpy(tbuf, nm); /* not name[]: parse it as an expression */
        }
        push(n, expr());
      next:
        if (isop(",")) lex(); else break;
      }
      expect(")");
      return n;
    }
    if (isop("[")) { lex(); n = mk(E_ARR); n->name = name; n->a = expr(); expect("]"); return n; }
    n = mk(E_VAR);
    n->name = name;
    return n;
  }
  if (tok == T_KW) {
    int k = tkw;
    if (k == K_SCALE || k == K_LENGTH || k == K_SQRT) {
      lex();
      if (k == K_SCALE && !isop("(")) return mk(E_SCALE);
      expect("(");
      n = mk(k == K_SCALE ? E_SCALEF : k == K_LENGTH ? E_LENGTH : E_SQRT);
      n->a = expr();
      expect(")");
      return n;
    }
    if (k == K_IBASE) { lex(); return mk(E_IBASE); }
    if (k == K_OBASE) { lex(); return mk(E_OBASE); }
    if (k == K_LAST) { lex(); return mk(E_LAST); }
    if (k == K_READ) { lex(); expect("("); expect(")"); return mk(E_READ); }
  }
  syntax();
  return NULL;
}
static node *postfix(void) {
  node *n = primary();
  if (n && is_lvalue(n) && (isop("++") || isop("--"))) {
    node *p = mk(E_POSTINC);
    p->op[0] = top[0];
    p->a = n;
    lex();
    return p;
  }
  return n;
}
static node *unary(void) {
  if (isop("-")) { lex(); node *n = mk(E_NEG); n->a = unary(); return n; }
  if (isop("++") || isop("--")) { node *n = mk(E_PREINC); n->op[0] = top[0]; lex(); n->a = unary(); if (!is_lvalue(n->a)) syntax(); return n; }
  return postfix();
}
static node *bin(char *op, node *a, node *b) { node *n = mk(E_BIN); strcpy(n->op, op); n->a = a; n->b = b; return n; }
static node *power(void) {
  node *a = unary();
  if (isop("^")) { lex(); return bin("^", a, power()); } /* right associative */
  return a;
}
static node *mul(void) {
  node *a = power();
  while (isop("*") || isop("/") || isop("%")) { char o[4]; strcpy(o, top); lex(); a = bin(o, a, power()); }
  return a;
}
static node *add(void) {
  node *a = mul();
  while (isop("+") || isop("-")) { char o[4]; strcpy(o, top); lex(); a = bin(o, a, mul()); }
  return a;
}
static node *assign(void) {
  node *a = add();
  if (is_lvalue(a) && (isop("=") || isop("+=") || isop("-=") || isop("*=") || isop("/=") || isop("%=") || isop("^="))) {
    node *n = mk(E_ASSIGN);
    strcpy(n->op, top);
    lex();
    n->a = a;
    n->b = assign();
    return n;
  }
  return a;
}
static node *rel(void) {
  node *a = assign();
  while (isop("==") || isop("!=") || isop("<") || isop("<=") || isop(">") || isop(">=")) { char o[4]; strcpy(o, top); lex(); a = bin(o, a, assign()); }
  return a;
}
static node *not_(void) {
  if (isop("!")) { lex(); node *n = mk(E_NOT); n->a = not_(); return n; }
  return rel();
}
static node *and_(void) {
  node *a = not_();
  while (isop("&&")) { lex(); skip_nl(); node *n = mk(E_AND); n->a = a; n->b = not_(); a = n; }
  return a;
}
static node *expr(void) {
  node *a = and_();
  while (isop("||")) { lex(); skip_nl(); node *n = mk(E_OR); n->a = a; n->b = and_(); a = n; }
  return a;
}

static void end_statement(void) {
  if (tok == T_NL || isop(";")) { lex(); return; }
  if (tok == T_EOF || isop("}")) return;
  syntax();
}
static node *block_body(void) { /* after '{' */
  node *b = mk(S_BLOCK);
  for (;;) {
    while (tok == T_NL || isop(";")) lex();
    if (isop("}")) { lex(); break; }
    if (tok == T_EOF || perr || incomplete) { syntax(); break; }
    node *s = statement();
    if (perr || incomplete) break;
    if (s) push(b, s);
    if (!isop("}")) end_statement();
  }
  return b;
}
static node *sub_statement(void) { skip_nl(); return statement(); }

static void define(node *d) {
  if (tok != T_NAME) { syntax(); return; }
  d->name = strdup(tbuf);
  lex();
  expect("(");
  while (tok == T_NAME) {
    d->params = realloc(d->params, (d->nparams + 1) * sizeof *d->params);
    d->param_arr = realloc(d->param_arr, (d->nparams + 1) * sizeof *d->param_arr);
    d->params[d->nparams] = strdup(tbuf);
    d->param_arr[d->nparams] = 0;
    lex();
    if (isop("[")) { lex(); expect("]"); d->param_arr[d->nparams] = 1; }
    d->nparams++;
    if (isop(",")) lex(); else break;
  }
  expect(")");
  skip_nl();
  expect("{");
  while (tok == T_NL) lex();
  if (iskw(K_AUTO)) {
    lex();
    while (tok == T_NAME) {
      d->autos = realloc(d->autos, (d->nautos + 1) * sizeof *d->autos);
      d->auto_arr = realloc(d->auto_arr, (d->nautos + 1) * sizeof *d->auto_arr);
      d->autos[d->nautos] = strdup(tbuf);
      d->auto_arr[d->nautos] = 0;
      lex();
      if (isop("[")) { lex(); expect("]"); d->auto_arr[d->nautos] = 1; }
      d->nautos++;
      if (isop(",")) lex(); else break;
    }
    end_statement();
  }
  d->a = block_body();
}

static void cleanup_and_exit(int code);
static node *statement(void) {
  node *n;
  if (tok == T_STR) { n = mk(S_STR); n->str = strdup(tbuf); lex(); return n; }
  if (isop("{")) { lex(); return block_body(); }
  if (tok == T_KW) {
    switch (tkw) {
    case K_QUIT: cleanup_and_exit(0); break; /* quit acts when it is read, as in POSIX bc */
    case K_HALT: lex(); return mk(S_HALT);
    case K_LIMITS: lex(); return mk(S_LIMITS);
    case K_BREAK: lex(); return mk(S_BREAK);
    case K_CONTINUE: lex(); return mk(S_CONT);
    case K_RETURN:
      lex();
      n = mk(S_RETURN);
      if (tok != T_NL && !isop(";") && !isop("}") && tok != T_EOF) n->a = expr();
      return n;
    case K_PRINT:
      lex();
      n = mk(S_PRINT);
      for (;;) {
        if (tok == T_STR) { node *s = mk(S_STR); s->str = strdup(tbuf); lex(); push(n, s); }
        else push(n, expr());
        if (isop(",")) lex(); else break;
      }
      return n;
    case K_IF:
      lex(); expect("(");
      n = mk(S_IF);
      n->a = expr();
      expect(")");
      n->b = sub_statement();
      { /* "else" may follow on a later line */
        int save = pos, st = tok, inc = incomplete;
        char sv[4096], so[4];
        strcpy(sv, tbuf);
        strcpy(so, top);
        while (tok == T_NL || isop(";")) lex();
        if (iskw(K_ELSE)) { lex(); n->c = sub_statement(); }
        else { pos = save; tok = st; incomplete = inc; strcpy(tbuf, sv); strcpy(top, so); }
      }
      return n;
    case K_WHILE:
      lex(); expect("(");
      n = mk(S_WHILE);
      n->a = expr();
      expect(")");
      n->b = sub_statement();
      return n;
    case K_FOR:
      lex(); expect("(");
      n = mk(S_FOR);
      if (!isop(";")) n->a = expr();
      expect(";");
      if (!isop(";")) n->b = expr();
      expect(";");
      if (!isop(")")) n->c = expr();
      expect(")");
      n->d = sub_statement();
      return n;
    case K_DEFINE:
      lex();
      n = mk(S_DEFINE);
      define(n);
      return n;
    }
  }
  if (tok == T_NL || isop(";")) return NULL;
  n = mk(S_EXPR);
  n->a = expr();
  return n;
}

/* ---- run time ---- */
static int scale_, ibase_ = 10, obase_ = 10, rterr, brk, cnt, ret;
static num *last_, *retval;
struct arr { num **v; int n; };
struct var { char *name; num *val; struct arr *arr; node *fn; struct var *next; };
static struct var *vars;
static struct var *lookup(const char *name) {
  for (struct var *v = vars; v; v = v->next) if (!strcmp(v->name, name)) return v;
  struct var *v = calloc(1, sizeof *v);
  v->name = strdup(name);
  v->next = vars;
  vars = v;
  return v;
}
static void runtime(const char *msg) { if (!rterr) { fprintf(stderr, "Runtime error: %s\n", msg); rterr = 1; } }
static void arr_free(struct arr *a) { if (!a) return; for (int i = 0; i < a->n; i++) num_free(a->v[i]); free(a->v); free(a); }
static struct arr *arr_dup(struct arr *a) {
  struct arr *r = calloc(1, sizeof *r);
  if (!a) return r;
  r->n = a->n;
  r->v = calloc(a->n ? a->n : 1, sizeof *r->v);
  for (int i = 0; i < a->n; i++) r->v[i] = a->v[i] ? num_dup(a->v[i]) : NULL;
  return r;
}
static num **arr_slot(struct var *v, long idx) {
  if (idx < 0 || idx > 16777215) { runtime("array index out of range"); return NULL; }
  if (!v->arr) v->arr = calloc(1, sizeof *v->arr);
  if (idx >= v->arr->n) {
    v->arr->v = realloc(v->arr->v, (idx + 1) * sizeof *v->arr->v);
    memset(v->arr->v + v->arr->n, 0, (idx + 1 - v->arr->n) * sizeof *v->arr->v);
    v->arr->n = idx + 1;
  }
  return &v->arr->v[idx];
}

static num *eval(node *n);
static long to_long(num *v) { int of; long r = num_long(v, &of); if (of) runtime("number too large"); return r; }
static num *get(node *n) {
  switch (n->type) {
  case E_VAR: { struct var *v = lookup(n->name); return v->val ? num_dup(v->val) : num_zero(); }
  case E_ARR: {
    num *i = eval(n->a);
    long idx = to_long(i);
    num_free(i);
    num **s = arr_slot(lookup(n->name), idx);
    return s && *s ? num_dup(*s) : num_zero();
  }
  case E_SCALE: return num_int(scale_);
  case E_IBASE: return num_int(ibase_);
  case E_OBASE: return num_int(obase_);
  case E_LAST: return last_ ? num_dup(last_) : num_zero();
  }
  return num_zero();
}
static void set(node *n, num *val) { /* takes ownership of val */
  long v;
  switch (n->type) {
  case E_VAR: { struct var *x = lookup(n->name); num_free(x->val); x->val = val; return; }
  case E_ARR: {
    num *i = eval(n->a);
    long idx = to_long(i);
    num_free(i);
    num **s = arr_slot(lookup(n->name), idx);
    if (s) { num_free(*s); *s = val; } else num_free(val);
    return;
  }
  case E_SCALE: v = to_long(val); if (v < 0 || v > 100000) runtime("scale out of range"); else scale_ = v; break;
  case E_IBASE: v = to_long(val); if (v < 2 || v > 16) runtime("ibase must be between 2 and 16"); else ibase_ = v; break;
  case E_OBASE: v = to_long(val); if (v < 2 || v > 999) runtime("obase must be between 2 and 999"); else obase_ = v; break;
  case E_LAST: num_free(last_); last_ = val; return;
  }
  num_free(val);
}
static num *arith(const char *op, num *a, num *b) {
  num *r = NULL;
  switch (op[0]) {
  case '+': r = num_add(a, b); break;
  case '-': r = num_sub(a, b); break;
  case '*': r = num_mul(a, b, scale_); break;
  case '/': r = num_div(a, b, scale_); if (!r) runtime("Divide by zero"); break;
  case '%': r = num_mod(a, b, scale_); if (!r) runtime("Modulo by zero"); break;
  case '^': {
    if (b->scale && !num_is_zero(b)) { num *t = num_trunc(b, 0); int nz = num_cmp(t, b); num_free(t); if (nz) fprintf(stderr, "Runtime warning: non-zero scale in exponent\n"); }
    long e = to_long(b);
    if (num_is_zero(a) && e < 0) { runtime("Divide by zero"); break; }
    r = num_pow(a, e, scale_);
    break;
  }
  }
  return r ? r : num_zero();
}
static num *boolean(int v) { return num_int(v ? 1 : 0); }
static num *call(node *n);

static num *eval(node *n) {
  if (!n || rterr) return num_zero();
  num *a, *b, *r;
  switch (n->type) {
  case E_NUM:
    if (!n->val || n->nparams != ibase_) { num_free(n->val); n->val = num_parse(n->str, ibase_); n->nparams = ibase_; }
    return num_dup(n->val);
  case E_VAR: case E_ARR: case E_SCALE: case E_IBASE: case E_OBASE: case E_LAST: return get(n);
  case E_NEG: a = eval(n->a); a->sign = -a->sign; if (num_is_zero(a)) a->sign = 1; return a;
  case E_NOT: a = eval(n->a); r = boolean(num_is_zero(a)); num_free(a); return r;
  case E_AND: a = eval(n->a); if (num_is_zero(a)) { num_free(a); return num_int(0); } num_free(a); b = eval(n->b); r = boolean(!num_is_zero(b)); num_free(b); return r;
  case E_OR: a = eval(n->a); if (!num_is_zero(a)) { num_free(a); return num_int(1); } num_free(a); b = eval(n->b); r = boolean(!num_is_zero(b)); num_free(b); return r;
  case E_BIN:
    a = eval(n->a);
    b = eval(n->b);
    if (strchr("+-*/%^", n->op[0]) && !n->op[1]) r = arith(n->op, a, b);
    else {
      int c = num_cmp(a, b);
      const char *o = n->op;
      r = boolean(!strcmp(o, "==") ? c == 0 : !strcmp(o, "!=") ? c != 0 : !strcmp(o, "<") ? c < 0 : !strcmp(o, "<=") ? c <= 0 : !strcmp(o, ">") ? c > 0 : c >= 0);
    }
    num_free(a); num_free(b);
    return r;
  case E_ASSIGN:
    b = eval(n->b);
    if (n->op[0] != '=') { a = get(n->a); r = arith(n->op, a, b); num_free(a); num_free(b); b = r; }
    set(n->a, num_dup(b));
    return b;
  case E_PREINC: case E_POSTINC: {
    a = get(n->a);
    num *one = num_int(1), *nv = n->op[0] == '+' ? num_add(a, one) : num_sub(a, one);
    num_free(one);
    set(n->a, num_dup(nv));
    if (n->type == E_PREINC) { num_free(a); return nv; }
    num_free(nv);
    return a;
  }
  case E_LENGTH: a = eval(n->a); r = num_int(num_length(a)); num_free(a); return r;
  case E_SCALEF: a = eval(n->a); r = num_int(a->scale); num_free(a); return r;
  case E_SQRT: a = eval(n->a); r = num_sqrt(a, scale_); num_free(a); if (!r) { runtime("Square root of a negative number"); r = num_zero(); } return r;
  case E_READ: {
    char line[1024];
    int k = 0;
    char c;
    while (k < 1023 && read(0, &c, 1) == 1 && c != '\n') line[k++] = c;
    line[k] = 0;
    char *e = line;
    while (*e == ' ') e++;
    return num_parse(*e ? e : "0", ibase_);
  }
  case E_CALL: return call(n);
  }
  return num_zero();
}

static void exec(node *s);
static num *call(node *n) {
  struct var *f = lookup(n->name);
  node *d = f->fn;
  if (!d) { runtime("function not defined"); return num_zero(); }
  if (n->nlist != d->nparams) { runtime("parameter number mismatch"); return num_zero(); }
  int total = d->nparams + d->nautos;
  num **sv = calloc(total + 1, sizeof *sv), **args = calloc(d->nparams + 1, sizeof *args);
  struct arr **sa = calloc(total + 1, sizeof *sa), **aargs = calloc(d->nparams + 1, sizeof *aargs);
  for (int i = 0; i < d->nparams; i++) { /* evaluate arguments before any binding */
    if (d->param_arr[i]) { node *x = n->list[i]; aargs[i] = arr_dup(x->type == E_ARR ? lookup(x->name)->arr : NULL); }
    else args[i] = eval(n->list[i]);
  }
  for (int i = 0; i < total; i++) {
    int isarr = i < d->nparams ? d->param_arr[i] : d->auto_arr[i - d->nparams];
    struct var *v = lookup(i < d->nparams ? d->params[i] : d->autos[i - d->nparams]);
    if (isarr) { sa[i] = v->arr; v->arr = i < d->nparams ? aargs[i] : NULL; }
    else { sv[i] = v->val; v->val = i < d->nparams ? args[i] : NULL; }
  }
  exec(d->a);
  num *r = retval ? retval : num_zero();
  retval = NULL;
  ret = 0;
  brk = cnt = 0;
  for (int i = total - 1; i >= 0; i--) {
    int isarr = i < d->nparams ? d->param_arr[i] : d->auto_arr[i - d->nparams];
    struct var *v = lookup(i < d->nparams ? d->params[i] : d->autos[i - d->nparams]);
    if (isarr) { arr_free(v->arr); v->arr = sa[i]; }
    else { num_free(v->val); v->val = sv[i]; }
  }
  free(sv); free(args); free(sa); free(aargs);
  return r;
}

static int col;
static void out(const char *s) { /* long numbers wrap at 70 columns with a backslash */
  for (; *s; s++) {
    if (*s == '\n') col = 0;
    else if (col == 69) { fputs("\\\n", stdout); col = 0; }
    if (*s != '\n') col++;
    putchar(*s);
  }
}
static void print_num(num *v) { char *s = num_str(v, obase_); out(s); free(s); }
static void print_str(const char *s, int esc) {
  for (; *s; s++) {
    char c = *s;
    if (esc && c == '\\' && s[1]) {
      switch (*++s) {
      case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'a': c = '\a'; break; case 'b': c = '\b'; break;
      case 'f': c = '\f'; break; case 'r': c = '\r'; break; case 'q': c = '"'; break; case '\\': c = '\\'; break; case 'e': c = '\\'; break;
      default: putchar('\\'); c = *s;
      }
    }
    putchar(c);
    col = c == '\n' ? 0 : col + 1;
  }
}

static void exec(node *s) {
  if (!s || rterr) return;
  num *v;
  switch (s->type) {
  case S_EXPR:
    v = eval(s->a);
    if (rterr) { num_free(v); return; }
    if (s->a->type == E_ASSIGN) { num_free(v); return; }
    print_num(v);
    out("\n");
    num_free(last_);
    last_ = v;
    return;
  case S_STR: print_str(s->str, 0); return;
  case S_PRINT:
    for (int i = 0; i < s->nlist && !rterr; i++) {
      if (s->list[i]->type == S_STR) print_str(s->list[i]->str, 1);
      else { v = eval(s->list[i]); print_num(v); num_free(last_); last_ = v; }
    }
    return;
  case S_BLOCK:
    for (int i = 0; i < s->nlist && !rterr && !brk && !cnt && !ret; i++) exec(s->list[i]);
    return;
  case S_IF:
    v = eval(s->a);
    if (!num_is_zero(v)) exec(s->b); else exec(s->c);
    num_free(v);
    return;
  case S_WHILE:
    for (;;) {
      v = eval(s->a);
      int go = !num_is_zero(v) && !rterr;
      num_free(v);
      if (!go) break;
      exec(s->b);
      if (brk) { brk = 0; break; }
      cnt = 0;
      if (ret || rterr) break;
    }
    return;
  case S_FOR:
    if (s->a) num_free(eval(s->a));
    for (;;) {
      if (s->b) { v = eval(s->b); int go = !num_is_zero(v) && !rterr; num_free(v); if (!go) break; }
      exec(s->d);
      if (brk) { brk = 0; break; }
      cnt = 0;
      if (ret || rterr) break;
      if (s->c) num_free(eval(s->c));
    }
    return;
  case S_BREAK: brk = 1; return;
  case S_CONT: cnt = 1; return;
  case S_RETURN: num_free(retval); retval = s->a ? eval(s->a) : num_zero(); ret = 1; return;
  case S_HALT: cleanup_and_exit(0); return;
  case S_LIMITS: printf("BC_BASE_MAX     = 999\nBC_DIM_MAX      = 16777215\nBC_SCALE_MAX    = 100000\nBC_STRING_MAX   = 4095\n"); return;
  case S_DEFINE: lookup(s->name)->fn = s; return;
  }
}

static void cleanup_and_exit(int code) { fflush(stdout); exit(code); }

/* ---- input ---- */
static char *buf;
static size_t blen, bcap;
static void append(const char *s, size_t n) {
  if (blen + n + 1 > bcap) { while (blen + n + 1 > bcap) bcap = bcap ? bcap * 2 : 4096; buf = realloc(buf, bcap); }
  memcpy(buf + blen, s, n);
  blen += n;
  buf[blen] = 0;
}
/* run every complete statement in buf; keep an unfinished one for later */
static void process(int at_eof) {
  size_t done = 0;
  for (;;) {
    src = buf + done;
    pos = 0;
    perr = incomplete = 0;
    lex();
    while (tok == T_NL || isop(";")) { done += pos; src = buf + done; pos = 0; lex(); }
    if (tok == T_EOF && !incomplete) { done = blen; break; }
    node *s = statement();
    if (incomplete && !at_eof) break;
    if (!perr && !incomplete && tok != T_NL && !isop(";") && tok != T_EOF) syntax();
    if (perr || incomplete) {
      fprintf(stderr, "bc: syntax error%s\n", incomplete ? ": unexpected end of input" : "");
      while (src[pos] && src[pos] != '\n') pos++; /* skip the rest of the line */
      done += pos;
      continue;
    }
    done = tok == T_EOF ? blen : done + pos; /* pos is just past the newline or ';' */
    rterr = brk = cnt = ret = 0;
    exec(s);
    fflush(stdout);
  }
  memmove(buf, buf + done, blen - done + 1);
  blen -= done;
}

static const char *mathlib =
  "define e(x) { auto s, r, t, i, k\n"
  "  if (x < 0) { s = scale; scale = s + 10; r = 1 / e(-x); scale = s; return (r / 1) }\n"
  "  s = scale; k = 0; t = x; while (t > 1) { t = t / 2; k = k + 1 }\n"
  "  scale = s + 10 + k; x = x / 2 ^ k\n"
  "  r = 1; t = 1; for (i = 1; 1; i++) { t = t * x / i; if (t == 0) break; r = r + t }\n"
  "  while (k > 0) { r = r * r; k = k - 1 }\n"
  "  scale = s; return (r / 1) }\n"
  "define l(x) { auto s, f, r, t, u, v, i, z\n"
  "  if (x <= 0) return (1 - 10 ^ scale)\n"
  "  s = scale; scale = s + 10; f = 2\n"
  "  while (x >= 2) { x = sqrt(x); f = f * 2 }\n"
  "  while (x <= .5) { x = sqrt(x); f = f * 2 }\n"
  "  u = (x - 1) / (x + 1); z = u * u; r = u; t = u\n"
  "  for (i = 3; 1; i = i + 2) { t = t * z; v = t / i; if (v == 0) break; r = r + v }\n"
  "  r = r * f; scale = s; return (r / 1) }\n"
  "define a(x) { auto s, r, t, u, i, z, n, f\n"
  "  s = scale; scale = s + 10; n = 1; if (x < 0) { n = -1; x = -x }\n"
  "  f = 1; while (x > .2) { x = x / (1 + sqrt(1 + x * x)); f = f * 2 }\n"
  "  z = x * x; r = x; t = x\n"
  "  for (i = 3; 1; i = i + 2) { t = -t * z; u = t / i; if (u == 0) break; r = r + u }\n"
  "  r = r * f * n; scale = s; return (r / 1) }\n"
  "define s(x) { auto s, r, t, i, z, p, n\n"
  "  s = scale; scale = s + 10; p = 8 * a(1)\n"
  "  scale = 0; n = x / p; scale = s + 10; x = x - n * p\n"
  "  z = x * x; r = x; t = x\n"
  "  for (i = 3; 1; i = i + 2) { t = -t * z / ((i - 1) * i); if (t == 0) break; r = r + t }\n"
  "  scale = s; return (r / 1) }\n"
  "define c(x) { auto s, r; s = scale; scale = s + 10; r = s(x + 2 * a(1)); scale = s; return (r / 1) }\n";

int main(int argc, char **argv) {
  int i = 1, lib = 0, quiet = 0;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'l') lib = 1;
      else if (*f == 'q' || *f == 's' || *f == 'w') quiet = 1;
      else { fprintf(stderr, "usage: bc [-lq] [file...]\n"); return 1; }
    }
  }
  if (lib) { append(mathlib, strlen(mathlib)); process(1); scale_ = 20; }
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "bc: cannot open %s\n", argv[i]); return 1; }
    char tmp[4096];
    long r;
    while ((r = read(fd, tmp, sizeof tmp)) > 0) append(tmp, r);
    close(fd);
    append("\n", 1);
    process(1);
  }
  struct termios t;
  int tty = __syscall3(SYS_ioctl, 0, TCGETS, &t) == 0;
  if (tty && !quiet) printf("bc for UC.OS: arbitrary precision calculator. Type quit to leave.\n");
  char line[4096];
  int n = 0;
  char c;
  while (read(0, &c, 1) == 1) { /* line at a time, so interactive use answers at once */
    if (n < (int)sizeof line) line[n++] = c;
    if (c == '\n' || n == (int)sizeof line) { append(line, n); n = 0; process(0); }
  }
  if (n) append(line, n);
  append("\n", 1);
  process(1);
  cleanup_and_exit(0);
  return 0;
}
