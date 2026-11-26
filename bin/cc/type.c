// SPDX-License-Identifier: GPL-2.0-or-later
/* C types with the wasm32 sizes and alignments clang uses (ILP32, 8-byte long long
 * and double, 16-byte long double, signed char). */
#include "cc.h"

#define BT(name, k, sz, al, u) static Type name##_s = {k, 0, 0, u, sz, al}; Type *ty_##name = &name##_s;
BT(void, TY_VOID, 1, 1, 0)
BT(bool, TY_BOOL, 1, 1, 1)
BT(char, TY_CHAR, 1, 1, 0)
BT(schar, TY_SCHAR, 1, 1, 0)
BT(uchar, TY_UCHAR, 1, 1, 1)
BT(short, TY_SHORT, 2, 2, 0)
BT(ushort, TY_USHORT, 2, 2, 1)
BT(int, TY_INT, 4, 4, 0)
BT(uint, TY_UINT, 4, 4, 1)
BT(long, TY_LONG, 4, 4, 0)
BT(ulong, TY_ULONG, 4, 4, 1)
BT(llong, TY_LLONG, 8, 8, 0)
BT(ullong, TY_ULLONG, 8, 8, 1)
BT(float, TY_FLOAT, 4, 4, 0)
BT(double, TY_DOUBLE, 8, 8, 0)
BT(ldouble, TY_LDOUBLE, 16, 16, 0)

static Type *new_type(int kind, int size, int align) {
  Type *t = xalloc(sizeof *t);
  t->kind = kind;
  t->size = size;
  t->align = align;
  return t;
}
Type *ptr_to(Type *base) {
  Type *t = new_type(TY_PTR, 4, 4);
  t->base = base;
  t->is_unsigned = 1;
  return t;
}
Type *array_of(Type *base, int len) {
  Type *t = new_type(TY_ARRAY, len < 0 ? 0 : base->size * len, base->align);
  t->base = base;
  t->len = len;
  return t;
}
Type *func_type(Type *ret) {
  Type *t = new_type(TY_FUNC, 1, 1);
  t->base = ret;
  return t;
}
/* Struct and union types cache the size of their Rec. Copies made before the
 * struct is complete are tracked so completing it updates them all. */
static Vec rec_copies;
void rec_track(Type *t) {
  if ((t->kind == TY_STRUCT || t->kind == TY_UNION) && !t->rec->complete) vec_push(&rec_copies, t);
}
void rec_completed(Rec *r) {
  for (int i = 0; i < rec_copies.len; i++) {
    Type *t = rec_copies.p[i];
    if (t->rec == r) { t->size = r->size; t->align = r->align; }
  }
}
Type *qualify(Type *t, int qual) {
  if (!qual || (t->qual & qual) == qual) return t;
  Type *c = xalloc(sizeof *c);
  *c = *t;
  c->qual |= qual;
  rec_track(c);
  return c;
}
Type *unqual(Type *t) {
  if (!t->qual) return t;
  Type *c = xalloc(sizeof *c);
  *c = *t;
  c->qual = 0;
  rec_track(c);
  return c;
}
int is_integer(Type *t) { return t->kind >= TY_BOOL && t->kind <= TY_ULLONG; }
int is_flonum(Type *t) { return t->kind >= TY_FLOAT && t->kind <= TY_LDOUBLE; }
int is_arith(Type *t) { return t->kind >= TY_BOOL && t->kind <= TY_LDOUBLE; }
int is_ptr(Type *t) { return t->kind == TY_PTR; }
int is_scalar(Type *t) { return is_arith(t) || t->kind == TY_PTR; }
int is_aggr(Type *t) { return t->kind == TY_STRUCT || t->kind == TY_UNION; }
int is_memclass(Type *t) { return is_aggr(t) || t->kind == TY_LDOUBLE || t->kind == TY_ARRAY; }

int same_type(Type *a, Type *b) {
  if (a == b) return 1;
  if (a->kind != b->kind) return 0;
  switch (a->kind) {
  case TY_PTR: return same_type(a->base, b->base) && (a->base->qual & ~Q_RESTRICT) == (b->base->qual & ~Q_RESTRICT);
  case TY_ARRAY: return same_type(a->base, b->base) && (a->len < 0 || b->len < 0 || a->len == b->len);
  case TY_STRUCT: case TY_UNION: return a->rec == b->rec;
  case TY_FUNC:
    if (!same_type(a->base, b->base)) return 0;
    if (!a->proto || !b->proto) return 1;
    if (a->nparams != b->nparams || a->variadic != b->variadic) return 0;
    for (int i = 0; i < a->nparams; i++)
      if (!same_type(unqual(a->params[i].ty), unqual(b->params[i].ty))) return 0;
    return 1;
  }
  return 1;
}

static void type_buf(Buf *b, Type *t) {
  if (t->qual & Q_CONST) buf_str(b, "const ");
  if (t->qual & Q_VOLATILE) buf_str(b, "volatile ");
  static const char *names[] = {"void", "_Bool", "char", "signed char", "unsigned char", "short", "unsigned short",
    "int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "float", "double",
    "long double"};
  if (t->kind <= TY_LDOUBLE) { buf_str(b, names[t->kind]); return; }
  switch (t->kind) {
  case TY_PTR: type_buf(b, t->base); buf_str(b, " *"); return;
  case TY_ARRAY: type_buf(b, t->base); buf_str(b, t->len >= 0 ? xfmt("[%d]", t->len) : "[]"); return;
  case TY_FUNC:
    type_buf(b, t->base);
    buf_str(b, " (");
    for (int i = 0; i < t->nparams; i++) {
      if (i) buf_str(b, ", ");
      type_buf(b, t->params[i].ty);
    }
    if (t->variadic) buf_str(b, t->nparams ? ", ..." : "...");
    else if (t->proto && !t->nparams) buf_str(b, "void");
    buf_str(b, ")");
    return;
  default:
    buf_str(b, t->kind == TY_STRUCT ? "struct " : "union ");
    buf_str(b, t->rec->tag ? t->rec->tag->name : "<anonymous>");
  }
}
char *type_str(Type *t) {
  Buf b = {0};
  type_buf(&b, t);
  buf_byte(&b, 0);
  return (char *)b.p;
}

/* The WebAssembly basic C ABI passes a struct that holds exactly one scalar (after
 * looking through nested structs and one-element arrays) as that scalar. Unions and
 * everything else go through memory. Returns the scalar type or NULL. */
Type *struct_field1(Type *t) {
  for (;;) {
    if (t->kind == TY_ARRAY) {
      if (t->len != 1) return NULL;
      t = t->base;
      continue;
    }
    if (t->kind != TY_STRUCT) return is_scalar(t) && t->kind != TY_LDOUBLE ? t : NULL;
    Member *only = NULL;
    for (Member *m = t->rec->members; m; m = m->next) {
      if (m->bitwidth) return NULL;
      if (m->ty->size == 0 && is_aggr(m->ty)) continue; /* empty struct member */
      if (only) return NULL;
      only = m;
    }
    if (!only) return NULL;
    t = only->ty;
  }
}
