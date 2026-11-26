// SPDX-License-Identifier: GPL-2.0-or-later
/* Syntax tree to WebAssembly function bodies.
 *
 * Expressions map onto the wasm operand stack directly. Scalar locals whose address
 * is never taken live in wasm locals; everything else (arrays, structs, locals whose
 * address escapes) lives in a frame on the linear-memory stack, addressed through
 * __stack_pointer exactly as clang does (grows down, 16-byte aligned).
 *
 * Control flow: if, loops and regular switches become wasm block/loop/br/br_table.
 * WebAssembly has no goto. A function that has goto labels (or a switch whose case
 * labels sit inside nested statements, as in Duff's device) is "flattened": only the
 * statements that contain a jump target are cut into numbered states, dispatched by a
 * loop around a br_table; everything else inside them stays structured. A goto is
 * then "set the state, branch to the dispatch loop". See flatten() below.
 *
 * Each function is generated twice. The first pass learns the frame size, whether a
 * frame pointer is needed and the state numbers of forward jumps; the second pass
 * emits the final code with that knowledge. */
#include "cc.h"

enum { WI32 = 0x7f, WI64 = 0x7e, WF32 = 0x7d, WF64 = 0x7c };

/* ---- per-function state ---- */
static Buf *code;
static Vec *relocs;
static Var *fn;
static int pass;
static int depth;
static int nparams_w;          /* wasm parameters */
static Buf ltypes;             /* types of the extra wasm locals */
static Vec freetmp[4];
static int fp_local;           /* -1: none yet */
static int frame_locals;       /* bytes of frame used by in-memory locals */
static int scratch, scratch_max, expr_depth;
static int sret_local, va_local;
static int need_fp, has_calls;
static int p1_need_fp, p1_frame, p1_calls, p1_nstates;

static int st_local, cur_state, nstates, dispatch_blk;
static int brk_blk, brk_st, cont_blk, cont_st;
static int sp_local;           /* VLA: stack pointer saved at entry */

static int wt(Type *t) {
  switch (t->kind) {
  case TY_LLONG: case TY_ULLONG: return WI64;
  case TY_FLOAT: return WF32;
  case TY_DOUBLE: return WF64;
  case TY_VOID: return 0;
  }
  return WI32;
}
static int widx(int w) { return w == WI32 ? 0 : w == WI64 ? 1 : w == WF32 ? 2 : 3; }

static void op(int b) { buf_byte(code, b); }
static void uleb(u64 v) { buf_uleb(code, v); }
static void i32c(int32_t v) { op(0x41); buf_sleb(code, v); }
static void i64c(i64 v) { op(0x42); buf_sleb(code, v); }
static void f32c(float f) { u32 b; memcpy(&b, &f, 4); op(0x43); buf_u32(code, b); }
static void f64c(double d) { u64 b; memcpy(&b, &d, 8); op(0x44); buf_u32(code, (u32)b); buf_u32(code, (u32)(b >> 32)); }
static void lget(int i) { op(0x20); uleb(i); }
static void lset(int i) { op(0x21); uleb(i); }
static void ltee(int i) { op(0x22); uleb(i); }
static void drop(void) { op(0x1a); }

static void reloc(int type, Sym *s, i64 addend) {
  Reloc *r = xalloc(sizeof *r);
  r->off = code->len;
  r->type = type;
  r->sym = s;
  r->addend = addend;
  vec_push(relocs, r);
}
static void gget_sp(void) { op(0x23); reloc(R_GLOBAL_INDEX_LEB, sym_stack_pointer(), 0); buf_uleb5(code, 0); }
static void gset_sp(void) { op(0x24); reloc(R_GLOBAL_INDEX_LEB, sym_stack_pointer(), 0); buf_uleb5(code, 0); }
static void data_addr(Sym *s, i64 addend) { op(0x41); reloc(R_MEMORY_ADDR_SLEB, s, addend); buf_sleb5(code, 0); }
static void func_addr(Sym *s) { op(0x41); reloc(R_TABLE_INDEX_SLEB, s, 0); buf_sleb5(code, 0); }
static void call_sym(Sym *s) { op(0x10); reloc(R_FUNCTION_INDEX_LEB, s, 0); buf_uleb5(code, 0); has_calls = 1; }
static void call_ind(int type) {
  op(0x11);
  reloc(R_TYPE_INDEX_LEB, NULL, type);
  buf_uleb5(code, type);
  reloc(R_TABLE_NUMBER_LEB, sym_table(), 0);
  buf_uleb5(code, 0);
  has_calls = 1;
}
static void mem_copy(void) { op(0xfc); uleb(10); op(0); op(0); }
static void mem_fill(void) { op(0xfc); uleb(11); op(0); }

static int new_local(int w) {
  buf_byte(&ltypes, w);
  return nparams_w + ltypes.len - 1;
}
static int tmp_get(int w) {
  Vec *v = &freetmp[widx(w)];
  if (v->len) return (int)(intptr_t)v->p[--v->len];
  return new_local(w);
}
static void tmp_put(int w, int i) { vec_push(&freetmp[widx(w)], (void *)(intptr_t)i); }

static int get_fp(void) {
  if (fp_local < 0) fp_local = new_local(WI32);
  need_fp = 1;
  return fp_local;
}

static void block(int bt) { op(0x02); op(bt ? bt : 0x40); depth++; }
static void loop(int bt) { op(0x03); op(bt ? bt : 0x40); depth++; }
static void if_(int bt) { op(0x04); op(bt ? bt : 0x40); depth++; }
static void else_(void) { op(0x05); }
static void end(void) { op(0x0b); depth--; }
static void br(int blk) { op(0x0c); uleb(depth - blk); }
static void br_if(int blk) { op(0x0d); uleb(depth - blk); }

static void zero_of(int w) {
  switch (w) {
  case WI32: i32c(0); break;
  case WI64: i64c(0); break;
  case WF32: f32c(0); break;
  case WF64: f64c(0); break;
  }
}

/* scratch memory in the frame, live for one full expression */
static int scratch_alloc(int size, int align) {
  if (align < 4) align = 4;
  scratch = (scratch + align - 1) / align * align;
  int off = frame_locals + scratch;
  scratch += size;
  if (scratch > scratch_max) scratch_max = scratch;
  get_fp();
  return off;
}
static void frame_addr(int off) {
  lget(get_fp());
  if (off) { i32c(off); op(0x6a); }
}

/* ---- types for the ABI ---- */
static int is_sret(Type *t) {
  if (t->kind == TY_LDOUBLE) return 1;
  if (!is_aggr(t)) return 0;
  if (t->size == 0) return 0;
  return struct_field1(t) == NULL;
}

/* wasm signature of a C function type. Prototype-less types use the argument types
 * a call supplies (args may be NULL for a definition). */
static void sig_of(Type *fty, Node **args, int nargs, u8 *sig, int *np, int *nr) {
  int n = 0;
  Type *ret = fty->base;
  if (is_sret(ret)) sig[n++] = WI32;
  int count = fty->proto || !args ? fty->nparams : nargs;
  for (int i = 0; i < count; i++) {
    Type *t = fty->proto || !args ? fty->params[i].ty : args[i]->ty;
    if (t->kind == TY_LDOUBLE) { sig[n++] = WI64; sig[n++] = WI64; continue; }
    if (is_aggr(t)) {
      Type *f = struct_field1(t);
      if (f) sig[n++] = wt(f);
      else if (t->size) sig[n++] = WI32;
      continue;
    }
    sig[n++] = wt(t);
  }
  if (fty->proto && fty->variadic) sig[n++] = WI32;
  *np = n;
  *nr = 0;
  if (ret->kind != TY_VOID && !is_sret(ret)) {
    if (is_aggr(ret)) {
      Type *f = struct_field1(ret);
      if (f) { sig[n++] = wt(f); *nr = 1; }
    } else {
      sig[n++] = wt(ret);
      *nr = 1;
    }
  }
}
void func_sig(Type *fty, u8 *sig, int *np, int *nr) { sig_of(fty, NULL, 0, sig, np, nr); }

/* ---- loads, stores, conversions ---- */
static void memarg(int align, u32 off) { uleb(align); uleb(off); }
static void load(Type *t, u32 off) {
  switch (t->kind) {
  case TY_BOOL: case TY_UCHAR: op(0x2d); memarg(0, off); return;
  case TY_CHAR: case TY_SCHAR: op(0x2c); memarg(0, off); return;
  case TY_SHORT: op(0x2e); memarg(1, off); return;
  case TY_USHORT: op(0x2f); memarg(1, off); return;
  case TY_LLONG: case TY_ULLONG: op(0x29); memarg(3, off); return;
  case TY_FLOAT: op(0x2a); memarg(2, off); return;
  case TY_DOUBLE: op(0x2b); memarg(3, off); return;
  default: op(0x28); memarg(2, off); return;
  }
}
static void store(Type *t, u32 off) {
  switch (t->kind) {
  case TY_BOOL: case TY_UCHAR: case TY_CHAR: case TY_SCHAR: op(0x3a); memarg(0, off); return;
  case TY_SHORT: case TY_USHORT: op(0x3b); memarg(1, off); return;
  case TY_LLONG: case TY_ULLONG: op(0x37); memarg(3, off); return;
  case TY_FLOAT: op(0x38); memarg(2, off); return;
  case TY_DOUBLE: op(0x39); memarg(3, off); return;
  default: op(0x36); memarg(2, off); return;
  }
}

static Sym *helper(const char *name, const u8 *sig, int np, int nr) { return sym_extern_func(name, sig, np, nr); }

/* value on the stack of type `from` becomes type `to` */
static void gen_ldconv_to(Type *from);
static void convert(Type *from, Type *to) {
  if (to->kind == TY_VOID) { if (wt(from) && !is_memclass(from)) drop(); else if (is_memclass(from)) drop(); return; }
  if (to->kind == TY_LDOUBLE || from->kind == TY_LDOUBLE) {
    if (to->kind == TY_LDOUBLE && from->kind == TY_LDOUBLE) return;
    if (to->kind == TY_LDOUBLE) { gen_ldconv_to(from); return; }
    /* long double (address) to something: compiler-rt helpers, as clang does */
    int t = tmp_get(WI32);
    ltee(t);
    op(0x29); memarg(3, 0);
    lget(t);
    op(0x29); memarg(3, 8);
    tmp_put(WI32, t);
    u8 sig[3] = {WI64, WI64, 0};
    const char *name;
    if (to->kind == TY_BOOL) {
      i64c(0); i64c(0);
      u8 s2[5] = {WI64, WI64, WI64, WI64, WI32};
      call_sym(helper("__netf2", s2, 4, 1));
      i32c(0); op(0x47);
      return;
    }
    if (to->kind == TY_FLOAT) { name = "__trunctfsf2"; sig[2] = WF32; }
    else if (to->kind == TY_DOUBLE) { name = "__trunctfdf2"; sig[2] = WF64; }
    else if (to->size == 8) { name = to->is_unsigned ? "__fixunstfdi" : "__fixtfdi"; sig[2] = WI64; }
    else { name = to->is_unsigned ? "__fixunstfsi" : "__fixtfsi"; sig[2] = WI32; }
    call_sym(helper(name, sig, 2, 1));
    if (is_integer(to) && to->size < 4) convert(to->is_unsigned ? ty_uint : ty_int, to);
    return;
  }
  int fw = wt(from), tw = wt(to);
  if (to->kind == TY_BOOL) {
    switch (fw) {
    case WI32: if (from->kind != TY_BOOL) { i32c(0); op(0x47); } return;
    case WI64: op(0x50); op(0x45); return;
    case WF32: f32c(0); op(0x5c); return;
    case WF64: f64c(0); op(0x62); return;
    }
    return;
  }
  if (is_flonum(to)) {
    if (fw == WI32) op(tw == WF32 ? (from->is_unsigned ? 0xb3 : 0xb2) : (from->is_unsigned ? 0xb8 : 0xb7));
    else if (fw == WI64) op(tw == WF32 ? (from->is_unsigned ? 0xb5 : 0xb4) : (from->is_unsigned ? 0xba : 0xb9));
    else if (fw == WF32 && tw == WF64) op(0xbb);
    else if (fw == WF64 && tw == WF32) op(0xb6);
    return;
  }
  /* to an integer or pointer */
  if (fw == WF32 || fw == WF64) {
    int u = to->is_unsigned;
    int sub = (tw == WI64 ? 4 : 0) + (fw == WF64 ? 2 : 0) + u;
    if (to->size < 4) sub = (fw == WF64 ? 2 : 0); /* small types: convert through int */
    op(0xfc);
    uleb(sub);
    if (to->size < 4) convert(ty_int, to);
    return;
  }
  if (fw == WI64 && tw == WI32) {
    op(0xa7);
    if (to->size < 4) convert(ty_int, to);
    return;
  }
  if (fw == WI32 && tw == WI64) {
    op(from->is_unsigned ? 0xad : 0xac);
    return;
  }
  if (tw == WI32 && to->size < 4) {
    if (from->size < to->size && (from->is_unsigned || !to->is_unsigned)) return; /* value already fits */
    if (from->size == to->size && from->is_unsigned == to->is_unsigned) return;
    switch (to->kind) {
    case TY_CHAR: case TY_SCHAR: op(0xc0); return;
    case TY_UCHAR: i32c(255); op(0x71); return;
    case TY_SHORT: op(0xc1); return;
    case TY_USHORT: i32c(65535); op(0x71); return;
    }
  }
}

/* ---- expressions ---- */
static void gen_expr(Node *n);
static void gen_void(Node *n);
static void gen_addr(Node *n);
static void gen_stmt(Node *s);
static void gen_cond(Node *n);

static int is_reg(Node *n) { return n->kind == N_VAR && n->var->is_local && !n->var->in_mem; }

/* Push a base address; *off gets a constant to put in the memarg. */
static void gen_addr_off(Node *n, u32 *off) {
  switch (n->kind) {
  case N_VAR: {
    Var *v = n->var;
    if (!v->is_local) { data_addr(sym_data(v), 0); *off = 0; return; }
    if (v->in_mem == 2) { lget(v->local); *off = 0; return; }
    if (!v->in_mem) error_at(n->tok, "internal error: address of a register variable");
    lget(get_fp());
    *off = v->frame_off;
    return;
  }
  case N_INITVAR: {
    extern void gen_local_init(Var *v, Init *in);
    gen_local_init(n->var, n->initz);
    Node tmp = *n;
    tmp.kind = N_VAR;
    gen_addr_off(&tmp, off);
    return;
  }
  case N_MEMBER: {
    u32 o;
    if (n->lhs->kind == N_CALL || n->lhs->kind == N_COND || n->lhs->kind == N_COMMA || n->lhs->kind == N_ASSIGN ||
        n->lhs->kind == N_STMTEXPR || n->lhs->kind == N_VAARG || n->lhs->kind == N_CAST) {
      gen_expr(n->lhs); /* a struct rvalue is its address */
      o = 0;
    } else {
      gen_addr_off(n->lhs, &o);
    }
    *off = o + n->mem->offset;
    return;
  }
  case N_DEREF: {
    Node *p = n->lhs;
    if (p->kind == N_ADD && p->rhs->kind == N_NUM && p->rhs->ival >= 0 && p->rhs->ival < 0x40000000) {
      gen_expr(p->lhs);
      *off = (u32)p->rhs->ival;
      return;
    }
    gen_expr(p);
    *off = 0;
    return;
  }
  case N_CALL: case N_COND: case N_COMMA: case N_ASSIGN: case N_STMTEXPR: case N_VAARG: case N_CAST:
    if (is_memclass(n->ty)) { gen_expr(n); *off = 0; return; }
    break;
  }
  error_at(n->tok, "internal error: not an lvalue");
}
static void gen_addr(Node *n) {
  u32 off;
  if (n->kind == N_VAR && n->ty->kind == TY_FUNC) { func_addr(sym_func(n->var)); return; }
  gen_addr_off(n, &off);
  if (off) { i32c((int32_t)off); op(0x6a); }
}

static int bit_unsigned(Member *m) { return m->ty->is_unsigned || m->ty->kind == TY_BOOL; }

/* container value on the stack -> field value as an int of the member's type */
static void bf_extract(Member *m) {
  int w = m->bitwidth, b = m->bitoff;
  if (m->ty->size == 8) {
    if (64 - b - w) { i64c(64 - b - w); op(0x86); }
    if (64 - w) { i64c(64 - w); op(bit_unsigned(m) ? 0x88 : 0x87); }
    return;
  }
  if (32 - b - w) { i32c(32 - b - w); op(0x74); }
  if (32 - w) { i32c(32 - w); op(bit_unsigned(m) ? 0x76 : 0x75); }
}
static Type *bf_ctype(Member *m) {
  switch (m->ty->size) {
  case 1: return ty_uchar;
  case 2: return ty_ushort;
  case 8: return ty_ullong;
  }
  return ty_uint;
}
static void gen_bf_load(Node *n) {
  u32 off;
  gen_addr_off(n, &off);
  load(bf_ctype(n->mem), off);
  bf_extract(n->mem);
}
/* stack: address (base), value; stores value into the bit-field at base+off */
static void gen_bf_store_at(Member *m, u32 off, int addr_tmp, int val_tmp) {
  int is64 = m->ty->size == 8;
  Type *ct = bf_ctype(m);
  u64 mask = (m->bitwidth == 64 ? ~0ull : ((1ull << m->bitwidth) - 1)) << m->bitoff;
  lget(addr_tmp);
  lget(addr_tmp);
  load(ct, off);
  if (is64) { i64c((i64)~mask); op(0x83); } else { i32c((int32_t)~(u32)mask); op(0x71); }
  lget(val_tmp);
  if (is64) {
    if (m->bitoff) { i64c(m->bitoff); op(0x86); }
    i64c((i64)mask); op(0x83); op(0x84);
  } else {
    if (m->bitoff) { i32c(m->bitoff); op(0x74); }
    i32c((int32_t)(u32)mask); op(0x71); op(0x72);
  }
  store(ct, off);
}

/* stack: dst, src. Small copies become loads and stores: memory.copy is a call into
 * the engine's runtime in V8, slow for a few bytes. */
static void gen_copy(int size) {
  if (size > 32 || size <= 0) {
    i32c(size);
    mem_copy();
    return;
  }
  int s = tmp_get(WI32), d = tmp_get(WI32);
  lset(s);
  lset(d);
  int off = 0;
  while (off < size) {
    int n = size - off >= 8 ? 8 : size - off >= 4 ? 4 : size - off >= 2 ? 2 : 1;
    lget(d);
    lget(s);
    switch (n) {
    case 8: op(0x29); memarg(0, off); op(0x37); memarg(0, off); break;
    case 4: op(0x28); memarg(0, off); op(0x36); memarg(0, off); break;
    case 2: op(0x2f); memarg(0, off); op(0x3b); memarg(0, off); break;
    default: op(0x2d); memarg(0, off); op(0x3a); memarg(0, off); break;
    }
    off += n;
  }
  tmp_put(WI32, s);
  tmp_put(WI32, d);
}

static void gen_assign(Node *n, int want) {
  Node *l = n->lhs;
  Type *ty = n->ty;
  if (is_memclass(ty)) {
    int t = tmp_get(WI32);
    gen_addr(l);
    if (want) ltee(t);
    gen_expr(n->rhs);
    gen_copy(ty->size);
    if (want) lget(t);
    tmp_put(WI32, t);
    return;
  }
  if (is_reg(l)) {
    gen_expr(n->rhs);
    if (want) ltee(l->var->local);
    else lset(l->var->local);
    return;
  }
  if (l->kind == N_MEMBER && l->mem->bitwidth) {
    u32 off;
    int w = wt(l->mem->ty);
    int at = tmp_get(WI32), vt = tmp_get(w);
    gen_addr_off(l, &off);
    lset(at);
    gen_expr(n->rhs);
    lset(vt);
    gen_bf_store_at(l->mem, off, at, vt);
    if (want) { lget(at); load(bf_ctype(l->mem), off); bf_extract(l->mem); }
    tmp_put(WI32, at);
    tmp_put(w, vt);
    return;
  }
  u32 off;
  gen_addr_off(l, &off);
  gen_expr(n->rhs);
  int t = 0, w = wt(ty);
  if (want) { t = tmp_get(w); ltee(t); }
  store(ty, off);
  if (want) { lget(t); tmp_put(w, t); }
}

static void binop(int kind, Type *ty) {
  int w = wt(ty), u = ty->is_unsigned || ty->kind == TY_PTR;
  static const u8 i32ops[] = {0x6a, 0x6b, 0x6c, 0x6d, 0x6f, 0x74, 0x75, 0x71, 0x72, 0x73};
  static const u8 i64ops[] = {0x7c, 0x7d, 0x7e, 0x7f, 0x81, 0x86, 0x87, 0x83, 0x84, 0x85};
  if (ty->kind == TY_LDOUBLE) {
    const char *name = kind == N_ADD ? "__addtf3" : kind == N_SUB ? "__subtf3" : kind == N_MUL ? "__multf3" : "__divtf3";
    (void)name;
    error_at(NULL, "internal error: long double binop");
  }
  if (w == WI32 || w == WI64) {
    const u8 *ops = w == WI32 ? i32ops : i64ops;
    int i;
    switch (kind) {
    case N_ADD: i = 0; break;
    case N_SUB: i = 1; break;
    case N_MUL: i = 2; break;
    case N_DIV: op(ops[3] + u); return;
    case N_MOD: op(ops[4] + u); return;
    case N_SHL: i = 5; break;
    case N_SHR: op(ops[6] + u); return;
    case N_BAND: i = 7; break;
    case N_BOR: i = 8; break;
    case N_BXOR: i = 9; break;
    case N_EQ: op(w == WI32 ? 0x46 : 0x51); return;
    case N_NE: op(w == WI32 ? 0x47 : 0x52); return;
    case N_LT: op((w == WI32 ? 0x48 : 0x53) + u); return;
    case N_LE: op((w == WI32 ? 0x4c : 0x57) + u); return;
    default: error_at(NULL, "internal error: binop %d", kind); return;
    }
    op(ops[i]);
    return;
  }
  int f = w == WF32;
  switch (kind) {
  case N_ADD: op(f ? 0x92 : 0xa0); return;
  case N_SUB: op(f ? 0x93 : 0xa1); return;
  case N_MUL: op(f ? 0x94 : 0xa2); return;
  case N_DIV: op(f ? 0x95 : 0xa3); return;
  case N_EQ: op(f ? 0x5b : 0x61); return;
  case N_NE: op(f ? 0x5c : 0x62); return;
  case N_LT: op(f ? 0x5d : 0x63); return;
  case N_LE: op(f ? 0x5f : 0x65); return;
  }
  error_at(NULL, "internal error: float binop %d", kind);
}

/* long double operations through compiler-rt helpers (the same calls clang makes) */
static void push_ld_halves(Node *n) {
  int t = tmp_get(WI32);
  gen_expr(n);
  ltee(t);
  op(0x29); memarg(3, 0);
  lget(t);
  op(0x29); memarg(3, 8);
  tmp_put(WI32, t);
}
static void gen_ld_binop(Node *n) {
  int k = n->kind;
  if (k == N_EQ || k == N_NE || k == N_LT || k == N_LE) {
    push_ld_halves(n->lhs);
    push_ld_halves(n->rhs);
    u8 s[5] = {WI64, WI64, WI64, WI64, WI32};
    const char *name = k == N_EQ ? "__eqtf2" : k == N_NE ? "__netf2" : k == N_LT ? "__lttf2" : "__letf2";
    call_sym(helper(name, s, 4, 1));
    i32c(0);
    op(k == N_EQ ? 0x46 : k == N_NE ? 0x47 : k == N_LT ? 0x48 : 0x4c);
    return;
  }
  int off = scratch_alloc(16, 16);
  frame_addr(off);
  push_ld_halves(n->lhs);
  push_ld_halves(n->rhs);
  u8 s[5] = {WI32, WI64, WI64, WI64, WI64};
  const char *name = k == N_ADD ? "__addtf3" : k == N_SUB ? "__subtf3" : k == N_MUL ? "__multf3" : "__divtf3";
  call_sym(helper(name, s, 5, 0));
  frame_addr(off);
}
static void gen_ldconv_to(Type *from) {
  /* value of `from` on the stack -> address of a long double temporary */
  int w = wt(from);
  int t = tmp_get(w);
  lset(t);
  int off = scratch_alloc(16, 16);
  frame_addr(off);
  lget(t);
  tmp_put(w, t);
  const char *name;
  u8 s[2] = {WI32, (u8)w};
  if (from->kind == TY_FLOAT) name = "__extendsftf2";
  else if (from->kind == TY_DOUBLE) name = "__extenddftf2";
  else if (w == WI64) name = from->is_unsigned ? "__floatunditf" : "__floatditf";
  else name = from->is_unsigned ? "__floatunsitf" : "__floatsitf";
  call_sym(helper(name, s, 2, 0));
  frame_addr(off);
}

u64 ld_bits(double d, u64 *hi) {
  u64 b;
  memcpy(&b, &d, 8);
  u64 sign = b >> 63, exp = (b >> 52) & 0x7ff, mant = b & ((1ull << 52) - 1);
  u64 qexp;
  if (exp == 0 && mant == 0) qexp = 0;
  else if (exp == 0x7ff) qexp = 0x7fff;
  else if (exp == 0) {
    int sh = 0;
    while (!(mant & (1ull << 52))) { mant <<= 1; sh++; }
    mant &= (1ull << 52) - 1;
    qexp = 16383 - 1022 - sh;
  } else qexp = exp - 1023 + 16383;
  *hi = sign << 63 | qexp << 48 | mant >> 4;
  return (mant & 15) << 60;
}

static Var *ld_const(double v) {
  Var *c = xalloc(sizeof *c);
  static int n;
  c->name = intern(xfmt(".L.ld.%d", n++), 0);
  c->name = intern(xfmt(".L.ld.%d", n - 1), (int)strlen(xfmt(".L.ld.%d", n - 1)));
  c->ty = ty_ldouble;
  c->is_static = c->is_def = c->is_const = 1;
  c->data = xalloc(16);
  c->datalen = 16;
  u64 hi, lo = ld_bits(v, &hi);
  for (int i = 0; i < 8; i++) { c->data[i] = (u8)(lo >> (8 * i)); c->data[8 + i] = (u8)(hi >> (8 * i)); }
  return c;
}

static void gen_opassign(Node *n, int want, int post) {
  Node *l = n->lhs;
  Type *lt = n->ty, *ct = n->fty;
  int kind = n->kind == N_OPASSIGN ? n->op : N_ADD;
  Node *rhs = n->kind == N_OPASSIGN ? n->rhs : NULL;
  if (lt->kind == TY_LDOUBLE || ct->kind == TY_LDOUBLE) error_at(n->tok, "long double compound assignment is not supported");
  /* the operand */
#define RHS() do { \
    if (rhs) gen_expr(rhs); \
    else if (is_flonum(ct)) { if (ct->kind == TY_FLOAT) f32c((float)n->ival); else f64c((double)n->ival); } \
    else if (wt(ct) == WI64) i64c(n->ival); \
    else i32c((int32_t)n->ival); \
  } while (0)
  int lw = wt(lt);
  if (is_reg(l)) {
    int v = l->var->local;
    if (post && want) lget(v);
    lget(v);
    convert(lt, ct);
    RHS();
    binop(kind, is_ptr(ct) ? ty_ulong : ct);
    convert(is_ptr(ct) ? ty_ulong : ct, lt);
    if (want && !post) ltee(v);
    else lset(v);
    return;
  }
  if (l->kind == N_MEMBER && l->mem->bitwidth) {
    u32 off;
    int at = tmp_get(WI32), vt = tmp_get(lw), old = 0;
    gen_addr_off(l, &off);
    lset(at);
    lget(at);
    load(bf_ctype(l->mem), off);
    bf_extract(l->mem);
    if (post && want) { old = tmp_get(lw); ltee(old); }
    convert(lt, ct);
    RHS();
    binop(kind, ct);
    convert(ct, lt);
    lset(vt);
    gen_bf_store_at(l->mem, off, at, vt);
    if (want) {
      if (post) { lget(old); tmp_put(lw, old); }
      else { lget(at); load(bf_ctype(l->mem), off); bf_extract(l->mem); }
    }
    tmp_put(WI32, at);
    tmp_put(lw, vt);
    return;
  }
  u32 off;
  int at = tmp_get(WI32), res = 0;
  Node *base = l->kind == N_DEREF ? (l->lhs->kind == N_ADD && l->lhs->rhs->kind == N_NUM ? l->lhs->lhs : l->lhs) :
               l->kind == N_MEMBER && l->lhs->kind == N_DEREF ? l->lhs->lhs : NULL;
  if (base && is_reg(base)) {
    gen_addr_off(l, &off);
    gen_addr_off(l, &off);
  } else {
    gen_addr_off(l, &off);
    ltee(at);
    lget(at);
  }
  load(lt, off);
  if (post && want) { res = tmp_get(lw); ltee(res); }
  convert(lt, ct);
  RHS();
  binop(kind, is_ptr(ct) ? ty_ulong : ct);
  convert(is_ptr(ct) ? ty_ulong : ct, lt);
  if (want && !post) { res = tmp_get(lw); ltee(res); }
  store(lt, off);
  if (want) { lget(res); tmp_put(lw, res); }
  tmp_put(WI32, at);
#undef RHS
}

/* push an i32 that is nonzero exactly when the scalar n is true */
static void gen_cond(Node *n) {
  if (n->kind == N_CAST && n->ty->kind == TY_BOOL) { gen_cond(n->lhs); return; }
  if (n->ty->kind == TY_LDOUBLE) { gen_expr(n); convert(ty_ldouble, ty_bool); return; }
  gen_expr(n);
  switch (wt(n->ty)) {
  case WI64: i64c(0); op(0x52); break;
  case WF32: f32c(0); op(0x5c); break;
  case WF64: f64c(0); op(0x62); break;
  }
}
/* push an i32 that is nonzero exactly when n is false */
static void gen_cond_not(Node *n) {
  if (is_integer(n->ty) || is_ptr(n->ty)) {
    int k = n->kind;
    if ((k == N_EQ || k == N_NE || k == N_LT || k == N_LE) && (is_integer(n->lhs->ty) || is_ptr(n->lhs->ty)) && wt(n->lhs->ty)) {
      Type *t = n->lhs->ty;
      int w = wt(t), u = t->is_unsigned || t->kind == TY_PTR;
      gen_expr(n->lhs);
      gen_expr(n->rhs);
      if (w == WI32) op(k == N_EQ ? 0x47 : k == N_NE ? 0x46 : k == N_LT ? 0x4e + u : 0x4a + u);
      else op(k == N_EQ ? 0x52 : k == N_NE ? 0x51 : k == N_LT ? 0x59 + u : 0x55 + u);
      return;
    }
    if (k == N_NOT) { gen_cond(n->lhs); return; }
  }
  gen_cond(n);
  op(0x45);
}
static int is_bool_valued(Node *n) {
  switch (n->kind) {
  case N_EQ: case N_NE: case N_LT: case N_LE: case N_LAND: case N_LOR: case N_NOT: return 1;
  case N_NUM: return n->ival == 0 || n->ival == 1;
  }
  return n->ty->kind == TY_BOOL;
}
static void gen_bool(Node *n) {
  gen_cond(n);
  if (!is_bool_valued(n) || wt(n->ty) != WI32) { i32c(0); op(0x47); }
}


/* Arguments in the order the wasm signature wants them. */
static void gen_arg(Node *a, Type *t) {
  if (t->kind == TY_LDOUBLE) { push_ld_halves(a); return; }
  if (is_aggr(t)) {
    Type *f = struct_field1(t);
    if (f) { gen_expr(a); load(f, 0); return; }
    if (!t->size) { gen_void(a); return; }
    int off = scratch_alloc(t->size, t->align);
    frame_addr(off);
    gen_expr(a);
    gen_copy(t->size);
    frame_addr(off);
    return;
  }
  gen_expr(a);
}

static void gen_call(Node *n, int want) {
  Type *fty = n->fty, *ret = fty->base;
  if (n->lhs->kind == N_VAR && n->lhs->ty->kind == TY_FUNC && !n->lhs->var->is_def && !n->lhs->var->is_static &&
      fty->proto && fty->nparams == 3 && n->nargs == 3 && is_ptr(ret)) {
    const char *nm = n->lhs->var->name->name;
    int k = !strcmp(nm, "memcpy") ? 1 : !strcmp(nm, "memmove") ? 2 : !strcmp(nm, "memset") ? 3 : 0;
    if (k) {
      int t = tmp_get(WI32);
      gen_expr(n->args[0]);
      if (want) ltee(t);
      gen_expr(n->args[1]);
      if (k != 3 && n->args[2]->kind == N_NUM && n->args[2]->ival <= 32 && n->args[2]->ival > 0) gen_copy((int)n->args[2]->ival);
      else { gen_expr(n->args[2]); if (k == 3) mem_fill(); else mem_copy(); }
      if (want) lget(t);
      tmp_put(WI32, t);
      return;
    }
  }
  u8 sig[512];
  int np, nr;
  sig_of(fty, n->args, n->nargs, sig, &np, &nr);
  if (np > 500) error_at(n->tok, "too many arguments");
  int sret = is_sret(ret), sret_off = 0;
  if (sret) { sret_off = scratch_alloc(ret->size, ret->align); frame_addr(sret_off); }
  int nfixed = fty->proto ? fty->nparams : n->nargs;
  for (int i = 0; i < nfixed; i++) gen_arg(n->args[i], fty->proto ? fty->params[i].ty : n->args[i]->ty);
  if (fty->proto && fty->variadic) {
    int total = 0;
    for (int i = nfixed; i < n->nargs; i++) {
      Type *t = n->args[i]->ty;
      int sz = is_aggr(t) ? 4 : t->size, al = is_aggr(t) ? 4 : t->align;
      total = (total + al - 1) / al * al + sz;
    }
    if (n->nargs > nfixed) {
      int area = scratch_alloc(total, 16), o = 0;
      for (int i = nfixed; i < n->nargs; i++) {
        Node *a = n->args[i];
        Type *t = a->ty;
        int sz = is_aggr(t) ? 4 : t->size, al = is_aggr(t) ? 4 : t->align;
        o = (o + al - 1) / al * al;
        if (is_aggr(t)) {
          int c = scratch_alloc(t->size ? t->size : 1, t->align);
          frame_addr(c);
          gen_expr(a);
          gen_copy(t->size);
          lget(get_fp());
          frame_addr(c);
          op(0x36); memarg(2, area + o);
        } else if (t->kind == TY_LDOUBLE) {
          frame_addr(area + o);
          gen_expr(a);
          gen_copy(16);
        } else {
          lget(get_fp());
          gen_expr(a);
          store(t, area + o);
        }
        o += sz;
      }
      frame_addr(area);
    } else {
      i32c(0);
    }
  }
  int type = wasm_functype(sig, np, nr);
  if (n->lhs->kind == N_VAR && n->lhs->ty->kind == TY_FUNC) {
    Var *v = n->lhs->var;
    Sym *s = sym_func(v);
    if (s->type != type) {
      if (!v->ty->proto && !v->is_def && !s->defined) {
        /* the first call fixes the import's type; every later call must agree */
        static Map fixed;
        if (map_get(&fixed, s->name)) error_at(n->tok, "calls to the unprototyped function '%s' with different argument types are not supported", s->name);
        map_put(&fixed, s->name, s);
        s->type = type;
      } else if (!v->ty->proto) {
        warn_at(n->tok, "call to '%s' does not match its definition", s->name);
        /* call through the table so the mismatch traps at run time, as on other targets it is undefined */
        func_addr(s);
        call_ind(type);
        goto after;
      } else {
        s->type = type;
      }
    }
    call_sym(s);
  } else {
    gen_expr(n->lhs);
    call_ind(type);
  }
after:
  if (sret) {
    if (want) frame_addr(sret_off);
    return;
  }
  if (is_aggr(ret)) {
    Type *f = struct_field1(ret);
    if (!f) { if (want) frame_addr(scratch_alloc(1, 4)); return; }
    int w = wt(f), t = tmp_get(w);
    lset(t);
    if (!want) { tmp_put(w, t); return; }
    int off = scratch_alloc(ret->size, ret->align);
    lget(get_fp());
    lget(t);
    store(f, off);
    frame_addr(off);
    tmp_put(w, t);
    return;
  }
  if (!want && nr) drop();
}

static void gen_builtin(Node *n) {
  Node **a = n->args;
  switch (n->bid) {
  case BI_VA_START: {
    u32 off;
    if (is_reg(a[0])) { lget(va_local); lset(a[0]->var->local); return; }
    gen_addr_off(a[0], &off);
    lget(va_local);
    op(0x36); memarg(2, off);
    return;
  }
  case BI_VA_END: if (n->nargs) gen_void(a[0]); return;
  case BI_UNREACHABLE: case BI_TRAP: op(0x00); return;
  case BI_ALLOCA:
    get_fp();
    gget_sp();
    gen_expr(a[0]);
    i32c(15); op(0x6a); i32c(-16); op(0x71);
    op(0x6b);
    { int t = tmp_get(WI32); ltee(t); gset_sp(); lget(t); tmp_put(WI32, t); }
    return;
  case BI_FRAME_ADDRESS: lget(get_fp()); return;
  case BI_CLZ: case BI_CTZ: case BI_POPCOUNT: {
    int is64 = wt(a[0]->ty) == WI64;
    gen_expr(a[0]);
    op((is64 ? 0x79 : 0x67) + (n->bid - BI_CLZ));
    if (is64) op(0xa7);
    return;
  }
  case BI_BSWAP16:
    gen_expr(a[0]);
    { int t = tmp_get(WI32); ltee(t); i32c(8); op(0x76); i32c(255); op(0x71); lget(t); i32c(8); op(0x74); i32c(0xff00); op(0x71); op(0x72); tmp_put(WI32, t); }
    return;
  case BI_BSWAP32: {
    int t = tmp_get(WI32);
    gen_expr(a[0]);
    lset(t);
    lget(t); i32c(24); op(0x76);
    lget(t); i32c(8); op(0x76); i32c(0xff00); op(0x71); op(0x72);
    lget(t); i32c(8); op(0x74); i32c(0xff0000); op(0x71); op(0x72);
    lget(t); i32c(24); op(0x74); op(0x72);
    tmp_put(WI32, t);
    return;
  }
  case BI_BSWAP64: {
    int t = tmp_get(WI64);
    gen_expr(a[0]);
    lset(t);
    i64c(0);
    for (int i = 0; i < 8; i++) {
      lget(t);
      int sh = 8 * i;
      if (sh) { i64c(sh); op(0x88); }
      i64c(0xff); op(0x83);
      if (56 - sh) { i64c(56 - sh); op(0x86); }
      op(0x84);
    }
    tmp_put(WI64, t);
    return;
  }
  case BI_SQRT: case BI_FLOOR: case BI_CEIL: case BI_TRUNC: case BI_FABS: case BI_RINT: {
    int f = n->ty->kind == TY_FLOAT;
    gen_expr(a[0]);
    static const u8 f32o[] = {0x91, 0x8e, 0x8d, 0x8f, 0x8b, 0x90}, f64o[] = {0x9f, 0x9c, 0x9b, 0x9d, 0x99, 0x9e};
    int i = n->bid - BI_SQRT;
    op(f ? f32o[i] : f64o[i]);
    return;
  }
  case BI_COPYSIGN:
    gen_expr(a[0]);
    gen_expr(a[1]);
    op(n->ty->kind == TY_FLOAT ? 0x98 : 0xa6);
    return;
  case BI_ISNAN: case BI_ISINF: case BI_ISFINITE: case BI_ISNORMAL: {
    int w = wt(a[0]->ty), f = w == WF32;
    int t = tmp_get(w);
    gen_expr(a[0]);
    if (n->bid == BI_ISNAN) { ltee(t); lget(t); op(f ? 0x5c : 0x62); tmp_put(w, t); return; }
    op(f ? 0x8b : 0x99);
    double inf = 1e308 * 10;
    if (n->bid == BI_ISINF) { if (f) f32c((float)inf); else f64c(inf); op(f ? 0x5b : 0x61); }
    else if (n->bid == BI_ISFINITE) { if (f) f32c((float)inf); else f64c(inf); op(f ? 0x5d : 0x63); }
    else {
      ltee(t);
      if (f) f32c(1.17549435e-38f); else f64c(2.2250738585072014e-308);
      op(f ? 0x60 : 0x66);
      lget(t);
      if (f) f32c((float)inf); else f64c(inf);
      op(f ? 0x5d : 0x63);
      op(0x71);
    }
    tmp_put(w, t);
    return;
  }
  case BI_SIGNBIT: {
    int w = wt(a[0]->ty);
    gen_expr(a[0]);
    if (w == WF32) { op(0xbc); i32c(31); op(0x76); }
    else { op(0xbd); i64c(63); op(0x88); op(0xa7); }
    return;
  }
  case BI_FPCLASSIFY: {
    int w = wt(a[5]->ty), f = w == WF32;
    int t = tmp_get(w);
    double inf = 1e308 * 10;
    gen_expr(a[5]);
    op(f ? 0x8b : 0x99);
    lset(t);
    lget(t); lget(t); op(f ? 0x5c : 0x62);
    if_(WI32); gen_expr(a[0]); else_();
    lget(t); if (f) f32c((float)inf); else f64c(inf); op(f ? 0x5b : 0x61);
    if_(WI32); gen_expr(a[1]); else_();
    lget(t); if (f) f32c(1.17549435e-38f); else f64c(2.2250738585072014e-308); op(f ? 0x60 : 0x66);
    if_(WI32); gen_expr(a[2]); else_();
    lget(t); if (f) f32c(0); else f64c(0); op(f ? 0x5b : 0x61);
    if_(WI32); gen_expr(a[4]); else_(); gen_expr(a[3]); end();
    end(); end(); end();
    tmp_put(w, t);
    return;
  }
  case BI_ISLESSGREATER: case BI_ISUNORDERED: {
    int w = wt(a[0]->ty), f = w == WF32;
    int x = tmp_get(w), y = tmp_get(w);
    gen_expr(a[0]); lset(x);
    gen_expr(a[1]); lset(y);
    if (n->bid == BI_ISLESSGREATER) {
      lget(x); lget(y); op(f ? 0x5d : 0x63);
      lget(x); lget(y); op(f ? 0x5e : 0x64);
    } else {
      lget(x); lget(x); op(f ? 0x5c : 0x62);
      lget(y); lget(y); op(f ? 0x5c : 0x62);
    }
    op(0x72);
    tmp_put(w, x);
    tmp_put(w, y);
    return;
  }
  case BI_MEMCPY: case BI_MEMMOVE: case BI_MEMSET: {
    int t = tmp_get(WI32);
    gen_expr(a[0]);
    ltee(t);
    gen_expr(a[1]);
    gen_expr(a[2]);
    if (n->bid == BI_MEMSET) mem_fill(); else mem_copy();
    lget(t);
    tmp_put(WI32, t);
    return;
  }
  case BI_MEMORY_SIZE: op(0x3f); op(0); return;
  case BI_MEMORY_GROW: gen_expr(a[1]); op(0x40); op(0); return;
  }
  error_at(n->tok, "internal error: builtin %d", n->bid);
}

static void gen_vaarg(Node *n) {
  /* ap is a pointer into the caller's argument area; each argument sits at its
   * natural alignment, aggregates as a pointer to a copy */
  Type *t = n->ty;
  int agg = is_aggr(t);
  int al = agg ? 4 : t->align, sz = agg ? 4 : t->size;
  int p = tmp_get(WI32);
  Node *ap = n->lhs;
  if (is_reg(ap)) {
    lget(ap->var->local);
    if (al > 4) { i32c(al - 1); op(0x6a); i32c(-al); op(0x71); }
    ltee(p);
    i32c(sz); op(0x6a);
    lset(ap->var->local);
  } else {
    u32 off;
    int at = tmp_get(WI32);
    gen_addr_off(ap, &off);
    ltee(at);
    lget(at);
    op(0x28); memarg(2, off);
    if (al > 4) { i32c(al - 1); op(0x6a); i32c(-al); op(0x71); }
    ltee(p);
    i32c(sz); op(0x6a);
    op(0x36); memarg(2, off);
    tmp_put(WI32, at);
  }
  lget(p);
  if (agg) op(0x28), memarg(2, 0);
  else if (t->kind != TY_LDOUBLE) load(t, 0);
  tmp_put(WI32, p);
}

extern void gen_local_init(Var *v, Init *in);

static void gen_expr(Node *n) {
  expr_depth++;
  switch (n->kind) {
  case N_NUM:
    if (wt(n->ty) == WI64) i64c(n->ival);
    else i32c((int32_t)n->ival);
    break;
  case N_FNUM:
    if (n->ty->kind == TY_FLOAT) f32c((float)n->fval);
    else if (n->ty->kind == TY_DOUBLE) f64c(n->fval);
    else data_addr(sym_data(ld_const(n->fval)), 0);
    break;
  case N_VAR: case N_MEMBER: case N_DEREF: {
    if (n->ty->kind == TY_FUNC) {
      if (n->kind == N_VAR) func_addr(sym_func(n->var));
      else gen_expr(n->lhs);
      break;
    }
    if (is_reg(n)) { lget(n->var->local); break; }
    if (n->kind == N_MEMBER && n->mem->bitwidth) { gen_bf_load(n); break; }
    if (is_memclass(n->ty)) { gen_addr(n); break; }
    u32 off;
    gen_addr_off(n, &off);
    load(n->ty, off);
    break;
  }
  case N_INITVAR:
    gen_local_init(n->var, n->initz);
    if (is_memclass(n->ty)) { Node v = *n; v.kind = N_VAR; gen_addr(&v); }
    else { Node v = *n; v.kind = N_VAR; gen_expr(&v); }
    break;
  case N_ADDR: gen_addr(n->lhs); break;
  case N_CAST:
    if (n->ty->kind == TY_VOID) { gen_void(n->lhs); break; }
    if (is_integer(n->ty) && is_integer(n->lhs->ty) && n->lhs->ty->size < 4 && n->ty->size == n->lhs->ty->size &&
        n->ty->kind != TY_BOOL && n->lhs->ty->kind != TY_BOOL &&
        (n->lhs->kind == N_DEREF || (n->lhs->kind == N_VAR && !is_reg(n->lhs)) || (n->lhs->kind == N_MEMBER && !n->lhs->mem->bitwidth))) {
      u32 off;
      gen_addr_off(n->lhs, &off);
      load(n->ty, off);
      break;
    }
    gen_expr(n->lhs);
    convert(n->lhs->ty, n->ty);
    break;
  case N_ASSIGN: gen_assign(n, 1); break;
  case N_OPASSIGN: gen_opassign(n, 1, 0); break;
  case N_PREINC: gen_opassign(n, 1, 0); break;
  case N_POSTINC: gen_opassign(n, 1, 1); break;
  case N_COMMA: gen_void(n->lhs); gen_expr(n->rhs); break;
  case N_CALL: gen_call(n, 1); break;
  case N_COND: {
    gen_cond(n->cond);
    int w = is_memclass(n->ty) ? WI32 : wt(n->ty);
    if_(w);
    if (w) gen_expr(n->then); else gen_void(n->then);
    else_();
    if (w) gen_expr(n->els); else gen_void(n->els);
    end();
    break;
  }
  case N_LAND: case N_LOR:
    gen_cond(n->lhs);
    if (n->kind == N_LOR) op(0x45);
    if_(WI32);
    if (n->kind == N_LAND) { gen_bool(n->rhs); else_(); i32c(0); }
    else { gen_bool(n->rhs); else_(); i32c(1); }
    end();
    break;
  case N_NOT:
    gen_cond(n->lhs);
    op(0x45);
    break;
  case N_NEG:
    if (is_flonum(n->ty)) {
      if (n->ty->kind == TY_LDOUBLE) error_at(n->tok, "long double arithmetic is not supported");
      gen_expr(n->lhs);
      op(n->ty->kind == TY_FLOAT ? 0x8c : 0x9a);
    } else {
      zero_of(wt(n->ty));
      gen_expr(n->lhs);
      binop(N_SUB, n->ty);
    }
    break;
  case N_BNOT:
    gen_expr(n->lhs);
    if (wt(n->ty) == WI64) { i64c(-1); op(0x85); } else { i32c(-1); op(0x73); }
    break;
  case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD: case N_SHL: case N_SHR:
  case N_BAND: case N_BOR: case N_BXOR: case N_EQ: case N_NE: case N_LT: case N_LE: {
    Type *ot = n->lhs->ty;
    if (ot->kind == TY_LDOUBLE) { gen_ld_binop(n); break; }
    /* multiply by a power of two: shift; unsigned divide and remainder too */
    if (is_integer(ot) && n->rhs->kind == N_NUM && n->rhs->ival > 0 && !(n->rhs->ival & (n->rhs->ival - 1)) &&
        (n->kind == N_MUL || (ot->is_unsigned && (n->kind == N_DIV || n->kind == N_MOD)))) {
      int sh = 0;
      while ((1LL << sh) < n->rhs->ival) sh++;
      int w64 = wt(ot) == WI64;
      gen_expr(n->lhs);
      if (n->kind == N_MOD) { if (w64) { i64c(n->rhs->ival - 1); op(0x83); } else { i32c((int32_t)(n->rhs->ival - 1)); op(0x71); } }
      else if (sh) {
        if (w64) i64c(sh); else i32c(sh);
        op(n->kind == N_MUL ? (w64 ? 0x86 : 0x74) : (w64 ? 0x88 : 0x76));
      }
      break;
    }
    gen_expr(n->lhs);
    gen_expr(n->rhs);
    binop(n->kind, (n->kind == N_ADD || n->kind == N_SUB) && is_ptr(n->ty) ? ty_ulong : ot);
    break;
  }
  case N_STMTEXPR: {
    Node *s = n->body->body;
    for (; s && s->next; s = s->next) gen_stmt(s);
    if (s && s->kind == N_EXPR && n->ty->kind != TY_VOID) gen_expr(s->lhs);
    else if (s) gen_stmt(s);
    break;
  }
  case N_BUILTIN: gen_builtin(n); break;
  case N_VAARG: gen_vaarg(n); break;
  case N_VLASIZE:
    if (n->var && n->var->ty->vla_size) lget(n->var->ty->vla_size->local);
    else error_at(n->tok, "sizeof of this variable length array type is not supported");
    break;
  default:
    error_at(n->tok, "internal error: cannot generate expression %d", n->kind);
  }
  expr_depth--;
}

static void gen_void(Node *n) {
  switch (n->kind) {
  case N_ASSIGN: expr_depth++; gen_assign(n, 0); expr_depth--; return;
  case N_OPASSIGN: case N_PREINC: case N_POSTINC: expr_depth++; gen_opassign(n, 0, 0); expr_depth--; return;
  case N_CALL: expr_depth++; gen_call(n, 0); expr_depth--; return;
  case N_COMMA: gen_void(n->lhs); gen_void(n->rhs); return;
  case N_CAST: if (n->ty->kind == TY_VOID || n->lhs->ty->kind != TY_LDOUBLE) { gen_void(n->lhs); return; } break;
  case N_NUM: case N_FNUM: return;
  case N_VAR: if (!(n->ty->qual & Q_VOLATILE)) return; break;
  case N_COND:
    gen_cond(n->cond);
    if_(0);
    gen_void(n->then);
    else_();
    gen_void(n->els);
    end();
    return;
  case N_LAND: case N_LOR:
    gen_cond(n->lhs);
    if (n->kind == N_LOR) op(0x45);
    if_(0);
    gen_void(n->rhs);
    end();
    return;
  case N_STMTEXPR: {
    for (Node *s = n->body->body; s; s = s->next) gen_stmt(s);
    return;
  }
  case N_INITVAR: gen_local_init(n->var, n->initz); return;
  }
  gen_expr(n);
  if (n->ty->kind != TY_VOID) drop();
}

/* ---- local initialisers ---- */
static void init_leaves(Var *v, Init *in, int off);

static void var_base(Var *v) {
  if (v->in_mem == 2) lget(v->local);
  else lget(get_fp());
}
static int var_off(Var *v) { return v->in_mem == 2 ? 0 : v->frame_off; }

static void init_store(Var *v, int off, Type *ty, Node *e, Member *bf) {
  if (bf) {
    int w = wt(bf->ty), at = tmp_get(WI32), vt = tmp_get(w);
    var_base(v);
    lset(at);
    gen_expr(e);
    lset(vt);
    gen_bf_store_at(bf, var_off(v) + off, at, vt);
    tmp_put(WI32, at);
    tmp_put(w, vt);
    return;
  }
  if (is_memclass(ty)) {
    var_base(v);
    if (var_off(v) + off) { i32c(var_off(v) + off); op(0x6a); }
    if (ty->kind == TY_ARRAY && e->kind == N_VAR && e->var->data) {
      /* char array from a string literal */
      int n = e->var->datalen < ty->size || ty->len < 0 ? e->var->datalen : ty->size;
      gen_addr(e);
      gen_copy(n);
      return;
    }
    gen_expr(e);
    gen_copy(ty->size);
    return;
  }
  var_base(v);
  gen_expr(e);
  store(ty, var_off(v) + off);
}

static void init_leaves(Var *v, Init *in, int off) {
  Type *ty = in->ty;
  if (in->expr) { init_store(v, off, ty, in->expr, NULL); return; }
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < in->nkids && (ty->len < 0 || i < ty->len); i++)
      if (in->kids[i]) init_leaves(v, in->kids[i], off + i * ty->base->size);
    return;
  }
  if (is_aggr(ty)) {
    for (int i = 0; i < in->nkids; i++) {
      Init *k = in->kids[i];
      if (!k) continue;
      Member *m = ty->rec->marr[i];
      if (m->bitwidth && k->expr) init_store(v, off + m->offset, m->ty, k->expr, m);
      else init_leaves(v, k, off + m->offset);
    }
  }
}

static int init_is_full(Init *in) {
  /* true when every byte is written by the initializer itself */
  if (in->expr) {
    if (in->ty->kind == TY_ARRAY && in->expr->kind == N_VAR && in->expr->var->data) return in->expr->var->datalen >= in->ty->size;
    return 1;
  }
  return 0;
}

void gen_local_init(Var *v, Init *in) {
  if (!in) return;
  if (!v->in_mem) {
    if (in->expr) { gen_expr(in->expr); lset(v->local); }
    else { zero_of(wt(v->ty)); lset(v->local); }
    return;
  }
  if (is_scalar(v->ty) && v->ty->kind != TY_LDOUBLE) {
    var_base(v);
    if (in->expr) gen_expr(in->expr);
    else zero_of(wt(v->ty));
    store(v->ty, var_off(v));
    return;
  }
  if (!init_is_full(in)) {
    var_base(v);
    if (var_off(v)) { i32c(var_off(v)); op(0x6a); }
    i32c(0);
    i32c(v->ty->size);
    mem_fill();
  }
  init_leaves(v, in, 0);
}

/* ---- statements ---- */
static void jump_state(int s) {
  i32c(s);
  lset(st_local);
  br(dispatch_blk);
}
static int start_state(void) {
  end();
  return ++cur_state;
}

static void gen_return(Node *s);

static void stmt_or_flat(Node *s);

static void gen_switch(Node *s);
static void flat_switch(Node *s);

static int mark(Node *s);
static int uses_continue(Node *s);
static int simple_cond(Node *n);

static void gen_stmt(Node *s) {
  if (!s) return;
  tick();
  if (!expr_depth) scratch = 0;
  switch (s->kind) {
  case N_NULL: return;
  case N_EXPR: gen_void(s->lhs); return;
  case N_BLOCK:
    for (Node *c = s->body; c; c = c->next) gen_stmt(c);
    return;
  case N_DECL: gen_local_init(s->var, s->initz); return;
  case N_VLADECL: {
    Var *v = s->var;
    Type *ty = v->ty;
    /* size = len * element size, computed now (C11 6.7.6.2p5) */
    Var *sz = xalloc(sizeof *sz);
    sz->is_local = 1;
    sz->local = new_local(WI32);
    ty->vla_size = sz;
    gen_expr(ty->vla_len);
    convert(ty->vla_len->ty, ty_ulong);
    Type *b = ty->base;
    if (b->kind == TY_ARRAY && b->vla_len) error_at(s->tok, "multidimensional variable length arrays are not supported");
    if (b->size != 1) { i32c(b->size); op(0x6c); }
    lset(sz->local);
    get_fp();
    gget_sp();
    lget(sz->local);
    i32c(15); op(0x6a); i32c(-16); op(0x71);
    op(0x6b);
    ltee(v->local);
    gset_sp();
    return;
  }
  case N_IF: {
    if (s->cond->kind == N_NUM) {
      if (s->cond->ival) gen_stmt(s->then);
      else gen_stmt(s->els);
      return;
    }
    gen_cond(s->cond);
    if_(0);
    gen_stmt(s->then);
    if (s->els) { else_(); gen_stmt(s->els); }
    end();
    return;
  }
  case N_WHILE: {
    int sb = brk_blk, ss = brk_st, cb = cont_blk, cs = cont_st;
    int forever = s->cond->kind == N_NUM && s->cond->ival;
    block(0);
    brk_blk = depth; brk_st = -1;
    if (!forever && simple_cond(s->cond)) {
      /* rotated: test once before the loop, then at the bottom */
      gen_cond_not(s->cond);
      br_if(brk_blk);
      loop(0);
      int top = depth;
      int has_cont = uses_continue(s->body);
      if (has_cont) block(0);
      cont_blk = depth; cont_st = -1;
      gen_stmt(s->body);
      if (has_cont) end();
      scratch = 0;
      gen_cond(s->cond);
      br_if(top);
      end();
      end();
      brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
      return;
    }
    loop(0);
    cont_blk = depth; cont_st = -1;
    if (!forever) {
      gen_cond_not(s->cond);
      br_if(brk_blk);
    }
    gen_stmt(s->body);
    br(cont_blk);
    end();
    end();
    brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
    return;
  }
  case N_DO: {
    int sb = brk_blk, ss = brk_st, cb = cont_blk, cs = cont_st;
    block(0);
    brk_blk = depth; brk_st = -1;
    loop(0);
    int top = depth;
    block(0);
    cont_blk = depth; cont_st = -1;
    gen_stmt(s->body);
    end();
    scratch = 0;
    if (s->cond->kind == N_NUM) { if (s->cond->ival) br(top); }
    else { gen_cond(s->cond); br_if(top); }
    end();
    end();
    brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
    return;
  }
  case N_FOR: {
    int sb = brk_blk, ss = brk_st, cb = cont_blk, cs = cont_st;
    if (s->init) gen_stmt(s->init);
    block(0);
    brk_blk = depth; brk_st = -1;
    int has_cond = s->cond && !(s->cond->kind == N_NUM && s->cond->ival);
    int rotate = has_cond && simple_cond(s->cond);
    int has_cont = uses_continue(s->body);
    scratch = 0;
    if (rotate) { gen_cond_not(s->cond); br_if(brk_blk); }
    loop(0);
    int top = depth;
    scratch = 0;
    if (has_cond && !rotate) {
      gen_cond_not(s->cond);
      br_if(brk_blk);
    }
    if (has_cont) block(0);
    cont_blk = depth; cont_st = -1;
    gen_stmt(s->body);
    if (has_cont) end();
    scratch = 0;
    if (s->inc) gen_void(s->inc);
    if (rotate) { scratch = 0; gen_cond(s->cond); br_if(top); }
    else br(top);
    end();
    end();
    brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
    return;
  }
  case N_SWITCH: gen_switch(s); return;
  case N_BREAK:
    if (brk_st >= 0) jump_state(brk_st);
    else br(brk_blk);
    return;
  case N_CONTINUE:
    if (cont_st >= 0) jump_state(cont_st);
    else br(cont_blk);
    return;
  case N_GOTO: jump_state(s->label->id); return;
  case N_RETURN: gen_return(s); return;
  case N_LABEL: case N_CASE:
    /* only reached for an unused label, or a case of a regular switch handled there */
    gen_stmt(s->body);
    return;
  }
  error_at(s->tok, "internal error: statement %d", s->kind);
}

/* does a loop body contain a continue for this loop (not for a nested one)? */
static int uses_continue(Node *s) {
  if (!s) return 0;
  switch (s->kind) {
  case N_CONTINUE: return 1;
  case N_BLOCK: for (Node *c = s->body; c; c = c->next) if (uses_continue(c)) return 1; return 0;
  case N_IF: return uses_continue(s->then) || uses_continue(s->els);
  case N_SWITCH: return uses_continue(s->body);
  case N_CASE: case N_LABEL: return uses_continue(s->body);
  case N_EXPR: return s->lhs->kind == N_STMTEXPR; /* be safe */
  }
  return 0; /* loops: their continues are their own */
}

/* small enough and free of side effects to be evaluated at both ends of a loop */
static int cond_size(Node *n, int budget) {
  if (!n || budget <= 0) return budget;
  switch (n->kind) {
  case N_CALL: case N_STMTEXPR: case N_ASSIGN: case N_OPASSIGN: case N_PREINC: case N_POSTINC: case N_INITVAR:
  case N_BUILTIN: case N_VAARG: case N_COND: case N_LAND: case N_LOR:
    return -1;
  }
  budget--;
  budget = cond_size(n->lhs, budget);
  if (budget < 0) return budget;
  return cond_size(n->rhs, budget);
}
static int simple_cond(Node *n) { return cond_size(n, 12) >= 0; }

static void epilogue(void) {
  if (pass == 2 && p1_need_fp && (p1_calls || fn->has_vla) && (p1_frame || fn->has_vla)) {
    if (fn->has_vla) {
      lget(sp_local);
    } else {
      lget(fp_local);
      i32c(p1_frame);
      op(0x6a);
    }
    gset_sp();
  }
}

static void gen_return(Node *s) {
  Type *rt = fn->ty->base;
  int is_main0 = 0;
  (void)is_main0;
  if (s->lhs && rt->kind != TY_VOID) {
    if (is_sret(rt)) {
      lget(sret_local);
      gen_expr(s->lhs);
      gen_copy(rt->size);
      epilogue();
      op(0x0f);
      return;
    }
    if (is_aggr(rt)) {
      Type *f = struct_field1(rt);
      if (!f) { gen_void(s->lhs); epilogue(); op(0x0f); return; }
      gen_expr(s->lhs);
      load(f, 0);
    } else {
      gen_expr(s->lhs);
    }
    epilogue();
    op(0x0f);
    return;
  }
  if (s->lhs) gen_void(s->lhs);
  if (rt->kind != TY_VOID && !is_sret(rt)) {
    Type *f = is_aggr(rt) ? struct_field1(rt) : rt;
    if (f) zero_of(wt(f));
  }
  epilogue();
  op(0x0f);
}

/* A switch whose case labels are all at the top level of its body becomes nested
 * blocks entered through br_table (dense values) or a chain of br_if (sparse). */
static int case_chain_len(Node *st, Node *sw, Node **after) {
  int n = 0;
  while (st && st->kind == N_CASE && st->cond == sw) { n++; st = st->body; }
  *after = st;
  return n;
}

static int switch_irregular(Node *sw) {
  int total = sw->defcase ? 1 : 0;
  for (Node *c = sw->cases; c; c = c->els) total++;
  int top = 0;
  Node *b = sw->body;
  Node *after;
  if (b->kind == N_BLOCK) {
    for (Node *st = b->body; st; st = st->next) top += case_chain_len(st, sw, &after);
  } else {
    top += case_chain_len(b, sw, &after);
  }
  return top != total;
}

static void gen_switch(Node *s) {
  int sb = brk_blk, ss = brk_st;
  Node *body = s->body;
  /* segments: top-level statements, each case chain starts one */
  Vec stmts = {0};
  if (body->kind == N_BLOCK) for (Node *st = body->body; st; st = st->next) vec_push(&stmts, st);
  else vec_push(&stmts, body);
  int nseg = 0;
  Node **segcase = xalloc((stmts.len + 1) * sizeof *segcase);
  for (int i = 0; i < stmts.len; i++) {
    Node *st = stmts.p[i];
    if (st->kind == N_CASE && st->cond == s) segcase[nseg++] = st;
  }
  Type *ct = s->cond->ty;
  int w = wt(ct);
  int t = tmp_get(w);
  gen_expr(s->cond);
  lset(t);
  block(0); /* exit */
  int exit_blk = depth;
  brk_blk = exit_blk;
  brk_st = -1;
  for (int i = nseg - 1; i >= 0; i--) block(0);
  /* the block for segment j is exit_blk + nseg - j... computed from depth below */
  int b0 = depth; /* innermost: segment 0 starts after its end */
  /* collect values */
  int ncase = 0;
  i64 lo = INT64_MAX, hi = INT64_MIN;
  int has_range = 0;
  for (Node *c = s->cases; c; c = c->els) {
    ncase++;
    if (c->ival != c->ival2) has_range = 1;
    if (c->ival < lo) lo = c->ival;
    if (c->ival2 > hi) hi = c->ival2;
  }
  /* segment index of each case */
  int defseg = -1;
  for (int j = 0; j < nseg; j++) {
    Node *after;
    for (Node *c = segcase[j]; c && c->kind == N_CASE && c->cond == s; c = c->body) {
      c->s1 = j;
      if (c->flag) defseg = j;
    }
    (void)after;
  }
  int deftarget = defseg >= 0 ? b0 - defseg : exit_blk;
  u64 range = ncase ? (u64)(hi - lo) : 0;
  if (w == WI32 && ncase >= 4 && !has_range && range < 4096 && range <= (u64)ncase * 3 + 8) {
    int n = (int)range + 1;
    int *tgt = xalloc(n * sizeof *tgt);
    for (int i = 0; i < n; i++) tgt[i] = deftarget;
    for (Node *c = s->cases; c; c = c->els) tgt[c->ival - lo] = b0 - c->s1;
    lget(t);
    if (lo) { i32c((int32_t)lo); op(0x6b); }
    op(0x0e);
    uleb(n);
    for (int i = 0; i < n; i++) uleb(depth - tgt[i]);
    uleb(depth - deftarget);
  } else {
    for (Node *c = s->cases; c; c = c->els) {
      lget(t);
      if (c->ival == c->ival2) {
        if (w == WI64) { i64c(c->ival); op(0x51); }
        else { i32c((int32_t)c->ival); op(0x46); }
      } else {
        if (w == WI64) { i64c(c->ival); op(0x7d); i64c(c->ival2 - c->ival); op(0x58); }
        else { i32c((int32_t)c->ival); op(0x6b); i32c((int32_t)(c->ival2 - c->ival)); op(0x4d); }
      }
      br_if(b0 - c->s1);
    }
    br(deftarget);
  }
  tmp_put(w, t);
  /* statements before the first case are unreachable but may declare variables */
  int seg = -1;
  for (int i = 0; i < stmts.len; i++) {
    Node *st = stmts.p[i];
    if (st->kind == N_CASE && st->cond == s) {
      end(); /* close the block of the next segment */
      seg++;
      Node *after;
      case_chain_len(st, s, &after);
      gen_stmt(after);
    } else {
      gen_stmt(st);
    }
  }
  if (seg != nseg - 1) error_at(s->tok, "internal error: switch segments");
  end(); /* exit */
  brk_blk = sb;
  brk_st = ss;
}

/* ---- flattened control flow (goto) ---- */
static int mark(Node *s) {
  if (!s) return 0;
  int t = 0;
  switch (s->kind) {
  case N_LABEL: t = s->label->used | mark(s->body); break;
  case N_CASE: t = mark(s->body) | ((Node *)s->cond)->irregular; break;
  case N_BLOCK: for (Node *c = s->body; c; c = c->next) t |= mark(c); break;
  case N_IF: t = mark(s->then) | mark(s->els); break;
  case N_WHILE: case N_DO: t = mark(s->body); break;
  case N_FOR: t = mark(s->init) | mark(s->body); break;
  case N_SWITCH:
    s->irregular = switch_irregular(s);
    t = mark(s->body);
    if (t && !s->irregular) { s->irregular = 1; mark(s->body); } /* flattened: every case becomes a state */
    break;
  }
  s->tgt = (u8)t;
  return t;
}

static void flatten(Node *s);
static void stmt_or_flat(Node *s) {
  if (s && s->tgt) flatten(s);
  else gen_stmt(s);
}

static void flatten(Node *s) {
  if (!expr_depth) scratch = 0;
  switch (s->kind) {
  case N_BLOCK:
    for (Node *c = s->body; c; c = c->next) stmt_or_flat(c);
    return;
  case N_LABEL: {
    int k = start_state();
    if (pass == 1) s->label->id = k;
    else if (s->label->id != k) error_at(s->tok, "internal error: label state");
    stmt_or_flat(s->body);
    return;
  }
  case N_IF: {
    gen_cond(s->cond);
    op(0x45);
    if_(0);
    jump_state(s->s1);
    end();
    stmt_or_flat(s->then);
    if (s->els) {
      jump_state(s->s2);
      s->s1 = start_state();
      stmt_or_flat(s->els);
      s->s2 = start_state();
    } else {
      s->s1 = start_state();
    }
    return;
  }
  case N_WHILE: {
    int sb = brk_blk, ss = brk_st, cb = cont_blk, cs = cont_st;
    s->s1 = start_state();
    gen_cond(s->cond);
    op(0x45);
    if_(0);
    jump_state(s->s2);
    end();
    brk_blk = -1; brk_st = s->s2; cont_blk = -1; cont_st = s->s1;
    stmt_or_flat(s->body);
    jump_state(s->s1);
    s->s2 = start_state();
    brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
    return;
  }
  case N_DO: {
    int sb = brk_blk, ss = brk_st, cb = cont_blk, cs = cont_st;
    s->s1 = start_state();
    brk_blk = -1; brk_st = s->s3; cont_blk = -1; cont_st = s->s2;
    stmt_or_flat(s->body);
    s->s2 = start_state();
    scratch = 0;
    gen_cond(s->cond);
    if_(0);
    jump_state(s->s1);
    end();
    s->s3 = start_state();
    brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
    return;
  }
  case N_FOR: {
    int sb = brk_blk, ss = brk_st, cb = cont_blk, cs = cont_st;
    if (s->init) stmt_or_flat(s->init);
    s->s1 = start_state();
    if (s->cond) {
      gen_cond(s->cond);
      op(0x45);
      if_(0);
      jump_state(s->s3);
      end();
    }
    brk_blk = -1; brk_st = s->s3; cont_blk = -1; cont_st = s->s2;
    stmt_or_flat(s->body);
    s->s2 = start_state();
    scratch = 0;
    if (s->inc) gen_void(s->inc);
    jump_state(s->s1);
    s->s3 = start_state();
    brk_blk = sb; brk_st = ss; cont_blk = cb; cont_st = cs;
    return;
  }
  case N_SWITCH: flat_switch(s); return;
  case N_CASE: {
    int k = start_state();
    s->s1 = k;
    stmt_or_flat(s->body);
    return;
  }
  }
  error_at(s->tok, "internal error: flatten %d", s->kind);
}

static void flat_switch(Node *s) {
  int sb = brk_blk, ss = brk_st;
  Type *ct = s->cond->ty;
  int w = wt(ct), t = tmp_get(w);
  gen_expr(s->cond);
  lset(t);
  for (Node *c = s->cases; c; c = c->els) {
    lget(t);
    if (c->ival == c->ival2) {
      if (w == WI64) { i64c(c->ival); op(0x51); } else { i32c((int32_t)c->ival); op(0x46); }
    } else {
      if (w == WI64) { i64c(c->ival); op(0x7d); i64c(c->ival2 - c->ival); op(0x58); }
      else { i32c((int32_t)c->ival); op(0x6b); i32c((int32_t)(c->ival2 - c->ival)); op(0x4d); }
    }
    if_(0);
    jump_state(c->s1);
    end();
  }
  tmp_put(w, t);
  jump_state(s->defcase ? s->defcase->s1 : s->s2);
  brk_blk = -1;
  brk_st = s->s2;
  /* the body: case labels of this switch become states. Statements without targets
   * that hold case labels of this switch cannot exist here: they would be marked. */
  int k = start_state(); /* code after the dispatch is unreachable: give it a state */
  (void)k;
  stmt_or_flat(s->body);
  s->s2 = start_state();
  brk_blk = sb;
  brk_st = ss;
}

/* ---- functions ---- */
static void assign_frame(void) {
  int off = 0;
  for (int i = 0; i < fn->locals.len; i++) {
    Var *v = fn->locals.p[i];
    v->in_mem = 0;
    if (v->ty->kind == TY_ARRAY && v->ty->vla_len) { v->in_mem = 2; continue; }
    if (v->addr_taken || is_memclass(v->ty) || v->align > v->ty->align) {
      if (v->is_param && is_aggr(v->ty) && !struct_field1(v->ty) && v->ty->size) { v->in_mem = 2; continue; }
      v->in_mem = 1;
      int al = v->align > v->ty->align ? v->align : v->ty->align;
      if (al < 1) al = 1;
      off = (off + al - 1) / al * al;
      v->frame_off = off;
      off += v->ty->size ? v->ty->size : 1;
    }
  }
  frame_locals = (off + 15) & ~15;
}

static Buf *body_buf;
static void gen_function_pass(Var *f, int p) {
  pass = p;
  fn = f;
  static Buf b;
  static Vec r;
  b.len = 0;
  r.len = 0;
  r.p = NULL;
  r.cap = 0;
  code = &b;
  relocs = &r;
  depth = 0;
  ltypes.len = 0;
  for (int i = 0; i < 4; i++) freetmp[i].len = 0;
  fp_local = -1;
  need_fp = 0;
  has_calls = 0;
  scratch = scratch_max = 0;
  expr_depth = 0;
  cur_state = -1;
  brk_blk = brk_st = cont_blk = cont_st = -1;
  assign_frame();
  /* wasm parameters */
  Type *fty = f->ty;
  int wi = 0;
  sret_local = -1;
  if (is_sret(fty->base)) sret_local = wi++;
  for (int i = 0; i < f->params.len; i++) {
    Var *v = f->params.p[i];
    v->param_idx = wi;
    Type *t = v->ty;
    if (t->kind == TY_LDOUBLE) wi += 2;
    else if (is_aggr(t)) { if (struct_field1(t) || t->size) wi++; }
    else wi++;
  }
  va_local = fty->variadic ? wi++ : -1;
  nparams_w = wi;
  if (pass == 2 && p1_need_fp) fp_local = new_local(WI32);
  if (f->has_vla) sp_local = new_local(WI32);
  /* locals in wasm locals */
  for (int i = 0; i < f->locals.len; i++) {
    Var *v = f->locals.p[i];
    if (v->in_mem == 1) continue;
    if (v->in_mem == 2) {
      if (v->is_param) v->local = v->param_idx;
      else v->local = new_local(WI32);
      continue;
    }
    if (v->is_param) { v->local = v->param_idx; continue; }
    v->local = new_local(wt(v->ty));
  }
  /* body */
  int flat = mark(f->body);
  if (flat) {
    st_local = new_local(WI32);
    nstates = pass == 2 ? p1_nstates : 1;
    loop(0);
    dispatch_blk = depth;
    for (int i = 0; i < nstates; i++) block(0);
    lget(st_local);
    op(0x0e);
    uleb(nstates);
    for (int i = 0; i < nstates; i++) uleb(i);
    uleb(0);
    start_state();
    flatten(f->body);
    while (depth > dispatch_blk) end();
    end(); /* loop */
  } else {
    gen_stmt(f->body);
  }
  /* falling off the end (not needed after a final return statement) */
  Node *last = f->body->body;
  while (last && last->next) last = last->next;
  if (!flat && last && last->kind == N_RETURN) goto done_ret;
  Node ret = {0};
  ret.kind = N_RETURN;
  ret.tok = f->tok;
  if (!strcmp(f->name->name, "main") && fty->base->kind == TY_INT) {
    Node zero = {0};
    zero.kind = N_NUM;
    zero.ty = ty_int;
    ret.lhs = &zero;
    gen_return(&ret);
  } else {
    gen_return(&ret);
  }
done_ret:
  if (pass == 1) {
    p1_need_fp = need_fp || f->has_vla;
    p1_frame = (frame_locals + scratch_max + 15) & ~15;
    p1_calls = has_calls;
    p1_nstates = cur_state + 1;
    return;
  }
  /* pass 2: prologue + locals header */
  Buf out = {0};
  /* locals: run-length by type */
  Buf lh = {0};
  int runs = 0;
  for (int i = 0; i < ltypes.len;) {
    int j = i;
    while (j < ltypes.len && ltypes.p[j] == ltypes.p[i]) j++;
    runs++;
    buf_uleb(&lh, j - i);
    buf_byte(&lh, ltypes.p[i]);
    i = j;
  }
  buf_uleb(&out, runs);
  buf_mem(&out, lh.p, lh.len);
  Buf pro = {0};
  Vec prorel = {0};
  code = &pro;
  relocs = &prorel;
  if (p1_need_fp) {
    gget_sp();
    if (f->has_vla) { ltee(sp_local); }
    if (p1_frame) { i32c(p1_frame); op(0x6b); }
    if ((p1_calls || f->has_vla) && p1_frame) { ltee(fp_local); gset_sp(); }
    else lset(fp_local);
    if (f->has_vla && !p1_frame) { /* fp = sp, nothing to move */ }
  }
  /* parameters that live in memory */
  for (int i = 0; i < f->params.len; i++) {
    Var *v = f->params.p[i];
    if (v->in_mem != 1) continue;
    Type *t = v->ty;
    if (t->kind == TY_LDOUBLE) {
      lget(fp_local); lget(v->param_idx); op(0x37); memarg(3, v->frame_off);
      lget(fp_local); lget(v->param_idx + 1); op(0x37); memarg(3, v->frame_off + 8);
      continue;
    }
    if (is_aggr(t)) {
      Type *fl = struct_field1(t);
      if (fl) { lget(fp_local); lget(v->param_idx); store(fl, v->frame_off); }
      continue;
    }
    lget(fp_local);
    lget(v->param_idx);
    store(t, v->frame_off);
  }
  int shift = out.len + pro.len;
  buf_mem(&out, pro.p, pro.len);
  Sym *s = f->sym;
  for (int i = 0; i < prorel.len; i++) {
    Reloc *rl = prorel.p[i];
    rl->off += out.len - pro.len;
    vec_push(&s->relocs, rl);
  }
  for (int i = 0; i < r.len; i++) {
    Reloc *rl = r.p[i];
    rl->off += shift;
    vec_push(&s->relocs, rl);
  }
  buf_mem(&out, b.p, b.len);
  buf_byte(&out, 0x0b);
  s->code = out;
  s->defined = 1;
  (void)body_buf;
}

static void gen_function(Var *f) {
  Sym *s = sym_func(f);
  (void)s;
  gen_function_pass(f, 1);
  gen_function_pass(f, 2);
}

/* int main(void) is emitted as __original_main plus a main(argc, argv) that calls
 * it, as clang does, so crt0's call to main(int, char **) matches. */
static void gen_main_wrapper(Var *orig) {
  Var *w = xalloc(sizeof *w);
  w->name = intern("main", 4);
  Type *ft = func_type(ty_int);
  ft->proto = 1;
  ft->nparams = 2;
  ft->params = xalloc(2 * sizeof(Param));
  ft->params[0].ty = ty_int;
  ft->params[1].ty = ptr_to(ptr_to(ty_char));
  w->ty = ft;
  w->is_func = w->is_def = 1;
  w->tok = orig->tok;
  Sym *s = sym_func(w);
  Buf out = {0};
  buf_uleb(&out, 0);
  code = &out;
  relocs = &s->relocs;
  call_sym(sym_func(orig));
  buf_byte(&out, 0x0b);
  s->code = out;
  s->defined = 1;
}

static Vec queue;
static void run_queue(void) {
  for (int i = 0; i < queue.len; i++) {
    Var *v = queue.p[i];
    if (v->emitted) continue;
    v->emitted = 1;
    int main0 = !strcmp(v->name->name, "main") && !v->is_static && v->ty->nparams == 0;
    if (main0) v->asmname = "__original_main";
    gen_function(v);
    if (main0) gen_main_wrapper(v);
    /* static and inline functions referenced from this one */
    for (int j = 0; j < globals.len; j++) {
      Var *g = globals.p[j];
      if (g->is_func && g->is_def && !g->emitted && g->sym && g->sym->var == g) vec_push(&queue, g);
    }
  }
}

void gen_late(Var *f) {
  vec_push(&queue, f);
  run_queue();
}

void gen_program(void) {
  for (int i = 0; i < globals.len; i++) {
    Var *v = globals.p[i];
    if (!v->is_func || !v->is_def) continue;
    int local_only = v->is_static || (v->is_inline && !v->has_extern_decl);
    if (!local_only || v->used_attr) vec_push(&queue, v);
  }
  run_queue();
}
