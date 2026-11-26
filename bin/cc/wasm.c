// SPDX-License-Identifier: GPL-2.0-or-later
/* The relocatable object file: a WebAssembly module plus the "linking" and "reloc.*"
 * custom sections described in the tool-conventions document Linking.md. wasm-ld and
 * our ld read it. Index fields that a linker rewrites are padded 5-byte LEBs. */
#include "cc.h"

static Vec syms;     /* Sym*, in creation order: this is the symbol table order */
static Vec types;    /* Buf* holding one encoded function type each */
static Sym *sp_sym, *table_sym;
static Map func_by_name;

int wasm_functype(const u8 *sig, int np, int nr) {
  Buf b = {0};
  buf_byte(&b, 0x60);
  buf_uleb(&b, np);
  buf_mem(&b, sig, np);
  buf_uleb(&b, nr);
  buf_mem(&b, sig + np, nr);
  for (int i = 0; i < types.len; i++) {
    Buf *t = types.p[i];
    if (t->len == b.len && !memcmp(t->p, b.p, b.len)) return i;
  }
  Buf *t = xalloc(sizeof *t);
  *t = b;
  vec_push(&types, t);
  return types.len - 1;
}

static Sym *new_sym(int kind, const char *name) {
  Sym *s = xalloc(sizeof *s);
  s->kind = kind;
  s->name = name;
  vec_push(&syms, s);
  return s;
}

static const char *var_sym_name(Var *v) { return v->asmname ? v->asmname : v->name->name; }

Sym *sym_func(Var *v) {
  if (v->sym) return v->sym;
  const char *name = var_sym_name(v);
  Sym *s = map_get(&func_by_name, name);
  if (s && !(s->flags & SF_LOCAL)) { v->sym = s; s->var = v; return s; }
  s = new_sym(SYM_FUNC, name);
  s->var = v;
  u8 sig[512];
  int np, nr;
  func_sig(v->ty, sig, &np, &nr);
  s->type = wasm_functype(sig, np, nr);
  int local = v->is_static || (v->is_inline && !v->has_extern_decl && v->is_def);
  if (local) s->flags |= SF_LOCAL;
  else if (!v->vis_default) s->flags |= SF_HIDDEN;
  if (v->weak) s->flags |= SF_WEAK;
  s->imp_module = v->import_module;
  s->imp_name = v->import_name;
  if (v->export_name) { s->exp_name = v->export_name; s->flags |= SF_EXPORTED | SF_NOSTRIP; }
  if (v->used_attr) s->flags |= SF_NOSTRIP;
  v->sym = s;
  if (!local) map_put(&func_by_name, name, s);
  return s;
}

Sym *sym_extern_func(const char *name, const u8 *sig, int np, int nr) {
  Sym *s = map_get(&func_by_name, name);
  if (s) return s;
  s = new_sym(SYM_FUNC, name);
  s->type = wasm_functype(sig, np, nr);
  s->flags = SF_HIDDEN;
  map_put(&func_by_name, name, s);
  return s;
}

static int is_const_data(Var *v) {
  if (v->is_const) return 1;
  Type *t = v->ty;
  while (t->kind == TY_ARRAY) t = t->base;
  return (t->qual & Q_CONST) && !(v->ty->qual & Q_VOLATILE);
}

Sym *sym_data(Var *v) {
  if (v->sym) return v->sym;
  Sym *s = new_sym(SYM_DATA, var_sym_name(v));
  s->var = v;
  if (v->is_static) s->flags |= SF_LOCAL;
  else if (!v->vis_default) s->flags |= SF_HIDDEN;
  if (v->weak) s->flags |= SF_WEAK;
  if (v->used_attr) s->flags |= SF_NOSTRIP;
  s->defined = v->is_def || v->is_tentative;
  v->sym = s;
  return s;
}

Sym *sym_stack_pointer(void) {
  if (!sp_sym) {
    sp_sym = new_sym(SYM_GLOBAL, "__stack_pointer");
    sp_sym->flags = SF_UNDEF;
  }
  return sp_sym;
}
Sym *sym_table(void) {
  if (!table_sym) {
    table_sym = new_sym(SYM_TABLE, "__indirect_function_table");
    table_sym->flags = SF_UNDEF | SF_NOSTRIP;
  }
  return table_sym;
}

static void section(Buf *out, int id, Buf *body) {
  buf_byte(out, id);
  buf_uleb(out, body->len);
  buf_mem(out, body->p, body->len);
}
static void custom(Buf *out, const char *name, Buf *body) {
  Buf b = {0};
  buf_name(&b, name);
  buf_mem(&b, body->p, body->len);
  section(out, 0, &b);
}
static int uleb_len(u64 v) {
  int n = 0;
  do { n++; v >>= 7; } while (v);
  return n;
}

static int has_addend(int type) {
  return type == R_MEMORY_ADDR_LEB || type == R_MEMORY_ADDR_SLEB || type == R_MEMORY_ADDR_I32 || type == 8 || type == 9;
}

void wasm_write(const char *path) {
  /* every defined global variable gets a symbol; static ones only if something uses them */
  for (int i = 0; i < globals.len; i++) {
    Var *v = globals.p[i];
    if (v->is_func || !(v->is_def || v->is_tentative)) continue;
    if (v->is_static && !v->sym && !v->used_attr) continue;
    sym_data(v);
  }
  /* data relocations name Vars; turn them into Syms (this can reach more statics) */
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind != SYM_DATA || !s->defined) continue;
    Var *v = s->var;
    for (int j = 0; j < v->relocs.len; j++) {
      Reloc *r = v->relocs.p[j];
      Var *t = (Var *)r->sym;
      if (r->type == R_TABLE_INDEX_I32) {
        r->sym = sym_func(t);
        if (t->is_def && !t->emitted) {
          /* a static function only referenced from data: generate it now */
          extern void gen_late(Var *f);
          gen_late(t);
        }
      } else {
        r->sym = sym_data(t);
      }
    }
  }
  /* function indices: imports first */
  int nimport = 0, nfunc = 0;
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind != SYM_FUNC) continue;
    if (!s->defined) {
      if (s->flags & SF_LOCAL) error_at(s->var ? s->var->tok : NULL, "function '%s' is used but never defined", s->name);
      s->flags |= SF_UNDEF;
      s->flags &= ~SF_HIDDEN;
      s->fidx = nimport++;
    }
  }
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind == SYM_FUNC && s->defined) { s->fidx = nimport + nfunc++; }
  }
  /* data layout */
  int nseg = 0;
  u32 addr = 0;
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind != SYM_DATA) continue;
    if (!s->defined) { s->flags |= SF_UNDEF; s->flags &= ~SF_HIDDEN; continue; }
    Var *v = s->var;
    int al = v->align > v->ty->align ? v->align : v->ty->align;
    if (al < 1) al = 1;
    s->align = al;
    s->size = v->datalen > v->ty->size ? v->datalen : v->ty->size;
    addr = (addr + al - 1) / al * al;
    s->addr = addr;
    s->seg = nseg++;
    addr += s->size;
  }
  int k = 0;
  for (int i = 0; i < syms.len; i++) ((Sym *)syms.p[i])->index = k++;

  Buf out = {0};
  buf_mem(&out, "\0asm\1\0\0\0", 8);
  int nsec = 0;
  /* type */
  Buf b = {0};
  buf_uleb(&b, types.len);
  for (int i = 0; i < types.len; i++) { Buf *t = types.p[i]; buf_mem(&b, t->p, t->len); }
  section(&out, 1, &b);
  nsec++;
  /* import */
  b = (Buf){0};
  int nimp = 1 + (sp_sym ? 1 : 0) + nimport + (table_sym ? 1 : 0);
  buf_uleb(&b, nimp);
  buf_name(&b, "env");
  buf_name(&b, "__linear_memory");
  buf_byte(&b, 2);
  buf_byte(&b, 0);
  buf_uleb(&b, (addr + 65535) / 65536);
  if (sp_sym) {
    buf_name(&b, "env");
    buf_name(&b, "__stack_pointer");
    buf_byte(&b, 3);
    buf_byte(&b, 0x7f);
    buf_byte(&b, 1);
  }
  for (int f = 0; f < nimport; f++) {
    for (int i = 0; i < syms.len; i++) {
      Sym *s = syms.p[i];
      if (s->kind != SYM_FUNC || s->defined || s->fidx != f) continue;
      const char *field = s->imp_name ? s->imp_name : s->name;
      buf_name(&b, s->imp_module ? s->imp_module : "env");
      buf_name(&b, field);
      if (strcmp(field, s->name)) s->flags |= SF_EXPLICIT;
      buf_byte(&b, 0);
      buf_uleb(&b, s->type);
    }
  }
  if (table_sym) {
    buf_name(&b, "env");
    buf_name(&b, "__indirect_function_table");
    buf_byte(&b, 1);
    buf_byte(&b, 0x70);
    buf_byte(&b, 0);
    buf_uleb(&b, 0);
  }
  section(&out, 2, &b);
  nsec++;
  /* function */
  b = (Buf){0};
  buf_uleb(&b, nfunc);
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind == SYM_FUNC && s->defined) buf_uleb(&b, s->type);
  }
  section(&out, 3, &b);
  nsec++;
  /* export: functions with export_name */
  b = (Buf){0};
  int nexp = 0;
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind == SYM_FUNC && s->defined && s->exp_name) nexp++;
  }
  if (nexp) {
    buf_uleb(&b, nexp);
    for (int i = 0; i < syms.len; i++) {
      Sym *s = syms.p[i];
      if (s->kind == SYM_FUNC && s->defined && s->exp_name) {
        buf_name(&b, s->exp_name);
        buf_byte(&b, 0);
        buf_uleb(&b, s->fidx);
      }
    }
    section(&out, 7, &b);
    nsec++;
  }
  /* code */
  int code_sec = nsec;
  b = (Buf){0};
  buf_uleb(&b, nfunc);
  Buf rc = {0};
  int nrc = 0;
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    if (s->kind != SYM_FUNC || !s->defined) continue;
    buf_uleb(&b, s->code.len);
    int base = b.len;
    for (int j = 0; j < s->relocs.len; j++) {
      Reloc *r = s->relocs.p[j];
      u8 *p = s->code.p + r->off;
      int idx;
      switch (r->type) {
      case R_FUNCTION_INDEX_LEB: put_uleb5(p, r->sym->fidx); idx = r->sym->index; break;
      case R_TYPE_INDEX_LEB: idx = (int)r->addend; break;
      case R_GLOBAL_INDEX_LEB: case R_TABLE_NUMBER_LEB: idx = r->sym->index; break;
      case R_MEMORY_ADDR_SLEB: put_sleb5(p, (int32_t)(r->sym->addr + r->addend)); idx = r->sym->index; break;
      case R_MEMORY_ADDR_LEB: put_uleb5(p, (u32)(r->sym->addr + r->addend)); idx = r->sym->index; break;
      default: idx = r->sym->index; break;
      }
      buf_byte(&rc, r->type);
      buf_uleb(&rc, base + r->off);
      buf_uleb(&rc, idx);
      if (has_addend(r->type)) buf_sleb(&rc, r->addend);
      nrc++;
    }
    buf_mem(&b, s->code.p, s->code.len);
  }
  section(&out, 10, &b);
  nsec++;
  /* data */
  int data_sec = -1;
  Buf rd = {0};
  int nrd = 0;
  if (nseg) {
    data_sec = nsec;
    b = (Buf){0};
    buf_uleb(&b, nseg);
    for (int i = 0; i < syms.len; i++) {
      Sym *s = syms.p[i];
      if (s->kind != SYM_DATA || !s->defined) continue;
      Var *v = s->var;
      buf_byte(&b, 0);
      buf_byte(&b, 0x41);
      buf_sleb(&b, (int32_t)s->addr);
      buf_byte(&b, 0x0b);
      buf_uleb(&b, s->size);
      int base = b.len;
      if (v->data) {
        u8 *d = xalloc(s->size + 1);
        memcpy(d, v->data, v->datalen < s->size ? v->datalen : s->size);
        for (int j = 0; j < v->relocs.len; j++) {
          Reloc *r = v->relocs.p[j];
          if (r->type == R_MEMORY_ADDR_I32) put_u32(d + r->off, (u32)(r->sym->addr + r->addend));
          buf_byte(&rd, r->type);
          buf_uleb(&rd, base + r->off);
          buf_uleb(&rd, r->sym->index);
          if (has_addend(r->type)) buf_sleb(&rd, r->addend);
          nrd++;
        }
        buf_mem(&b, d, s->size);
      } else {
        for (int j = 0; j < s->size; j++) buf_byte(&b, 0);
      }
    }
    section(&out, 11, &b);
    nsec++;
  }
  /* linking */
  b = (Buf){0};
  buf_uleb(&b, 2);
  Buf st = {0};
  buf_uleb(&st, syms.len);
  for (int i = 0; i < syms.len; i++) {
    Sym *s = syms.p[i];
    buf_byte(&st, s->kind);
    buf_uleb(&st, s->flags);
    switch (s->kind) {
    case SYM_FUNC:
      buf_uleb(&st, s->fidx);
      if (s->defined || (s->flags & SF_EXPLICIT)) buf_name(&st, s->name);
      break;
    case SYM_DATA:
      buf_name(&st, s->name);
      if (s->defined) { buf_uleb(&st, s->seg); buf_uleb(&st, 0); buf_uleb(&st, s->size); }
      break;
    case SYM_GLOBAL: case SYM_TABLE:
      buf_uleb(&st, 0);
      break;
    }
  }
  buf_byte(&b, 8);
  buf_uleb(&b, st.len);
  buf_mem(&b, st.p, st.len);
  if (nseg) {
    Buf si = {0};
    buf_uleb(&si, nseg);
    for (int i = 0; i < syms.len; i++) {
      Sym *s = syms.p[i];
      if (s->kind != SYM_DATA || !s->defined) continue;
      Var *v = s->var;
      int zero = !v->data || !v->relocs.len;
      if (zero && v->data) for (int j = 0; j < v->datalen; j++) if (v->data[j]) { zero = 0; break; }
      const char *prefix = zero && !is_const_data(v) ? ".bss." : is_const_data(v) ? ".rodata." : ".data.";
      if (!v->data) prefix = is_const_data(v) ? ".rodata." : ".bss.";
      buf_name(&si, xfmt("%s%s", prefix, s->name));
      int lg = 0;
      while ((1 << lg) < s->align) lg++;
      buf_uleb(&si, lg);
      buf_uleb(&si, 0);
    }
    buf_byte(&b, 5);
    buf_uleb(&b, si.len);
    buf_mem(&b, si.p, si.len);
  }
  custom(&out, "linking", &b);
  if (nrc) {
    b = (Buf){0};
    buf_uleb(&b, code_sec);
    buf_uleb(&b, nrc);
    buf_mem(&b, rc.p, rc.len);
    custom(&out, "reloc.CODE", &b);
  }
  if (nrd) {
    b = (Buf){0};
    buf_uleb(&b, data_sec);
    buf_uleb(&b, nrd);
    buf_mem(&b, rd.p, rd.len);
    custom(&out, "reloc.DATA", &b);
  }
  /* target features: what our code can contain */
  b = (Buf){0};
  static const char *feats[] = {"bulk-memory", "bulk-memory-opt", "call-indirect-overlong", "mutable-globals",
                                "nontrapping-fptoint", "sign-ext"};
  buf_uleb(&b, 6);
  for (int i = 0; i < 6; i++) { buf_byte(&b, '+'); buf_name(&b, feats[i]); }
  custom(&out, "target_features", &b);
  (void)uleb_len;

  FILE *f = strcmp(path, "-") ? fopen(path, "wb") : stdout;
  if (!f) fatal("cannot write '%s'", path);
  if (fwrite(out.p, 1, out.len, f) != (size_t)out.len) fatal("cannot write '%s'", path);
  if (f != stdout) fclose(f);
}
