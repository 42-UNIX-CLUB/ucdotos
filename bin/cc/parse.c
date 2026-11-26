// SPDX-License-Identifier: GPL-2.0-or-later
/* Tokens to a typed syntax tree (C11 6.5 to 6.9). Every node gets its type when it is
 * built; conversions are explicit N_CAST nodes, array and function designators decay
 * where the standard says, and constant subexpressions are folded on the spot. */
#include "cc.h"

enum {
  KW_AUTO = 1, KW_BREAK, KW_CASE, KW_CHAR, KW_CONST, KW_CONTINUE, KW_DEFAULT, KW_DO, KW_DOUBLE,
  KW_ELSE, KW_ENUM, KW_EXTERN, KW_FLOAT, KW_FOR, KW_GOTO, KW_IF, KW_INLINE, KW_INT, KW_LONG,
  KW_REGISTER, KW_RESTRICT, KW_RETURN, KW_SHORT, KW_SIGNED, KW_SIZEOF, KW_STATIC, KW_STRUCT,
  KW_SWITCH, KW_TYPEDEF, KW_UNION, KW_UNSIGNED, KW_VOID, KW_VOLATILE, KW_WHILE, KW_ALIGNAS,
  KW_ALIGNOF, KW_ATOMIC, KW_BOOL, KW_COMPLEX, KW_GENERIC, KW_IMAGINARY, KW_NORETURN,
  KW_STATIC_ASSERT, KW_THREAD_LOCAL, KW_TYPEOF, KW_TYPEOF_UNQUAL, KW_ATTRIBUTE, KW_ASM, KW_VA_LIST,
  KW_VA_ARG, KW_OFFSETOF, KW_INT128, KW_TYPES_COMPAT, KW_CHOOSE_EXPR, KW_LABEL
};
static const char *kwnames[] = {
  NULL, "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
  "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict",
  "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
  "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex",
  "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "typeof",
  "typeof_unqual", "__attribute__", "asm", "__builtin_va_list", "",
  "", "__int128", "", "",
  "__label__",
};

enum { BK_VAR, BK_TYPEDEF, BK_ENUM, BK_TAG };
typedef struct Binding {
  Ident *id;
  int kind, depth;
  Var *var;
  Type *ty;
  i64 val;
  struct Binding *prev, *snext;
} Binding;

typedef struct Attr {
  int noreturn, weak, packed, aligned, noinline, used, constructor, vis_default, deprecated;
  const char *imp_mod, *imp_name, *exp_name, *asmname;
} Attr;

enum { SC_NONE, SC_TYPEDEF, SC_EXTERN, SC_STATIC, SC_AUTO, SC_REGISTER };
typedef struct DeclSpec {
  int storage, is_inline, noreturn, align, tls;
  Attr attr;
} DeclSpec;

Vec globals;
static Tok **tk;
static int tpos;
static int depth;
static Binding *scope_head[1024];
static Var *cur_fn;
static Node *cur_switch;
static int brk_depth, cont_depth;
static Map gtab;       /* name -> Var with linkage */
static Map strtab;     /* string literal contents -> Var */
static int nstr, nanon;
static Vec fn_labels;  /* Ident* with a label in the current function */
static Ident *id_func, *id_FUNCTION, *id_PRETTY;

/* ---- token helpers ---- */
static Tok *peek(void) { return tk[tpos]; }
static Tok *peekn(int n) {
  int i = tpos;
  while (n-- > 0 && tk[i]->kind != T_EOF) i++;
  return tk[i];
}
static Tok *next(void) {
  Tok *t = tk[tpos];
  if (t->kind != T_EOF) tpos++;
  return t;
}
static int is_p(Tok *t, int p) { return t->kind == T_PUNCT && t->punct == p; }
static int is_kw(Tok *t, int kw) { return t->kind == T_IDENT && t->id->kw == kw; }
static int accept(int p) {
  if (is_p(peek(), p)) { tpos++; return 1; }
  return 0;
}
static int accept_kw(int kw) {
  if (is_kw(peek(), kw)) { tpos++; return 1; }
  return 0;
}
static const char *pname(int p) {
  static char b[4];
  switch (p) {
  case P_ARROW: return "->";
  case P_ELLIPSIS: return "...";
  }
  b[0] = (char)p;
  b[1] = 0;
  return b;
}
static Tok *expect(int p) {
  Tok *t = peek();
  if (!is_p(t, p)) {
    if (t->kind == T_OTHER) error_at(t, "stray '%s' in program", tok_str(t));
    error_at(t, "expected '%s' before '%s'", pname(p), tok_str(t));
  }
  tpos++;
  return t;
}
static Ident *expect_ident(void) {
  Tok *t = peek();
  if (t->kind != T_IDENT || t->id->kw) error_at(t, "expected an identifier before '%s'", tok_str(t));
  tpos++;
  return t->id;
}

/* ---- scopes ---- */
static void push_scope(void) {
  if (++depth >= 1024) error_at(peek(), "blocks nested too deeply");
  scope_head[depth] = NULL;
}
static void pop_scope(void) {
  for (Binding *b = scope_head[depth]; b; b = b->snext) {
    if (b->kind == BK_TAG) b->id->tag = b->prev;
    else b->id->sym = b->prev;
  }
  depth--;
}
static Binding *bind(Ident *id, int kind) {
  Binding *b = xalloc(sizeof *b);
  b->id = id;
  b->kind = kind;
  b->depth = depth;
  if (kind == BK_TAG) { b->prev = id->tag; id->tag = b; }
  else { b->prev = id->sym; id->sym = b; }
  b->snext = scope_head[depth];
  scope_head[depth] = b;
  return b;
}

static int is_typename(Tok *t) {
  if (t->kind != T_IDENT) return 0;
  switch (t->id->kw) {
  case KW_VOID: case KW_CHAR: case KW_SHORT: case KW_INT: case KW_LONG: case KW_FLOAT: case KW_DOUBLE:
  case KW_SIGNED: case KW_UNSIGNED: case KW_BOOL: case KW_STRUCT: case KW_UNION: case KW_ENUM:
  case KW_TYPEDEF: case KW_EXTERN: case KW_STATIC: case KW_AUTO: case KW_REGISTER: case KW_CONST:
  case KW_VOLATILE: case KW_RESTRICT: case KW_INLINE: case KW_NORETURN: case KW_ALIGNAS: case KW_ATOMIC:
  case KW_THREAD_LOCAL: case KW_TYPEOF: case KW_TYPEOF_UNQUAL: case KW_ATTRIBUTE: case KW_VA_LIST:
  case KW_COMPLEX: case KW_INT128:
    return 1;
  case 0: return t->id->sym && t->id->sym->kind == BK_TYPEDEF;
  }
  return 0;
}

/* ---- nodes ---- */
static Node *new_node(int kind, Tok *t) {
  Node *n = xalloc(sizeof *n);
  n->kind = kind;
  n->tok = t;
  return n;
}
static i64 wrap(i64 v, Type *t) {
  switch (t->kind) {
  case TY_BOOL: return v != 0;
  case TY_CHAR: case TY_SCHAR: return (int8_t)v;
  case TY_UCHAR: return (uint8_t)v;
  case TY_SHORT: return (int16_t)v;
  case TY_USHORT: return (uint16_t)v;
  case TY_INT: case TY_LONG: return (int32_t)v;
  case TY_UINT: case TY_ULONG: case TY_PTR: return (uint32_t)v;
  }
  return v;
}
static Node *new_num(i64 v, Type *ty, Tok *t) {
  Node *n = new_node(N_NUM, t);
  n->ty = ty;
  n->ival = wrap(v, ty);
  return n;
}
static Node *new_fnum(double v, Type *ty, Tok *t) {
  Node *n = new_node(N_FNUM, t);
  n->ty = ty;
  n->fval = ty->kind == TY_FLOAT ? (double)(float)v : v;
  return n;
}
static Node *new_var(Var *v, Tok *t) {
  Node *n = new_node(N_VAR, t);
  n->var = v;
  n->ty = v->ty;
  return n;
}
static int is_null_const(Node *n) {
  while (n->kind == N_CAST && (is_integer(n->ty) || (is_ptr(n->ty) && n->ty->base->kind == TY_VOID))) n = n->lhs;
  /* `(void *)0` has been folded into a number of type void * by the time it gets here */
  return n->kind == N_NUM && n->ival == 0 && (is_integer(n->ty) || (is_ptr(n->ty) && n->ty->base->kind == TY_VOID));
}

static Node *cast(Node *n, Type *ty);

/* Array to pointer, function to pointer (C11 6.3.2.1). */
static Node *rval(Node *n) {
  if (n->ty->kind == TY_ARRAY) {
    Node *a = new_node(N_ADDR, n->tok);
    a->lhs = n;
    a->ty = ptr_to(n->ty->base);
    if (n->kind == N_VAR) n->var->addr_taken = 1;
    return a;
  }
  if (n->ty->kind == TY_FUNC) {
    Node *a = new_node(N_ADDR, n->tok);
    a->lhs = n;
    a->ty = ptr_to(n->ty);
    return a;
  }
  return n;
}

static int rank(Type *t) {
  switch (t->kind) {
  case TY_BOOL: return 0;
  case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 1;
  case TY_SHORT: case TY_USHORT: return 2;
  case TY_INT: case TY_UINT: return 3;
  case TY_LONG: case TY_ULONG: return 4;
  default: return 5;
  }
}
static Type *promoted(Node *n) {
  Type *t = n->ty;
  if (n->kind == N_MEMBER && n->mem->bitwidth) {
    if (n->mem->bitwidth < 32) return ty_int;
    if (n->mem->bitwidth == 32) return t->is_unsigned ? ty_uint : ty_int;
  }
  if (is_integer(t) && rank(t) < 3) return ty_int;
  if (t->is_enum) return t->is_unsigned ? ty_uint : ty_int;
  return unqual(t);
}
static Node *promote(Node *n) {
  n = rval(n);
  if (!is_integer(n->ty)) return n;
  Type *p = promoted(n);
  return p->kind == n->ty->kind && !n->ty->is_enum && !n->ty->qual ? n : cast(n, p);
}
static Type *to_unsigned(Type *t) {
  switch (t->kind) {
  case TY_INT: return ty_uint;
  case TY_LONG: return ty_ulong;
  case TY_LLONG: return ty_ullong;
  }
  return t;
}
static Type *common_type(Node *a, Node *b) {
  Type *x = a->ty, *y = b->ty;
  if (x->kind == TY_LDOUBLE || y->kind == TY_LDOUBLE) return ty_ldouble;
  if (x->kind == TY_DOUBLE || y->kind == TY_DOUBLE) return ty_double;
  if (x->kind == TY_FLOAT || y->kind == TY_FLOAT) return ty_float;
  x = promoted(a);
  y = promoted(b);
  if (x->kind == y->kind) return x;
  if (x->is_unsigned == y->is_unsigned) return rank(x) >= rank(y) ? x : y;
  Type *u = x->is_unsigned ? x : y, *s = x->is_unsigned ? y : x;
  if (rank(u) >= rank(s)) return u;
  if (s->size > u->size) return s;
  return to_unsigned(s);
}

/* ---- constant folding ---- */
static int fold_ok = 1; /* off while parsing sizeof operands etc. is not needed: folding is pure */

static Node *fold_binary(int kind, Node *l, Node *r, Type *ty, Tok *t) {
  if (!fold_ok) return NULL;
  if (l->kind == N_NUM && r->kind == N_NUM && is_integer(l->ty) && is_integer(r->ty)) {
    int u = l->ty->is_unsigned;
    int bits = l->ty->size * 8;
    u64 a = l->ival, b = r->ival;
    if (u && bits == 32) { a = (u32)a; b = (u32)b; }
    i64 v;
    switch (kind) {
    case N_ADD: v = a + b; break;
    case N_SUB: v = a - b; break;
    case N_MUL: v = a * b; break;
    case N_DIV: case N_MOD:
      if (b == 0) return NULL;
      if (u) v = kind == N_DIV ? (i64)(a / b) : (i64)(a % b);
      else if (l->ival == (bits == 64 ? INT64_MIN : INT32_MIN) && r->ival == -1) v = kind == N_DIV ? l->ival : 0;
      else v = kind == N_DIV ? l->ival / r->ival : l->ival % r->ival;
      break;
    case N_SHL: if (b >= (u64)bits) return NULL; v = (i64)(a << b); break;
    case N_SHR: if (b >= (u64)bits) return NULL; v = u ? (i64)(a >> b) : l->ival >> b; break;
    case N_BAND: v = a & b; break;
    case N_BOR: v = a | b; break;
    case N_BXOR: v = a ^ b; break;
    case N_EQ: v = a == b; break;
    case N_NE: v = a != b; break;
    case N_LT: v = u ? a < b : l->ival < r->ival; break;
    case N_LE: v = u ? a <= b : l->ival <= r->ival; break;
    default: return NULL;
    }
    return new_num(v, ty, t);
  }
  if (l->kind == N_FNUM && r->kind == N_FNUM) {
    double a = l->fval, b = r->fval, v;
    switch (kind) {
    case N_ADD: v = a + b; break;
    case N_SUB: v = a - b; break;
    case N_MUL: v = a * b; break;
    case N_DIV: v = a / b; break;
    case N_EQ: return new_num(a == b, ty, t);
    case N_NE: return new_num(a != b, ty, t);
    case N_LT: return new_num(a < b, ty, t);
    case N_LE: return new_num(a <= b, ty, t);
    default: return NULL;
    }
    if (ty->kind == TY_LDOUBLE) return NULL;
    return new_fnum(v, ty, t);
  }
  return NULL;
}

static Node *cast(Node *n, Type *ty) {
  Tok *t = n->tok;
  if (ty->kind == TY_VOID) {
    Node *c = new_node(N_CAST, t);
    c->lhs = n;
    c->ty = ty_void;
    return c;
  }
  n = rval(n);
  if (n->kind == N_NUM && is_integer(n->ty)) {
    if (is_integer(ty) || is_ptr(ty)) {
      i64 v = n->ival;
      if (n->ty->is_unsigned && n->ty->size == 4) v = (u32)v;
      return new_num(v, unqual(ty), t);
    }
    if (is_flonum(ty) && ty->kind != TY_LDOUBLE)
      return new_fnum(n->ty->is_unsigned ? (double)(u64)n->ival : (double)n->ival, unqual(ty), t);
  }
  if (n->kind == N_FNUM) {
    if (is_flonum(ty) && ty->kind != TY_LDOUBLE) return new_fnum(n->fval, unqual(ty), t);
    if (is_integer(ty) && n->fval > -9.2e18 && n->fval < 9.2e18) {
      if (ty->kind == TY_BOOL) return new_num(n->fval != 0, ty_bool, t);
      return new_num(ty->kind == TY_ULLONG && n->fval >= 9.2e18 ? (i64)(u64)n->fval : (i64)n->fval, unqual(ty), t);
    }
  }
  if (same_type(unqual(n->ty), unqual(ty)) && n->ty->kind == ty->kind && n->ty->is_enum == ty->is_enum && !is_aggr(ty)) {
    if (n->ty->qual == 0) return n;
  }
  if (is_aggr(ty) || is_aggr(n->ty)) {
    if (is_aggr(ty) && is_aggr(n->ty) && ty->rec == n->ty->rec) return n;
    error_at(t, "cannot convert '%s' to '%s'", type_str(n->ty), type_str(ty));
  }
  if (n->ty->kind == TY_VOID) error_at(t, "void value used where a value is needed");
  Node *c = new_node(N_CAST, t);
  c->lhs = n;
  c->ty = unqual(ty);
  return c;
}

/* Conversion as if by assignment, with the usual diagnostics. */
static Node *assign_conv(Node *n, Type *ty, Tok *t, const char *what) {
  n = rval(n);
  Type *from = n->ty;
  if (is_aggr(ty)) {
    if (!is_aggr(from) || from->rec != ty->rec) error_at(t, "incompatible types in %s: '%s' from '%s'", what, type_str(ty), type_str(from));
    return n;
  }
  if (ty->kind == TY_BOOL && (is_scalar(from))) return cast(n, ty);
  if (is_arith(ty) && is_arith(from)) return cast(n, ty);
  if (is_ptr(ty)) {
    if (is_null_const(n)) return cast(n, ty);
    if (is_ptr(from)) {
      Type *a = ty->base, *b = from->base;
      if (a->kind != TY_VOID && b->kind != TY_VOID && !same_type(unqual(a), unqual(b)) &&
          !(is_integer(a) && is_integer(b) && a->size == b->size))
        warn_at(t, "incompatible pointer types in %s: '%s' from '%s'", what, type_str(ty), type_str(from));
      else if ((b->qual & ~a->qual & (Q_CONST | Q_VOLATILE)) && opt_wall)
        warn_at(t, "%s discards qualifiers from pointer target type", what);
      return cast(n, ty);
    }
    if (is_integer(from)) { warn_at(t, "%s makes a pointer from an integer without a cast", what); return cast(n, ty); }
  }
  if (is_integer(ty) && is_ptr(from)) { warn_at(t, "%s makes an integer from a pointer without a cast", what); return cast(n, ty); }
  error_at(t, "incompatible types in %s: '%s' from '%s'", what, type_str(ty), type_str(from));
  return n;
}

static void check_scalar(Node *n) {
  if (!is_scalar(n->ty)) error_at(n->tok, "a scalar is required here, not '%s'", type_str(n->ty));
}

static Node *binary(int kind, Node *l, Node *r, Type *ty, Tok *t) {
  Node *f = fold_binary(kind, l, r, ty, t);
  if (f) return f;
  Node *n = new_node(kind, t);
  n->lhs = l;
  n->rhs = r;
  n->ty = ty;
  return n;
}

static Node *arith(int kind, Node *l, Node *r, Tok *t) {
  l = rval(l);
  r = rval(r);
  int intonly = kind == N_MOD || kind == N_BAND || kind == N_BOR || kind == N_BXOR;
  if (!(intonly ? is_integer(l->ty) && is_integer(r->ty) : is_arith(l->ty) && is_arith(r->ty)))
    error_at(t, "invalid operands to binary operator ('%s' and '%s')", type_str(l->ty), type_str(r->ty));
  Type *ty = common_type(l, r);
  return binary(kind, cast(l, ty), cast(r, ty), ty, t);
}

static Node *scale(Node *i, int size, Tok *t) {
  i = cast(i, i->ty->size == 8 ? ty_llong : ty_long);
  if (size == 1) return i;
  return binary(N_MUL, i, new_num(size, i->ty, t), i->ty, t);
}

static int elem_size(Type *p, Tok *t) {
  Type *b = p->base;
  if (b->kind == TY_VOID || b->kind == TY_FUNC) return 1; /* GNU extension */
  if (b->size == 0 && b->kind == TY_STRUCT && !b->rec->complete) error_at(t, "arithmetic on a pointer to an incomplete type");
  return b->size;
}

static Node *ptr_add(Node *p, Node *i, Tok *t) {
  Node *off = scale(i, elem_size(p->ty, t), t);
  if (off->ty->size == 8) off = cast(off, ty_long);
  Node *n = new_node(N_ADD, t);
  n->lhs = p;
  n->rhs = off;
  n->ty = p->ty;
  return n;
}

static Node *add(Node *l, Node *r, Tok *t) {
  l = rval(l);
  r = rval(r);
  if (is_arith(l->ty) && is_arith(r->ty)) return arith(N_ADD, l, r, t);
  if (is_ptr(l->ty) && is_integer(r->ty)) return ptr_add(l, r, t);
  if (is_integer(l->ty) && is_ptr(r->ty)) return ptr_add(r, l, t);
  error_at(t, "invalid operands to '+' ('%s' and '%s')", type_str(l->ty), type_str(r->ty));
  return NULL;
}
static Node *sub(Node *l, Node *r, Tok *t) {
  l = rval(l);
  r = rval(r);
  if (is_arith(l->ty) && is_arith(r->ty)) return arith(N_SUB, l, r, t);
  if (is_ptr(l->ty) && is_integer(r->ty)) {
    Node *off = scale(r, elem_size(l->ty, t), t);
    if (off->ty->size == 8) off = cast(off, ty_long);
    Node *n = new_node(N_SUB, t);
    n->lhs = l;
    n->rhs = off;
    n->ty = l->ty;
    return n;
  }
  if (is_ptr(l->ty) && is_ptr(r->ty)) {
    int sz = elem_size(l->ty, t);
    Node *d = new_node(N_SUB, t);
    d->lhs = cast(l, ty_long);
    d->rhs = cast(r, ty_long);
    d->ty = ty_long;
    if (sz <= 1) return d;
    return binary(N_DIV, d, new_num(sz, ty_long, t), ty_long, t);
  }
  error_at(t, "invalid operands to '-' ('%s' and '%s')", type_str(l->ty), type_str(r->ty));
  return NULL;
}
static Node *shift(int kind, Node *l, Node *r, Tok *t) {
  l = promote(l);
  r = promote(r);
  if (!is_integer(l->ty) || !is_integer(r->ty)) error_at(t, "invalid operands to shift");
  return binary(kind, l, cast(r, l->ty), l->ty, t);
}
static Node *compare(int kind, Node *l, Node *r, Tok *t) {
  l = rval(l);
  r = rval(r);
  if (is_arith(l->ty) && is_arith(r->ty)) {
    Type *ty = common_type(l, r);
    return binary(kind, cast(l, ty), cast(r, ty), ty_int, t);
  }
  if (is_ptr(l->ty) && is_ptr(r->ty)) {
    if (l->ty->base->kind != TY_VOID && r->ty->base->kind != TY_VOID && !same_type(unqual(l->ty->base), unqual(r->ty->base)) && opt_wall)
      warn_at(t, "comparison of distinct pointer types ('%s' and '%s')", type_str(l->ty), type_str(r->ty));
    return binary(kind, cast(l, ty_ulong), cast(r, ty_ulong), ty_int, t);
  }
  if (is_ptr(l->ty) && is_integer(r->ty)) {
    if (!is_null_const(r)) warn_at(t, "comparison between pointer and integer");
    return binary(kind, cast(l, ty_ulong), cast(r, ty_ulong), ty_int, t);
  }
  if (is_integer(l->ty) && is_ptr(r->ty)) {
    if (!is_null_const(l)) warn_at(t, "comparison between pointer and integer");
    return binary(kind, cast(l, ty_ulong), cast(r, ty_ulong), ty_int, t);
  }
  error_at(t, "invalid operands to comparison ('%s' and '%s')", type_str(l->ty), type_str(r->ty));
  return NULL;
}
static Node *truth(Node *n) {
  n = rval(n);
  check_scalar(n);
  return n;
}
static Node *logical(int kind, Node *l, Node *r, Tok *t) {
  l = truth(l);
  r = truth(r);
  if (l->kind == N_NUM && r->kind == N_NUM)
    return new_num(kind == N_LAND ? (l->ival && r->ival) : (l->ival || r->ival), ty_int, t);
  if (l->kind == N_NUM && kind == N_LAND && !l->ival) return new_num(0, ty_int, t);
  if (l->kind == N_NUM && kind == N_LOR && l->ival) return new_num(1, ty_int, t);
  Node *n = new_node(kind, t);
  n->lhs = l;
  n->rhs = r;
  n->ty = ty_int;
  return n;
}

static int is_lvalue(Node *n) {
  switch (n->kind) {
  case N_VAR: case N_DEREF: case N_INITVAR: return 1;
  case N_MEMBER: return 1;
  }
  return 0;
}
static void check_modifiable(Node *n, Tok *t) {
  if (!is_lvalue(n)) error_at(t, "expression is not assignable");
  if (n->ty->kind == TY_ARRAY) error_at(t, "array type '%s' is not assignable", type_str(n->ty));
  if (n->ty->kind == TY_FUNC) error_at(t, "a function is not assignable");
  if (n->ty->qual & Q_CONST) warn_at(t, "assignment to a const object");
}

static Node *do_assign(Node *l, Node *r, Tok *t) {
  check_modifiable(l, t);
  Node *n = new_node(N_ASSIGN, t);
  n->lhs = l;
  n->rhs = assign_conv(r, unqual(l->ty), t, "assignment");
  n->ty = unqual(l->ty);
  return n;
}

static Node *op_assign(int kind, Node *l, Node *r, Tok *t) {
  check_modifiable(l, t);
  r = rval(r);
  Node *n = new_node(N_OPASSIGN, t);
  n->op = kind;
  n->lhs = l;
  n->ty = unqual(l->ty);
  if (is_ptr(l->ty) && (kind == N_ADD || kind == N_SUB)) {
    if (!is_integer(r->ty)) error_at(t, "invalid operands to compound assignment");
    Node *off = scale(r, elem_size(l->ty, t), t);
    if (off->ty->size == 8) off = cast(off, ty_long);
    n->rhs = off;
    n->fty = l->ty;
    return n;
  }
  if (kind == N_SHL || kind == N_SHR) {
    if (!is_integer(l->ty) || !is_integer(r->ty)) error_at(t, "invalid operands to shift");
    n->fty = promoted(l);
    n->rhs = cast(promote(r), n->fty);
    return n;
  }
  int intonly = kind == N_MOD || kind == N_BAND || kind == N_BOR || kind == N_BXOR;
  if (!(intonly ? is_integer(l->ty) && is_integer(r->ty) : is_arith(l->ty) && is_arith(r->ty)))
    error_at(t, "invalid operands to compound assignment ('%s' and '%s')", type_str(l->ty), type_str(r->ty));
  n->fty = common_type(l, r);
  n->rhs = cast(r, n->fty);
  return n;
}

static Node *incdec(Node *l, int delta, int post, Tok *t) {
  check_modifiable(l, t);
  if (!is_scalar(l->ty)) error_at(t, "cannot increment or decrement '%s'", type_str(l->ty));
  Node *n = new_node(post ? N_POSTINC : N_PREINC, t);
  n->lhs = l;
  n->ty = unqual(l->ty);
  if (is_ptr(l->ty)) { n->ival = (i64)delta * elem_size(l->ty, t); n->fty = l->ty; }
  else { n->ival = delta; n->fty = is_flonum(l->ty) ? unqual(l->ty) : promoted(l); }
  return n;
}

static Node *addr_of(Node *n, Tok *t) {
  if (n->kind == N_MEMBER && n->mem->bitwidth) error_at(t, "cannot take the address of a bit-field");
  if (n->ty->kind == TY_FUNC) {
    Node *a = new_node(N_ADDR, t);
    a->lhs = n;
    a->ty = ptr_to(n->ty);
    return a;
  }
  if (!is_lvalue(n)) error_at(t, "cannot take the address of an rvalue");
  if (n->kind == N_DEREF) {
    Node *p = n->lhs;
    if (p->ty->kind == TY_PTR && same_type(p->ty->base, n->ty)) return p;
  }
  if (n->kind == N_VAR) n->var->addr_taken = 1;
  if (n->kind == N_INITVAR) n->var->addr_taken = 1;
  Node *a = new_node(N_ADDR, t);
  a->lhs = n;
  a->ty = ptr_to(n->ty);
  return a;
}

static Node *deref(Node *n, Tok *t) {
  n = rval(n);
  if (!is_ptr(n->ty)) error_at(t, "indirection requires a pointer operand ('%s' invalid)", type_str(n->ty));
  if (n->ty->base->kind == TY_FUNC && n->kind == N_ADDR) return n->lhs;
  if (n->ty->base->kind == TY_VOID) warn_at(t, "dereferencing a 'void *' pointer");
  Node *d = new_node(N_DEREF, t);
  d->lhs = n;
  d->ty = n->ty->base;
  return d;
}

/* Find a member, looking through anonymous structs and unions. Builds the chain of
 * N_MEMBER nodes. */
static Member *find_member(Rec *r, Ident *name, Member **path, int *np) {
  for (Member *m = r->members; m; m = m->next) {
    if (m->name == name) { path[(*np)++] = m; return m; }
    if (!m->name && is_aggr(m->ty) && !m->bitwidth) {
      int save = *np;
      path[(*np)++] = m;
      if (find_member(m->ty->rec, name, path, np)) return m;
      *np = save;
    }
  }
  return NULL;
}
static Node *member(Node *s, Ident *name, Tok *t) {
  if (!is_aggr(s->ty)) error_at(t, "member reference base type '%s' is not a structure or union", type_str(s->ty));
  if (!s->ty->rec->complete) error_at(t, "incomplete type '%s'", type_str(s->ty));
  Member *path[64];
  int np = 0;
  if (!find_member(s->ty->rec, name, path, &np)) error_at(t, "no member named '%s' in '%s'", name->name, type_str(s->ty));
  for (int i = 0; i < np; i++) {
    Node *n = new_node(N_MEMBER, t);
    n->lhs = s;
    n->mem = path[i];
    n->ty = qualify(path[i]->ty, s->ty->qual);
    s = n;
  }
  return s;
}

static Node *comma(Node *l, Node *r, Tok *t) {
  Node *n = new_node(N_COMMA, t);
  n->lhs = l;
  n->rhs = rval(r);
  n->ty = n->rhs->ty;
  return n;
}

/* ---- declarations ---- */
static Type *declspec(DeclSpec *ds);
static Type *declarator(Type *ty, Tok **name);
static Type *abstract_declarator(Type *ty);
static Type *type_name(void);
static Node *assign(void);
static Node *expr(void);
static Node *cast_expr(void);
static Node *conditional(void);
static Node *compound_stmt(void);
static Node *stmt(void);
static void parse_attrs(Attr *a);
static Init *initializer_for(Type *ty, Type **outty);

static void decode_string(Tok *t, Buf *out, int esz);
static i64 const_int(void) {
  Tok *t = peek();
  Node *n = conditional();
  n = rval(n);
  if (n->kind != N_NUM || !is_integer(n->ty)) error_at(t, "expression is not an integer constant expression");
  return n->ival;
}

static void static_assert_decl(void) {
  Tok *t = next();
  expect('(');
  i64 v = const_int();
  const char *msg = NULL;
  if (accept(',')) {
    Tok *s = next();
    if (s->kind != T_STR) error_at(s, "expected a string literal");
    Buf b = {0};
    tpos--;
    while (peek()->kind == T_STR) decode_string(next(), &b, 1);
    buf_byte(&b, 0);
    msg = (char *)b.p;
  }
  expect(')');
  expect(';');
  if (!v) error_at(t, "static assertion failed%s%s", msg ? ": " : "", msg ? msg : "");
}

static const char *attr_string(Tok *where) {
  expect('(');
  Tok *s = next();
  if (s->kind != T_STR) error_at(s, "expected a string literal");
  expect(')');
  (void)where;
  return xstrndup(s->s + 1, s->len - 2);
}

static void parse_attrs(Attr *a) {
  for (;;) {
    if (accept_kw(KW_ASM)) {
      expect('(');
      Tok *s = next();
      if (s->kind != T_STR) error_at(s, "expected a string literal in asm label");
      if (a) a->asmname = xstrndup(s->s + 1, s->len - 2);
      expect(')');
      continue;
    }
    if (!accept_kw(KW_ATTRIBUTE)) return;
    expect('(');
    expect('(');
    while (!is_p(peek(), ')')) {
      if (accept(',')) continue;
      Tok *t = next();
      if (t->kind != T_IDENT) error_at(t, "expected an attribute name");
      const char *n = t->id->name;
      size_t l = strlen(n);
      char buf[64];
      if (l > 4 && l < 60 && n[0] == '_' && n[1] == '_' && n[l - 1] == '_' && n[l - 2] == '_') {
        memcpy(buf, n + 2, l - 4);
        buf[l - 4] = 0;
        n = buf;
      }
      Attr dummy;
      if (!a) { memset(&dummy, 0, sizeof dummy); a = &dummy; }
      if (!strcmp(n, "noreturn")) a->noreturn = 1;
      else if (!strcmp(n, "weak")) a->weak = 1;
      else if (!strcmp(n, "packed")) a->packed = 1;
      else if (!strcmp(n, "noinline")) a->noinline = 1;
      else if (!strcmp(n, "used")) a->used = 1;
      else if (!strcmp(n, "deprecated")) { a->deprecated = 1; if (is_p(peek(), '(')) attr_string(t); }
      else if (!strcmp(n, "import_module")) a->imp_mod = attr_string(t);
      else if (!strcmp(n, "import_name")) a->imp_name = attr_string(t);
      else if (!strcmp(n, "export_name")) a->exp_name = attr_string(t);
      else if (!strcmp(n, "visibility")) a->vis_default = !strcmp(attr_string(t), "default");
      else if (!strcmp(n, "aligned")) {
        if (accept('(')) {
          i64 v = const_int();
          if (v <= 0 || (v & (v - 1))) error_at(t, "requested alignment is not a positive power of 2");
          if (v > a->aligned) a->aligned = (int)v;
          expect(')');
        } else {
          a->aligned = 16;
        }
      } else if (!strcmp(n, "constructor") || !strcmp(n, "destructor")) {
        warn_at(t, "attribute '%s' is not supported and is ignored", n);
        if (accept('(')) { const_int(); expect(')'); }
      } else if (!strcmp(n, "alias") || !strcmp(n, "ifunc")) {
        error_at(t, "attribute '%s' is not supported", n);
      } else {
        /* format, unused, nonnull, const, pure, cold, malloc, section and the rest:
         * accepted, no effect on the generated code */
        if (accept('(')) {
          int d = 1;
          while (d) {
            Tok *x = next();
            if (x->kind == T_EOF) error_at(t, "unterminated attribute");
            if (is_p(x, '(')) d++;
            else if (is_p(x, ')')) d--;
          }
        }
      }
      if (a == &dummy) a = NULL;
    }
    expect(')');
    expect(')');
  }
}

static int type_qualifiers(void) {
  int q = 0;
  for (;;) {
    if (accept_kw(KW_CONST)) q |= Q_CONST;
    else if (accept_kw(KW_VOLATILE)) q |= Q_VOLATILE;
    else if (accept_kw(KW_RESTRICT)) q |= Q_RESTRICT;
    else if (is_kw(peek(), KW_ATOMIC) && !is_p(peekn(1), '(')) next();
    else if (is_kw(peek(), KW_ATTRIBUTE)) parse_attrs(NULL);
    else return q;
  }
}

static void layout_struct(Rec *r, Tok *t) {
  int bits = 0, align = 1, maxsize = 0, idx = 0;
  for (Member *m = r->members; m; m = m->next) {
    m->idx = idx++;
    Type *ty = m->ty;
    int a = ty->align;
    if (r->packed) a = 1;
    if (m->bitwidth || (m->name == NULL && m->offset == -2)) {
      int w = m->bitwidth;
      int unit = ty->size * 8;
      if (r->is_union) {
        m->offset = 0;
        m->bitoff = 0;
        if (ty->size > maxsize) maxsize = ty->size;
        if (m->name && a > align) align = a;
        continue;
      }
      if (w == 0) {
        bits = (bits + ty->align * 8 - 1) / (ty->align * 8) * (ty->align * 8);
        m->offset = bits / 8;
        continue;
      }
      if (!r->packed && bits / unit != (bits + w - 1) / unit) bits = (bits + ty->align * 8 - 1) / (ty->align * 8) * (ty->align * 8);
      if (r->packed) {
        /* packed bit-fields: container starts at the byte that holds the first bit */
        m->offset = bits / 8;
        m->bitoff = bits % 8;
        if (m->bitoff + w > ty->size * 8) error_at(m->tok, "packed bit-field crossing its container is not supported");
      } else {
        m->offset = bits / unit * ty->size;
        m->bitoff = bits - m->offset * 8;
      }
      bits += w;
      if (m->name && a > align) align = a;
      continue;
    }
    if (r->is_union) {
      m->offset = 0;
      if (ty->size > maxsize) maxsize = ty->size;
      if (a > align) align = a;
      continue;
    }
    bits = (bits + a * 8 - 1) / (a * 8) * (a * 8);
    m->offset = bits / 8;
    if (ty->kind == TY_ARRAY && ty->len < 0) {
      if (m->next) error_at(m->tok, "flexible array member must be the last member");
      r->flexible = 1;
    } else {
      if (ty->size == 0 && ty->kind == TY_STRUCT && !ty->rec->complete) error_at(m->tok, "member has incomplete type '%s'", type_str(ty));
      bits += ty->size * 8;
    }
    if (a > align) align = a;
  }
  if (r->align > align) align = r->align;
  int size = r->is_union ? maxsize : (bits + 7) / 8;
  r->size = (size + align - 1) / align * align;
  r->align = align;
  r->nmembers = idx;
  r->marr = xalloc((idx + 1) * sizeof *r->marr);
  for (Member *m = r->members; m; m = m->next) r->marr[m->idx] = m;
  r->complete = 1;
  (void)t;
}

static Type *rec_type(Rec *r) {
  Type *t = xalloc(sizeof *t);
  t->kind = r->is_union ? TY_UNION : TY_STRUCT;
  t->rec = r;
  t->size = r->size;
  t->align = r->align;
  rec_track(t);
  return t;
}

static Type *struct_decl(int is_union) {
  Tok *kwt = tk[tpos - 1];
  Attr a = {0};
  parse_attrs(&a);
  Ident *tag = NULL;
  Tok *tagtok = peek();
  if (peek()->kind == T_IDENT && !peek()->id->kw) tag = next()->id;
  parse_attrs(&a);
  if (!tag && !is_p(peek(), '{')) error_at(peek(), "expected '{' or a tag name after '%s'", is_union ? "union" : "struct");
  if (!is_p(peek(), '{')) {
    Binding *b = tag->tag;
    int fwd = is_p(peek(), ';');
    if (b && !(fwd && b->depth != depth)) {
      if (b->ty->kind != (is_union ? TY_UNION : TY_STRUCT)) error_at(tagtok, "'%s' defined as the wrong kind of tag", tag->name);
      return b->ty;
    }
    Rec *r = xalloc(sizeof *r);
    r->tag = tag;
    r->is_union = is_union;
    Type *t = rec_type(r);
    bind(tag, BK_TAG)->ty = t;
    return t;
  }
  next(); /* { */
  Rec *r = NULL;
  Type *ty = NULL;
  if (tag && tag->tag && tag->tag->depth == depth) {
    ty = tag->tag->ty;
    if (ty->kind != (is_union ? TY_UNION : TY_STRUCT)) error_at(tagtok, "'%s' defined as the wrong kind of tag", tag->name);
    r = ty->rec;
    if (r->complete) error_at(tagtok, "redefinition of '%s %s'", is_union ? "union" : "struct", tag->name);
  } else {
    r = xalloc(sizeof *r);
    r->tag = tag;
    r->is_union = is_union;
    ty = rec_type(r);
    if (tag) bind(tag, BK_TAG)->ty = ty;
  }
  Member head = {0}, *cur = &head;
  while (!accept('}')) {
    if (is_kw(peek(), KW_STATIC_ASSERT)) { static_assert_decl(); continue; }
    if (accept(';')) continue;
    DeclSpec ds = {0};
    Tok *st = peek();
    Type *base = declspec(&ds);
    if (ds.storage) error_at(st, "storage class in a struct member");
    if (is_aggr(base) && accept(';')) {
      /* anonymous struct or union member (C11) */
      Member *m = xalloc(sizeof *m);
      m->ty = base;
      m->tok = st;
      cur = cur->next = m;
      continue;
    }
    for (int first = 1; !accept(';'); first = 0) {
      if (!first) expect(',');
      Member *m = xalloc(sizeof *m);
      Tok *name = NULL;
      m->tok = peek();
      if (is_p(peek(), ':')) m->ty = base;
      else m->ty = declarator(base, &name);
      if (name) { m->name = name->id; m->tok = name; }
      Attr ma = {0};
      parse_attrs(&ma);
      if (accept(':')) {
        Tok *wt = peek();
        i64 w = const_int();
        if (!is_integer(m->ty)) error_at(wt, "bit-field has non-integral type '%s'", type_str(m->ty));
        if (w < 0 || w > m->ty->size * 8) error_at(wt, "bit-field width %lld is out of range", (long long)w);
        if (w == 0 && name) error_at(wt, "named bit-field has zero width");
        m->bitwidth = (int)w;
        if (w == 0) m->offset = -2;
        parse_attrs(&ma);
      }
      if (m->ty->kind == TY_FUNC) error_at(m->tok, "member '%s' declared as a function", name ? name->id->name : "");
      if (ma.aligned || ds.align) {
        int al = ma.aligned > ds.align ? ma.aligned : ds.align;
        Type *c = xalloc(sizeof *c);
        *c = *(m->ty);
        if (al > c->align) c->align = al;
        m->ty = c;
      }
      if (ma.packed && m->ty->align > 1) { Type *c = xalloc(sizeof *c); *c = *m->ty; c->align = 1; m->ty = c; }
      if (!m->name && !m->bitwidth && m->offset != -2) continue;
      cur = cur->next = m;
    }
  }
  r->members = head.next;
  Attr post = {0};
  parse_attrs(&post);
  r->packed = a.packed || post.packed;
  if (a.aligned || post.aligned) r->align = a.aligned > post.aligned ? a.aligned : post.aligned;
  layout_struct(r, kwt);
  rec_completed(r);
  return ty;
}

static Type *enum_decl(void) {
  Attr a = {0};
  parse_attrs(&a);
  Ident *tag = NULL;
  Tok *tagtok = peek();
  if (peek()->kind == T_IDENT && !peek()->id->kw) tag = next()->id;
  parse_attrs(&a);
  if (!is_p(peek(), '{')) {
    if (!tag) error_at(peek(), "expected '{' after 'enum'");
    if (tag->tag) {
      if (!tag->tag->ty->is_enum) error_at(tagtok, "'%s' defined as the wrong kind of tag", tag->name);
      return tag->tag->ty;
    }
    Type *t = xalloc(sizeof *t);
    *t = *ty_uint;
    t->is_enum = 1;
    bind(tag, BK_TAG)->ty = t;
    return t;
  }
  next();
  Type *t = xalloc(sizeof *t);
  *t = *ty_uint;
  t->is_enum = 1;
  if (tag) bind(tag, BK_TAG)->ty = t;
  i64 val = 0, minv = 0, maxv = 0;
  int first = 1;
  Vec consts = {0};
  while (!accept('}')) {
    if (!first) { expect(','); if (accept('}')) break; }
    first = 0;
    Tok *nt = peek();
    Ident *name = expect_ident();
    parse_attrs(NULL);
    if (accept('=')) val = const_int();
    Binding *b = bind(name, BK_ENUM);
    b->val = val;
    b->ty = ty_int;
    vec_push(&consts, b);
    (void)nt;
    if (val < minv) minv = val;
    if (val > maxv) maxv = val;
    val++;
  }
  parse_attrs(NULL);
  if (minv < 0) *t = *ty_int;
  if (minv < INT32_MIN || maxv > (minv < 0 ? INT32_MAX : (i64)UINT32_MAX)) *t = minv < 0 ? *ty_llong : *ty_ullong;
  t->is_enum = 1;
  /* constants that do not fit int take the enum's type (a common extension) */
  for (int i = 0; i < consts.len; i++) {
    Binding *b = consts.p[i];
    if (b->val < INT32_MIN || b->val > INT32_MAX) b->ty = t;
  }
  return t;
}

static Type *typeof_spec(int unq) {
  expect('(');
  Type *t;
  if (is_typename(peek())) t = type_name();
  else { Node *e = expr(); t = e->ty; }
  expect(')');
  return unq ? unqual(t) : t;
}

enum {
  S_VOID = 1 << 0, S_BOOL = 1 << 2, S_CHAR = 1 << 4, S_SHORT = 1 << 6, S_INT = 1 << 8,
  S_LONG = 1 << 10, S_FLOAT = 1 << 12, S_DOUBLE = 1 << 14, S_OTHER = 1 << 16,
  S_SIGNED = 1 << 17, S_UNSIGNED = 1 << 18
};

static Type *declspec(DeclSpec *ds) {
  int cnt = 0, qual = 0;
  Type *ty = ty_int;
  Tok *first = peek();
  for (;;) {
    Tok *t = peek();
    if (t->kind != T_IDENT) break;
    int kw = t->id->kw;
    if (kw == KW_ATTRIBUTE) { parse_attrs(ds ? &ds->attr : NULL); continue; }
    if (kw == KW_TYPEDEF || kw == KW_EXTERN || kw == KW_STATIC || kw == KW_AUTO || kw == KW_REGISTER) {
      if (!ds) error_at(t, "storage class specifier is not allowed here");
      int sc = kw == KW_TYPEDEF ? SC_TYPEDEF : kw == KW_EXTERN ? SC_EXTERN : kw == KW_STATIC ? SC_STATIC : kw == KW_AUTO ? SC_AUTO : SC_REGISTER;
      if (ds->storage && ds->storage != sc) error_at(t, "cannot combine storage class specifiers");
      ds->storage = sc;
      next();
      continue;
    }
    if (kw == KW_THREAD_LOCAL) { if (ds) ds->tls = 1; next(); continue; }
    if (kw == KW_INLINE) { if (ds) ds->is_inline = 1; next(); continue; }
    if (kw == KW_NORETURN) { if (ds) ds->noreturn = 1; next(); continue; }
    if (kw == KW_CONST) { qual |= Q_CONST; next(); continue; }
    if (kw == KW_VOLATILE) { qual |= Q_VOLATILE; next(); continue; }
    if (kw == KW_RESTRICT) { qual |= Q_RESTRICT; next(); continue; }
    if (kw == KW_ATOMIC) {
      next();
      if (accept('(')) { ty = type_name(); expect(')'); cnt += S_OTHER; }
      continue;
    }
    if (kw == KW_ALIGNAS) {
      next();
      expect('(');
      int al;
      if (is_typename(peek())) al = type_name()->align;
      else al = (int)const_int();
      expect(')');
      if (ds && al > ds->align) ds->align = al;
      continue;
    }
    if (kw == KW_COMPLEX || kw == KW_IMAGINARY) error_at(t, "_Complex is not supported");
    if (kw == KW_INT128) error_at(t, "__int128 is not supported");
    if (kw == KW_STRUCT || kw == KW_UNION || kw == KW_ENUM || kw == KW_TYPEOF || kw == KW_TYPEOF_UNQUAL || kw == KW_VA_LIST ||
        (kw == 0 && t->id->sym && t->id->sym->kind == BK_TYPEDEF)) {
      if (cnt) break;
      next();
      if (kw == KW_STRUCT || kw == KW_UNION) ty = struct_decl(kw == KW_UNION);
      else if (kw == KW_ENUM) ty = enum_decl();
      else if (kw == KW_TYPEOF || kw == KW_TYPEOF_UNQUAL) ty = typeof_spec(kw == KW_TYPEOF_UNQUAL);
      else if (kw == KW_VA_LIST) ty = ptr_to(ty_char);
      else ty = t->id->sym->ty;
      cnt += S_OTHER;
      continue;
    }
    int add;
    switch (kw) {
    case KW_VOID: add = S_VOID; break;
    case KW_BOOL: add = S_BOOL; break;
    case KW_CHAR: add = S_CHAR; break;
    case KW_SHORT: add = S_SHORT; break;
    case KW_INT: add = S_INT; break;
    case KW_LONG: add = S_LONG; break;
    case KW_FLOAT: add = S_FLOAT; break;
    case KW_DOUBLE: add = S_DOUBLE; break;
    case KW_SIGNED: add = S_SIGNED; break;
    case KW_UNSIGNED: add = S_UNSIGNED; break;
    default: add = 0;
    }
    if (!add) break;
    if (cnt & S_OTHER) error_at(t, "invalid combination of type specifiers");
    cnt += add;
    next();
    switch (cnt) {
    case S_VOID: ty = ty_void; break;
    case S_BOOL: ty = ty_bool; break;
    case S_CHAR: ty = ty_char; break;
    case S_SIGNED + S_CHAR: ty = ty_schar; break;
    case S_UNSIGNED + S_CHAR: ty = ty_uchar; break;
    case S_SHORT: case S_SHORT + S_INT: case S_SIGNED + S_SHORT: case S_SIGNED + S_SHORT + S_INT: ty = ty_short; break;
    case S_UNSIGNED + S_SHORT: case S_UNSIGNED + S_SHORT + S_INT: ty = ty_ushort; break;
    case S_INT: case S_SIGNED: case S_SIGNED + S_INT: ty = ty_int; break;
    case S_UNSIGNED: case S_UNSIGNED + S_INT: ty = ty_uint; break;
    case S_LONG: case S_LONG + S_INT: case S_SIGNED + S_LONG: case S_SIGNED + S_LONG + S_INT: ty = ty_long; break;
    case S_UNSIGNED + S_LONG: case S_UNSIGNED + S_LONG + S_INT: ty = ty_ulong; break;
    case S_LONG + S_LONG: case S_LONG + S_LONG + S_INT: case S_SIGNED + S_LONG + S_LONG:
    case S_SIGNED + S_LONG + S_LONG + S_INT: ty = ty_llong; break;
    case S_UNSIGNED + S_LONG + S_LONG: case S_UNSIGNED + S_LONG + S_LONG + S_INT: ty = ty_ullong; break;
    case S_FLOAT: ty = ty_float; break;
    case S_DOUBLE: ty = ty_double; break;
    case S_LONG + S_DOUBLE: ty = ty_ldouble; break;
    default: error_at(t, "invalid combination of type specifiers");
    }
  }
  if (cnt == 0 && peek() == first) error_at(first, "expected a type");
  if (cnt == 0 && opt_wall && ds && !ds->storage) warn_at(first, "type specifier missing, defaults to 'int'");
  return qualify(ty, qual);
}

static Type *func_params(Type *ret) {
  Type *f = func_type(ret);
  f->proto = 1;
  if (is_kw(peek(), KW_VOID) && is_p(peekn(1), ')')) { next(); next(); return f; }
  if (accept(')')) { f->proto = 0; return f; }
  /* K&R identifier list */
  if (peek()->kind == T_IDENT && !peek()->id->kw && !is_typename(peek()) && (is_p(peekn(1), ',') || is_p(peekn(1), ')'))) {
    Vec ps = {0};
    for (;;) {
      Param *p = xalloc(sizeof *p);
      p->tok = peek();
      p->name = expect_ident();
      p->ty = ty_int;
      vec_push(&ps, p);
      if (accept(')')) break;
      expect(',');
    }
    f->proto = 0;
    f->nparams = ps.len;
    f->params = xalloc(ps.len * sizeof(Param));
    for (int i = 0; i < ps.len; i++) f->params[i] = *(Param *)ps.p[i];
    f->variadic = -1; /* marks a K&R list until the definition supplies types */
    return f;
  }
  Vec ps = {0};
  push_scope(); /* prototype scope: struct tags declared here stay here */
  for (;;) {
    if (accept(P_ELLIPSIS)) { f->variadic = 1; expect(')'); break; }
    DeclSpec ds = {0};
    Tok *st = peek();
    Type *base = declspec(&ds);
    if (ds.storage && ds.storage != SC_REGISTER) error_at(st, "invalid storage class for a parameter");
    Tok *name = NULL;
    Type *t = declarator(base, &name);
    parse_attrs(NULL);
    if (t->kind == TY_ARRAY) t = qualify(ptr_to(t->base), t->qual);
    else if (t->kind == TY_FUNC) t = ptr_to(t);
    if (t->kind == TY_VOID) error_at(st, "parameter has type void");
    Param *p = xalloc(sizeof *p);
    p->ty = t;
    p->name = name ? name->id : NULL;
    p->tok = name ? name : st;
    vec_push(&ps, p);
    if (accept(')')) break;
    expect(',');
  }
  pop_scope();
  f->nparams = ps.len;
  f->params = xalloc((ps.len + 1) * sizeof(Param));
  for (int i = 0; i < ps.len; i++) f->params[i] = *(Param *)ps.p[i];
  return f;
}

static Type *type_suffix(Type *ty) {
  if (accept('(')) {
    if (ty->kind == TY_FUNC) error_at(tk[tpos - 1], "a function cannot return a function");
    if (ty->kind == TY_ARRAY) error_at(tk[tpos - 1], "a function cannot return an array");
    return func_params(ty);
  }
  if (accept('[')) {
    Tok *t = tk[tpos - 1];
    int q = 0;
    for (;;) {
      if (accept_kw(KW_STATIC)) continue;
      int qq = type_qualifiers();
      if (!qq) break;
      q |= qq;
    }
    int len = -1;
    Node *vla = NULL;
    if (accept('*')) {
      expect(']');
    } else if (!accept(']')) {
      Tok *lt = peek();
      Node *n = rval(assign());
      if (n->kind == N_NUM && is_integer(n->ty)) {
        if (n->ival < 0) error_at(lt, "array has negative size");
        if (n->ival > 0x7fffffff) error_at(lt, "array is too large");
        len = (int)n->ival;
      } else {
        if (!is_integer(n->ty)) error_at(lt, "array size has non-integer type '%s'", type_str(n->ty));
        vla = n;
      }
      expect(']');
    }
    Type *base = type_suffix(ty);
    if (base->kind == TY_FUNC) error_at(t, "array of functions");
    if (base->kind == TY_VOID) error_at(t, "array of void");
    Type *a = array_of(base, len);
    if (vla) { a->vla_len = vla; a->len = -2; a->size = 0; }
    (void)q;
    return a;
  }
  return ty;
}

static int nested_declarator_start(void) {
  /* '(' that begins a nested declarator rather than a parameter list */
  Tok *n = peekn(1);
  if (is_p(n, '*') || is_p(n, '(') || is_p(n, '[') || is_p(n, '^')) return 1;
  if (is_kw(n, KW_ATTRIBUTE)) return 1;
  if (n->kind == T_IDENT && !is_typename(n)) return 1;
  return 0;
}

static Type *declarator(Type *ty, Tok **name) {
  for (;;) {
    parse_attrs(NULL);
    if (!accept('*')) break;
    int q = type_qualifiers();
    ty = qualify(ptr_to(ty), q);
  }
  parse_attrs(NULL);
  if (is_p(peek(), '(') && nested_declarator_start()) {
    int start = ++tpos, d = 1;
    while (d) {
      Tok *t = next();
      if (t->kind == T_EOF) error_at(tk[start - 1], "unbalanced parentheses in declarator");
      if (is_p(t, '(')) d++;
      else if (is_p(t, ')')) d--;
    }
    Type *outer = type_suffix(ty);
    int end = tpos;
    tpos = start;
    Type *t = declarator(outer, name);
    expect(')');
    tpos = end;
    return t;
  }
  if (peek()->kind == T_IDENT && !peek()->id->kw) {
    if (name) *name = peek();
    next();
  }
  return type_suffix(ty);
}
static Type *abstract_declarator(Type *ty) { return declarator(ty, NULL); }
static Type *type_name(void) {
  Type *t = declspec(NULL);
  return abstract_declarator(t);
}

/* ---- literals ---- */
static Node *number(Tok *t) {
  char buf[256];
  if (t->len >= (int)sizeof buf) error_at(t, "number too long");
  memcpy(buf, t->s, t->len);
  buf[t->len] = 0;
  int ishex = buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X');
  int isfloat = 0;
  for (int i = 0; buf[i]; i++) {
    if (buf[i] == '.' || (!ishex && (buf[i] == 'e' || buf[i] == 'E')) || (ishex && (buf[i] == 'p' || buf[i] == 'P'))) isfloat = 1;
  }
  if (isfloat) {
    char *end;
    int len = t->len;
    Type *ty = ty_double;
    char c = buf[len - 1];
    if (c == 'f' || c == 'F') { ty = ty_float; buf[--len] = 0; }
    else if (c == 'l' || c == 'L') { ty = ty_ldouble; buf[--len] = 0; }
    double v = strtod(buf, &end);
    if (*end) error_at(t, "invalid floating constant '%.*s'", t->len, t->s);
    if (ty == ty_float) v = strtof(buf, &end);
    Node *n = new_node(N_FNUM, t);
    n->ty = ty;
    n->fval = v;
    return n;
  }
  u64 v = 0;
  const char *p = buf;
  int base = 10;
  if (ishex) { base = 16; p += 2; }
  else if (buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) { base = 2; p += 2; }
  else if (buf[0] == '0') base = 8;
  int overflow = 0, digits = 0;
  for (; *p; p++) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else if (*p == '\'') continue;
    else break;
    if (d >= base) break;
    if (v > (UINT64_MAX - d) / base) overflow = 1;
    v = v * base + d;
    digits++;
  }
  int u = 0, l = 0;
  for (; *p; p++) {
    if ((*p == 'u' || *p == 'U') && !u) u = 1;
    else if ((*p == 'l' || *p == 'L') && !l) { l = 1; if (p[1] == *p) { l = 2; p++; } }
    else error_at(t, "invalid suffix '%s' on integer constant", p);
  }
  if (!digits && base != 8) error_at(t, "invalid integer constant '%s'", buf);
  if (overflow) warn_at(t, "integer constant is too large for its type");
  Type *ty;
  if (base == 10) {
    if (u) ty = l == 2 || v > UINT32_MAX ? ty_ullong : l ? ty_ulong : ty_uint;
    else if (l == 2) ty = v > INT64_MAX ? ty_ullong : ty_llong;
    else if (v <= INT32_MAX) ty = l ? ty_long : ty_int;
    else ty = v > INT64_MAX ? ty_ullong : ty_llong;
  } else {
    if (u) ty = l == 2 || v > UINT32_MAX ? ty_ullong : l ? ty_ulong : ty_uint;
    else if (l == 2) ty = v > INT64_MAX ? ty_ullong : ty_llong;
    else if (v <= INT32_MAX) ty = l ? ty_long : ty_int;
    else if (v <= UINT32_MAX) ty = l ? ty_ulong : ty_uint;
    else ty = v > INT64_MAX ? ty_ullong : ty_llong;
  }
  return new_num((i64)v, ty, t);
}

i64 char_value(Tok *t, int *ok);

static Var *new_global_var(const char *name, Type *ty) {
  Var *v = xalloc(sizeof *v);
  v->name = intern(name, (int)strlen(name));
  v->ty = ty;
  v->is_static = 1;
  v->is_def = 1;
  vec_push(&globals, v);
  return v;
}

/* Decode a string literal (with escapes and UTF-8) into elements of size esz. */
static void decode_string(Tok *t, Buf *out, int esz) {
  const char *p = t->s;
  while (*p != '"') p++;
  p++;
  while (*p != '"') {
    u32 c;
    int raw_byte = 0;
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
      case 'x':
        p++;
        c = 0;
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) error_at(t, "\\x used with no following hex digits");
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
          c = c * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
          p++;
        }
        raw_byte = 1;
        break;
      case 'u': case 'U': {
        int nd = *p == 'u' ? 4 : 8;
        p++;
        c = 0;
        for (int i = 0; i < nd; i++, p++) c = c * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
        break;
      }
      default:
        if (*p >= '0' && *p <= '7') {
          c = 0;
          for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++) c = c * 8 + *p++ - '0';
          raw_byte = 1;
        } else {
          c = (u8)*p++;
        }
      }
    } else if (esz > 1 && (u8)*p >= 0x80) {
      u8 b = (u8)*p++;
      int extra = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
      c = b & (0x3f >> extra);
      while (extra-- && ((u8)*p & 0xc0) == 0x80) c = (c << 6) | ((u8)*p++ & 0x3f);
    } else {
      c = (u8)*p++;
      raw_byte = 1;
    }
    if (esz == 1) {
      if (raw_byte || c < 0x80) buf_byte(out, c & 0xff);
      else if (c < 0x800) { buf_byte(out, 0xc0 | c >> 6); buf_byte(out, 0x80 | (c & 63)); }
      else if (c < 0x10000) { buf_byte(out, 0xe0 | c >> 12); buf_byte(out, 0x80 | ((c >> 6) & 63)); buf_byte(out, 0x80 | (c & 63)); }
      else { buf_byte(out, 0xf0 | c >> 18); buf_byte(out, 0x80 | ((c >> 12) & 63)); buf_byte(out, 0x80 | ((c >> 6) & 63)); buf_byte(out, 0x80 | (c & 63)); }
    } else if (esz == 2) {
      if (c >= 0x10000) {
        c -= 0x10000;
        u32 hi = 0xd800 + (c >> 10), lo = 0xdc00 + (c & 0x3ff);
        buf_byte(out, hi); buf_byte(out, hi >> 8); buf_byte(out, lo); buf_byte(out, lo >> 8);
      } else {
        buf_byte(out, c); buf_byte(out, c >> 8);
      }
    } else {
      buf_u32(out, c);
    }
  }
}

static Node *string_lit(void) {
  Tok *first = peek();
  int esz = 1;
  Type *ety = ty_char;
  for (int i = 0; peekn(i)->kind == T_STR; i++) {
    Tok *t = peekn(i);
    if (t->s[0] == 'L') { esz = 4; ety = ty_int; }
    else if (t->s[0] == 'U') { esz = 4; ety = ty_uint; }
    else if (t->s[0] == 'u' && t->s[1] != '8') { esz = 2; ety = ty_ushort; }
  }
  Buf b = {0};
  while (peek()->kind == T_STR) decode_string(next(), &b, esz);
  for (int i = 0; i < esz; i++) buf_byte(&b, 0);
  int n = b.len / esz;
  Type *ty = array_of(ety, n);
  /* identical literals share storage (C11 6.4.5p7 allows it) */
  char *key = xalloc(b.len * 2 + 8);
  key[0] = '0' + esz;
  for (int i = 0; i < b.len; i++) { key[1 + 2 * i] = "0123456789abcdef"[b.p[i] >> 4]; key[2 + 2 * i] = "0123456789abcdef"[b.p[i] & 15]; }
  Var *v = map_get(&strtab, key);
  if (!v) {
    v = new_global_var(xfmt(".L.str.%d", nstr++), ty);
    v->data = b.p;
    v->datalen = b.len;
    v->is_const = 1;
    v->tok = first;
    map_put(&strtab, key, v);
  }
  return new_var(v, first);
}

/* ---- expressions ---- */
static Node *primary(void);
static Node *postfix(Node *n);
static Node *unary(void);

static Var *new_local(Ident *name, Type *ty, Tok *t) {
  Var *v = xalloc(sizeof *v);
  v->name = name;
  v->ty = ty;
  v->tok = t;
  v->is_local = 1;
  if (cur_fn) vec_push(&cur_fn->locals, v);
  if (name) bind(name, BK_VAR)->var = v;
  return v;
}

static Node *compound_literal(Type *ty, Tok *t) {
  Type *final;
  if (!cur_fn || depth == 0) {
    Var *v = new_global_var(xfmt(".L.compound.%d", nanon++), ty);
    v->tok = t;
    Init *in = initializer_for(ty, &final);
    v->ty = final;
    extern void global_init(Var *v, Init *in);
    global_init(v, in);
    return new_var(v, t);
  }
  Var *v = new_local(NULL, ty, t);
  Init *in = initializer_for(ty, &final);
  v->ty = final;
  Node *n = new_node(N_INITVAR, t);
  n->var = v;
  n->initz = in;
  n->ty = final;
  return n;
}

static Node *builtin_call(Tok *t);
static Node *generic_selection(Tok *t);

static Node *func_name_var(Tok *t) {
  if (!cur_fn) error_at(t, "'%s' outside a function", t->id->name);
  Buf b = {0};
  buf_str(&b, cur_fn->name->name);
  buf_byte(&b, 0);
  Var *v = new_global_var(xfmt(".L.func.%d", nanon++), array_of(ty_char, b.len));
  v->data = b.p;
  v->datalen = b.len;
  v->is_const = 1;
  bind(t->id, BK_VAR)->var = v;
  return new_var(v, t);
}

static Node *ident_expr(Tok *t) {
  Binding *b = t->id->sym;
  if (!b) {
    if (t->id == id_func || t->id == id_FUNCTION || t->id == id_PRETTY) return func_name_var(t);
    if (!strncmp(t->id->name, "__builtin_", 10)) {
      Ident *lib = intern(t->id->name + 10, t->id->len - 10);
      if (lib->sym && lib->sym->kind == BK_VAR) b = lib->sym;
    }
    if (!b && is_p(peek(), '(')) {
      /* C89 implicit declaration: int name() */
      warn_at(t, "implicit declaration of function '%s'", t->id->name);
      Var *v = map_get(&gtab, t->id->name);
      if (!v) {
        v = xalloc(sizeof *v);
        v->name = t->id;
        v->ty = func_type(ty_int);
        v->is_func = 1;
        v->tok = t;
        map_put(&gtab, t->id->name, v);
        vec_push(&globals, v);
      }
      Binding *nb = xalloc(sizeof *nb);
      nb->id = t->id;
      nb->kind = BK_VAR;
      nb->var = v;
      nb->depth = 0;
      /* file scope binding, inserted under any block bindings */
      Binding **pp = &t->id->sym;
      while (*pp) pp = &(*pp)->prev;
      *pp = nb;
      nb->snext = scope_head[0];
      scope_head[0] = nb;
      b = nb;
    }
    if (!b) error_at(t, "use of undeclared identifier '%s'", t->id->name);
  }
  if (b->kind == BK_ENUM) return new_num(b->val, b->ty, t);
  if (b->kind == BK_TYPEDEF) error_at(t, "unexpected type name '%s'", t->id->name);
  Var *v = b->var;
  v->referenced = 1;
  return new_var(v, t);
}

static Node *primary(void) {
  Tok *t = peek();
  if (accept('(')) {
    if (is_p(peek(), '{')) {
      /* GNU statement expression */
      if (!cur_fn) error_at(t, "statement expression outside a function");
      Node *n = new_node(N_STMTEXPR, t);
      n->body = compound_stmt();
      expect(')');
      Node *last = n->body->body;
      while (last && last->next) last = last->next;
      n->ty = last && last->kind == N_EXPR ? rval(last->lhs)->ty : ty_void;
      if (last && last->kind == N_EXPR) last->lhs = rval(last->lhs);
      return n;
    }
    Node *n = expr();
    expect(')');
    return n;
  }
  if (t->kind == T_NUM) { next(); return number(t); }
  if (t->kind == T_CHAR) {
    next();
    int ok;
    i64 v = char_value(t, &ok);
    if (!ok) error_at(t, "empty character constant");
    Type *ty = ty_int;
    if (t->s[0] == 'u' && t->s[1] == '\'') ty = ty_ushort;
    else if (t->s[0] == 'U') ty = ty_uint;
    return new_num(v, ty, t);
  }
  if (t->kind == T_STR) return string_lit();
  if (t->kind == T_IDENT) {
    switch (t->id->kw) {
    case 0: break;
    case KW_GENERIC: next(); return generic_selection(t);
    default: error_at(t, "expected an expression before '%s'", tok_str(t));
    }
    next();
    if (!strncmp(t->id->name, "__builtin_", 10) && is_p(peek(), '(')) {
      Node *b = builtin_call(t);
      if (b) return b;
    }
    return ident_expr(t);
  }
  if (t->kind == T_OTHER) error_at(t, "stray '%s' in program", tok_str(t));
  error_at(t, "expected an expression before '%s'", tok_str(t));
  return NULL;
}

static Node *call(Node *fn, Tok *t) {
  Type *fty;
  Node *callee = fn;
  if (fn->ty->kind == TY_FUNC) fty = fn->ty;
  else {
    callee = rval(fn);
    if (!is_ptr(callee->ty) || callee->ty->base->kind != TY_FUNC) error_at(t, "called object type '%s' is not a function or function pointer", type_str(fn->ty));
    fty = callee->ty->base;
  }
  Vec args = {0};
  int i = 0;
  if (!accept(')')) {
    for (;;) {
      Tok *at = peek();
      Node *a = rval(assign());
      if (fty->proto && i < fty->nparams) a = assign_conv(a, unqual(fty->params[i].ty), at, "argument passing");
      else {
        if (fty->proto && !fty->variadic) error_at(at, "too many arguments to function call, expected %d", fty->nparams);
        if (a->ty->kind == TY_FLOAT) a = cast(a, ty_double);
        else if (is_integer(a->ty)) a = promote(a);
        if (a->ty->kind == TY_VOID) error_at(at, "argument has type void");
      }
      vec_push(&args, a);
      i++;
      if (accept(')')) break;
      expect(',');
    }
  }
  if (fty->proto && i < fty->nparams) error_at(t, "too few arguments to function call, expected %d, have %d", fty->nparams, i);
  Node *n = new_node(N_CALL, t);
  n->lhs = callee;
  n->fty = fty;
  n->args = (Node **)args.p;
  n->nargs = args.len;
  n->ty = unqual(fty->base);
  if (n->ty->kind == TY_STRUCT && !n->ty->rec->complete) error_at(t, "calling a function with incomplete return type");
  return n;
}

static Node *postfix(Node *n) {
  for (;;) {
    Tok *t = peek();
    if (accept('(')) { n = call(n, t); continue; }
    if (accept('[')) {
      Node *idx = expr();
      expect(']');
      n = deref(add(n, idx, t), t);
      continue;
    }
    if (accept('.')) { n = member(n, expect_ident(), t); continue; }
    if (accept(P_ARROW)) {
      Node *d = rval(n);
      if (!is_ptr(d->ty)) error_at(t, "member reference type '%s' is not a pointer", type_str(d->ty));
      n = member(deref(d, t), expect_ident(), t);
      continue;
    }
    if (accept(P_INC)) { n = incdec(n, 1, 1, t); continue; }
    if (accept(P_DEC)) { n = incdec(n, -1, 1, t); continue; }
    return n;
  }
}

static Node *sizeof_type(Type *ty, Tok *t) {
  if (ty->kind == TY_FUNC) { warn_at(t, "sizeof applied to a function type"); return new_num(1, ty_ulong, t); }
  if (ty->kind == TY_VOID) return new_num(1, ty_ulong, t);
  if (ty->kind == TY_ARRAY && ty->vla_len) {
    Node *n = new_node(N_VLASIZE, t);
    n->var = NULL;
    n->ty = ty_ulong;
    n->fty = ty;
    return n;
  }
  if (is_aggr(ty) && !ty->rec->complete) error_at(t, "invalid application of 'sizeof' to an incomplete type '%s'", type_str(ty));
  if (ty->kind == TY_ARRAY && ty->len < 0) error_at(t, "invalid application of 'sizeof' to an incomplete array type");
  return new_num(ty->size, ty_ulong, t);
}

static Node *unary(void) {
  Tok *t = peek();
  if (t->kind == T_PUNCT) {
    switch (t->punct) {
    case '+': next(); { Node *n = promote(cast_expr()); if (!is_arith(n->ty)) error_at(t, "invalid argument type to unary '+'"); return n; }
    case '-': {
      next();
      Node *n = promote(cast_expr());
      if (!is_arith(n->ty)) error_at(t, "invalid argument type '%s' to unary '-'", type_str(n->ty));
      if (n->kind == N_NUM) return new_num(-(u64)n->ival, n->ty, t);
      if (n->kind == N_FNUM) return new_fnum(-n->fval, n->ty, t);
      Node *r = new_node(N_NEG, t);
      r->lhs = n;
      r->ty = n->ty;
      return r;
    }
    case '~': {
      next();
      Node *n = promote(cast_expr());
      if (!is_integer(n->ty)) error_at(t, "invalid argument type '%s' to unary '~'", type_str(n->ty));
      if (n->kind == N_NUM) return new_num(~n->ival, n->ty, t);
      Node *r = new_node(N_BNOT, t);
      r->lhs = n;
      r->ty = n->ty;
      return r;
    }
    case '!': {
      next();
      Node *n = truth(cast_expr());
      if (n->kind == N_NUM) return new_num(!n->ival, ty_int, t);
      if (n->kind == N_FNUM) return new_num(!n->fval, ty_int, t);
      Node *r = new_node(N_NOT, t);
      r->lhs = n;
      r->ty = ty_int;
      return r;
    }
    case '&': next(); return addr_of(cast_expr(), t);
    case '*': next(); return deref(cast_expr(), t);
    case P_INC: next(); return incdec(unary(), 1, 0, t);
    case P_DEC: next(); return incdec(unary(), -1, 0, t);
    case P_LAND: error_at(t, "labels as values (&&label) are not supported");
    }
  }
  if (is_kw(t, KW_SIZEOF)) {
    next();
    if (is_p(peek(), '(') && is_typename(peekn(1))) {
      next();
      Type *ty = type_name();
      expect(')');
      if (is_p(peek(), '{')) return postfix(compound_literal(ty, t)); /* sizeof (T){...} */
      return sizeof_type(ty, t);
    }
    Node *n = unary();
    if (n->kind == N_MEMBER && n->mem->bitwidth) error_at(t, "sizeof applied to a bit-field");
    if (n->ty->kind == TY_ARRAY && n->ty->vla_len) {
      Node *s = new_node(N_VLASIZE, t);
      s->ty = ty_ulong;
      s->fty = n->ty;
      s->var = n->kind == N_VAR ? n->var : NULL;
      return s;
    }
    return sizeof_type(n->ty, t);
  }
  if (is_kw(t, KW_ALIGNOF)) {
    next();
    if (is_p(peek(), '(') && is_typename(peekn(1))) {
      next();
      Type *ty = type_name();
      expect(')');
      while (ty->kind == TY_ARRAY) ty = ty->base;
      return new_num(ty->align, ty_ulong, t);
    }
    Node *n = unary();
    if (n->kind == N_VAR && n->var->align) return new_num(n->var->align, ty_ulong, t);
    return new_num(n->ty->align, ty_ulong, t);
  }
  return postfix(primary());
}

static Node *cast_expr(void) {
  Tok *t = peek();
  if (is_p(t, '(') && is_typename(peekn(1))) {
    next();
    Type *ty = type_name();
    expect(')');
    if (is_p(peek(), '{')) return postfix(compound_literal(ty, t));
    Node *n = cast_expr();
    if (ty->kind != TY_VOID && !is_scalar(ty) && !(is_aggr(ty) && is_aggr(n->ty) && ty->rec == n->ty->rec))
      error_at(t, "cannot cast to '%s'", type_str(ty));
    n = rval(n);
    if (ty->kind != TY_VOID && !is_scalar(n->ty) && !is_aggr(ty)) error_at(t, "cannot cast '%s' to '%s'", type_str(n->ty), type_str(ty));
    if (is_flonum(ty) && is_ptr(n->ty)) error_at(t, "cannot cast a pointer to a floating type");
    if (is_ptr(ty) && is_flonum(n->ty)) error_at(t, "cannot cast a floating value to a pointer");
    Node *c = cast(n, ty);
    if (c == n && ty->kind != TY_VOID) {
      /* a cast is never an lvalue */
      if (is_lvalue(n) || n->ty->qual) {
        c = new_node(N_CAST, t);
        c->lhs = n;
        c->ty = unqual(ty);
      }
    }
    return c;
  }
  return unary();
}

static int binop_prec(Tok *t, int *kind) {
  if (t->kind != T_PUNCT) return -1;
  switch (t->punct) {
  case '*': *kind = N_MUL; return 10;
  case '/': *kind = N_DIV; return 10;
  case '%': *kind = N_MOD; return 10;
  case '+': *kind = N_ADD; return 9;
  case '-': *kind = N_SUB; return 9;
  case P_SHL: *kind = N_SHL; return 8;
  case P_SHR: *kind = N_SHR; return 8;
  case '<': *kind = N_LT; return 7;
  case '>': *kind = -N_LT; return 7;
  case P_LE: *kind = N_LE; return 7;
  case P_GE: *kind = -N_LE; return 7;
  case P_EQ: *kind = N_EQ; return 6;
  case P_NE: *kind = N_NE; return 6;
  case '&': *kind = N_BAND; return 5;
  case '^': *kind = N_BXOR; return 4;
  case '|': *kind = N_BOR; return 3;
  case P_LAND: *kind = N_LAND; return 2;
  case P_LOR: *kind = N_LOR; return 1;
  }
  return -1;
}

static Node *binexpr(int minprec) {
  Node *l = cast_expr();
  for (;;) {
    Tok *t = peek();
    int kind;
    int p = binop_prec(t, &kind);
    if (p < minprec) return l;
    next();
    Node *r = binexpr(p + 1);
    switch (kind) {
    case N_ADD: l = add(l, r, t); break;
    case N_SUB: l = sub(l, r, t); break;
    case N_MUL: case N_DIV: case N_MOD: case N_BAND: case N_BOR: case N_BXOR: l = arith(kind, l, r, t); break;
    case N_SHL: case N_SHR: l = shift(kind, l, r, t); break;
    case N_LT: case N_LE: case N_EQ: case N_NE: l = compare(kind, l, r, t); break;
    case -N_LT: l = compare(N_LT, r, l, t); break;
    case -N_LE: l = compare(N_LE, r, l, t); break;
    case N_LAND: case N_LOR: l = logical(kind, l, r, t); break;
    }
  }
}

static Node *conditional(void) {
  Node *c = binexpr(1);
  Tok *t = peek();
  if (!accept('?')) return c;
  c = truth(c);
  Node *a;
  if (is_p(peek(), ':')) {
    /* GNU a ?: b */
    if (!cur_fn) error_at(t, "'?:' with an omitted operand outside a function");
    Var *tmp = new_local(NULL, unqual(c->ty), t);
    Node *set = new_node(N_ASSIGN, t);
    set->lhs = new_var(tmp, t);
    set->rhs = c;
    set->ty = tmp->ty;
    c = set;
    a = new_var(tmp, t);
  } else {
    a = rval(expr());
  }
  expect(':');
  Node *b = rval(conditional());
  Type *ty;
  if (is_arith(a->ty) && is_arith(b->ty)) {
    ty = common_type(a, b);
    a = cast(a, ty);
    b = cast(b, ty);
  } else if (a->ty->kind == TY_VOID || b->ty->kind == TY_VOID) {
    ty = ty_void;
  } else if (is_ptr(a->ty) && is_ptr(b->ty)) {
    ty = a->ty->base->kind == TY_VOID ? a->ty : b->ty->base->kind == TY_VOID ? b->ty : a->ty;
    if (is_null_const(a) && !is_null_const(b)) ty = b->ty;
    else if (is_null_const(b) && !is_null_const(a)) ty = a->ty;
    b = cast(b, ty);
    a = cast(a, ty);
  } else if (is_ptr(a->ty) && is_integer(b->ty)) {
    if (!is_null_const(b)) warn_at(t, "pointer/integer type mismatch in conditional expression");
    ty = a->ty;
    b = cast(b, ty);
  } else if (is_integer(a->ty) && is_ptr(b->ty)) {
    if (!is_null_const(a)) warn_at(t, "pointer/integer type mismatch in conditional expression");
    ty = b->ty;
    a = cast(a, ty);
  } else if (is_aggr(a->ty) && is_aggr(b->ty) && a->ty->rec == b->ty->rec) {
    ty = unqual(a->ty);
  } else {
    error_at(t, "incompatible operand types ('%s' and '%s')", type_str(a->ty), type_str(b->ty));
    return NULL;
  }
  if (c->kind == N_NUM) return c->ival ? a : b;
  Node *n = new_node(N_COND, t);
  n->cond = c;
  n->then = a;
  n->els = b;
  n->ty = ty;
  return n;
}

static Node *assign(void) {
  Node *l = conditional();
  Tok *t = peek();
  if (t->kind != T_PUNCT) return l;
  int k;
  switch (t->punct) {
  case '=': next(); return do_assign(l, assign(), t);
  case P_ADDEQ: k = N_ADD; break;
  case P_SUBEQ: k = N_SUB; break;
  case P_MULEQ: k = N_MUL; break;
  case P_DIVEQ: k = N_DIV; break;
  case P_MODEQ: k = N_MOD; break;
  case P_SHLEQ: k = N_SHL; break;
  case P_SHREQ: k = N_SHR; break;
  case P_ANDEQ: k = N_BAND; break;
  case P_XOREQ: k = N_BXOR; break;
  case P_OREQ: k = N_BOR; break;
  default: return l;
  }
  next();
  return op_assign(k, l, assign(), t);
}

static Node *expr(void) {
  Node *n = assign();
  while (is_p(peek(), ',')) {
    Tok *t = next();
    n = comma(n, assign(), t);
  }
  return n;
}

static Node *generic_selection(Tok *t) {
  expect('(');
  Node *c = rval(assign());
  Type *ct = unqual(c->ty);
  Node *chosen = NULL, *def = NULL;
  while (accept(',')) {
    if (accept_kw(KW_DEFAULT)) {
      expect(':');
      def = assign();
      continue;
    }
    Type *ty = type_name();
    expect(':');
    Node *e = assign();
    if (!chosen && same_type(unqual(ty), ct) && ty->kind == ct->kind && ty->is_enum == ct->is_enum &&
        !(is_integer(ty) && ty->is_unsigned != ct->is_unsigned))
      chosen = e;
  }
  expect(')');
  if (!chosen) chosen = def;
  if (!chosen) error_at(t, "controlling expression type '%s' not compatible with any generic association", type_str(ct));
  return chosen;
}

/* ---- builtins ---- */
static const struct { const char *name; int id; int arg; } builtin_tab[] = {
  {"__builtin_va_start", BI_VA_START, 0}, {"__builtin_va_end", BI_VA_END, 0}, {"__builtin_va_copy", BI_VA_COPY, 0},
  {"__builtin_unreachable", BI_UNREACHABLE, 0}, {"__builtin_trap", BI_TRAP, 0}, {"__builtin_alloca", BI_ALLOCA, 0},
  {"__builtin_clz", BI_CLZ, 4}, {"__builtin_clzl", BI_CLZ, 4}, {"__builtin_clzll", BI_CLZ, 8},
  {"__builtin_ctz", BI_CTZ, 4}, {"__builtin_ctzl", BI_CTZ, 4}, {"__builtin_ctzll", BI_CTZ, 8},
  {"__builtin_popcount", BI_POPCOUNT, 4}, {"__builtin_popcountl", BI_POPCOUNT, 4}, {"__builtin_popcountll", BI_POPCOUNT, 8},
  {"__builtin_bswap16", BI_BSWAP16, 2}, {"__builtin_bswap32", BI_BSWAP32, 4}, {"__builtin_bswap64", BI_BSWAP64, 8},
  {"__builtin_sqrt", BI_SQRT, 'd'}, {"__builtin_sqrtf", BI_SQRT, 'f'}, {"__builtin_floor", BI_FLOOR, 'd'},
  {"__builtin_floorf", BI_FLOOR, 'f'}, {"__builtin_ceil", BI_CEIL, 'd'}, {"__builtin_ceilf", BI_CEIL, 'f'},
  {"__builtin_trunc", BI_TRUNC, 'd'}, {"__builtin_truncf", BI_TRUNC, 'f'}, {"__builtin_fabs", BI_FABS, 'd'},
  {"__builtin_fabsf", BI_FABS, 'f'}, {"__builtin_rint", BI_RINT, 'd'}, {"__builtin_rintf", BI_RINT, 'f'},
  {"__builtin_nearbyint", BI_RINT, 'd'}, {"__builtin_nearbyintf", BI_RINT, 'f'},
  {"__builtin_copysign", BI_COPYSIGN, 'd'}, {"__builtin_copysignf", BI_COPYSIGN, 'f'},
  {"__builtin_isnan", BI_ISNAN, 0}, {"__builtin_isinf", BI_ISINF, 0}, {"__builtin_isfinite", BI_ISFINITE, 0},
  {"__builtin_isnormal", BI_ISNORMAL, 0}, {"__builtin_signbit", BI_SIGNBIT, 0}, {"__builtin_signbitf", BI_SIGNBIT, 0},
  {"__builtin_fpclassify", BI_FPCLASSIFY, 0}, {"__builtin_islessgreater", BI_ISLESSGREATER, 0},
  {"__builtin_isunordered", BI_ISUNORDERED, 0}, {"__builtin_memcpy", BI_MEMCPY, 0}, {"__builtin_memmove", BI_MEMMOVE, 0},
  {"__builtin_memset", BI_MEMSET, 0}, {"__builtin_wasm_memory_size", BI_MEMORY_SIZE, 0},
  {"__builtin_wasm_memory_grow", BI_MEMORY_GROW, 0}, {"__builtin_frame_address", BI_FRAME_ADDRESS, 0},
  /* handled in the parser */
  {"__builtin_expect", -1, 0}, {"__builtin_constant_p", -2, 0}, {"__builtin_assume_aligned", -3, 0},
  {"__builtin_prefetch", -4, 0}, {"__builtin_inf", -5, 'd'}, {"__builtin_inff", -5, 'f'},
  {"__builtin_huge_val", -5, 'd'}, {"__builtin_huge_valf", -5, 'f'}, {"__builtin_nan", -6, 'd'},
  {"__builtin_nanf", -6, 'f'}, {"__builtin_isgreater", -7, N_LT}, {"__builtin_isgreaterequal", -8, N_LE},
  {"__builtin_isless", -9, N_LT}, {"__builtin_islessequal", -10, N_LE}, {"__builtin_offsetof", -11, 0},
  {"__builtin_types_compatible_p", -12, 0}, {"__builtin_choose_expr", -13, 0}, {"__builtin_va_arg", -14, 0},
  {"__builtin_abs", -15, 0}, {"__builtin_classify_type", -16, 0}, {"__builtin_LINE", -17, 0},
};

int builtin_known(const char *name) {
  for (size_t i = 0; i < sizeof builtin_tab / sizeof *builtin_tab; i++)
    if (!strcmp(builtin_tab[i].name, name)) return 1;
  return 0;
}

static Node *bnode(int id, Type *ty, Tok *t, Vec *args) {
  Node *n = new_node(N_BUILTIN, t);
  n->bid = id;
  n->ty = ty;
  n->args = (Node **)args->p;
  n->nargs = args->len;
  return n;
}

static Vec call_args(Tok *t, int want) {
  Vec v = {0};
  expect('(');
  if (!accept(')')) {
    for (;;) {
      vec_push(&v, assign());
      if (accept(')')) break;
      expect(',');
    }
  }
  if (want >= 0 && v.len != want) error_at(t, "'%s' takes %d argument%s", t->id->name, want, want == 1 ? "" : "s");
  return v;
}

static double make_nan(int is_float) {
  if (is_float) { float f; u32 b = 0x7fc00000; memcpy(&f, &b, 4); return f; }
  double d;
  u64 b = 0x7ff8000000000000ull;
  memcpy(&d, &b, 8);
  return d;
}

static Node *builtin_call(Tok *t) {
  int id = 0, arg = 0;
  size_t i;
  for (i = 0; i < sizeof builtin_tab / sizeof *builtin_tab; i++)
    if (!strcmp(builtin_tab[i].name, t->id->name)) { id = builtin_tab[i].id; arg = builtin_tab[i].arg; break; }
  if (i == sizeof builtin_tab / sizeof *builtin_tab) return NULL;
  Vec a;
  switch (id) {
  case -1: a = call_args(t, 2); return rval(a.p[0]);
  case -2: { a = call_args(t, 1); Node *x = rval(a.p[0]); return new_num(x->kind == N_NUM || x->kind == N_FNUM, ty_int, t); }
  case -3: a = call_args(t, -1); return rval(a.p[0]);
  case -4: a = call_args(t, -1); return cast(new_num(0, ty_int, t), ty_void);
  case -5: call_args(t, 0); { double inf = 1e308; inf *= 10; return new_fnum(inf, arg == 'f' ? ty_float : ty_double, t); }
  case -6: call_args(t, 1); return new_fnum(make_nan(arg == 'f'), arg == 'f' ? ty_float : ty_double, t);
  case -7: case -8: case -9: case -10: {
    a = call_args(t, 2);
    Node *x = a.p[0], *y = a.p[1];
    return id >= -8 ? compare(arg, y, x, t) : compare(arg, x, y, t);
  }
  case -11: {
    expect('(');
    Type *ty = type_name();
    expect(',');
    if (!is_aggr(ty)) error_at(t, "offsetof requires a struct or union type");
    /* member designator: a.b[3].c */
    i64 off = 0;
    Type *cur = ty;
    for (int first = 1;; first = 0) {
      if (first || accept('.')) {
        Ident *name = expect_ident();
        Member *path[64];
        int np = 0;
        if (!is_aggr(cur) || !find_member(cur->rec, name, path, &np)) error_at(t, "no member named '%s'", name->name);
        for (int k = 0; k < np; k++) off += path[k]->offset;
        cur = path[np - 1]->ty;
        if (path[np - 1]->bitwidth) error_at(t, "offsetof applied to a bit-field");
      } else if (accept('[')) {
        i64 idx = const_int();
        expect(']');
        if (cur->kind != TY_ARRAY) error_at(t, "subscript of a non-array in offsetof");
        cur = cur->base;
        off += idx * cur->size;
      } else break;
    }
    expect(')');
    return new_num(off, ty_ulong, t);
  }
  case -12: {
    expect('(');
    Type *x = type_name();
    expect(',');
    Type *y = type_name();
    expect(')');
    return new_num(same_type(unqual(x), unqual(y)) && x->kind == y->kind, ty_int, t);
  }
  case -13: {
    expect('(');
    i64 c = const_int();
    expect(',');
    Node *x = assign();
    expect(',');
    Node *y = assign();
    expect(')');
    return c ? x : y;
  }
  case -14: {
    expect('(');
    Node *ap = assign();
    expect(',');
    Type *ty = type_name();
    expect(')');
    if (!is_lvalue(ap)) error_at(t, "va_arg needs a va_list lvalue");
    Node *n = new_node(N_VAARG, t);
    n->lhs = ap;
    n->ty = unqual(ty);
    if (cur_fn) cur_fn->uses_va = 1;
    return n;
  }
  case -15: {
    a = call_args(t, 1);
    Node *x = promote(a.p[0]);
    Node *neg = new_node(N_NEG, t);
    neg->lhs = x;
    neg->ty = x->ty;
    Node *c = new_node(N_COND, t);
    c->cond = compare(N_LT, x, new_num(0, x->ty, t), t);
    c->then = neg;
    c->els = x;
    c->ty = x->ty;
    return c;
  }
  case -16: { a = call_args(t, 1); Node *x = rval(a.p[0]); return new_num(is_integer(x->ty) ? 1 : is_flonum(x->ty) ? 8 : is_ptr(x->ty) ? 5 : 12, ty_int, t); }
  case -17: call_args(t, 0); return new_num(t->line, ty_int, t);
  }
  a = call_args(t, -1);
  for (int k = 0; k < a.len; k++) a.p[k] = rval(a.p[k]);
  Node **x = (Node **)a.p;
  switch (id) {
  case BI_VA_START:
    if (a.len < 1) error_at(t, "va_start needs a va_list");
    if (!cur_fn || !cur_fn->ty->variadic) error_at(t, "va_start used in a function with fixed arguments");
    cur_fn->uses_va = 1;
    return bnode(id, ty_void, t, &a);
  case BI_VA_END: return bnode(id, ty_void, t, &a);
  case BI_VA_COPY:
    if (a.len != 2) error_at(t, "va_copy takes 2 arguments");
    return do_assign(deref(addr_of(x[0], t), t), x[1], t);
  case BI_UNREACHABLE: case BI_TRAP: return bnode(id, ty_void, t, &a);
  case BI_ALLOCA:
    if (a.len != 1) error_at(t, "alloca takes 1 argument");
    x[0] = cast(x[0], ty_ulong);
    if (cur_fn) cur_fn->has_vla = 1;
    return bnode(id, ptr_to(ty_void), t, &a);
  case BI_CLZ: case BI_CTZ: case BI_POPCOUNT:
    if (a.len != 1) error_at(t, "'%s' takes 1 argument", t->id->name);
    x[0] = cast(x[0], arg == 8 ? ty_ullong : ty_uint);
    return bnode(id, ty_int, t, &a);
  case BI_BSWAP16: case BI_BSWAP32: case BI_BSWAP64: {
    Type *ty = arg == 2 ? ty_ushort : arg == 4 ? ty_uint : ty_ullong;
    if (a.len != 1) error_at(t, "'%s' takes 1 argument", t->id->name);
    x[0] = cast(x[0], arg == 8 ? ty_ullong : ty_uint);
    return bnode(id, ty, t, &a);
  }
  case BI_SQRT: case BI_FLOOR: case BI_CEIL: case BI_TRUNC: case BI_FABS: case BI_RINT: case BI_COPYSIGN: {
    Type *ty = arg == 'f' ? ty_float : ty_double;
    int want = id == BI_COPYSIGN ? 2 : 1;
    if (a.len != want) error_at(t, "'%s' takes %d argument%s", t->id->name, want, want == 1 ? "" : "s");
    for (int k = 0; k < a.len; k++) {
      if (!is_arith(x[k]->ty)) error_at(t, "'%s' needs a number", t->id->name);
      x[k] = cast(x[k], ty);
    }
    if (x[0]->kind == N_FNUM && id == BI_FABS) return new_fnum(x[0]->fval < 0 ? -x[0]->fval : x[0]->fval, ty, t);
    return bnode(id, ty, t, &a);
  }
  case BI_ISNAN: case BI_ISINF: case BI_ISFINITE: case BI_ISNORMAL: case BI_SIGNBIT:
    if (a.len != 1 || !is_flonum(x[0]->ty)) {
      if (a.len == 1 && is_integer(x[0]->ty)) x[0] = cast(x[0], ty_double);
      else error_at(t, "'%s' needs one floating argument", t->id->name);
    }
    if (x[0]->ty->kind == TY_LDOUBLE) error_at(t, "long double arithmetic is not supported");
    return bnode(id, ty_int, t, &a);
  case BI_FPCLASSIFY:
    if (a.len != 6) error_at(t, "__builtin_fpclassify takes 6 arguments");
    for (int k = 0; k < 5; k++) x[k] = cast(x[k], ty_int);
    if (is_integer(x[5]->ty)) x[5] = cast(x[5], ty_double);
    return bnode(id, ty_int, t, &a);
  case BI_ISLESSGREATER: case BI_ISUNORDERED: {
    if (a.len != 2) error_at(t, "'%s' takes 2 arguments", t->id->name);
    Type *ty = common_type(x[0], x[1]);
    x[0] = cast(x[0], ty);
    x[1] = cast(x[1], ty);
    return bnode(id, ty_int, t, &a);
  }
  case BI_MEMCPY: case BI_MEMMOVE: case BI_MEMSET:
    if (a.len != 3) error_at(t, "'%s' takes 3 arguments", t->id->name);
    x[0] = cast(x[0], ptr_to(ty_void));
    x[1] = id == BI_MEMSET ? cast(x[1], ty_int) : cast(x[1], ptr_to(ty_void));
    x[2] = cast(x[2], ty_ulong);
    return bnode(id, ptr_to(ty_void), t, &a);
  case BI_MEMORY_SIZE: case BI_MEMORY_GROW:
    if (a.len != (id == BI_MEMORY_SIZE ? 1 : 2)) error_at(t, "wrong number of arguments to '%s'", t->id->name);
    if (x[0]->kind != N_NUM || x[0]->ival != 0) error_at(t, "memory index must be the constant 0");
    if (id == BI_MEMORY_GROW) x[1] = cast(x[1], ty_ulong);
    return bnode(id, ty_ulong, t, &a);
  case BI_FRAME_ADDRESS:
    if (cur_fn) cur_fn->has_vla = 1;
    return bnode(id, ptr_to(ty_void), t, &a);
  }
  error_at(t, "unsupported builtin '%s'", t->id->name);
  return NULL;
}

/* ---- initializers ---- */
static Init *new_init(Type *ty) {
  Init *in = xalloc(sizeof *in);
  in->ty = ty;
  if (is_aggr(ty)) {
    in->nkids = ty->rec->nmembers;
    in->kids = xalloc((in->nkids + 1) * sizeof *in->kids);
  }
  return in;
}
static Type *kid_type(Init *in, int i) {
  if (in->ty->kind == TY_ARRAY) return in->ty->base;
  return in->ty->rec->marr[i]->ty;
}
static Init *kid(Init *in, int i) {
  if (in->ty->kind == TY_ARRAY) {
    if (i >= in->nkids) {
      int n = in->nkids ? in->nkids : 4;
      while (n <= i) n *= 2;
      Init **k = xalloc(n * sizeof *k);
      if (in->nkids) memcpy(k, in->kids, in->nkids * sizeof *k);
      in->kids = k;
      in->nkids = n;
    }
    if (i + 1 > in->max) in->max = i + 1;
  }
  if (!in->kids[i]) in->kids[i] = new_init(kid_type(in, i));
  if (in->ty->kind == TY_UNION) {
    for (int j = 0; j < in->nkids; j++) if (j != i) in->kids[j] = NULL;
    in->mem = in->ty->rec->marr[i];
  }
  return in->kids[i];
}
static int skip_unnamed(Init *in, int i) {
  if (in->ty->kind == TY_ARRAY) return i;
  while (i < in->nkids && !in->ty->rec->marr[i]->name && in->ty->rec->marr[i]->bitwidth) i++;
  return i;
}
static int init_full(Init *in, int i) {
  if (in->ty->kind == TY_ARRAY) return in->ty->len >= 0 && i >= in->ty->len;
  if (in->ty->kind == TY_UNION) return i > 0;
  return skip_unnamed(in, i) >= in->nkids;
}

static void initializer(Init *in);
static void init_list(Init *in, int braced, int idx, int n);

static int is_char_array(Type *t) { return t->kind == TY_ARRAY && is_integer(t->base) && t->base->size == 1; }
static int is_wchar_array(Type *t) { return t->kind == TY_ARRAY && is_integer(t->base) && t->base->size > 1; }

static int designator_start(Tok *t) { return is_p(t, '.') || is_p(t, '['); }

/* One designator in the context of aggregate `in`. Returns the index; for a range
 * [a ... b] *hi is set to b. */
static int parse_designator(Init *in, int *hi, int *anon_path, int *nanon_p) {
  Tok *t = peek();
  *hi = -1;
  *nanon_p = 0;
  if (accept('[')) {
    if (in->ty->kind != TY_ARRAY) error_at(t, "array designator used for a non-array type '%s'", type_str(in->ty));
    i64 lo = const_int();
    if (accept(P_ELLIPSIS)) *hi = (int)const_int();
    expect(']');
    if (lo < 0 || (in->ty->len >= 0 && lo >= in->ty->len)) error_at(t, "array designator index %lld is out of bounds", (long long)lo);
    return (int)lo;
  }
  expect('.');
  Ident *name = expect_ident();
  if (!is_aggr(in->ty)) error_at(t, "field designator '%s' used for a non-struct type '%s'", name->name, type_str(in->ty));
  Member *path[64];
  int np = 0;
  if (!find_member(in->ty->rec, name, path, &np)) error_at(t, "field designator '%s' does not refer to any field in '%s'", name->name, type_str(in->ty));
  for (int i = 1; i < np; i++) anon_path[(*nanon_p)++] = path[i]->idx;
  return path[0]->idx;
}

static void designated(Init *in, int idx, int hi, int *anon, int nanon);

/* After a designator chain that ends inside `in` at idx: the value, then continue
 * the rest of `in` as an unbraced list. */
static void desig_into(Init *in, int idx, int hi, int *anon, int nanon) {
  if (nanon) {
    Init *k = kid(in, idx);
    desig_into(k, anon[0], -1, anon + 1, nanon - 1);
    return;
  }
  if (designator_start(peek())) {
    Init *k = kid(in, idx);
    int h2, an2[64], nan2;
    int i2 = parse_designator(k, &h2, an2, &nan2);
    designated(k, i2, h2, an2, nan2);
    return;
  }
  accept('=');
  Init *k = kid(in, idx);
  initializer(k);
  if (hi >= 0) {
    if (hi < idx) error_at(peek(), "array designator range is empty");
    for (int j = idx + 1; j <= hi; j++) { kid(in, j); in->kids[j] = k; }
    idx = hi;
  }
}

static void designated(Init *in, int idx, int hi, int *anon, int nanon) {
  desig_into(in, idx, hi, anon, nanon);
  int end = hi >= 0 ? hi : idx;
  if (nanon) { /* continue inside the anonymous member */
    Init *k = in->kids[idx];
    (void)k;
  }
  init_list(in, 0, end + 1, 1);
}

static void init_list(Init *in, int braced, int idx, int n) {
  for (;;) {
    if (n > 0) {
      if (!is_p(peek(), ',')) break;
      if (!braced) {
        Tok *after = peekn(1);
        if (is_p(after, '}') || designator_start(after)) break;
        if (init_full(in, idx)) break;
      }
      next();
      if (braced && is_p(peek(), '}')) break;
    } else if (braced && is_p(peek(), '}')) {
      break;
    }
    if (braced && designator_start(peek())) {
      int hi, anon[64], nanon;
      int i = parse_designator(in, &hi, anon, &nanon);
      desig_into(in, i, hi, anon, nanon);
      idx = (hi >= 0 ? hi : i) + 1;
      n++;
      continue;
    }
    idx = skip_unnamed(in, idx);
    if (init_full(in, idx)) {
      if (!braced) break;
      warn_at(peek(), "excess elements in initializer");
      Init *junk = new_init(ty_int);
      if (is_p(peek(), '{')) {
        int d = 0;
        do { Tok *x = next(); if (is_p(x, '{')) d++; else if (is_p(x, '}')) d--; } while (d);
      } else {
        junk->expr = assign();
      }
      n++;
      continue;
    }
    initializer(kid(in, idx));
    idx++;
    n++;
  }
}

static void initializer(Init *in) {
  Type *ty = in->ty;
  if (ty->kind == TY_ARRAY && ty->vla_len) error_at(peek(), "variable length arrays cannot be initialized");
  if ((is_char_array(ty) || is_wchar_array(ty)) && peek()->kind == T_STR) {
    in->expr = string_lit();
    return;
  }
  if ((is_char_array(ty) || is_wchar_array(ty)) && is_p(peek(), '{') && peekn(1)->kind == T_STR) {
    int i = 1;
    while (peekn(i)->kind == T_STR) i++;
    if (is_p(peekn(i), '}') || (is_p(peekn(i), ',') && is_p(peekn(i + 1), '}'))) {
      next();
      in->expr = string_lit();
      accept(',');
      expect('}');
      return;
    }
  }
  if (is_p(peek(), '{')) {
    Tok *lb = next();
    if (!is_aggr(ty) && ty->kind != TY_ARRAY) {
      if (accept('}')) { in->expr = new_num(0, ty_int, lb); return; }
      initializer(in);
      while (accept(',')) {
        if (is_p(peek(), '}')) break;
        warn_at(peek(), "excess elements in scalar initializer");
        assign();
      }
      expect('}');
      return;
    }
    if (is_aggr(ty) && !ty->rec->complete) error_at(lb, "initializing an incomplete type '%s'", type_str(ty));
    init_list(in, 1, 0, 0);
    expect('}');
    return;
  }
  if (is_aggr(ty)) {
    int save = tpos;
    Node *e = assign();
    if (is_aggr(e->ty) && e->ty->rec == ty->rec) { in->expr = e; return; }
    tpos = save;
    init_list(in, 0, 0, 0);
    return;
  }
  if (ty->kind == TY_ARRAY) {
    init_list(in, 0, 0, 0);
    return;
  }
  Tok *t = peek();
  in->expr = assign_conv(assign(), unqual(ty), t, "initialization");
}

static Init *initializer_for(Type *ty, Type **outty) {
  Init *in = new_init(ty);
  initializer(in);
  if (ty->kind == TY_ARRAY && ty->len < 0) {
    int len = in->max;
    if (in->expr) len = in->expr->ty->len;
    Type *t = array_of(ty->base, len);
    t->qual = ty->qual;
    in->ty = t;
    ty = t;
  }
  /* a struct with a flexible array member initialised with elements grows */
  *outty = ty;
  return in;
}

/* ---- global data ---- */
static void put_le(u8 *p, u64 v, int n) {
  for (int i = 0; i < n; i++) p[i] = (u8)(v >> (8 * i));
}

static int eval_addr(Node *n, Var **sym, i64 *off) {
  switch (n->kind) {
  case N_ADDR: {
    Node *x = n->lhs;
    if (x->kind == N_VAR) {
      if (x->var->is_local) return 0;
      *sym = x->var;
      *off = 0;
      x->var->referenced = 1;
      return 1;
    }
    if (x->kind == N_MEMBER) {
      Node *a = new_node(N_ADDR, x->tok);
      a->lhs = x->lhs;
      a->ty = ptr_to(x->lhs->ty);
      if (!eval_addr(a, sym, off)) return 0;
      if (x->mem->bitwidth) return 0;
      *off += x->mem->offset;
      return 1;
    }
    if (x->kind == N_DEREF) return eval_addr(x->lhs, sym, off);
    return 0;
  }
  case N_VAR:
    if (n->ty->kind == TY_FUNC || n->ty->kind == TY_ARRAY) {
      if (n->var->is_local) return 0;
      *sym = n->var;
      *off = 0;
      n->var->referenced = 1;
      return 1;
    }
    return 0;
  case N_ADD: case N_SUB: {
    if (n->rhs->kind != N_NUM) {
      if (n->kind == N_ADD && n->lhs->kind == N_NUM && is_ptr(n->rhs->ty)) {
        if (!eval_addr(n->rhs, sym, off)) return 0;
        *off += n->lhs->ival;
        return 1;
      }
      return 0;
    }
    if (!eval_addr(n->lhs, sym, off)) return 0;
    *off += n->kind == N_ADD ? n->rhs->ival : -n->rhs->ival;
    return 1;
  }
  case N_CAST:
    if (n->ty->size != 4 && n->ty->kind != TY_PTR) return 0;
    return eval_addr(n->lhs, sym, off);
  case N_NUM:
    *sym = NULL;
    *off = n->ival;
    return 1;
  case N_COND:
    if (n->cond->kind == N_NUM) return eval_addr(n->cond->ival ? n->then : n->els, sym, off);
    return 0;
  }
  return 0;
}

static void add_reloc(Var *gv, int off, Var *target, i64 addend) {
  Reloc *r = xalloc(sizeof *r);
  r->off = off;
  r->type = target->ty->kind == TY_FUNC ? R_TABLE_INDEX_I32 : R_MEMORY_ADDR_I32;
  r->sym = (Sym *)target; /* resolved to a Sym when the object is written */
  r->addend = addend;
  if (r->type == R_TABLE_INDEX_I32 && addend) error_at(target->tok, "offset from a function address in a constant");
  vec_push(&gv->relocs, r);
}

u64 ld_bits(double d, u64 *hi); /* gen.c: double to binary128 */

static void write_scalar(Var *gv, u8 *buf, int off, Type *ty, Node *e, Member *bf) {
  Tok *t = e->tok;
  e = rval(e);
  if (is_flonum(ty)) {
    if (e->kind == N_NUM) e = cast(e, ty);
    if (e->kind != N_FNUM) error_at(t, "initializer element is not a compile-time constant");
    if (ty->kind == TY_FLOAT) { float f = (float)e->fval; u32 b; memcpy(&b, &f, 4); put_le(buf + off, b, 4); }
    else if (ty->kind == TY_DOUBLE) { u64 b; memcpy(&b, &e->fval, 8); put_le(buf + off, b, 8); }
    else { u64 hi, lo = ld_bits(e->fval, &hi); put_le(buf + off, lo, 8); put_le(buf + off + 8, hi, 8); }
    return;
  }
  if (e->kind == N_FNUM) e = cast(e, ty);
  if (e->kind == N_NUM) {
    i64 v = e->ival;
    if (bf) {
      u64 cur = 0;
      for (int i = 0; i < ty->size; i++) cur |= (u64)buf[off + i] << (8 * i);
      u64 mask = (bf->bitwidth == 64 ? ~0ull : ((1ull << bf->bitwidth) - 1)) << bf->bitoff;
      cur = (cur & ~mask) | (((u64)v << bf->bitoff) & mask);
      put_le(buf + off, cur, ty->size);
      return;
    }
    put_le(buf + off, (u64)v, ty->size);
    return;
  }
  Var *sym = NULL;
  i64 addend = 0;
  if (ty->size == 4 && eval_addr(e, &sym, &addend)) {
    if (!sym) { put_le(buf + off, (u64)addend, 4); return; }
    put_le(buf + off, 0, 4);
    add_reloc(gv, off, sym, addend);
    return;
  }
  error_at(t, "initializer element is not a compile-time constant");
}

static void write_init(Var *gv, u8 *buf, int off, Init *in) {
  Type *ty = in->ty;
  if (in->expr) {
    Node *e = in->expr;
    if (ty->kind == TY_ARRAY && e->kind == N_VAR && e->var->data && e->ty->kind == TY_ARRAY) {
      /* char array from a string literal */
      int n = e->var->datalen;
      if (ty->len >= 0 && n > ty->size) {
        if (n - e->ty->base->size > ty->size) warn_at(e->tok, "initializer string is too long for the array");
        n = ty->size;
      }
      memcpy(buf + off, e->var->data, n);
      return;
    }
    if (is_aggr(ty)) {
      /* struct copied from a constant compound literal or another constant object */
      Node *x = e;
      if (x->kind == N_VAR && !x->var->is_local && x->var->data && x->var->is_def) {
        memcpy(buf + off, x->var->data, ty->size);
        for (int i = 0; i < x->var->relocs.len; i++) {
          Reloc *r = x->var->relocs.p[i];
          add_reloc(gv, off + r->off, (Var *)r->sym, r->addend);
        }
        return;
      }
      error_at(e->tok, "initializer element is not a compile-time constant");
    }
    write_scalar(gv, buf, off, ty, e, NULL);
    return;
  }
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < in->nkids && (ty->len < 0 || i < ty->len); i++)
      if (in->kids[i]) write_init(gv, buf, off + i * ty->base->size, in->kids[i]);
    return;
  }
  if (is_aggr(ty)) {
    for (int i = 0; i < in->nkids; i++) {
      Init *k = in->kids[i];
      if (!k) continue;
      Member *m = ty->rec->marr[i];
      if (m->bitwidth && k->expr) write_scalar(gv, buf, off + m->offset, m->ty, k->expr, m);
      else if (m->ty->kind == TY_ARRAY && m->ty->len < 0) {
        /* flexible array member with an initializer (GNU) */
        write_init(gv, buf, off + m->offset, k);
      } else write_init(gv, buf, off + m->offset, k);
    }
    return;
  }
}

static int flex_extra(Init *in) {
  /* bytes a flexible array member initializer adds beyond sizeof */
  if (!is_aggr(in->ty) || !in->ty->rec->flexible || in->ty->kind != TY_STRUCT) return 0;
  int last = in->nkids - 1;
  Init *k = last >= 0 ? in->kids[last] : NULL;
  if (!k) return 0;
  Member *m = in->ty->rec->marr[last];
  int n = k->expr ? k->expr->ty->size : k->max * m->ty->base->size;
  int extra = m->offset + n - in->ty->size;
  return extra > 0 ? extra : 0;
}

void global_init(Var *v, Init *in) {
  int size = v->ty->size + flex_extra(in);
  v->data = xalloc(size + 1);
  v->datalen = size;
  write_init(v, v->data, 0, in);
  v->is_def = 1;
  v->is_tentative = 0;
}

/* ---- declarations ---- */
static Var *global_var(Ident *name, Type *ty, DeclSpec *ds, Tok *t) {
  Var *v = map_get(&gtab, name->name);
  int is_static = ds->storage == SC_STATIC;
  if (v && depth == 0 && name->sym && name->sym->depth == 0 && name->sym->kind != BK_VAR)
    error_at(t, "redefinition of '%s' as a different kind of symbol", name->name);
  if (v) {
    if (ty->kind == TY_FUNC && v->ty->kind == TY_FUNC) {
      if (!same_type(v->ty, ty)) errorc_at(t, "conflicting types for '%s' ('%s' and earlier '%s')", name->name, type_str(ty), type_str(v->ty));
      if (ty->proto && !v->ty->proto) v->ty = ty;
    } else if ((ty->kind == TY_FUNC) != (v->ty->kind == TY_FUNC)) {
      error_at(t, "redefinition of '%s' as a different kind of symbol", name->name);
    } else if (!same_type(unqual(v->ty), unqual(ty))) {
      errorc_at(t, "conflicting types for '%s' ('%s' and earlier '%s')", name->name, type_str(ty), type_str(v->ty));
    } else if (v->ty->kind == TY_ARRAY && v->ty->len < 0 && ty->len >= 0) {
      v->ty = ty;
    }
    if (is_static && !v->is_static && depth == 0) {
      if (v->is_def || v->ty->kind != TY_FUNC) errorc_at(t, "static declaration of '%s' follows a non-static declaration", name->name);
      v->is_static = 1;
    }
  } else {
    v = xalloc(sizeof *v);
    v->name = name;
    v->ty = ty;
    v->tok = t;
    v->is_static = is_static;
    v->is_func = ty->kind == TY_FUNC;
    map_put(&gtab, name->name, v);
    vec_push(&globals, v);
  }
  if (ds->storage == SC_EXTERN) v->has_extern_decl = 1;
  if (ds->is_inline) v->is_inline = 1;
  Attr *a = &ds->attr;
  if (a->weak) v->weak = 1;
  if (a->noreturn || ds->noreturn) v->noreturn = 1;
  if (a->used) v->used_attr = 1;
  if (a->imp_mod) v->import_module = a->imp_mod;
  if (a->imp_name) v->import_name = a->imp_name;
  if (a->exp_name) v->export_name = a->exp_name;
  if (a->asmname) v->asmname = a->asmname;
  if (a->vis_default) v->vis_default = 1;
  int al = a->aligned > ds->align ? a->aligned : ds->align;
  if (al > v->align) v->align = al;
  if (ds->tls) v->is_tls = 1;
  /* the binding: at file scope, or a block-scope extern */
  if (!name->sym || name->sym->depth != depth || name->sym->var != v) bind(name, BK_VAR)->var = v;
  return v;
}

static void kr_params(Type *fty) {
  /* K&R parameter declarations before the body */
  while (!is_p(peek(), '{')) {
    DeclSpec ds = {0};
    Type *base = declspec(&ds);
    for (;;) {
      Tok *name = NULL;
      Type *t = declarator(base, &name);
      if (!name) error_at(peek(), "expected a parameter name");
      if (t->kind == TY_ARRAY) t = ptr_to(t->base);
      else if (t->kind == TY_FUNC) t = ptr_to(t);
      int found = 0;
      for (int i = 0; i < fty->nparams; i++)
        if (fty->params[i].name == name->id) { fty->params[i].ty = t; found = 1; }
      if (!found) error_at(name, "declaration of '%s' does not match any parameter", name->id->name);
      if (accept(';')) break;
      expect(',');
    }
  }
  for (int i = 0; i < fty->nparams; i++) {
    Type *t = fty->params[i].ty;
    if (t->kind == TY_FLOAT) fty->params[i].ty = ty_double; /* K&R: promoted */
    else if (is_integer(t) && rank(t) < 3) fty->params[i].ty = ty_int;
  }
  fty->variadic = 0;
}

static void check_labels(void) {
  for (int i = 0; i < fn_labels.len; i++) {
    Ident *id = fn_labels.p[i];
    Label *l = id->label;
    if (l && l->used && !l->defined) errorc_at(l->tok, "use of undeclared label '%s'", id->name);
    id->label = NULL;
  }
  fn_labels.len = 0;
}

static void function_def(Var *fn, Type *fty, Tok *name) {
  if (fn->is_def) error_at(name, "redefinition of '%s'", name->id->name);
  if (fty->variadic < 0) kr_params(fty);
  fn->ty = fty;
  fn->is_def = 1;
  fn->tok = name;
  cur_fn = fn;
  push_scope();
  for (int i = 0; i < fty->nparams; i++) {
    Param *p = &fty->params[i];
    if (!p->name) {
      Var *v = new_local(NULL, p->ty, p->tok);
      v->is_param = 1;
      vec_push(&fn->params, v);
      continue;
    }
    Var *v = new_local(p->name, p->ty, p->tok);
    v->is_param = 1;
    vec_push(&fn->params, v);
  }
  if (fty->base->kind != TY_VOID && is_aggr(fty->base) && !fty->base->rec->complete)
    error_at(name, "function has incomplete return type '%s'", type_str(fty->base));
  fn->body = compound_stmt();
  pop_scope();
  check_labels();
  cur_fn = NULL;
}

static Node *local_decl_init(Var *v, Tok *t) {
  Type *final;
  Init *in = initializer_for(v->ty, &final);
  v->ty = final;
  Node *n = new_node(N_DECL, t);
  n->var = v;
  n->initz = in;
  return n;
}

/* A declaration inside a function. Returns a statement list (possibly empty). */
static Node *declaration(void) {
  DeclSpec ds = {0};
  Tok *st = peek();
  Type *base = declspec(&ds);
  Node head = {0}, *cur = &head;
  if (accept(';')) return new_node(N_NULL, st);
  for (int first = 1; !accept(';'); first = 0) {
    if (!first && !is_p(peek(), ',')) error_at(peek(), "expected ';' at end of declaration");
    if (!first) expect(',');
    Tok *name = NULL;
    Type *ty = declarator(base, &name);
    parse_attrs(&ds.attr);
    if (!name) error_at(peek(), "declaration does not declare anything");
    if (ds.storage == SC_TYPEDEF) {
      Type *c = xalloc(sizeof *c);
      *c = *ty;
      c->tdname = name->id;
      rec_track(c);
      if (name->id->sym && name->id->sym->depth == depth && name->id->sym->kind != BK_TYPEDEF) error_at(name, "redefinition of '%s'", name->id->name);
      bind(name->id, BK_TYPEDEF)->ty = c;
      continue;
    }
    if (ty->kind == TY_FUNC || ds.storage == SC_EXTERN) {
      if (ds.storage == SC_STATIC && ty->kind == TY_FUNC) error_at(name, "function declared in block scope cannot have 'static' storage class");
      DeclSpec d2 = ds;
      if (d2.storage != SC_STATIC) d2.storage = SC_EXTERN;
      int save = depth;
      global_var(name->id, ty, &d2, name);
      (void)save;
      if (is_p(peek(), '=')) error_at(peek(), "'extern' variable cannot have an initializer here");
      continue;
    }
    if (ty->kind == TY_VOID) error_at(name, "variable has incomplete type 'void'");
    if (name->id->sym && name->id->sym->depth == depth && name->id->sym->kind == BK_VAR && name->id->sym->var->is_local)
      errorc_at(name, "redefinition of '%s'", name->id->name);
    if (ds.storage == SC_STATIC) {
      /* static local: a global with a unique name */
      Var *v = new_global_var(xfmt("%s.%s.%d", cur_fn->name->name, name->id->name, nanon++), ty);
      v->tok = name;
      v->is_def = 1;
      int al = ds.attr.aligned > ds.align ? ds.attr.aligned : ds.align;
      v->align = al;
      bind(name->id, BK_VAR)->var = v;
      if (accept('=')) {
        Type *final;
        Init *in = initializer_for(ty, &final);
        v->ty = final;
        global_init(v, in);
      } else {
        if (ty->kind == TY_ARRAY && ty->len < 0) error_at(name, "definition of variable with array type needs an explicit size or an initializer");
        if (is_aggr(ty) && !ty->rec->complete) error_at(name, "variable has incomplete type '%s'", type_str(ty));
      }
      continue;
    }
    Var *v = new_local(name->id, ty, name);
    int al = ds.attr.aligned > ds.align ? ds.attr.aligned : ds.align;
    v->align = al;
    if (ty->kind == TY_ARRAY && ty->vla_len) {
      cur_fn->has_vla = 1;
      Node *n = new_node(N_VLADECL, name);
      n->var = v;
      cur = cur->next = n;
      if (is_p(peek(), '=')) error_at(peek(), "variable length arrays cannot be initialized");
      continue;
    }
    if (accept('=')) {
      cur = cur->next = local_decl_init(v, name);
    } else {
      if (ty->kind == TY_ARRAY && ty->len < 0) error_at(name, "definition of variable with array type needs an explicit size or an initializer");
      if (is_aggr(ty) && !ty->rec->complete) error_at(name, "variable has incomplete type '%s'", type_str(ty));
    }
    if (is_aggr(v->ty) && !v->ty->rec->complete) error_at(name, "variable has incomplete type '%s'", type_str(v->ty));
  }
  Node *b = new_node(N_BLOCK, st);
  b->body = head.next;
  b->flag = 1; /* no new scope */
  return b;
}

/* ---- statements ---- */
static Node *expr_stmt_value(void) {
  Node *e = expr();
  return e;
}

static Node *asm_stmt(Tok *t) {
  while (is_kw(peek(), KW_VOLATILE) || is_kw(peek(), KW_INLINE) || is_kw(peek(), KW_GOTO)) next();
  expect('(');
  Tok *s = peek();
  if (s->kind != T_STR) error_at(s, "expected a string in asm statement");
  int empty = 1;
  while (peek()->kind == T_STR) { Tok *x = next(); if (x->len > 2) empty = 0; }
  int d = 1;
  while (d) {
    Tok *x = next();
    if (x->kind == T_EOF) error_at(t, "unterminated asm statement");
    if (is_p(x, '(')) d++;
    else if (is_p(x, ')')) d--;
  }
  expect(';');
  if (!empty) error_at(t, "inline assembly is not supported (only an empty asm, used as a compiler barrier, is accepted)");
  return new_node(N_NULL, t);
}

static Label *get_label(Ident *id, Tok *t) {
  if (!id->label) {
    Label *l = xalloc(sizeof *l);
    l->name = id;
    l->tok = t;
    id->label = l;
    vec_push(&fn_labels, id);
  }
  return id->label;
}

static Node *stmt(void) {
  Tok *t = peek();
  tick();
  if (t->kind == T_IDENT) {
    if (!t->id->kw && is_p(peekn(1), ':')) {
      next();
      next();
      Label *l = get_label(t->id, t);
      if (l->defined) error_at(t, "redefinition of label '%s'", t->id->name);
      l->defined = 1;
      l->tok = t;
      parse_attrs(NULL);
      Node *n = new_node(N_LABEL, t);
      n->label = l;
      if (is_p(peek(), '}')) n->body = new_node(N_NULL, t); /* C23: label at end of block */
      else n->body = stmt();
      l->stmt = n;
      cur_fn->has_goto = 1;
      return n;
    }
    switch (t->id->kw) {
    case KW_IF: {
      next();
      Node *n = new_node(N_IF, t);
      expect('(');
      n->cond = truth(expr());
      expect(')');
      n->then = stmt();
      if (accept_kw(KW_ELSE)) n->els = stmt();
      return n;
    }
    case KW_WHILE: {
      next();
      Node *n = new_node(N_WHILE, t);
      expect('(');
      n->cond = truth(expr());
      expect(')');
      brk_depth++; cont_depth++;
      n->body = stmt();
      brk_depth--; cont_depth--;
      return n;
    }
    case KW_DO: {
      next();
      Node *n = new_node(N_DO, t);
      brk_depth++; cont_depth++;
      n->body = stmt();
      brk_depth--; cont_depth--;
      if (!accept_kw(KW_WHILE)) error_at(peek(), "expected 'while' in do/while loop");
      expect('(');
      n->cond = truth(expr());
      expect(')');
      expect(';');
      return n;
    }
    case KW_FOR: {
      next();
      Node *n = new_node(N_FOR, t);
      expect('(');
      push_scope();
      if (is_typename(peek())) n->init = declaration();
      else if (!accept(';')) { Node *e = new_node(N_EXPR, peek()); e->lhs = expr(); n->init = e; expect(';'); }
      if (!is_p(peek(), ';')) n->cond = truth(expr());
      expect(';');
      if (!is_p(peek(), ')')) n->inc = expr();
      expect(')');
      brk_depth++; cont_depth++;
      n->body = stmt();
      brk_depth--; cont_depth--;
      pop_scope();
      return n;
    }
    case KW_SWITCH: {
      next();
      Node *n = new_node(N_SWITCH, t);
      expect('(');
      Node *c = promote(expr());
      if (!is_integer(c->ty)) error_at(t, "statement requires an expression of integer type ('%s' invalid)", type_str(c->ty));
      n->cond = c;
      expect(')');
      Node *save = cur_switch;
      cur_switch = n;
      brk_depth++;
      n->body = stmt();
      brk_depth--;
      cur_switch = save;
      return n;
    }
    case KW_CASE: {
      next();
      if (!cur_switch) error_at(t, "'case' statement not in switch statement");
      Node *n = new_node(N_CASE, t);
      n->cond = cur_switch;
      Type *st = cur_switch->cond->ty;
      i64 lo = const_int();
      i64 hi = lo;
      if (accept(P_ELLIPSIS)) hi = const_int();
      lo = wrap(lo, st);
      hi = wrap(hi, st);
      expect(':');
      n->ival = lo;
      n->ival2 = hi;
      for (Node *c = cur_switch->cases; c; c = c->els)
        if (!(hi < c->ival || lo > c->ival2) && !(st->is_unsigned && ((u64)hi < (u64)c->ival || (u64)lo > (u64)c->ival2)))
          error_at(t, "duplicate case value");
      n->els = cur_switch->cases;
      cur_switch->cases = n;
      if (is_p(peek(), '}')) n->body = new_node(N_NULL, t);
      else n->body = stmt();
      return n;
    }
    case KW_DEFAULT: {
      next();
      if (!cur_switch) error_at(t, "'default' statement not in switch statement");
      expect(':');
      if (cur_switch->defcase) error_at(t, "multiple default labels in one switch");
      Node *n = new_node(N_CASE, t);
      n->flag = 1;
      n->cond = cur_switch;
      cur_switch->defcase = n;
      if (is_p(peek(), '}')) n->body = new_node(N_NULL, t);
      else n->body = stmt();
      return n;
    }
    case KW_BREAK:
      next();
      if (!brk_depth) error_at(t, "'break' statement not in loop or switch statement");
      expect(';');
      return new_node(N_BREAK, t);
    case KW_CONTINUE:
      next();
      if (!cont_depth) error_at(t, "'continue' statement not in loop statement");
      expect(';');
      return new_node(N_CONTINUE, t);
    case KW_GOTO: {
      next();
      if (is_p(peek(), '*')) error_at(t, "computed goto is not supported");
      Tok *lt = peek();
      Ident *id = expect_ident();
      expect(';');
      Node *n = new_node(N_GOTO, t);
      n->label = get_label(id, lt);
      n->label->used = 1;
      cur_fn->has_goto = 1;
      return n;
    }
    case KW_RETURN: {
      next();
      Node *n = new_node(N_RETURN, t);
      Type *rt = cur_fn->ty->base;
      if (!accept(';')) {
        Node *e = expr();
        expect(';');
        if (rt->kind == TY_VOID) {
          if (rval(e)->ty->kind != TY_VOID) warn_at(t, "void function '%s' should not return a value", cur_fn->name->name);
          n->lhs = cast(e, ty_void);
        } else {
          n->lhs = assign_conv(e, unqual(rt), t, "return");
        }
      } else if (rt->kind != TY_VOID) {
        warn_at(t, "non-void function '%s' should return a value", cur_fn->name->name);
      }
      return n;
    }
    case KW_STATIC_ASSERT: static_assert_decl(); return new_node(N_NULL, t);
    case KW_ASM: next(); return asm_stmt(t);
    }
  }
  if (is_p(t, '{')) return compound_stmt();
  if (accept(';')) return new_node(N_NULL, t);
  if (is_typename(t)) return declaration();
  Node *n = new_node(N_EXPR, t);
  n->lhs = expr_stmt_value();
  expect(';');
  return n;
}

static Node *compound_stmt(void) {
  Tok *t = expect('{');
  Node *n = new_node(N_BLOCK, t);
  Node head = {0}, *cur = &head;
  push_scope();
  while (!accept('}')) {
    if (peek()->kind == T_EOF) error_at(t, "expected '}' at end of input");
    Node *s;
    if (is_typename(peek()) && !is_p(peekn(1), ':')) s = declaration();
    else if (is_kw(peek(), KW_LABEL)) { next(); while (!accept(';')) next(); continue; }
    else s = stmt();
    cur = cur->next = s;
  }
  pop_scope();
  n->body = head.next;
  return n;
}

/* ---- top level ---- */
static void toplevel(void) {
  Tok *st = peek();
  tick();
  if (accept(';')) return;
  if (is_kw(st, KW_STATIC_ASSERT)) { static_assert_decl(); return; }
  if (is_kw(st, KW_ASM)) { next(); asm_stmt(st); return; }
  DeclSpec ds = {0};
  Type *base = ty_int;
  if (!is_typename(st)) {
    if (st->kind == T_IDENT && !st->id->kw && (is_p(peekn(1), '(') || is_p(peekn(1), '*'))) {
      warn_at(st, "type specifier missing, defaults to 'int'");
      base = ty_int;
    } else if (st->kind == T_OTHER) {
      error_at(st, "stray '%s' in program", tok_str(st));
    } else {
      error_at(st, "expected a declaration before '%s'", tok_str(st));
    }
  } else {
    base = declspec(&ds);
  }
  if (accept(';')) return;
  for (int first = 1;; first = 0) {
    if (!first && !is_p(peek(), ',')) error_at(peek(), "expected ';' after top level declarator");
    if (!first) expect(',');
    Tok *name = NULL;
    Type *ty = declarator(base, &name);
    parse_attrs(&ds.attr);
    if (!name) error_at(peek(), "declaration does not declare anything");
    if (ds.storage == SC_TYPEDEF) {
      Type *c = xalloc(sizeof *c);
      *c = *ty;
      c->tdname = name->id;
      rec_track(c);
      Binding *old = name->id->sym;
      if (old && old->depth == 0 && old->kind == BK_TYPEDEF && !same_type(old->ty, c))
        error_at(name, "typedef redefinition with different types ('%s' vs '%s')", type_str(c), type_str(old->ty));
      bind(name->id, BK_TYPEDEF)->ty = c;
    } else if (ty->kind == TY_FUNC) {
      if (ty->base->kind == TY_FUNC || ty->base->kind == TY_ARRAY) error_at(name, "invalid function return type");
      Var *fn = global_var(name->id, ty, &ds, name);
      if (first && (is_p(peek(), '{') || (ty->variadic < 0 && !is_p(peek(), ';') && !is_p(peek(), ',')))) {
        function_def(fn, ty, name);
        return;
      }
    } else {
      if (ds.storage == SC_AUTO || ds.storage == SC_REGISTER) error_at(st, "illegal storage class on file-scoped variable");
      if (ty->kind == TY_VOID) error_at(name, "variable has incomplete type 'void'");
      if (ty->kind == TY_ARRAY && ty->vla_len) error_at(name, "variable length array declaration not allowed at file scope");
      Var *v = global_var(name->id, ty, &ds, name);
      if (accept('=')) {
        if (v->is_def && v->data) error_at(name, "redefinition of '%s'", name->id->name);
        Type *final;
        Init *in = initializer_for(v->ty, &final);
        v->ty = final;
        global_init(v, in);
        if (ds.storage == SC_EXTERN) warn_at(name, "'extern' variable has an initializer");
      } else if (ds.storage != SC_EXTERN && !v->is_def) {
        v->is_tentative = 1;
      }
    }
    if (accept(';')) return;
  }
}

void parse(Tok **toks, int ntok) {
  (void)ntok;
  for (int i = 1; i < (int)(sizeof kwnames / sizeof *kwnames); i++) if (kwnames[i][0]) intern(kwnames[i], (int)strlen(kwnames[i]))->kw = i;
  intern("_Alignof", 8)->kw = KW_ALIGNOF;
  intern("__builtin_va_list", 17)->kw = KW_VA_LIST;
  intern("__auto_type", 11)->kw = 0;
  id_func = intern("__func__", 8);
  id_FUNCTION = intern("__FUNCTION__", 12);
  id_PRETTY = intern("__PRETTY_FUNCTION__", 19);
  tk = toks;
  tpos = 0;
  depth = 0;
  while (peek()->kind != T_EOF) toplevel();
  /* tentative definitions with incomplete array type become one element (C11 6.9.2p5) */
  for (int i = 0; i < globals.len; i++) {
    Var *v = globals.p[i];
    if (v->is_tentative && !v->is_def) {
      if (v->ty->kind == TY_ARRAY && v->ty->len < 0) {
        warn_at(v->tok, "tentative array definition assumed to have one element");
        v->ty = array_of(v->ty->base, 1);
      }
      if (is_aggr(v->ty) && !v->ty->rec->complete) errorc_at(v->tok, "tentative definition has incomplete type '%s'", type_str(v->ty));
    }
  }
  if (nerrors) exit(1);
}

i64 eval_int(Node *n, int *ok) {
  *ok = n->kind == N_NUM;
  return n->ival;
}
