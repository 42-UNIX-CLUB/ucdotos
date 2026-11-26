// SPDX-License-Identifier: GPL-2.0-or-later
/* ld: the UC.OS static linker for WebAssembly objects.
 *
 * Reads relocatable objects in the tool-conventions "linking" format (from our cc or
 * from clang) and ar archives of them, resolves symbols, drops what nothing uses,
 * and writes a program the way boot/host.mjs wants one: imports host.syscall and
 * host.block, exports memory and _start, a 1 MiB stack at the bottom of memory
 * (stack first, so an overflow traps), then the data. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;

/* ---- small utilities ---- */
static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("ld: error: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}
static int nerr;
static void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("ld: error: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  if (++nerr > 50) die("too many errors");
}
static void *xmalloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) die("out of memory");
  return p;
}
static void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) die("out of memory");
  return p;
}
static char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  return p;
}
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

typedef struct { void **p; int len, cap; } Vec;
static void push(Vec *v, void *x) {
  if (v->len == v->cap) { v->cap = v->cap ? v->cap * 2 : 16; v->p = xrealloc(v->p, v->cap * sizeof *v->p); }
  v->p[v->len++] = x;
}

typedef struct { u8 *p; size_t len, cap; } Buf;
static void bbyte(Buf *b, int c) {
  if (b->len == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->p = xrealloc(b->p, b->cap); }
  b->p[b->len++] = (u8)c;
}
static void bmem(Buf *b, const void *p, size_t n) {
  if (b->len + n > b->cap) { while (b->len + n > b->cap) b->cap = b->cap ? b->cap * 2 : 256; b->p = xrealloc(b->p, b->cap); }
  memcpy(b->p + b->len, p, n);
  b->len += n;
}
static void buleb(Buf *b, u64 v) { do { u8 c = v & 0x7f; v >>= 7; bbyte(b, v ? c | 0x80 : c); } while (v); }
static void bsleb(Buf *b, i64 v) {
  for (;;) {
    u8 c = v & 0x7f;
    v >>= 7;
    if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { bbyte(b, c); return; }
    bbyte(b, c | 0x80);
  }
}
static void bname(Buf *b, const char *s) { size_t n = strlen(s); buleb(b, n); bmem(b, s, n); }
static void put_uleb5(u8 *p, u32 v) { for (int i = 0; i < 4; i++) { p[i] = (v & 0x7f) | 0x80; v >>= 7; } p[4] = v & 0x7f; }
static void put_sleb5(u8 *p, i32 v) { for (int i = 0; i < 4; i++) { p[i] = (v & 0x7f) | 0x80; v >>= 7; } p[4] = v & 0x7f; }
static void put_u32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* string -> pointer hash map */
typedef struct { const char **k; void **v; int cap, n; } Map;
static unsigned hstr(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (u8)*s++) * 16777619u; return h; }
static void *mget(Map *m, const char *k) {
  if (!m->cap) return NULL;
  for (unsigned i = hstr(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (!m->k[i]) return NULL;
    if (!strcmp(m->k[i], k)) return m->v[i];
  }
}
static void mput(Map *m, const char *k, void *v) {
  if ((m->n + 1) * 2 > m->cap) {
    Map n = {0};
    n.cap = m->cap ? m->cap * 2 : 256;
    n.k = xmalloc(n.cap * sizeof *n.k);
    n.v = xmalloc(n.cap * sizeof *n.v);
    for (int i = 0; i < m->cap; i++) if (m->k[i]) mput(&n, m->k[i], m->v[i]);
    free(m->k);
    free(m->v);
    *m = n;
  }
  for (unsigned i = hstr(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (!m->k[i]) { m->k[i] = k; m->v[i] = v; m->n++; return; }
    if (!strcmp(m->k[i], k)) { m->v[i] = v; return; }
  }
}

/* ---- reading ---- */
typedef struct { const u8 *p, *end; const char *what; } Rd;
static void rcheck(Rd *r, size_t n) { if ((size_t)(r->end - r->p) < n) die("%s: truncated or malformed", r->what); }
static u8 rbyte(Rd *r) { rcheck(r, 1); return *r->p++; }
static u64 ruleb(Rd *r) {
  u64 v = 0;
  int s = 0;
  for (;;) {
    u8 c = rbyte(r);
    if (s < 64) v |= (u64)(c & 0x7f) << s;
    s += 7;
    if (!(c & 0x80)) return v;
  }
}
static i64 rsleb(Rd *r) {
  i64 v = 0;
  int s = 0;
  u8 c;
  do { c = rbyte(r); if (s < 64) v |= (i64)((u64)(c & 0x7f) << s); s += 7; } while (c & 0x80);
  if (s < 64 && (c & 0x40)) v |= (i64)(~0ull << s);
  return v;
}
static char *rname(Rd *r) {
  u64 n = ruleb(r);
  rcheck(r, n);
  char *s = xstrndup((const char *)r->p, n);
  r->p += n;
  return s;
}

/* a constant expression: one instruction and end */
static void skip_constexpr(Rd *r) {
  int op = rbyte(r);
  switch (op) {
  case 0x41: case 0x42: rsleb(r); break;
  case 0x43: rcheck(r, 4); r->p += 4; break;
  case 0x44: rcheck(r, 8); r->p += 8; break;
  case 0x23: ruleb(r); break;
  default: die("%s: unsupported constant expression", r->what);
  }
  if (rbyte(r) != 0x0b) die("%s: unsupported constant expression", r->what);
}

/* ---- objects ---- */
enum { K_FUNC, K_DATA, K_GLOBAL, K_SECTION, K_TAG, K_TABLE };
#define F_WEAK 1
#define F_LOCAL 2
#define F_HIDDEN 4
#define F_UNDEF 0x10
#define F_EXPORTED 0x20
#define F_EXPLICIT 0x40
#define F_NOSTRIP 0x80
#define F_TLS 0x100
#define F_ABS 0x200

typedef struct GSym GSym;
typedef struct Obj Obj;

typedef struct LSym {
  int kind, flags;
  char *name;
  u32 index;           /* function / global / table index in the object */
  u32 seg, off, size;  /* data */
  GSym *g;             /* non-local symbols */
} LSym;

typedef struct Import { char *module, *field; int kind; u32 type; u8 gtype, gmut; } Import;
typedef struct Reloc { u8 type; u32 off, index; i32 addend; } Reloc;
typedef struct Seg {
  char *name;
  u32 align, flags;
  const u8 *data;
  u32 size;
  u32 dataoff;          /* offset of data within the data section payload */
  int live;
  u32 addr;             /* final address */
  Vec relocs;           /* Reloc* (offsets relative to the segment's data) */
} Seg;
typedef struct Func {
  u32 type;             /* object type index */
  const u8 *body;
  u32 size;
  u32 codeoff;          /* offset of the body within the code section payload */
  int live;
  u32 outidx;
  Vec relocs;           /* Reloc* (offsets relative to the body) */
} Func;
typedef struct OGlobal { u8 type, mut; const u8 *init; u32 initlen; int live; u32 outidx; } OGlobal;

struct Obj {
  char *name;
  const u8 *data;
  size_t len;
  Vec types;            /* offsets: u8* to "0x60 ..." entries; lengths in typelen */
  Vec typelen;
  u32 *typemap;         /* object type -> output type */
  Import *imports;
  int nimports, nfimports, ngimports, ntimports;
  Import **fimport, **gimport; /* by index within kind */
  Func *funcs;
  int nfuncs;
  OGlobal *globals;
  int nglobals;
  Seg *segs;
  int nsegs;
  LSym *syms;
  int nsyms;
  Vec exports;          /* char* name, index pairs for functions */
  Vec initfuncs;        /* (priority, symbol) */
  int loaded;
};

struct GSym {
  char *name;
  int kind;
  Obj *def;             /* defining object, NULL if undefined */
  int defsym;           /* index into def->syms */
  int weak_def;
  int referenced, strong_ref;
  Obj *refobj;          /* first object that referenced it */
  u32 reftype;          /* type of the first reference (function), as an output type */
  int has_reftype;
  /* lazy: an archive member defines it */
  struct Member *lazy;
  /* explicit imports */
  char *imp_module, *imp_field;
  int explicit_import;
  /* linker-defined */
  int synthetic;
  u32 value;
  /* output */
  int is_import;
  u32 outidx;           /* function index or global index */
  u32 table_slot;
  int stub;             /* weak undefined function: index of the trapping stub */
};

typedef struct Member { struct Archive *ar; const char *name; size_t off, len; int loaded; } Member;
typedef struct Archive { char *path; u8 *data; size_t len; } Archive;

static Map gsyms;
static Vec objs;      /* Obj* in load order */
static Vec outtypes;  /* Buf* */
static int verbose, gc = 1, allow_undef, strip, whole_archive;
static const char *entry = "_start";
static u32 stack_size = 1048576;
static Vec exports_extra, undef_extra;

static u32 out_type(const u8 *p, u32 len) {
  for (int i = 0; i < outtypes.len; i++) {
    Buf *b = outtypes.p[i];
    if (b->len == len && !memcmp(b->p, p, len)) return i;
  }
  Buf *b = xmalloc(sizeof *b);
  bmem(b, p, len);
  push(&outtypes, b);
  return outtypes.len - 1;
}

static GSym *gsym(const char *name) {
  GSym *g = mget(&gsyms, name);
  if (!g) {
    g = xmalloc(sizeof *g);
    g->name = xstrdup(name);
    g->kind = -1;
    mput(&gsyms, g->name, g);
  }
  return g;
}

static void parse_obj(Obj *o) {
  Rd r = {o->data, o->data + o->len, o->name};
  rcheck(&r, 8);
  if (memcmp(r.p, "\0asm\1\0\0\0", 8)) die("%s: not a WebAssembly object", o->name);
  r.p += 8;
  const u8 *code_start = NULL, *data_start = NULL;
  int secidx = 0, code_sec = -1, data_sec = -1;
  Vec relocsecs = {0};
  const u8 *linking = NULL, *linking_end = NULL;
  while (r.p < r.end) {
    int id = rbyte(&r);
    u64 sz = ruleb(&r);
    rcheck(&r, sz);
    Rd s = {r.p, r.p + sz, o->name};
    r.p += sz;
    switch (id) {
    case 0: {
      char *name = rname(&s);
      if (!strcmp(name, "linking")) { linking = s.p; linking_end = s.end; }
      else if (!strncmp(name, "reloc.", 6)) { Rd *x = xmalloc(sizeof *x); *x = s; push(&relocsecs, x); }
      free(name);
      break;
    }
    case 1: {
      u64 n = ruleb(&s);
      for (u64 i = 0; i < n; i++) {
        const u8 *start = s.p;
        if (rbyte(&s) != 0x60) die("%s: unsupported type form", o->name);
        u64 np = ruleb(&s); rcheck(&s, np); s.p += np;
        u64 nr = ruleb(&s); rcheck(&s, nr); s.p += nr;
        push(&o->types, (void *)start);
        push(&o->typelen, (void *)(intptr_t)(s.p - start));
      }
      break;
    }
    case 2: {
      u64 n = ruleb(&s);
      o->imports = xmalloc((n + 1) * sizeof *o->imports);
      o->fimport = xmalloc((n + 1) * sizeof *o->fimport);
      o->gimport = xmalloc((n + 1) * sizeof *o->gimport);
      for (u64 i = 0; i < n; i++) {
        Import *im = &o->imports[o->nimports++];
        im->module = rname(&s);
        im->field = rname(&s);
        im->kind = rbyte(&s);
        switch (im->kind) {
        case 0: im->type = (u32)ruleb(&s); o->fimport[o->nfimports++] = im; break;
        case 1: { rbyte(&s); int fl = rbyte(&s); ruleb(&s); if (fl & 1) ruleb(&s); o->ntimports++; break; }
        case 2: { int fl = rbyte(&s); ruleb(&s); if (fl & 1) ruleb(&s); break; }
        case 3: im->gtype = rbyte(&s); im->gmut = rbyte(&s); o->gimport[o->ngimports++] = im; break;
        default: die("%s: unsupported import kind %d", o->name, im->kind);
        }
      }
      break;
    }
    case 3: {
      u64 n = ruleb(&s);
      o->funcs = xmalloc((n + 1) * sizeof *o->funcs);
      o->nfuncs = (int)n;
      for (u64 i = 0; i < n; i++) o->funcs[i].type = (u32)ruleb(&s);
      break;
    }
    case 6: {
      u64 n = ruleb(&s);
      o->globals = xmalloc((n + 1) * sizeof *o->globals);
      o->nglobals = (int)n;
      for (u64 i = 0; i < n; i++) {
        o->globals[i].type = rbyte(&s);
        o->globals[i].mut = rbyte(&s);
        o->globals[i].init = s.p;
        skip_constexpr(&s);
        o->globals[i].initlen = (u32)(s.p - o->globals[i].init);
      }
      break;
    }
    case 7: {
      u64 n = ruleb(&s);
      for (u64 i = 0; i < n; i++) {
        char *name = rname(&s);
        int kind = rbyte(&s);
        u64 idx = ruleb(&s);
        if (kind == 0) { push(&o->exports, name); push(&o->exports, (void *)(intptr_t)idx); }
      }
      break;
    }
    case 10: {
      code_sec = secidx;
      code_start = s.p;
      u64 n = ruleb(&s);
      if ((int)n != o->nfuncs) die("%s: function and code section counts differ", o->name);
      for (u64 i = 0; i < n; i++) {
        u64 bl = ruleb(&s);
        rcheck(&s, bl);
        o->funcs[i].codeoff = (u32)(s.p - code_start);
        o->funcs[i].body = s.p;
        o->funcs[i].size = (u32)bl;
        s.p += bl;
      }
      break;
    }
    case 11: {
      data_sec = secidx;
      data_start = s.p;
      u64 n = ruleb(&s);
      o->segs = xmalloc((n + 1) * sizeof *o->segs);
      o->nsegs = (int)n;
      for (u64 i = 0; i < n; i++) {
        u64 fl = ruleb(&s);
        if (fl != 0) die("%s: unsupported data segment flags %d", o->name, (int)fl);
        skip_constexpr(&s);
        u64 len = ruleb(&s);
        rcheck(&s, len);
        o->segs[i].dataoff = (u32)(s.p - data_start);
        o->segs[i].data = s.p;
        o->segs[i].size = (u32)len;
        s.p += len;
      }
      break;
    }
    case 4: case 5: case 8: case 9: case 12: case 13:
      break; /* table, memory, start, elem, datacount, tag: nothing an object uses here */
    default:
      die("%s: unknown section %d", o->name, id);
    }
    secidx++;
  }
  if (!linking) die("%s: no \"linking\" section: not a relocatable object", o->name);
  /* the linking section */
  Rd l = {linking, linking_end, o->name};
  if (ruleb(&l) != 2) die("%s: unsupported linking section version", o->name);
  while (l.p < l.end) {
    int type = rbyte(&l);
    u64 len = ruleb(&l);
    rcheck(&l, len);
    Rd ss = {l.p, l.p + len, o->name};
    l.p += len;
    if (type == 8) {
      u64 n = ruleb(&ss);
      o->syms = xmalloc((n + 1) * sizeof *o->syms);
      o->nsyms = (int)n;
      for (u64 i = 0; i < n; i++) {
        LSym *sy = &o->syms[i];
        sy->kind = rbyte(&ss);
        sy->flags = (int)ruleb(&ss);
        int undef = sy->flags & F_UNDEF;
        switch (sy->kind) {
        case K_FUNC: case K_GLOBAL: case K_TAG: case K_TABLE:
          sy->index = (u32)ruleb(&ss);
          if (!undef || (sy->flags & F_EXPLICIT)) sy->name = rname(&ss);
          else if (sy->kind == K_FUNC) {
            if (sy->index >= (u32)o->nfimports) die("%s: bad function symbol", o->name);
            sy->name = xstrdup(o->fimport[sy->index]->field);
          } else if (sy->kind == K_GLOBAL) {
            if (sy->index >= (u32)o->ngimports) die("%s: bad global symbol", o->name);
            sy->name = xstrdup(o->gimport[sy->index]->field);
          } else {
            sy->name = xstrdup("__indirect_function_table");
          }
          break;
        case K_DATA:
          sy->name = rname(&ss);
          if (!undef) { sy->seg = (u32)ruleb(&ss); sy->off = (u32)ruleb(&ss); sy->size = (u32)ruleb(&ss); }
          break;
        case K_SECTION:
          sy->index = (u32)ruleb(&ss);
          sy->name = xstrdup("<section>");
          break;
        default: die("%s: unknown symbol kind %d", o->name, sy->kind);
        }
        if (sy->flags & F_TLS) die("%s: thread-local storage is not supported", o->name);
      }
    } else if (type == 5) {
      u64 n = ruleb(&ss);
      for (u64 i = 0; i < n && (int)i < o->nsegs; i++) {
        o->segs[i].name = rname(&ss);
        o->segs[i].align = (u32)ruleb(&ss);
        o->segs[i].flags = (u32)ruleb(&ss);
      }
    } else if (type == 6) {
      u64 n = ruleb(&ss);
      for (u64 i = 0; i < n; i++) {
        push(&o->initfuncs, (void *)(intptr_t)ruleb(&ss));
        push(&o->initfuncs, (void *)(intptr_t)ruleb(&ss));
      }
    }
    /* comdat (7) and target arch (9): not needed for C */
  }
  /* relocations, attached to the function or segment they fall in */
  for (int i = 0; i < relocsecs.len; i++) {
    Rd *s = relocsecs.p[i];
    int target = (int)ruleb(s);
    u64 n = ruleb(s);
    int is_code = target == code_sec, is_data = target == data_sec;
    int cur = 0;
    for (u64 k = 0; k < n; k++) {
      Reloc *rl = xmalloc(sizeof *rl);
      rl->type = rbyte(s);
      rl->off = (u32)ruleb(s);
      rl->index = (u32)ruleb(s);
      switch (rl->type) {
      case 3: case 4: case 5: case 8: case 9: case 11: case 14: case 15: case 16: case 23: rl->addend = (i32)rsleb(s); break;
      }
      if (is_code) {
        while (cur < o->nfuncs && rl->off >= o->funcs[cur].codeoff + o->funcs[cur].size) cur++;
        if (cur >= o->nfuncs || rl->off < o->funcs[cur].codeoff) die("%s: relocation outside any function", o->name);
        rl->off -= o->funcs[cur].codeoff;
        push(&o->funcs[cur].relocs, rl);
      } else if (is_data) {
        while (cur < o->nsegs && rl->off >= o->segs[cur].dataoff + o->segs[cur].size) cur++;
        if (cur >= o->nsegs || rl->off < o->segs[cur].dataoff) die("%s: relocation outside any data segment", o->name);
        rl->off -= o->segs[cur].dataoff;
        push(&o->segs[cur].relocs, rl);
      }
      /* relocations in custom sections (debug info) are dropped with the sections */
    }
  }
  for (int i = 0; i < o->nsegs; i++) if (!o->segs[i].name) o->segs[i].name = xstrdup(".data");
}

static u32 sym_functype(Obj *o, LSym *sy) {
  u32 t;
  if (sy->flags & F_UNDEF) t = o->fimport[sy->index]->type;
  else {
    u32 fi = sy->index - o->nfimports;
    if (fi >= (u32)o->nfuncs) die("%s: bad function symbol '%s'", o->name, sy->name);
    t = o->funcs[fi].type;
  }
  return o->typemap[t];
}

static void load_member(Member *m);
static Vec pending; /* Member* to load */

static void add_obj(Obj *o) {
  if (o->loaded) return;
  o->loaded = 1;
  parse_obj(o);
  push(&objs, o);
  o->typemap = xmalloc((o->types.len + 1) * sizeof *o->typemap);
  for (int i = 0; i < o->types.len; i++) o->typemap[i] = out_type(o->types.p[i], (u32)(intptr_t)o->typelen.p[i]);
  for (int i = 0; i < o->nsyms; i++) {
    LSym *sy = &o->syms[i];
    if (sy->kind == K_SECTION) continue;
    if (sy->flags & F_LOCAL) continue;
    GSym *g = gsym(sy->name);
    sy->g = g;
    if (g->kind >= 0 && g->kind != sy->kind && !g->synthetic)
      err("symbol '%s' is a %s in %s but a %s elsewhere", sy->name, sy->kind == K_FUNC ? "function" : "data object", o->name,
          g->kind == K_FUNC ? "function" : "data object");
    if (g->kind < 0) g->kind = sy->kind;
    if (sy->flags & F_UNDEF) {
      g->referenced = 1;
      if (!(sy->flags & F_WEAK)) g->strong_ref = 1;
      if (!g->refobj) g->refobj = o;
      if (sy->kind == K_FUNC) {
        Import *im = o->fimport[sy->index];
        if (!g->has_reftype) { g->reftype = sym_functype(o, sy); g->has_reftype = 1; }
        if (strcmp(im->module, "env") || (sy->flags & F_EXPLICIT)) {
          g->explicit_import = 1;
          g->imp_module = im->module;
          g->imp_field = im->field;
        }
      }
      if (!g->def && g->lazy && !(sy->flags & F_WEAK)) { push(&pending, g->lazy); g->lazy = NULL; }
      continue;
    }
    /* a definition */
    int weak = sy->flags & F_WEAK;
    if (g->def && !g->synthetic) {
      if (g->weak_def && !weak) { g->def = o; g->defsym = i; g->weak_def = 0; }
      else if (!g->weak_def && !weak) err("duplicate symbol '%s' in %s and %s", sy->name, g->def->name, o->name);
      continue;
    }
    g->def = o;
    g->defsym = i;
    g->weak_def = weak;
    g->synthetic = 0;
    g->lazy = NULL;
  }
}

/* ---- archives ---- */
static Vec archives;
static Member *new_member(Archive *a, const char *name, size_t off, size_t len) {
  Member *m = xmalloc(sizeof *m);
  m->ar = a;
  m->name = name;
  m->off = off;
  m->len = len;
  return m;
}
static void load_member(Member *m) {
  if (m->loaded) return;
  m->loaded = 1;
  Obj *o = xmalloc(sizeof *o);
  size_t n = strlen(m->ar->path) + strlen(m->name) + 3;
  o->name = xmalloc(n);
  snprintf(o->name, n, "%s(%s)", m->ar->path, m->name);
  o->data = m->ar->data + m->off;
  o->len = m->len;
  if (verbose) fprintf(stderr, "ld: loading %s\n", o->name);
  add_obj(o);
}

static void scan_member_syms(Archive *a, Member *m);

static void add_archive(const char *path, u8 *data, size_t len) {
  Archive *a = xmalloc(sizeof *a);
  a->path = xstrdup(path);
  a->data = data;
  a->len = len;
  push(&archives, a);
  size_t p = 8;
  const char *longnames = NULL;
  size_t longlen = 0;
  const u8 *symtab = NULL;
  size_t symlen = 0;
  Vec members = {0};
  while (p + 60 <= len) {
    const char *h = (const char *)data + p;
    char sizestr[11];
    memcpy(sizestr, h + 48, 10);
    sizestr[10] = 0;
    size_t sz = (size_t)strtoul(sizestr, NULL, 10);
    size_t body = p + 60;
    if (body + sz > len) die("%s: truncated archive", path);
    char nm[17];
    memcpy(nm, h, 16);
    nm[16] = 0;
    if (!strncmp(nm, "/               ", 16) || !strncmp(nm, "/SYM64/", 7)) {
      symtab = data + body;
      symlen = sz;
    } else if (!strncmp(nm, "//", 2)) {
      longnames = (const char *)data + body;
      longlen = sz;
    } else if (!strncmp(nm, "__.SYMDEF", 9)) {
      /* BSD symbol table: we scan members instead */
    } else {
      char *name;
      size_t off = body, msz = sz;
      if (nm[0] == '/' && nm[1] >= '0' && nm[1] <= '9') {
        size_t lo = (size_t)strtoul(nm + 1, NULL, 10);
        if (!longnames || lo >= longlen) die("%s: bad long member name", path);
        const char *s = longnames + lo, *e = s;
        while (e < longnames + longlen && *e != '/' && *e != '\n') e++;
        name = xstrndup(s, e - s);
      } else if (!strncmp(nm, "#1/", 3)) {
        size_t nl = (size_t)strtoul(nm + 3, NULL, 10);
        name = xstrndup((const char *)data + body, nl);
        off += nl;
        msz -= nl;
      } else {
        int e = 16;
        while (e > 0 && (nm[e - 1] == ' ' || nm[e - 1] == '/')) e--;
        name = xstrndup(nm, e);
      }
      Member *m = new_member(a, name, off, msz);
      push(&members, m);
    }
    p = body + sz;
    if (p & 1) p++;
  }
  if (whole_archive) {
    for (int k = 0; k < members.len; k++) push(&pending, members.p[k]);
  } else if (symtab && symlen >= 4) {
    /* GNU: big-endian count, offsets, then the names */
    u32 n = (u32)symtab[0] << 24 | (u32)symtab[1] << 16 | (u32)symtab[2] << 8 | symtab[3];
    const u8 *offs = symtab + 4;
    const char *names = (const char *)(offs + 4 * n), *end = (const char *)symtab + symlen;
    for (u32 i = 0; i < n && names < end; i++) {
      u32 hoff = (u32)offs[4 * i] << 24 | (u32)offs[4 * i + 1] << 16 | (u32)offs[4 * i + 2] << 8 | offs[4 * i + 3];
      const char *nm = names;
      names += strlen(names) + 1;
      Member *m = NULL;
      for (int k = 0; k < members.len; k++) {
        Member *x = members.p[k];
        size_t hdr = x->off - 60;
        if (hdr == hoff || (x->off > hoff && x->off - hoff <= 60 + 64)) { m = x; break; }
      }
      if (!m) continue;
      GSym *g = gsym(nm);
      if (g->def) continue;
      if (g->strong_ref) push(&pending, m);
      else if (!g->lazy) g->lazy = m;
    }
  } else {
    for (int k = 0; k < members.len; k++) scan_member_syms(a, members.p[k]);
  }
  while (pending.len) load_member(pending.p[--pending.len]);
}

/* No symbol index: read each member's symbol table to find what it defines. */
static void scan_member_syms(Archive *a, Member *m) {
  Obj tmp = {0};
  tmp.name = (char *)m->name;
  tmp.data = a->data + m->off;
  tmp.len = m->len;
  parse_obj(&tmp);
  for (int i = 0; i < tmp.nsyms; i++) {
    LSym *sy = &tmp.syms[i];
    if ((sy->flags & (F_UNDEF | F_LOCAL)) || sy->kind == K_SECTION) continue;
    GSym *g = gsym(sy->name);
    if (g->def) continue;
    if (g->strong_ref) push(&pending, m);
    else if (!g->lazy) g->lazy = m;
  }
}

static u8 *read_whole(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 65536, n = 0;
  u8 *p = xmalloc(cap);
  for (;;) {
    if (cap - n < 65536) p = xrealloc(p, cap *= 2);
    size_t r = fread(p + n, 1, cap - n, f);
    if (!r) break;
    n += r;
  }
  fclose(f);
  *len = n;
  return p;
}

static void add_file(const char *path) {
  size_t len;
  u8 *data = read_whole(path, &len);
  if (!data) die("cannot open '%s'", path);
  if (len >= 8 && !memcmp(data, "!<arch>\n", 8)) { add_archive(path, data, len); return; }
  if (len >= 8 && !memcmp(data, "!<thin>\n", 8)) die("%s: thin archives are not supported", path);
  Obj *o = xmalloc(sizeof *o);
  o->name = xstrdup(path);
  o->data = data;
  o->len = len;
  add_obj(o);
  while (pending.len) load_member(pending.p[--pending.len]);
}

/* ---- liveness (what nothing reaches is dropped, like wasm-ld --gc-sections) ---- */


static void mark_def(Obj *o, LSym *sy);
static void mark_gsym(GSym *g) {
  if (g->def) mark_def(g->def, &g->def->syms[g->defsym]);
}
static void mark_relocs(Obj *o, Vec *rels) {
  for (int i = 0; i < rels->len; i++) {
    Reloc *r = rels->p[i];
    if (r->type == 6) continue; /* type index */
    if (r->index >= (u32)o->nsyms) die("%s: relocation names symbol %u of %d", o->name, r->index, o->nsyms);
    LSym *sy = &o->syms[r->index];
    if (sy->g) mark_gsym(sy->g);
    else if (!(sy->flags & F_UNDEF)) mark_def(o, sy);
  }
}
static void mark_def(Obj *o, LSym *sy) {
  if (sy->flags & F_UNDEF) { if (sy->g) mark_gsym(sy->g); return; }
  if (sy->kind == K_FUNC) {
    Func *f = &o->funcs[sy->index - o->nfimports];
    if (f->live) return;
    f->live = 1;
    mark_relocs(o, &f->relocs);
  } else if (sy->kind == K_DATA) {
    if (sy->seg >= (u32)o->nsegs) return;
    Seg *s = &o->segs[sy->seg];
    if (s->live) return;
    s->live = 1;
    mark_relocs(o, &s->relocs);
  } else if (sy->kind == K_GLOBAL) {
    if (sy->index >= (u32)o->ngimports) o->globals[sy->index - o->ngimports].live = 1;
  }
}

/* ---- output ---- */
static Buf out;
static void section(int id, Buf *b) {
  bbyte(&out, id);
  buleb(&out, b->len);
  bmem(&out, b->p, b->len);
}

static Vec outfuncs;   /* Func* in output order (after imports) */
static Vec funcobj;    /* Obj* parallel to outfuncs */
static Vec stubs;      /* GSym* needing a trap stub */
static Vec tablefuncs; /* u32 function indices, slot = i + 1 */
static Vec outglobals; /* description in GDesc */
typedef struct GDesc { u8 type, mut; const u8 *init; u32 initlen; i32 ival; const char *name; } GDesc;
static Vec imports_out; /* GSym* */

static u32 heap_base, data_end;

static u32 func_index_of(Obj *o, LSym *sy, int *ok) {
  *ok = 1;
  if (sy->g) {
    GSym *g = sy->g;
    if (g->def) { LSym *d = &g->def->syms[g->defsym]; return g->def->funcs[d->index - g->def->nfimports].outidx; }
    if (g->is_import) return g->outidx;
    if (g->stub >= 0 && g->stub) return (u32)g->stub;
    *ok = 0;
    return 0;
  }
  return o->funcs[sy->index - o->nfimports].outidx;
}
static u32 table_slot(u32 fidx);
static u32 data_addr_of(Obj *o, LSym *sy, int *ok) {
  *ok = 1;
  if (sy->g && !sy->g->def) {
    if (sy->g->synthetic) return sy->g->value;
    *ok = 0;
    return 0;
  }
  Obj *d = sy->g ? sy->g->def : o;
  LSym *ds = sy->g ? &d->syms[sy->g->defsym] : sy;
  if (ds->flags & F_ABS) return ds->off;
  return d->segs[ds->seg].addr + ds->off;
}
static u32 global_index_of(Obj *o, LSym *sy) {
  if (sy->g && !sy->g->def) return sy->g->outidx;
  Obj *d = sy->g ? sy->g->def : o;
  LSym *ds = sy->g ? &d->syms[sy->g->defsym] : sy;
  return d->globals[ds->index - d->ngimports].outidx;
}

static u32 table_slot(u32 fidx) {
  for (int i = 0; i < tablefuncs.len; i++) if ((u32)(intptr_t)tablefuncs.p[i] == fidx) return (u32)i + 1;
  push(&tablefuncs, (void *)(intptr_t)fidx);
  return (u32)tablefuncs.len;
}

static void apply_relocs(Obj *o, u8 *p, Vec *rels) {
  for (int i = 0; i < rels->len; i++) {
    Reloc *r = rels->p[i];
    u8 *at = p + r->off;
    int ok = 1;
    if (r->type == 6) { put_uleb5(at, o->typemap[r->index]); continue; }
    LSym *sy = &o->syms[r->index];
    switch (r->type) {
    case 0: { u32 f = func_index_of(o, sy, &ok); if (!ok) err("%s: undefined function '%s'", o->name, sy->name); put_uleb5(at, f); break; }
    case 26: { u32 f = func_index_of(o, sy, &ok); put_u32(at, f); break; }
    case 1: case 2: {
      u32 slot = 0;
      if (sy->g && !sy->g->def && !sy->g->is_import) slot = 0; /* weak undefined: null pointer */
      else { u32 f = func_index_of(o, sy, &ok); slot = ok ? table_slot(f) : 0; }
      if (r->type == 1) put_sleb5(at, (i32)slot); else put_u32(at, slot);
      break;
    }
    case 3: case 4: case 5: {
      u32 a = data_addr_of(o, sy, &ok) + r->addend;
      if (r->type == 3) put_uleb5(at, a);
      else if (r->type == 4) put_sleb5(at, (i32)a);
      else put_u32(at, a);
      break;
    }
    case 7: put_uleb5(at, global_index_of(o, sy)); break;
    case 13: put_u32(at, global_index_of(o, sy)); break;
    case 20: put_uleb5(at, 0); break;
    case 10: die("%s: exception handling is not supported", o->name);
    default: die("%s: unsupported relocation type %d (position-independent or 64-bit code?)", o->name, r->type);
    }
  }
}

static int seg_class(const char *name) {
  if (!strncmp(name, ".rodata", 7)) return 0;
  if (!strncmp(name, ".bss", 4)) return 2;
  return 1;
}

static void usage(void) {
  fputs("usage: ld [options] file...\n"
        "  -o FILE            output (default a.out)\n"
        "  -L DIR, -l NAME    library search path, library libNAME.a\n"
        "  -e NAME, --entry=NAME   entry point (default _start)\n"
        "  -z stack-size=N    stack size (default 1048576)\n"
        "  --export=NAME      export a function\n"
        "  -u NAME            treat NAME as referenced\n"
        "  --crt0=A,B         start file: A, or B when main takes three arguments\n"
        "  --no-gc-sections   keep everything that was loaded\n"
        "  --allow-undefined  undefined functions become imports from \"env\"\n"
        "  -s, --strip-all    no name section\n"
        "  -v                 tell what is loaded\n", stderr);
}

int main(int argc, char **argv) {
  const char *outpath = "a.out";
  Vec libdirs = {0}, inputs = {0};
  const char *crt0[2] = {NULL, NULL};
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
#define NEXT() (++i < argc ? argv[i] : (die("missing argument to '%s'", a), ""))
    if (a[0] != '-') { push(&inputs, a); continue; }
    if (!strcmp(a, "-o")) outpath = NEXT();
    else if (!strncmp(a, "-o", 2)) outpath = a + 2;
    else if (!strcmp(a, "-L")) push(&libdirs, NEXT());
    else if (!strncmp(a, "-L", 2)) push(&libdirs, a + 2);
    else if (!strcmp(a, "-l")) { char *n = NEXT(); char *s = xmalloc(strlen(n) + 4); sprintf(s, "-l%s", n); push(&inputs, s); }
    else if (!strncmp(a, "-l", 2)) push(&inputs, a);
    else if (!strcmp(a, "-e")) entry = NEXT();
    else if (!strncmp(a, "--entry=", 8)) entry = a + 8;
    else if (!strcmp(a, "--no-entry")) entry = NULL;
    else if (!strcmp(a, "-z")) {
      char *z = NEXT();
      if (!strncmp(z, "stack-size=", 11)) stack_size = (u32)strtoul(z + 11, NULL, 0);
    }
    else if (!strncmp(a, "--export=", 9)) push(&exports_extra, a + 9);
    else if (!strcmp(a, "--export")) push(&exports_extra, NEXT());
    else if (!strcmp(a, "-u")) push(&undef_extra, NEXT());
    else if (!strncmp(a, "--crt0=", 7)) {
      char *s = xstrdup(a + 7), *c = strchr(s, ',');
      if (c) { *c = 0; crt0[1] = c + 1; }
      crt0[0] = s;
    }
    else if (!strcmp(a, "--no-gc-sections")) gc = 0;
    else if (!strcmp(a, "--gc-sections")) gc = 1;
    else if (!strcmp(a, "--allow-undefined")) allow_undef = 1;
    else if (!strcmp(a, "-s") || !strcmp(a, "--strip-all")) strip = 1;
    else if (!strcmp(a, "--whole-archive")) push(&inputs, "\x01+");
    else if (!strcmp(a, "--no-whole-archive")) push(&inputs, "\x01-");
    else if (!strcmp(a, "-v")) verbose = 1;
    else if (!strcmp(a, "--version")) { puts("ld (UC.OS linker) 0.1, target wasm32"); return 0; }
    else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
    else if (!strcmp(a, "--stack-first") || !strcmp(a, "--fatal-warnings") || !strcmp(a, "-static") ||
             !strcmp(a, "--strip-debug") || !strcmp(a, "-S") || !strcmp(a, "--start-group") || !strcmp(a, "--end-group") ||
             !strcmp(a, "-(") || !strcmp(a, "-)")) ;
    else { fprintf(stderr, "ld: unknown option '%s'\n", a); return 1; }
#undef NEXT
  }
  if (!inputs.len) { usage(); return 1; }
  for (int i = 0; i < undef_extra.len; i++) { GSym *g = gsym(undef_extra.p[i]); g->referenced = g->strong_ref = 1; }
  int crt0_done = !crt0[0];
  for (int i = 0; i < inputs.len; i++) {
    const char *in = inputs.p[i];
    char *path = (char *)in;
    if (in[0] == 1) { whole_archive = in[1] == '+'; continue; }
    if (!strncmp(in, "-l", 2)) {
      path = NULL;
      for (int d = 0; d < libdirs.len && !path; d++) {
        char *p = xmalloc(strlen(libdirs.p[d]) + strlen(in) + 8);
        sprintf(p, "%s/lib%s.a", (char *)libdirs.p[d], in + 2);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); path = p; }
      }
      if (!path) die("cannot find library '%s'", in);
    }
    int is_ar = 0;
    {
      FILE *f = fopen(path, "rb");
      char m[8] = {0};
      if (f) { if (fread(m, 1, 8, f) != 8) m[0] = 0; fclose(f); }
      is_ar = !memcmp(m, "!<arch>\n", 8);
    }
    if (is_ar && !crt0_done) {
      /* choose the start file now: user objects are in, libraries are next */
      GSym *m = mget(&gsyms, "main");
      const char *c = crt0[0];
      if (m && m->def && crt0[1]) {
        LSym *d = &m->def->syms[m->defsym];
        if (d->kind == K_FUNC) {
          u32 t = sym_functype(m->def, d);
          Buf *b = outtypes.p[t];
          if (b->len >= 2 && b->p[1] == 3) c = crt0[1];
        }
      }
      add_file(c);
      crt0_done = 1;
    }
    add_file(path);
  }
  if (!crt0_done) {
    GSym *m = mget(&gsyms, "main");
    const char *c = crt0[0];
    if (m && m->def && crt0[1]) {
      LSym *d = &m->def->syms[m->defsym];
      u32 t = sym_functype(m->def, d);
      Buf *b = outtypes.p[t];
      if (b->len >= 2 && b->p[1] == 3) c = crt0[1];
    }
    add_file(c);
  }
  /* linker-provided symbols */
  static const char *synth[] = {"__stack_pointer", "__indirect_function_table", "__heap_base", "__data_end",
                                "__dso_handle", "__global_base", "__heap_end", "__memory_base", "__table_base",
                                "__stack_low", "__stack_high", 0};
  for (int i = 0; synth[i]; i++) {
    GSym *g = mget(&gsyms, synth[i]);
    if (g && !g->def) g->synthetic = 1;
  }
  /* undefined symbols */
  for (int i = 0; i < gsyms.cap; i++) {
    GSym *g = gsyms.v[i];
    if (!gsyms.k[i] || g->def || g->synthetic || !g->referenced) continue;
    if (g->kind == K_FUNC && (g->explicit_import || allow_undef)) { g->is_import = 1; continue; }
    if (!g->strong_ref) continue; /* weak undefined: address 0, calls trap */
    err("undefined symbol: %s (first referenced in %s)", g->name, g->refobj ? g->refobj->name : "the command line");
  }
  if (nerr) return 1;
  /* signature checks: every direct call must match the definition exactly (wasm traps
   * otherwise). Taking the address is fine: calls through the pointer use the type of
   * the pointer, as in C. */
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    u8 *called = xmalloc(o->nsyms + 1);
    for (int fi = 0; fi < o->nfuncs; fi++)
      for (int k = 0; k < o->funcs[fi].relocs.len; k++) {
        Reloc *r = o->funcs[fi].relocs.p[k];
        if ((r->type == 0 || r->type == 26) && r->index < (u32)o->nsyms) called[r->index] = 1;
      }
    for (int i = 0; i < o->nsyms; i++) {
      LSym *sy = &o->syms[i];
      if (sy->kind != K_FUNC || !(sy->flags & F_UNDEF) || !sy->g || !sy->g->def || !called[i]) continue;
      GSym *g = sy->g;
      u32 want = sym_functype(g->def, &g->def->syms[g->defsym]), have = sym_functype(o, sy);
      if (want != have) err("function signature mismatch: %s (defined in %s, called with a different type in %s)", g->name, g->def->name, o->name);
    }
  }
  if (nerr) return 1;
  /* roots */
  GSym *eg = entry ? mget(&gsyms, entry) : NULL;
  if (entry && (!eg || !eg->def)) die("entry symbol '%s' is not defined", entry);
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    for (int i = 0; i < o->nsyms; i++) {
      LSym *sy = &o->syms[i];
      if ((sy->flags & F_UNDEF) || sy->kind == K_SECTION) continue;
      int root = !gc || (sy->flags & (F_EXPORTED | F_NOSTRIP));
      if (sy->g && sy->g->def != o) root = 0; /* a definition that lost to another one */
      if (root) mark_def(o, sy);
    }
    for (int i = 0; i + 1 < o->initfuncs.len; i += 2) mark_def(o, &o->syms[(intptr_t)o->initfuncs.p[i + 1]]);
    if (!gc) { for (int i = 0; i < o->nfuncs; i++) o->funcs[i].live = 1; for (int i = 0; i < o->nsegs; i++) o->segs[i].live = 1; }
  }
  if (eg) mark_gsym(eg);
  int export_sp = 0;
  for (int i = 0; i < exports_extra.len; i++) {
    if (!strcmp(exports_extra.p[i], "__stack_pointer")) { export_sp = 1; continue; } /* the global */
    GSym *g = mget(&gsyms, exports_extra.p[i]);
    if (!g || !g->def) die("exported symbol '%s' is not defined", (char *)exports_extra.p[i]);
    mark_gsym(g);
  }
  /* imports: functions used and not defined (explicit imports) */
  int nimp = 0;
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    int any = 0;
    for (int i = 0; i < o->nfuncs; i++) any |= o->funcs[i].live;
    for (int i = 0; i < o->nsegs; i++) any |= o->segs[i].live;
    if (!any) continue;
    for (int fi = 0; fi < o->nfuncs; fi++) {
      if (!o->funcs[fi].live) continue;
      Vec *rels = &o->funcs[fi].relocs;
      for (int k = 0; k < rels->len; k++) {
        Reloc *r = rels->p[k];
        if (r->type == 6) continue;
        LSym *sy = &o->syms[r->index];
        if (sy->kind != K_FUNC || !sy->g || sy->g->def) continue;
        GSym *g = sy->g;
        if (g->is_import && !g->outidx && !g->stub) {
          int seen = 0;
          for (int j = 0; j < imports_out.len; j++) if (imports_out.p[j] == g) seen = 1;
          if (!seen) { g->outidx = nimp++; push(&imports_out, g); }
        }
      }
    }
  }
  /* defined functions */
  u32 fidx = nimp;
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    for (int i = 0; i < o->nfuncs; i++) {
      if (!o->funcs[i].live) continue;
      o->funcs[i].outidx = fidx++;
      push(&outfuncs, &o->funcs[i]);
      push(&funcobj, o);
    }
  }
  /* stubs for weak undefined functions that are called */
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    for (int fi = 0; fi < o->nfuncs; fi++) {
      if (!o->funcs[fi].live) continue;
      Vec *rels = &o->funcs[fi].relocs;
      for (int k = 0; k < rels->len; k++) {
        Reloc *r = rels->p[k];
        if (r->type != 0) continue;
        LSym *sy = &o->syms[r->index];
        GSym *g = sy->g;
        if (!g || g->def || g->is_import || g->stub) continue;
        g->stub = (int)fidx++;
        g->reftype = sym_functype(o, sy);
        push(&stubs, g);
      }
    }
  }
  /* globals: the stack pointer, then any the objects define */
  GDesc *spd = xmalloc(sizeof *spd);
  spd->type = 0x7f;
  spd->mut = 1;
  spd->name = "__stack_pointer";
  push(&outglobals, spd);
  GSym *spg = mget(&gsyms, "__stack_pointer");
  if (spg) spg->outidx = 0;
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    for (int i = 0; i < o->nglobals; i++) {
      if (!o->globals[i].live) continue;
      GDesc *d = xmalloc(sizeof *d);
      d->type = o->globals[i].type;
      d->mut = o->globals[i].mut;
      d->init = o->globals[i].init;
      d->initlen = o->globals[i].initlen;
      o->globals[i].outidx = outglobals.len;
      push(&outglobals, d);
    }
  }
  /* other imported globals must be linker symbols we know */
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    for (int i = 0; i < o->nsyms; i++) {
      LSym *sy = &o->syms[i];
      if (sy->kind == K_GLOBAL && (sy->flags & F_UNDEF) && sy->g && !sy->g->def && strcmp(sy->name, "__stack_pointer"))
        die("%s: undefined global '%s' (position-independent code is not supported)", o->name, sy->name);
    }
  }
  /* memory layout: stack first, then .rodata, .data, .bss */
  u32 addr = (stack_size + 15) & ~15u;
  u32 seg_start[3], seg_end[3];
  for (int cls = 0; cls < 3; cls++) {
    u32 maxal = 1;
    for (int oi = 0; oi < objs.len; oi++) {
      Obj *o = objs.p[oi];
      for (int i = 0; i < o->nsegs; i++) {
        Seg *s = &o->segs[i];
        if (!s->live || seg_class(s->name) != cls) continue;
        u32 al = 1u << s->align;
        if (al > maxal) maxal = al;
      }
    }
    addr = (addr + maxal - 1) & ~(maxal - 1);
    seg_start[cls] = addr;
    for (int oi = 0; oi < objs.len; oi++) {
      Obj *o = objs.p[oi];
      for (int i = 0; i < o->nsegs; i++) {
        Seg *s = &o->segs[i];
        if (!s->live || seg_class(s->name) != cls) continue;
        u32 al = 1u << s->align;
        addr = (addr + al - 1) & ~(al - 1);
        s->addr = addr;
        addr += s->size;
      }
    }
    seg_end[cls] = addr;
  }
  data_end = addr;
  heap_base = (addr + 15) & ~15u;
  struct { const char *n; u32 v; } vals[] = {{"__heap_base", heap_base}, {"__data_end", data_end}, {"__dso_handle", 0},
    {"__global_base", (stack_size + 15) & ~15u}, {"__heap_end", 0}, {"__memory_base", 0}, {"__table_base", 1},
    {"__stack_low", 0}, {"__stack_high", (stack_size + 15) & ~15u}};
  for (size_t i = 0; i < sizeof vals / sizeof *vals; i++) {
    GSym *g = mget(&gsyms, vals[i].n);
    if (g) g->value = vals[i].v;
  }
  spd->ival = (i32)((stack_size + 15) & ~15u);
  u32 pages = (heap_base + 65535) / 65536;

  /* code first (it assigns table slots), assembled into the module afterwards */
  Buf code = {0};
  buleb(&code, outfuncs.len + stubs.len);
  for (int i = 0; i < outfuncs.len; i++) {
    Func *f = outfuncs.p[i];
    Obj *o = funcobj.p[i];
    u8 *body = xmalloc(f->size);
    memcpy(body, f->body, f->size);
    apply_relocs(o, body, &f->relocs);
    buleb(&code, f->size);
    bmem(&code, body, f->size);
    free(body);
  }
  for (int i = 0; i < stubs.len; i++) {
    static const u8 trap[] = {0, 0, 0x0b}; /* no locals; unreachable; end */
    buleb(&code, sizeof trap);
    bmem(&code, trap, sizeof trap);
  }
  Buf data = {0};
  int nds = 0;
  for (int cls = 0; cls < 2; cls++) if (seg_end[cls] > seg_start[cls]) nds++;
  buleb(&data, nds);
  for (int cls = 0; cls < 2; cls++) {
    if (seg_end[cls] == seg_start[cls]) continue;
    u32 n = seg_end[cls] - seg_start[cls];
    u8 *img = xmalloc(n);
    for (int oi = 0; oi < objs.len; oi++) {
      Obj *o = objs.p[oi];
      for (int i = 0; i < o->nsegs; i++) {
        Seg *s = &o->segs[i];
        if (!s->live || seg_class(s->name) != cls) continue;
        u8 *p = img + (s->addr - seg_start[cls]);
        memcpy(p, s->data, s->size);
        apply_relocs(o, p, &s->relocs);
      }
    }
    bbyte(&data, 0);
    bbyte(&data, 0x41);
    bsleb(&data, (i32)seg_start[cls]);
    bbyte(&data, 0x0b);
    buleb(&data, n);
    bmem(&data, img, n);
    free(img);
  }
  if (nerr) return 1;

  bmem(&out, "\0asm\1\0\0\0", 8);
  Buf b = {0};
  /* types */
  for (int i = 0; i < stubs.len; i++) (void)0;
  buleb(&b, outtypes.len);
  for (int i = 0; i < outtypes.len; i++) { Buf *t = outtypes.p[i]; bmem(&b, t->p, t->len); }
  section(1, &b);
  /* imports */
  b = (Buf){0};
  buleb(&b, imports_out.len);
  for (int i = 0; i < imports_out.len; i++) {
    GSym *g = imports_out.p[i];
    bname(&b, g->imp_module ? g->imp_module : "env");
    bname(&b, g->imp_field ? g->imp_field : g->name);
    bbyte(&b, 0);
    buleb(&b, g->reftype);
  }
  section(2, &b);
  /* functions */
  b = (Buf){0};
  buleb(&b, outfuncs.len + stubs.len);
  for (int i = 0; i < outfuncs.len; i++) { Func *f = outfuncs.p[i]; buleb(&b, ((Obj *)funcobj.p[i])->typemap[f->type]); }
  for (int i = 0; i < stubs.len; i++) buleb(&b, ((GSym *)stubs.p[i])->reftype);
  section(3, &b);
  /* table */
  b = (Buf){0};
  buleb(&b, 1);
  bbyte(&b, 0x70);
  bbyte(&b, 1);
  buleb(&b, tablefuncs.len + 1);
  buleb(&b, tablefuncs.len + 1);
  section(4, &b);
  /* memory */
  b = (Buf){0};
  buleb(&b, 1);
  bbyte(&b, 0);
  buleb(&b, pages);
  section(5, &b);
  /* globals */
  b = (Buf){0};
  buleb(&b, outglobals.len);
  for (int i = 0; i < outglobals.len; i++) {
    GDesc *d = outglobals.p[i];
    bbyte(&b, d->type);
    bbyte(&b, d->mut);
    if (d->init) bmem(&b, d->init, d->initlen);
    else { bbyte(&b, 0x41); bsleb(&b, d->ival); bbyte(&b, 0x0b); }
  }
  section(6, &b);
  /* exports: memory, the entry, and functions marked for export */
  b = (Buf){0};
  Vec en = {0}, ei = {0};
  if (eg) {
    LSym *d = &eg->def->syms[eg->defsym];
    push(&en, (void *)entry);
    push(&ei, (void *)(intptr_t)eg->def->funcs[d->index - eg->def->nfimports].outidx);
  }
  for (int oi = 0; oi < objs.len; oi++) {
    Obj *o = objs.p[oi];
    for (int i = 0; i + 1 < o->exports.len; i += 2) {
      const char *name = o->exports.p[i];
      u32 idx = (u32)(intptr_t)o->exports.p[i + 1];
      if (idx < (u32)o->nfimports || !o->funcs[idx - o->nfimports].live) continue;
      int dup = 0;
      for (int k = 0; k < en.len; k++) if (!strcmp(en.p[k], name)) dup = 1;
      if (dup) continue;
      push(&en, (void *)name);
      push(&ei, (void *)(intptr_t)o->funcs[idx - o->nfimports].outidx);
    }
  }
  for (int i = 0; i < exports_extra.len; i++) {
    if (!strcmp(exports_extra.p[i], "__stack_pointer")) continue;
    GSym *g = mget(&gsyms, exports_extra.p[i]);
    LSym *d = &g->def->syms[g->defsym];
    if (d->kind != K_FUNC) die("--export=%s: only functions can be exported", g->name);
    push(&en, g->name);
    push(&ei, (void *)(intptr_t)g->def->funcs[d->index - g->def->nfimports].outidx);
  }
  buleb(&b, en.len + 1 + export_sp);
  bname(&b, "memory");
  bbyte(&b, 2);
  buleb(&b, 0);
  for (int i = 0; i < en.len; i++) { bname(&b, en.p[i]); bbyte(&b, 0); buleb(&b, (u32)(intptr_t)ei.p[i]); }
  if (export_sp) { bname(&b, "__stack_pointer"); bbyte(&b, 3); buleb(&b, 0); }
  section(7, &b);
  /* table elements */
  if (tablefuncs.len) {
    b = (Buf){0};
    buleb(&b, 1);
    bbyte(&b, 0);
    bbyte(&b, 0x41);
    bsleb(&b, 1);
    bbyte(&b, 0x0b);
    buleb(&b, tablefuncs.len);
    for (int i = 0; i < tablefuncs.len; i++) buleb(&b, (u32)(intptr_t)tablefuncs.p[i]);
    section(9, &b);
  }
  section(10, &code);
  if (nds) section(11, &data);
  /* function names, so traps and profiles say where they happened */
  if (!strip) {
    Buf names = {0};
    int n = imports_out.len + outfuncs.len + stubs.len;
    buleb(&names, n);
    for (int i = 0; i < imports_out.len; i++) { buleb(&names, i); bname(&names, ((GSym *)imports_out.p[i])->name); }
    /* map function -> symbol name */
    for (int i = 0; i < outfuncs.len; i++) {
      Func *f = outfuncs.p[i];
      Obj *o = funcobj.p[i];
      const char *nm = "?";
      for (int k = 0; k < o->nsyms; k++) {
        LSym *sy = &o->syms[k];
        if (sy->kind == K_FUNC && !(sy->flags & F_UNDEF) && &o->funcs[sy->index - o->nfimports] == f) { nm = sy->name; break; }
      }
      buleb(&names, f->outidx);
      bname(&names, nm);
    }
    for (int i = 0; i < stubs.len; i++) {
      GSym *g = stubs.p[i];
      char *nm = xmalloc(strlen(g->name) + 20);
      sprintf(nm, "undefined_weak:%s", g->name);
      buleb(&names, (u32)g->stub);
      bname(&names, nm);
    }
    Buf ns = {0};
    bname(&ns, "name");
    bbyte(&ns, 1);
    buleb(&ns, names.len);
    bmem(&ns, names.p, names.len);
    section(0, &ns);
  }
  FILE *f = fopen(outpath, "wb");
  if (!f) die("cannot write '%s'", outpath);
  if (fwrite(out.p, 1, out.len, f) != out.len) die("cannot write '%s'", outpath);
  fclose(f);
  return 0;
}
