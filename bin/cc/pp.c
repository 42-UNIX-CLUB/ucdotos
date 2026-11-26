// SPDX-License-Identifier: GPL-2.0-or-later
/* The C preprocessor (C11 6.10). Macro expansion follows the hide-set algorithm
 * described by Dave Prosser for the ANSI C committee: every token carries the set of
 * macro names that must not expand it again, so rescanning is exact without a
 * separate "painted blue" pass. */
#include "cc.h"
#include <time.h>

typedef struct Macro {
  Ident *name;
  int funclike, nparams, variadic, builtin;
  Ident **params;
  Tok **body;
  int nbody;
  Tok *tok;
} Macro;

enum { B_FILE = 1, B_LINE, B_COUNTER, B_INCLUDE_LEVEL, B_BASE_FILE, B_PRAGMA };

typedef struct Src {
  File *file;
  Tok *toks;
  int pos, ntok, condbase;
} Src;

typedef struct Cond { int taken, in_else; Tok *tok; } Cond;
typedef struct Dir { char *path; int sys; } Dir;

static Vec dirs;            /* Dir* : -I directories first, then system ones */
static Src srcs[200];
static int nsrc;
static Vec pend;            /* Tok* pushed back, top = last */
static Cond conds[256];
static int ncond;
static Map files;           /* path -> File* (lexed once) */
static Map filetoks;        /* path -> Tok* */
static Map filentok;
static Buf predef;
static int counter;
static File *base_file;
static Tok eof_tok;
Vec pp_deps;

static Ident *id_defined, *id_va_args, *id_va_opt, *id_has_include, *id_has_include_next,
    *id_has_attribute, *id_has_builtin, *id_has_feature, *id_has_extension, *id_has_c_attr,
    *id_has_declspec, *id_pragma_op, *id_once;

static Tok *raw(void);
static void directive(Tok *hash);
static Vec expand_list(Tok **toks, int n);

static int is_punct(Tok *t, int p) { return t->kind == T_PUNCT && t->punct == p; }
static int is_id(Tok *t, const char *s) { return t->kind == T_IDENT && !strcmp(t->id->name, s); }

static Tok *copy_tok(Tok *t) {
  Tok *c = xalloc(sizeof *c);
  *c = *t;
  return c;
}

/* ---- hide sets ---- */
static int hide_has(Hide *h, Ident *id) {
  for (; h; h = h->next)
    if (h->id == id) return 1;
  return 0;
}
static Hide *hide_add(Hide *h, Ident *id) {
  if (hide_has(h, id)) return h;
  Hide *n = xalloc(sizeof *n);
  n->id = id;
  n->next = h;
  return n;
}
static Hide *hide_union(Hide *a, Hide *b) {
  for (; b; b = b->next) a = hide_add(a, b->id);
  return a;
}
static Hide *hide_inter(Hide *a, Hide *b) {
  Hide *r = NULL;
  for (; a; a = a->next)
    if (hide_has(b, a->id)) r = hide_add(r, a->id);
  return r;
}

/* ---- token sources ---- */
static void push_tok(Tok *t) { vec_push(&pend, t); }

static Tok *peek_raw(void) {
  if (pend.len) return pend.p[pend.len - 1];
  Src *s = &srcs[nsrc - 1];
  return &s->toks[s->pos];
}

static void pop_src(void) {
  Src *s = &srcs[nsrc - 1];
  if (ncond > s->condbase) error_at(conds[ncond - 1].tok, "unterminated conditional directive");
  nsrc--;
}

static Tok *raw(void) {
  if (pend.len) return pend.p[--pend.len];
  for (;;) {
    Src *s = &srcs[nsrc - 1];
    Tok *t = &s->toks[s->pos];
    if (t->kind != T_EOF) { s->pos++; return t; }
    if (nsrc == 1) { pop_src(); nsrc = 1; return t; }
    pop_src();
  }
}

/* The tokens of the rest of the current directive line. */
static Tok **line_toks(int *n) {
  Src *s = &srcs[nsrc - 1];
  int start = s->pos;
  while (s->toks[s->pos].kind != T_EOF && !s->toks[s->pos].bol) s->pos++;
  *n = s->pos - start;
  Tok **v = xalloc((*n + 1) * sizeof *v);
  for (int i = 0; i < *n; i++) v[i] = &s->toks[start + i];
  v[*n] = &eof_tok;
  return v;
}
static void skip_line(void) {
  Src *s = &srcs[nsrc - 1];
  while (s->toks[s->pos].kind != T_EOF && !s->toks[s->pos].bol) s->pos++;
}

/* ---- spelling ---- */
static void spell(Buf *b, Tok *t) {
  if (t->kind == T_IDENT) buf_mem(b, t->id->name, t->id->len);
  else if (t->kind != T_PLACEMARKER && t->kind != T_EOF) buf_mem(b, t->s, t->len);
}

static Tok *new_tok_from(const char *text, int len, Tok *like) {
  char *s = xstrndup(text, len);
  File *f = new_file(like && like->file ? like->file->name : "<scratch>", "<scratch>", s, len);
  int n;
  Tok *toks = lex_file(f, &n);
  if (n != 2) return NULL;
  Tok *t = copy_tok(&toks[0]);
  if (like) { t->file = like->file; t->line = like->line; t->col = like->col; t->s = like->s; t->len = like->len;
    if (t->kind != T_IDENT) { t->s = f->data; t->len = len; } }
  t->bol = 0;
  return t;
}

static Tok *stringize(Tok **arg, int n, Tok *where) {
  Buf b = {0};
  buf_byte(&b, '"');
  for (int i = 0; i < n; i++) {
    Tok *t = arg[i];
    if (t->kind == T_PLACEMARKER) continue;
    if (i && t->space && b.len > 1) buf_byte(&b, ' ');
    if (t->kind == T_STR || t->kind == T_CHAR) {
      for (int j = 0; j < t->len; j++) {
        if (t->s[j] == '"' || t->s[j] == '\\') buf_byte(&b, '\\');
        buf_byte(&b, t->s[j]);
      }
    } else {
      spell(&b, t);
    }
  }
  buf_byte(&b, '"');
  Tok *t = new_tok_from((char *)b.p, b.len, NULL);
  t->file = where->file; t->line = where->line; t->col = where->col;
  return t;
}

static Tok *paste(Tok *a, Tok *b, Tok *where) {
  if (a->kind == T_PLACEMARKER) return b;
  if (b->kind == T_PLACEMARKER) return a;
  Buf s = {0};
  spell(&s, a);
  spell(&s, b);
  Tok *t = new_tok_from((char *)s.p, s.len, NULL);
  if (!t) error_at(where, "pasting \"%s\" and \"%s\" does not give a valid token", tok_str(a), tok_str(b));
  t->file = a->file; t->line = a->line; t->col = a->col;
  t->space = a->space;
  t->hide = hide_inter(a->hide, b->hide); /* hidden only if hidden in both operands */
  return t;
}

/* ---- macro expansion ---- */
typedef struct { Tok **t; int n; } Arg;

static int param_index(Macro *m, Tok *t) {
  if (t->kind != T_IDENT) return -1;
  for (int i = 0; i < m->nparams; i++)
    if (m->params[i] == t->id) return i;
  return -1;
}

static Tok *placemarker(void) {
  Tok *t = xalloc(sizeof *t);
  t->kind = T_PLACEMARKER;
  return t;
}

/* Replace the parameters in a macro body (C11 6.10.3.1 to 6.10.3.3). */
static Vec subst(Macro *m, Arg *args, int nargs, Hide *hs, Tok *origin) {
  Vec out = {0};
  Tok **body = m->body;
  int n = m->nbody;
  Arg *va = m->variadic ? &args[m->nparams - 1] : NULL;
  for (int i = 0; i < n; i++) {
    Tok *t = body[i];
    int pi;
    /* __VA_OPT__(x): x if the variable arguments are not empty */
    if (m->variadic && t->kind == T_IDENT && t->id == id_va_opt && i + 1 < n && is_punct(body[i + 1], '(')) {
      int depth = 0, j = i + 1;
      for (; j < n; j++) {
        if (is_punct(body[j], '(')) depth++;
        else if (is_punct(body[j], ')') && --depth == 0) break;
      }
      int nonempty = 0;
      for (int k = 0; k < va->n; k++) if (va->t[k]->kind != T_PLACEMARKER) nonempty = 1;
      if (nonempty) {
        Macro sub = *m;
        sub.body = body + i + 2;
        sub.nbody = j - i - 2;
        Vec r = subst(&sub, args, nargs, NULL, origin);
        for (int k = 0; k < r.len; k++) vec_push(&out, r.p[k]);
      } else {
        vec_push(&out, placemarker());
      }
      i = j;
      continue;
    }
    if (m->funclike && is_punct(t, '#') && i + 1 < n && (pi = param_index(m, body[i + 1])) >= 0) {
      Tok *s = stringize(args[pi].t, args[pi].n, t);
      s->space = t->space;
      vec_push(&out, s);
      i++;
      continue;
    }
    if (is_punct(t, P_HASHHASH) && i + 1 < n) {
      Tok *rhs = body[i + 1];
      pi = param_index(m, rhs);
      /* GNU: , ## __VA_ARGS__ drops the comma when there are no variable arguments */
      if (pi >= 0 && m->variadic && pi == m->nparams - 1 && out.len && is_punct(out.p[out.len - 1], ',')) {
        if (args[pi].n == 0) { out.len--; i++; continue; }
        for (int k = 0; k < args[pi].n; k++) vec_push(&out, copy_tok(args[pi].t[k]));
        i++;
        continue;
      }
      Tok *lhs = out.len ? out.p[out.len - 1] : placemarker();
      if (out.len) out.len--;
      if (pi >= 0) {
        Arg *a = &args[pi];
        if (a->n == 0) {
          vec_push(&out, lhs);
        } else {
          vec_push(&out, paste(lhs, a->t[0], t));
          for (int k = 1; k < a->n; k++) vec_push(&out, copy_tok(a->t[k]));
        }
      } else {
        vec_push(&out, paste(lhs, rhs, t));
      }
      i++;
      continue;
    }
    if ((pi = param_index(m, t)) >= 0) {
      Arg *a = &args[pi];
      if (i + 1 < n && is_punct(body[i + 1], P_HASHHASH)) {
        /* operand of ##: the argument as written */
        if (a->n == 0) vec_push(&out, placemarker());
        for (int k = 0; k < a->n; k++) {
          Tok *c = copy_tok(a->t[k]);
          if (k == 0) c->space = t->space;
          vec_push(&out, c);
        }
      } else {
        Vec e = expand_list(a->t, a->n);
        for (int k = 0; k < e.len; k++) {
          Tok *c = copy_tok(e.p[k]);
          if (k == 0) c->space = t->space;
          vec_push(&out, c);
        }
        if (e.len == 0) vec_push(&out, placemarker());
      }
      continue;
    }
    vec_push(&out, copy_tok(t));
  }
  for (int i = 0; i < out.len; i++) {
    Tok *t = out.p[i];
    t->hide = hide_union(t->hide, hs);
    if (!t->origin) t->origin = origin;
    t->bol = 0;
  }
  return out;
}

static int pending_space; /* an empty expansion passes its leading space on */
static void push_list(Vec *v, Tok *first_space_from) {
  /* drop placemarkers; the first token takes the spacing of the macro name */
  int first = 1, any = 0;
  for (int i = 0; i < v->len; i++) if (((Tok *)v->p[i])->kind != T_PLACEMARKER) any = 1;
  if (!any && first_space_from->space) pending_space = 1;
  for (int i = v->len - 1; i >= 0; i--) {
    Tok *t = v->p[i];
    if (t->kind == T_PLACEMARKER) continue;
    push_tok(t);
  }
  for (int i = 0; i < v->len; i++) {
    Tok *t = v->p[i];
    if (t->kind == T_PLACEMARKER) continue;
    if (first) { t->space = first_space_from->space; t->bol = first_space_from->bol; first = 0; }
    break;
  }
}

static Tok *root_of(Tok *t) {
  while (t->origin) t = t->origin;
  return t;
}

static Tok *str_tok(const char *s, Tok *where) {
  Buf b = {0};
  buf_byte(&b, '"');
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') buf_byte(&b, '\\');
    buf_byte(&b, *s);
  }
  buf_byte(&b, '"');
  Tok *t = new_tok_from((char *)b.p, b.len, NULL);
  t->file = where->file; t->line = where->line; t->col = where->col;
  return t;
}
static Tok *num_tok(long v, Tok *where) {
  char s[32];
  snprintf(s, sizeof s, "%ld", v);
  Tok *t = new_tok_from(s, (int)strlen(s), NULL);
  t->file = where->file; t->line = where->line; t->col = where->col;
  return t;
}

static void do_pragma(Tok **t, int n, Tok *where);

/* Collect the arguments of a function-like macro call. The '(' is consumed. */
static int read_args(Macro *m, Tok *name, Arg **out, Tok **rparen) {
  Vec args = {0};
  Vec cur = {0};
  int depth = 0;
  for (;;) {
    int from_file = pend.len == 0;
    Tok *t = raw();
    if (t->kind == T_EOF) error_at(name, "unterminated call of macro '%s'", name->id->name);
    if (from_file && t->bol && is_punct(t, '#')) { directive(t); continue; }
    if (depth == 0 && (is_punct(t, ')') || (is_punct(t, ',') && !(m->variadic && args.len == m->nparams - 1)))) {
      Arg *a = xalloc(sizeof *a);
      a->t = (Tok **)cur.p;
      a->n = cur.len;
      vec_push(&args, a);
      cur = (Vec){0};
      if (is_punct(t, ')')) { *rparen = t; break; }
      continue;
    }
    if (is_punct(t, '(')) depth++;
    else if (is_punct(t, ')')) depth--;
    vec_push(&cur, t);
  }
  int nargs = args.len;
  if (m->nparams == 0 && nargs == 1 && ((Arg *)args.p[0])->n == 0) nargs = 0;
  if (m->variadic && nargs == m->nparams - 1) { vec_push(&args, xalloc(sizeof(Arg))); nargs++; }
  if (nargs != m->nparams) error_at(name, "macro '%s' expects %d arguments, given %d", name->id->name, m->nparams, nargs);
  Arg *v = xalloc((nargs + 1) * sizeof *v);
  for (int i = 0; i < nargs; i++) v[i] = *(Arg *)args.p[i];
  *out = v;
  return nargs;
}

/* The next token after macro expansion. Returns a T_EOF token at the end of input
 * or at a sentinel pushed by expand_list(). */
static Tok *expand_next1(void);
static Tok *expand_next(void) {
  Tok *t = expand_next1();
  if (pending_space && t->kind != T_EOF) {
    pending_space = 0;
    if (!t->space) { t = copy_tok(t); t->space = 1; }
  }
  return t;
}
static Tok *expand_next1(void) {
  for (;;) {
    int from_file = pend.len == 0;
    Tok *t = raw();
    if (t->kind == T_EOF) return t;
    if (from_file && t->bol && is_punct(t, '#')) { directive(t); continue; }
    if (t->kind != T_IDENT) return t;
    Macro *m = t->id->macro;
    if (!m || hide_has(t->hide, t->id)) return t;
    if (m->builtin) {
      Tok *r = root_of(t), *nt;
      switch (m->builtin) {
      case B_FILE: nt = str_tok(r->file->name, t); break;
      case B_BASE_FILE: nt = str_tok(base_file->name, t); break;
      case B_LINE: nt = num_tok(r->line, t); break;
      case B_COUNTER: nt = num_tok(counter++, t); break;
      case B_INCLUDE_LEVEL: nt = num_tok(nsrc - 2 < 0 ? 0 : nsrc - 2, t); break;
      default: { /* _Pragma("...") */
        Tok *lp = raw(), *s = raw(), *rp = raw();
        if (!is_punct(lp, '(') || s->kind != T_STR || !is_punct(rp, ')')) error_at(t, "_Pragma takes a string literal in parentheses");
        Buf b = {0};
        for (int i = 1; i < s->len - 1; i++) {
          if (s->s[i] == '\\' && (s->s[i + 1] == '"' || s->s[i + 1] == '\\')) i++;
          buf_byte(&b, s->s[i]);
        }
        File *f = new_file(t->file->name, "<pragma>", (char *)b.p, b.len);
        int n;
        Tok *pt = lex_file(f, &n);
        Tok **v = xalloc((n + 1) * sizeof *v);
        for (int i = 0; i < n; i++) v[i] = &pt[i];
        do_pragma(v, n - 1, t);
        continue;
      }
      }
      nt->space = t->space;
      nt->bol = t->bol;
      nt->origin = t->origin;
      return nt;
    }
    if (!m->funclike) {
      Vec v = subst(m, NULL, 0, hide_add(t->hide, t->id), t);
      push_list(&v, t);
      continue;
    }
    Tok *nx = peek_raw();
    if (!is_punct(nx, '(')) return t;
    raw();
    Arg *args;
    Tok *rp;
    int nargs = read_args(m, t, &args, &rp);
    Vec v = subst(m, args, nargs, hide_add(hide_inter(t->hide, rp->hide), t->id), t);
    push_list(&v, t);
  }
}

/* Fully expand a token list on its own (macro arguments, #if, #include lines). */
static Vec expand_list(Tok **toks, int n) {
  Tok *sentinel = xalloc(sizeof *sentinel);
  sentinel->kind = T_EOF;
  push_tok(sentinel);
  for (int i = n - 1; i >= 0; i--) push_tok(toks[i]);
  Vec out = {0};
  for (;;) {
    Tok *t = expand_next();
    if (t == sentinel) break;
    if (t->kind == T_EOF) error_at(t, "internal error: lost expansion sentinel");
    if (t->kind != T_PLACEMARKER) vec_push(&out, t);
  }
  return out;
}

/* ---- #define ---- */
static int same_body(Macro *a, Macro *b) {
  if (a->funclike != b->funclike || a->nparams != b->nparams || a->nbody != b->nbody || a->variadic != b->variadic) return 0;
  for (int i = 0; i < a->nparams; i++) if (a->params[i] != b->params[i]) return 0;
  for (int i = 0; i < a->nbody; i++) {
    Tok *x = a->body[i], *y = b->body[i];
    if (x->kind != y->kind || (i && x->space != y->space)) return 0;
    if (x->kind == T_IDENT ? x->id != y->id : (x->len != y->len || memcmp(x->s, y->s, x->len))) return 0;
  }
  return 1;
}

static void define_macro(Tok **t, int n, Tok *where) {
  if (n == 0 || t[0]->kind != T_IDENT) error_at(n ? t[0] : where, "macro name must be an identifier");
  Macro *m = xalloc(sizeof *m);
  m->name = t[0]->id;
  m->tok = t[0];
  if (t[0]->id == id_defined) error_at(t[0], "'defined' cannot be used as a macro name");
  int i = 1;
  if (n > 1 && is_punct(t[1], '(') && !t[1]->space) {
    m->funclike = 1;
    Vec ps = {0};
    i = 2;
    if (i < n && is_punct(t[i], ')')) {
      i++;
    } else {
      for (;;) {
        if (i >= n) error_at(where, "missing ')' in macro parameter list");
        if (is_punct(t[i], P_ELLIPSIS)) {
          m->variadic = 1;
          vec_push(&ps, id_va_args);
          i++;
          if (i >= n || !is_punct(t[i], ')')) error_at(t[i - 1], "expected ')' after '...'");
          i++;
          break;
        }
        if (t[i]->kind != T_IDENT) error_at(t[i], "expected a parameter name");
        vec_push(&ps, t[i]->id);
        i++;
        if (i < n && is_punct(t[i], P_ELLIPSIS)) { m->variadic = 1; i++; } /* GNU: name... */
        if (i < n && is_punct(t[i], ')')) { i++; break; }
        if (m->variadic) error_at(t[i < n ? i : n - 1], "expected ')' after variadic parameter");
        if (i >= n || !is_punct(t[i], ',')) error_at(t[i < n ? i : n - 1], "expected ',' or ')' in macro parameters");
        i++;
      }
    }
    m->nparams = ps.len;
    m->params = (Ident **)ps.p;
  }
  m->body = t + i;
  m->nbody = n - i;
  if (m->nbody > 0) m->body[0] = copy_tok(m->body[0]), m->body[0]->space = 0;
  for (int k = 0; k < m->nbody; k++) {
    if (is_punct(m->body[k], P_HASHHASH) && (k == 0 || k == m->nbody - 1)) error_at(m->body[k], "'##' cannot appear at either end of a macro body");
    if (m->funclike && is_punct(m->body[k], '#') && (k + 1 >= m->nbody || param_index(m, m->body[k + 1]) < 0))
      error_at(m->body[k], "'#' is not followed by a macro parameter");
  }
  Macro *old = m->name->macro;
  if (old && !old->builtin && !same_body(old, m)) warn_at(t[0], "'%s' redefined", m->name->name);
  m->name->macro = m;
}

/* ---- #if expressions ---- */
typedef struct { i64 v; int u; } Val;
static Tok **ex_t;
static int ex_i, ex_n;
static Tok *ex_where;
static int ex_skip; /* inside an operand that is not evaluated */

static Val ex_cond(void);
static Tok *ex_peek(void) { return ex_i < ex_n ? ex_t[ex_i] : &eof_tok; }
static int ex_accept(int p) {
  if (ex_i < ex_n && is_punct(ex_t[ex_i], p)) { ex_i++; return 1; }
  return 0;
}

i64 char_value(Tok *t, int *ok);
static i64 parse_ppnum(Tok *t, int *is_unsigned) {
  const char *s = t->s;
  int len = t->len;
  char *end;
  char buf[128];
  if (len >= (int)sizeof buf) error_at(t, "number too long");
  memcpy(buf, s, len);
  buf[len] = 0;
  u64 v;
  if (buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) v = strtoull(buf + 2, &end, 2);
  else v = strtoull(buf, &end, 0);
  int u = 0, l = 0;
  for (; *end; end++) {
    if (*end == 'u' || *end == 'U') u = 1;
    else if (*end == 'l' || *end == 'L') l++;
    else error_at(t, "invalid integer constant '%s' in preprocessor expression", buf);
  }
  (void)l;
  *is_unsigned = u || v > (u64)INT64_MAX;
  return (i64)v;
}

static Val ex_primary(void) {
  Tok *t = ex_peek();
  Val r = {0, 0};
  if (ex_accept('(')) {
    r = ex_cond();
    if (!ex_accept(')')) error_at(ex_peek()->kind == T_EOF ? ex_where : ex_peek(), "expected ')' in preprocessor expression");
    return r;
  }
  if (ex_accept('!')) { r = ex_primary(); r.v = !r.v; r.u = 0; return r; }
  if (ex_accept('-')) { r = ex_primary(); r.v = -(u64)r.v; return r; }
  if (ex_accept('+')) return ex_primary();
  if (ex_accept('~')) { r = ex_primary(); r.v = ~r.v; return r; }
  if (t->kind == T_NUM) { ex_i++; r.v = parse_ppnum(t, &r.u); return r; }
  if (t->kind == T_CHAR) { ex_i++; int ok; r.v = char_value(t, &ok); return r; }
  if (t->kind == T_IDENT) { ex_i++; return r; } /* identifiers that are not macros are 0 */
  error_at(t->kind == T_EOF ? ex_where : t, "invalid token in preprocessor expression");
  return r;
}

static Val ex_bin(int prec);
static int binprec(Tok *t) {
  if (t->kind != T_PUNCT) return -1;
  switch (t->punct) {
  case '*': case '/': case '%': return 10;
  case '+': case '-': return 9;
  case P_SHL: case P_SHR: return 8;
  case '<': case '>': case P_LE: case P_GE: return 7;
  case P_EQ: case P_NE: return 6;
  case '&': return 5;
  case '^': return 4;
  case '|': return 3;
  case P_LAND: return 2;
  case P_LOR: return 1;
  }
  return -1;
}
static Val ex_bin(int minprec) {
  Val l = ex_primary();
  for (;;) {
    Tok *op = ex_peek();
    int p = binprec(op);
    if (p < minprec) return l;
    ex_i++;
    int skip = (op->punct == P_LAND && !l.v) || (op->punct == P_LOR && l.v);
    ex_skip += skip;
    Val r = ex_bin(p + 1);
    ex_skip -= skip;
    int u = l.u || r.u;
    u64 a = l.v, b = r.v;
    i64 v;
    switch (op->punct) {
    case '*': v = a * b; break;
    case '/': case '%':
      if (b == 0) {
        if (!ex_skip) error_at(op, "division by zero in preprocessor expression");
        v = 0;
        break;
      }
      if (u) v = op->punct == '/' ? (i64)(a / b) : (i64)(a % b);
      else if (l.v == INT64_MIN && r.v == -1) v = op->punct == '/' ? l.v : 0;
      else v = op->punct == '/' ? l.v / r.v : l.v % r.v;
      break;
    case '+': v = a + b; break;
    case '-': v = a - b; break;
    case P_SHL: v = b >= 64 ? 0 : (i64)(a << b); break;
    case P_SHR: v = b >= 64 ? (u || l.v >= 0 ? 0 : -1) : u ? (i64)(a >> b) : l.v >> b; break;
    case '<': v = u ? a < b : l.v < r.v; u = 0; break;
    case '>': v = u ? a > b : l.v > r.v; u = 0; break;
    case P_LE: v = u ? a <= b : l.v <= r.v; u = 0; break;
    case P_GE: v = u ? a >= b : l.v >= r.v; u = 0; break;
    case P_EQ: v = a == b; u = 0; break;
    case P_NE: v = a != b; u = 0; break;
    case '&': v = a & b; break;
    case '^': v = a ^ b; break;
    case '|': v = a | b; break;
    case P_LAND: v = l.v && r.v; u = 0; break;
    default: v = l.v || r.v; u = 0; break;
    }
    l.v = v;
    l.u = u;
  }
}
static Val ex_cond(void) {
  Val c = ex_bin(1);
  if (!ex_accept('?')) return c;
  ex_skip += !c.v;
  Val a = ex_cond();
  ex_skip -= !c.v;
  if (!ex_accept(':')) error_at(ex_peek()->kind == T_EOF ? ex_where : ex_peek(), "expected ':' in preprocessor expression");
  ex_skip += !!c.v;
  Val b = ex_cond();
  ex_skip -= !!c.v;
  Val r = c.v ? a : b;
  r.u = a.u || b.u;
  return r;
}

static char *find_include(const char *name, int quoted, int after, int *diridx, int *sys);

static int has_include(Tok **t, int n, int *i, int next, Tok *where) {
  /* __has_include ( "x" ) or ( <x> ), possibly from macros */
  if (*i >= n || !is_punct(t[*i], '(')) error_at(where, "expected '(' after __has_include");
  (*i)++;
  char *name;
  int quoted;
  if (*i < n && t[*i]->kind == T_STR) {
    name = xstrndup(t[*i]->s + 1, t[*i]->len - 2);
    quoted = 1;
    (*i)++;
  } else if (*i < n && is_punct(t[*i], '<')) {
    Buf b = {0};
    (*i)++;
    while (*i < n && !is_punct(t[*i], '>')) {
      if (b.len && t[*i]->space) buf_byte(&b, ' ');
      spell(&b, t[*i]);
      (*i)++;
    }
    buf_byte(&b, 0);
    (*i)++;
    name = (char *)b.p;
    quoted = 0;
  } else {
    error_at(where, "expected a header name in __has_include");
    return 0;
  }
  if (*i >= n || !is_punct(t[*i], ')')) error_at(where, "expected ')' after __has_include");
  (*i)++;
  int di, sys;
  int after = -1;
  if (next) after = srcs[nsrc - 1].file->incdir;
  return find_include(name, quoted, after, &di, &sys) != NULL;
}

int builtin_known(const char *name); /* parse.c */

static i64 eval_if(Tok **t, int n, Tok *where) {
  /* defined and __has_* are handled before macro expansion */
  Vec v = {0};
  for (int i = 0; i < n; i++) {
    Tok *x = t[i];
    if (x->kind == T_IDENT && x->id == id_defined) {
      int paren = 0;
      i++;
      if (i < n && is_punct(t[i], '(')) { paren = 1; i++; }
      if (i >= n || t[i]->kind != T_IDENT) error_at(x, "'defined' needs a macro name");
      int d = t[i]->id->macro != NULL;
      if (paren) { i++; if (i >= n || !is_punct(t[i], ')')) error_at(x, "expected ')' after defined(name"); }
      vec_push(&v, num_tok(d, x));
      continue;
    }
    if (x->kind == T_IDENT && (x->id == id_has_include || x->id == id_has_include_next)) {
      i++;
      int r = has_include(t, n, &i, x->id == id_has_include_next, x);
      i--;
      vec_push(&v, num_tok(r, x));
      continue;
    }
    if (x->kind == T_IDENT && (x->id == id_has_attribute || x->id == id_has_builtin || x->id == id_has_feature ||
                               x->id == id_has_extension || x->id == id_has_c_attr || x->id == id_has_declspec)) {
      if (i + 3 >= n + 1 || !is_punct(t[i + 1], '(')) error_at(x, "expected '(' after %s", x->id->name);
      int j = i + 2, depth = 1, r = 0;
      Tok *arg = j < n ? t[j] : NULL;
      for (; j < n; j++) {
        if (is_punct(t[j], '(')) depth++;
        else if (is_punct(t[j], ')') && --depth == 0) break;
      }
      if (arg && arg->kind == T_IDENT) {
        const char *a = arg->id->name;
        if (x->id == id_has_builtin) r = builtin_known(a);
        else if (x->id == id_has_attribute) {
          static const char *known[] = {"noreturn", "noinline", "always_inline", "weak", "aligned", "packed", "unused",
            "used", "format", "import_module", "import_name", "export_name", "visibility", "const", "pure",
            "deprecated", "maybe_unused", "cold", "hot", "malloc", "nonnull", "warn_unused_result", "__noreturn__",
            "__unused__", "__aligned__", "__packed__", "__format__", "section", "constructor", "fallthrough", 0};
          for (int k = 0; known[k]; k++) if (!strcmp(known[k], a)) r = 1;
        }
      }
      vec_push(&v, num_tok(r, x));
      i = j;
      continue;
    }
    vec_push(&v, x);
  }
  Vec e = expand_list((Tok **)v.p, v.len);
  ex_t = (Tok **)e.p;
  ex_n = e.len;
  ex_i = 0;
  ex_where = where;
  if (ex_n == 0) error_at(where, "#if with no expression");
  Val r = ex_cond();
  if (ex_i < ex_n) error_at(ex_t[ex_i], "extra tokens at end of #if expression");
  return r.v != 0;
}

/* ---- skipping groups ---- */
static Tok *dir_name(Tok *hash) {
  Src *s = &srcs[nsrc - 1];
  Tok *n = &s->toks[s->pos];
  if (n->kind == T_EOF || n->bol) return NULL;
  (void)hash;
  return n;
}

/* Skip lines until the #elif/#else/#endif that ends this group. Returns with the
 * conditional resolved: either a taken branch begins or the #endif was consumed. */
static void skip_group(void) {
  Src *s = &srcs[nsrc - 1];
  int depth = 0;
  for (;;) {
    Tok *t = &s->toks[s->pos];
    if (t->kind == T_EOF) error_at(conds[ncond - 1].tok, "unterminated conditional directive");
    s->pos++;
    if (!(t->bol && is_punct(t, '#'))) continue;
    Tok *d = &s->toks[s->pos];
    if (d->kind != T_IDENT || d->bol) continue;
    const char *nm = d->id->name;
    if (!strcmp(nm, "if") || !strcmp(nm, "ifdef") || !strcmp(nm, "ifndef")) { depth++; skip_line(); continue; }
    if (depth > 0) {
      if (!strcmp(nm, "endif")) depth--;
      skip_line();
      continue;
    }
    Cond *c = &conds[ncond - 1];
    if (!strcmp(nm, "endif")) { s->pos++; skip_line(); ncond--; return; }
    if (!strcmp(nm, "else")) {
      s->pos++;
      skip_line();
      if (c->in_else) error_at(d, "#else after #else");
      c->in_else = 1;
      if (!c->taken) { c->taken = 1; return; }
      continue;
    }
    if (!strcmp(nm, "elif") || !strcmp(nm, "elifdef") || !strcmp(nm, "elifndef")) {
      s->pos++;
      int n;
      Tok **lt = line_toks(&n);
      if (c->in_else) error_at(d, "#elif after #else");
      if (c->taken) continue;
      int v;
      if (nm[4] == 0) v = eval_if(lt, n, d);
      else {
        if (n < 1 || lt[0]->kind != T_IDENT) error_at(d, "expected a macro name");
        v = (lt[0]->id->macro != NULL) == (nm[4] == 'd');
      }
      if (v) { c->taken = 1; return; }
      continue;
    }
    skip_line();
  }
}

/* ---- #include ---- */
static char *join(const char *dir, int dirlen, const char *name) {
  if (dirlen < 0) return xstrdup(name);
  return xfmt("%.*s/%s", dirlen, dir, name);
}

static char *find_include(const char *name, int quoted, int after, int *diridx, int *sys) {
  *diridx = -1;
  *sys = 0;
  if (name[0] == '/') return file_exists(name) ? xstrdup(name) : NULL;
  if (quoted && after < 0) {
    File *cur = srcs[nsrc - 1].file;
    char *p = join(cur->path, cur->dirlen, name);
    if (map_get(&files, p) || file_exists(p)) { *sys = cur->sysdir; *diridx = cur->incdir; return p; }
  }
  for (int i = after + 1; i < dirs.len; i++) {
    Dir *d = dirs.p[i];
    char *p = xfmt("%s/%s", d->path, name);
    if (map_get(&files, p) || file_exists(p)) { *diridx = i; *sys = d->sys; return p; }
  }
  return NULL;
}

/* Recognise the classic include guard: the whole file is #ifndef X ... #endif. */
static Ident *detect_guard(Tok *t, int n) {
  int i = 0;
  if (n < 4 || !is_punct(&t[0], '#') || !t[0].bol) return NULL;
  Ident *g = NULL;
  if (is_id(&t[1], "ifndef") && t[2].kind == T_IDENT && t[3].bol) { g = t[2].id; i = 3; }
  else if (is_id(&t[1], "if") && is_punct(&t[2], '!') && t[3].kind == T_IDENT && t[3].id == id_defined) {
    int j = 4;
    if (is_punct(&t[j], '(')) j++;
    if (t[j].kind != T_IDENT) return NULL;
    g = t[j].id;
    j++;
    if (is_punct(&t[j], ')')) j++;
    if (!t[j].bol) return NULL;
    i = j;
  } else return NULL;
  int depth = 1;
  for (; i < n; i++) {
    if (!(t[i].bol && is_punct(&t[i], '#')) || t[i + 1].bol || t[i + 1].kind != T_IDENT) continue;
    const char *d = t[i + 1].id->name;
    if (!strcmp(d, "if") || !strcmp(d, "ifdef") || !strcmp(d, "ifndef")) depth++;
    else if (!strcmp(d, "endif") && --depth == 0) {
      int j = i + 2;
      while (j < n && !t[j].bol) j++;
      return t[j].kind == T_EOF ? g : NULL;
    } else if (depth == 1 && (!strcmp(d, "else") || !strcmp(d, "elif"))) return NULL;
  }
  return NULL;
}

static void push_file(File *f, Tok *toks, int ntok) {
  if (nsrc >= (int)(sizeof srcs / sizeof *srcs)) error_at(&toks[0], "#include nested too deeply");
  Src *s = &srcs[nsrc++];
  s->file = f;
  s->toks = toks;
  s->pos = 0;
  s->ntok = ntok;
  s->condbase = ncond;
}

static int include_file(const char *path, const char *name, int diridx, int sys, Tok *where) {
  File *f = map_get(&files, path);
  Tok *toks;
  int ntok;
  if (f) {
    if (f->once) return 1;
    if (f->guard && f->guard->macro) return 1;
    toks = map_get(&filetoks, path);
    ntok = (int)(intptr_t)map_get(&filentok, path);
  } else {
    size_t len;
    char *data = read_file(path, &len);
    if (!data) return 0;
    f = new_file(path, path, data, (int)len);
    free(data);
    f->sysdir = sys;
    f->incdir = diridx;
    toks = lex_file(f, &ntok);
    f->guard = detect_guard(toks, ntok);
    map_put(&files, f->path, f);
    map_put(&filetoks, f->path, toks);
    map_put(&filentok, f->path, (void *)(intptr_t)ntok);
    vec_push(&pp_deps, f);
  }
  (void)where;
  push_file(f, toks, ntok);
  return 1;
}

static void do_include(Tok **t, int n, Tok *where, int next) {
  Vec e = {0};
  if (n > 0 && (t[0]->kind == T_STR || is_punct(t[0], '<'))) {
    for (int i = 0; i < n; i++) vec_push(&e, t[i]);
  } else {
    e = expand_list(t, n);
  }
  if (e.len == 0) error_at(where, "#include expects \"FILENAME\" or <FILENAME>");
  Tok **v = (Tok **)e.p;
  char *name;
  int quoted;
  if (v[0]->kind == T_STR) {
    name = xstrndup(v[0]->s + 1, v[0]->len - 2);
    quoted = 1;
  } else if (is_punct(v[0], '<')) {
    Buf b = {0};
    int i = 1;
    /* use the source text between < and > when it is all in one place */
    for (; i < e.len && !is_punct(v[i], '>'); i++) {
      if (b.len && v[i]->space) buf_byte(&b, ' ');
      spell(&b, v[i]);
    }
    if (i >= e.len) error_at(where, "missing '>' in #include");
    buf_byte(&b, 0);
    name = (char *)b.p;
    quoted = 0;
  } else {
    error_at(v[0], "#include expects \"FILENAME\" or <FILENAME>");
    return;
  }
  int after = -1;
  if (next) {
    after = srcs[nsrc - 1].file->incdir;
    if (after < 0 && nsrc > 1) after = -1;
  }
  int di, sys;
  char *path = find_include(name, quoted, after, &di, &sys);
  if (!path || !include_file(path, name, di, sys, where)) error_at(where, "'%s' file not found", name);
}

/* ---- #pragma ---- */
static void do_pragma(Tok **t, int n, Tok *where) {
  if (n >= 1 && t[0]->kind == T_IDENT && t[0]->id == id_once) {
    srcs[nsrc - 1].file->once = 1;
    return;
  }
  (void)where; /* other pragmas (GCC, pack, STDC) are accepted and ignored */
}

static void directive(Tok *hash) {
  Tok *d = dir_name(hash);
  if (!d) return; /* null directive */
  Src *s = &srcs[nsrc - 1];
  s->pos++;
  if (d->kind != T_IDENT) {
    if (d->kind == T_NUM) { skip_line(); return; } /* # 12 "file": a line marker */
    error_at(d, "invalid preprocessing directive");
  }
  const char *nm = d->id->name;
  int n;
  if (!strcmp(nm, "define")) { Tok **t = line_toks(&n); define_macro(t, n, d); return; }
  if (!strcmp(nm, "undef")) {
    Tok **t = line_toks(&n);
    if (n < 1 || t[0]->kind != T_IDENT) error_at(d, "macro name missing");
    t[0]->id->macro = NULL;
    return;
  }
  if (!strcmp(nm, "include") || !strcmp(nm, "include_next") || !strcmp(nm, "import")) {
    Tok **t = line_toks(&n);
    do_include(t, n, d, nm[7] == '_');
    return;
  }
  if (!strcmp(nm, "if") || !strcmp(nm, "ifdef") || !strcmp(nm, "ifndef")) {
    Tok **t = line_toks(&n);
    int v;
    if (nm[2] == 0) v = eval_if(t, n, d);
    else {
      if (n < 1 || t[0]->kind != T_IDENT) error_at(d, "macro name missing");
      v = (t[0]->id->macro != NULL) == (nm[2] == 'd');
    }
    if (ncond >= 256) error_at(d, "conditionals nested too deeply");
    conds[ncond++] = (Cond){v, 0, d};
    if (!v) skip_group();
    return;
  }
  if (!strcmp(nm, "elif") || !strcmp(nm, "else") || !strcmp(nm, "elifdef") || !strcmp(nm, "elifndef")) {
    if (ncond <= s->condbase) error_at(d, "#%s without #if", nm);
    Cond *c = &conds[ncond - 1];
    if (c->in_else) error_at(d, "#%s after #else", nm);
    if (nm[2] == 's') c->in_else = 1;
    skip_line();
    c->taken = 1; /* the group just processed was taken; skip the rest */
    skip_group();
    return;
  }
  if (!strcmp(nm, "endif")) {
    if (ncond <= s->condbase) error_at(d, "#endif without #if");
    ncond--;
    skip_line();
    return;
  }
  if (!strcmp(nm, "error") || !strcmp(nm, "warning")) {
    Tok **t = line_toks(&n);
    Buf b = {0};
    for (int i = 0; i < n; i++) {
      if (i && t[i]->space) buf_byte(&b, ' ');
      spell(&b, t[i]);
    }
    buf_byte(&b, 0);
    if (nm[0] == 'e') error_at(d, "#error %s", (char *)b.p);
    warn_at(d, "#warning %s", (char *)b.p);
    return;
  }
  if (!strcmp(nm, "pragma")) { Tok **t = line_toks(&n); do_pragma(t, n, d); return; }
  if (!strcmp(nm, "line") || !strcmp(nm, "ident") || !strcmp(nm, "sccs") || !strcmp(nm, "assert") || !strcmp(nm, "unassert")) { skip_line(); return; }
  error_at(d, "invalid preprocessing directive '#%s'", nm);
}

/* ---- setup ---- */
void pp_add_include(const char *dir, int sys) {
  Dir *d = xalloc(sizeof *d);
  d->path = xstrdup(dir);
  size_t l = strlen(d->path);
  while (l > 1 && d->path[l - 1] == '/') d->path[--l] = 0;
  d->sys = sys;
  /* -I directories come before system ones, in command line order */
  if (!sys) {
    int i = 0;
    while (i < dirs.len && !((Dir *)dirs.p[i])->sys) i++;
    vec_push(&dirs, NULL);
    memmove(&dirs.p[i + 1], &dirs.p[i], (dirs.len - 1 - i) * sizeof *dirs.p);
    dirs.p[i] = d;
  } else {
    vec_push(&dirs, d);
  }
}

void pp_define(const char *def) {
  const char *eq = strchr(def, '=');
  buf_str(&predef, "#define ");
  if (eq) {
    buf_mem(&predef, def, (int)(eq - def));
    buf_byte(&predef, ' ');
    buf_str(&predef, eq + 1);
  } else {
    buf_str(&predef, def);
    buf_str(&predef, " 1");
  }
  buf_byte(&predef, '\n');
}
void pp_undef(const char *name) {
  buf_str(&predef, "#undef ");
  buf_str(&predef, name);
  buf_byte(&predef, '\n');
}

/* The target's predefined macros. Values match clang 23 for wasm32-unknown-unknown,
 * so code (and our libc's inttypes.h) sees the same types. */
static const char *target_macros =
  "#define __STDC__ 1\n#define __STDC_VERSION__ 201112L\n#define __STDC_HOSTED__ 0\n"
  "#define __STDC_UTF_16__ 1\n#define __STDC_UTF_32__ 1\n#define __STDC_NO_ATOMICS__ 1\n"
  "#define __STDC_NO_COMPLEX__ 1\n#define __STDC_NO_THREADS__ 1\n#define __STDC_NO_VLA__ 1\n"
  "#define __ucos__ 1\n#define __UCOS_CC__ 1\n"
  "#define __wasm 1\n#define __wasm__ 1\n#define __wasm32 1\n#define __wasm32__ 1\n"
  "#define __wasm_bulk_memory__ 1\n#define __wasm_sign_ext__ 1\n#define __wasm_nontrapping_fptoint__ 1\n"
  "#define __wasm_mutable_globals__ 1\n"
  "#define __ILP32__ 1\n#define _ILP32 1\n#define __CHAR_BIT__ 8\n"
  "#define __ORDER_LITTLE_ENDIAN__ 1234\n#define __ORDER_BIG_ENDIAN__ 4321\n#define __ORDER_PDP_ENDIAN__ 3412\n"
  "#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__\n#define __LITTLE_ENDIAN__ 1\n"
  "#define __BIGGEST_ALIGNMENT__ 16\n#define __USER_LABEL_PREFIX__\n"
  "#define __SIZEOF_SHORT__ 2\n#define __SIZEOF_INT__ 4\n#define __SIZEOF_LONG__ 4\n#define __SIZEOF_LONG_LONG__ 8\n"
  "#define __SIZEOF_POINTER__ 4\n#define __SIZEOF_FLOAT__ 4\n#define __SIZEOF_DOUBLE__ 8\n#define __SIZEOF_LONG_DOUBLE__ 16\n"
  "#define __SIZEOF_SIZE_T__ 4\n#define __SIZEOF_PTRDIFF_T__ 4\n#define __SIZEOF_WCHAR_T__ 4\n#define __SIZEOF_WINT_T__ 4\n"
  "#define __SCHAR_MAX__ 127\n#define __SHRT_MAX__ 32767\n#define __INT_MAX__ 2147483647\n"
  "#define __LONG_MAX__ 2147483647L\n#define __LONG_LONG_MAX__ 9223372036854775807LL\n"
  "#define __WCHAR_MAX__ 2147483647\n#define __WCHAR_MIN__ (-__WCHAR_MAX__ - 1)\n#define __WCHAR_TYPE__ int\n"
  "#define __WINT_MAX__ 2147483647\n#define __WINT_MIN__ (-__WINT_MAX__ - 1)\n#define __WINT_TYPE__ int\n"
  "#define __SIZE_MAX__ 4294967295UL\n#define __SIZE_TYPE__ long unsigned int\n"
  "#define __PTRDIFF_MAX__ 2147483647L\n#define __PTRDIFF_TYPE__ long int\n"
  "#define __INTMAX_MAX__ 9223372036854775807LL\n#define __INTMAX_TYPE__ long long int\n"
  "#define __UINTMAX_MAX__ 18446744073709551615ULL\n#define __UINTMAX_TYPE__ long long unsigned int\n"
  "#define __INTPTR_MAX__ 2147483647L\n#define __INTPTR_TYPE__ long int\n"
  "#define __UINTPTR_MAX__ 4294967295UL\n#define __UINTPTR_TYPE__ long unsigned int\n"
  "#define __CHAR16_TYPE__ unsigned short\n#define __CHAR32_TYPE__ unsigned int\n"
  "#define __INT8_TYPE__ signed char\n#define __INT16_TYPE__ short\n#define __INT32_TYPE__ int\n#define __INT64_TYPE__ long long int\n"
  "#define __UINT8_TYPE__ unsigned char\n#define __UINT16_TYPE__ unsigned short\n#define __UINT32_TYPE__ unsigned int\n"
  "#define __UINT64_TYPE__ long long unsigned int\n"
  "#define __INT8_MAX__ 127\n#define __INT16_MAX__ 32767\n#define __INT32_MAX__ 2147483647\n#define __INT64_MAX__ 9223372036854775807LL\n"
  "#define __UINT8_MAX__ 255\n#define __UINT16_MAX__ 65535\n#define __UINT32_MAX__ 4294967295U\n#define __UINT64_MAX__ 18446744073709551615ULL\n"
  "#define __INT8_C(c) c\n#define __INT16_C(c) c\n#define __INT32_C(c) c\n#define __INT64_C(c) c##LL\n"
  "#define __UINT8_C(c) c\n#define __UINT16_C(c) c\n#define __UINT32_C(c) c##U\n#define __UINT64_C(c) c##ULL\n"
  "#define __INTMAX_C(c) c##LL\n#define __UINTMAX_C(c) c##ULL\n"
  "#define __INT8_FMTd__ \"hhd\"\n#define __INT8_FMTi__ \"hhi\"\n#define __INT16_FMTd__ \"hd\"\n#define __INT16_FMTi__ \"hi\"\n"
  "#define __INT32_FMTd__ \"d\"\n#define __INT32_FMTi__ \"i\"\n#define __INT64_FMTd__ \"lld\"\n#define __INT64_FMTi__ \"lli\"\n"
  "#define __UINT8_FMTo__ \"hho\"\n#define __UINT8_FMTu__ \"hhu\"\n#define __UINT8_FMTx__ \"hhx\"\n#define __UINT8_FMTX__ \"hhX\"\n"
  "#define __UINT16_FMTo__ \"ho\"\n#define __UINT16_FMTu__ \"hu\"\n#define __UINT16_FMTx__ \"hx\"\n#define __UINT16_FMTX__ \"hX\"\n"
  "#define __UINT32_FMTo__ \"o\"\n#define __UINT32_FMTu__ \"u\"\n#define __UINT32_FMTx__ \"x\"\n#define __UINT32_FMTX__ \"X\"\n"
  "#define __UINT64_FMTo__ \"llo\"\n#define __UINT64_FMTu__ \"llu\"\n#define __UINT64_FMTx__ \"llx\"\n#define __UINT64_FMTX__ \"llX\"\n"
  "#define __INTPTR_FMTd__ \"ld\"\n#define __INTPTR_FMTi__ \"li\"\n"
  "#define __UINTPTR_FMTo__ \"lo\"\n#define __UINTPTR_FMTu__ \"lu\"\n#define __UINTPTR_FMTx__ \"lx\"\n#define __UINTPTR_FMTX__ \"lX\"\n"
  "#define __INTMAX_FMTd__ \"lld\"\n#define __INTMAX_FMTi__ \"lli\"\n"
  "#define __UINTMAX_FMTo__ \"llo\"\n#define __UINTMAX_FMTu__ \"llu\"\n#define __UINTMAX_FMTx__ \"llx\"\n#define __UINTMAX_FMTX__ \"llX\"\n"
  "#define __SIZE_FMTo__ \"lo\"\n#define __SIZE_FMTu__ \"lu\"\n#define __SIZE_FMTx__ \"lx\"\n#define __SIZE_FMTX__ \"lX\"\n"
  "#define __PTRDIFF_FMTd__ \"ld\"\n#define __PTRDIFF_FMTi__ \"li\"\n"
  "#define __FLT_RADIX__ 2\n#define __FLT_MANT_DIG__ 24\n#define __FLT_DIG__ 6\n#define __FLT_MIN_EXP__ (-125)\n"
  "#define __FLT_MAX_EXP__ 128\n#define __FLT_MIN_10_EXP__ (-37)\n#define __FLT_MAX_10_EXP__ 38\n"
  "#define __FLT_MAX__ 3.40282347e+38F\n#define __FLT_MIN__ 1.17549435e-38F\n#define __FLT_EPSILON__ 1.19209290e-7F\n"
  "#define __FLT_DENORM_MIN__ 1.40129846e-45F\n#define __FLT_DECIMAL_DIG__ 9\n"
  "#define __DBL_MANT_DIG__ 53\n#define __DBL_DIG__ 15\n#define __DBL_MIN_EXP__ (-1021)\n#define __DBL_MAX_EXP__ 1024\n"
  "#define __DBL_MIN_10_EXP__ (-307)\n#define __DBL_MAX_10_EXP__ 308\n#define __DBL_MAX__ 1.7976931348623157e+308\n"
  "#define __DBL_MIN__ 2.2250738585072014e-308\n#define __DBL_EPSILON__ 2.2204460492503131e-16\n"
  "#define __DBL_DENORM_MIN__ 4.9406564584124654e-324\n#define __DBL_DECIMAL_DIG__ 17\n"
  "#define __LDBL_MANT_DIG__ 113\n#define __LDBL_DIG__ 33\n#define __LDBL_MIN_EXP__ (-16381)\n#define __LDBL_MAX_EXP__ 16384\n"
  "#define __DECIMAL_DIG__ 36\n"
  "#define __FINITE_MATH_ONLY__ 0\n#define __NO_INLINE__ 1\n#define __GNUC_STDC_INLINE__ 1\n"
  "#define __extension__\n#define __restrict restrict\n#define __restrict__ restrict\n"
  "#define __inline inline\n#define __inline__ inline\n#define __const const\n#define __volatile volatile\n"
  "#define __volatile__ volatile\n#define __signed signed\n#define __signed__ signed\n#define __alignof__ _Alignof\n"
  "#define __alignof _Alignof\n#define __asm__ asm\n#define __asm asm\n#define __typeof__ typeof\n#define __typeof typeof\n";

void pp_init(void) {
  id_defined = intern("defined", 7);
  id_va_args = intern("__VA_ARGS__", 11);
  id_va_opt = intern("__VA_OPT__", 10);
  id_has_include = intern("__has_include", 13);
  id_has_include_next = intern("__has_include_next", 18);
  id_has_attribute = intern("__has_attribute", 15);
  id_has_builtin = intern("__has_builtin", 13);
  id_has_feature = intern("__has_feature", 13);
  id_has_extension = intern("__has_extension", 15);
  id_has_c_attr = intern("__has_c_attribute", 17);
  id_has_declspec = intern("__has_declspec_attribute", 24);
  id_pragma_op = intern("_Pragma", 7);
  id_once = intern("once", 4);
  static const struct { const char *n; int b; } bi[] = {
    {"__FILE__", B_FILE}, {"__LINE__", B_LINE}, {"__COUNTER__", B_COUNTER}, {"__INCLUDE_LEVEL__", B_INCLUDE_LEVEL},
    {"__BASE_FILE__", B_BASE_FILE}, {"_Pragma", B_PRAGMA},
  };
  for (size_t i = 0; i < sizeof bi / sizeof *bi; i++) {
    Macro *m = xalloc(sizeof *m);
    m->name = intern(bi[i].n, (int)strlen(bi[i].n));
    m->builtin = bi[i].b;
    m->name->macro = m;
  }
  /* __has_include and friends are recognised only in #if; define them so #ifdef works */
  const char *has[] = {"__has_include", "__has_include_next", "__has_attribute", "__has_builtin", "__has_feature",
                       "__has_extension", "__has_c_attribute", "__has_declspec_attribute"};
  for (size_t i = 0; i < sizeof has / sizeof *has; i++) {
    Macro *m = xalloc(sizeof *m);
    m->name = intern(has[i], (int)strlen(has[i]));
    m->builtin = 99;
    m->name->macro = m;
  }
  buf_str(&predef, target_macros);
  time_t now = time(NULL);
  struct tm *tm = gmtime(&now);
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char d[96];
  snprintf(d, sizeof d, "#define __DATE__ \"%s %2d %d\"\n#define __TIME__ \"%02d:%02d:%02d\"\n", mon[tm->tm_mon],
           tm->tm_mday, tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
  buf_str(&predef, d);
}

Tok **preprocess(const char *path, int *ntok) {
  size_t len;
  char *data = read_file(path, &len);
  if (!data) fatal("cannot read '%s'", path);
  const char *name = strcmp(path, "-") ? path : "<stdin>";
  File *f = new_file(name, path, data, (int)len);
  free(data);
  base_file = f;
  vec_push(&pp_deps, f);
  int n;
  Tok *toks = lex_file(f, &n);
  map_put(&files, f->path, f);
  push_file(f, toks, n);
  File *pf = new_file("<built-in>", "<built-in>", (char *)predef.p, predef.len);
  Tok *ptoks = lex_file(pf, &n);
  push_file(pf, ptoks, n);
  Vec out = {0};
  eof_tok.kind = T_EOF;
  for (;;) {
    Tok *t = expand_next();
    if ((out.len & 63) == 0) tick();
    vec_push(&out, t);
    if (t->kind == T_EOF) break;
  }
  *ntok = out.len;
  return (Tok **)out.p;
}

/* -E output: tokens with their spacing, and line markers when the file changes. */
void pp_write(FILE *out, Tok **toks, int n) {
  File *cur = NULL;
  int line = 0;
  for (int i = 0; i < n; i++) {
    Tok *t = toks[i];
    if (t->kind == T_EOF) break;
    Tok *r = root_of(t);
    if (r->file != cur) {
      if (cur) fputc('\n', out);
      fprintf(out, "# %d \"%s\"\n", r->line, r->file->name);
      cur = r->file;
      line = r->line;
    } else if (t->bol && !t->origin) {
      if (r->line > line && r->line - line < 8) while (line < r->line) { fputc('\n', out); line++; }
      else if (r->line != line) { fprintf(out, "\n# %d \"%s\"\n", r->line, r->file->name); line = r->line; }
      else fputc('\n', out);
    } else if (t->space || (t->bol && i)) {
      fputc(' ', out);
    }
    if (t->kind == T_IDENT) fwrite(t->id->name, 1, t->id->len, out);
    else fwrite(t->s, 1, t->len, out);
  }
  fputc('\n', out);
}

/* character constant value, shared with the parser */
i64 char_value(Tok *t, int *ok) {
  const char *p = t->s;
  int wide = 0;
  if (*p == 'L' || *p == 'U' || *p == 'u') { wide = *p; p++; if (*p == '8') p++; }
  p++;
  i64 v = 0;
  int count = 0;
  *ok = 1;
  while (*p != '\'') {
    u32 c;
    if (*p == '\\') {
      p++;
      switch (*p) {
      case 'n': c = '\n'; p++; break;
      case 't': c = '\t'; p++; break;
      case 'r': c = '\r'; p++; break;
      case 'a': c = 7; p++; break;
      case 'b': c = 8; p++; break;
      case 'f': c = 12; p++; break;
      case 'v': c = 11; p++; break;
      case 'e': c = 27; p++; break;
      case 'x': p++; c = 0; while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) { c = c * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); p++; } break;
      case 'u': case 'U': { int nd = *p == 'u' ? 4 : 8; p++; c = 0; for (int i = 0; i < nd && *p != '\''; i++, p++) c = c * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); break; }
      default:
        if (*p >= '0' && *p <= '7') { c = 0; for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++) c = c * 8 + *p++ - '0'; }
        else c = (u8)*p++;
      }
    } else if (wide && (u8)*p >= 0x80) {
      /* UTF-8 sequence to a code point */
      u8 b = (u8)*p++;
      int extra = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
      c = b & (0x3f >> extra);
      while (extra-- && ((u8)*p & 0xc0) == 0x80) c = (c << 6) | ((u8)*p++ & 0x3f);
    } else {
      c = (u8)*p++;
    }
    if (wide) v = c;
    else v = (v << 8) | (c & 0xff);
    count++;
  }
  if (count == 0) *ok = 0;
  if (!wide) {
    if (count == 1) v = (signed char)v; /* char is signed on wasm32 */
    else v = (int32_t)v;
  } else if (wide == 'L') v = (int32_t)v;
  else if (wide == 'u') v = (uint16_t)v;
  else v = (u32)v;
  return v;
}
