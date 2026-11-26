// SPDX-License-Identifier: GPL-2.0-or-later
/* wasmfork: the build-time stack transform that makes fork() possible in a UC.OS program.
 *
 *   wasmfork [--all | --root=NAME] [--stats] in.wasm out.wasm
 *
 * A WebAssembly call stack lives inside the engine and cannot be copied. The clone primitive
 * copies memory and globals into a new instance; this transform makes the stack rebuildable:
 *
 *  - S = the functions that can be on the stack when fork happens: those that can reach
 *    __fork_point by direct calls, or by call_indirect of a type that some function in S
 *    (present in the table) has. The search stops at __fork_catch, where main is run.
 *  - Each function in S keeps its code, plus: its whole body is wrapped in a try_table that
 *    catches the "fork" tag, saves every local and the number of the call site it was in
 *    into a frame in linear memory, and throws again. Before each call that can reach fork,
 *    the call site number goes into a local; operand-stack values that are live across the
 *    call are moved into locals first. When fork is not called, that is all that runs.
 *  - A second copy F$rw of each such function rewinds: on entry, if the state is "rewinding",
 *    it reloads its locals from its frame; every stretch of code between control
 *    instructions and calls is guarded by "if not rewinding"; an if takes the arm that holds
 *    the saved call site; at the saved site it calls the callee's F$rw. The innermost one,
 *    __fork_point$rw, switches to "normal" and returns the result (child pid, or 0).
 *    A second table holds the F$rw of every function of the first table, for call_indirect.
 *  - The three placeholders in libc/src/forkhook.c get real bodies here.
 *
 * Limits (the tool stops with a message): SIMD, atomics, the legacy exception instructions,
 * tail calls, multi-value calls or blocks with parameters, v128 or reference values live in a
 * function of S. clang -O2 output for this tree has none of these.
 *
 * --all: instrument every function that makes a call (to measure the cost of instrumenting
 * everything). --root=NAME: pretend NAME is the fork point (to measure the call graph of a
 * program that does not fork). --stats: print sizes. Written for the host C compiler; it uses
 * only the C library, so it can be built for UC.OS too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;

static const char *cur_fn = "";
static __attribute__((noreturn)) void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "wasmfork: ");
  if (*cur_fn) fprintf(stderr, "in %s: ", cur_fn);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }

/* ---- byte buffers ---- */
typedef struct { u8 *p; size_t n, cap; } buf;
static void bput(buf *b, u8 c) { if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->p = xrealloc(b->p, b->cap); } b->p[b->n++] = c; }
static void bmem(buf *b, const void *s, size_t n) { for (size_t i = 0; i < n; i++) bput(b, ((const u8 *)s)[i]); }
static void buleb(buf *b, u64 v) { do { u8 c = v & 0x7f; v >>= 7; if (v) c |= 0x80; bput(b, c); } while (v); }
static void bsleb(buf *b, s64 v) {
  for (;;) { u8 c = v & 0x7f; v >>= 7; int done = (v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40)); if (!done) c |= 0x80; bput(b, c); if (done) break; }
}
static void bbuf(buf *b, const buf *s) { bmem(b, s->p, s->n); }

/* ---- reading ---- */
typedef struct { const u8 *p, *end; } rd;
static u8 rbyte(rd *r) { if (r->p >= r->end) die("truncated module"); return *r->p++; }
static u64 ruleb(rd *r) { u64 v = 0; int s = 0; u8 c; do { c = rbyte(r); v |= (u64)(c & 0x7f) << s; s += 7; } while (c & 0x80); return v; }
static s64 rsleb(rd *r) { s64 v = 0; int s = 0; u8 c; do { c = rbyte(r); v |= (s64)(c & 0x7f) << s; s += 7; } while (c & 0x80); if (s < 64 && (c & 0x40)) v |= -((s64)1 << s); return v; }

/* ---- the module ---- */
enum { I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c, V128 = 0x7b, FUNCREF = 0x70, EXTERNREF = 0x6f, ANY = 0 };
struct functype { int np, nr; u8 p[64], r[8]; };
static struct functype *types;
static int ntypes;
struct section { u8 id; const u8 *data; u32 size; };
static struct section secs[64];
static int nsecs;
static u32 *ftype;           /* per function index: type */
static int nimpf, nfunc;     /* imported functions, all functions */
static u8 *gtype;            /* per global index: value type */
static int nglob, nimpglob;
static int ntags, ntables;
static u32 tab0_size;
static s32 *tab0;            /* table 0 contents (function index or -1) */
static char **fname;
struct body { const u8 *p; u32 len; };
static struct body *bodies;  /* per defined function */

static int find_sec(int id) { for (int i = 0; i < nsecs; i++) if (secs[i].id == id) return i; return -1; }
static int type_eq(int t, int np, const u8 *p, int nr, const u8 *r) {
  return types[t].np == np && types[t].nr == nr && !memcmp(types[t].p, p, np) && !memcmp(types[t].r, r, nr);
}
static u8 rvaltype(rd *r) { u8 t = rbyte(r); if (t != I32 && t != I64 && t != F32 && t != F64 && t != V128 && t != FUNCREF && t != EXTERNREF) die("unsupported value type 0x%x", t); return t; }
static void skip_limits(rd *r) { u8 f = rbyte(r); ruleb(r); if (f & 1) ruleb(r); }
static u32 const_expr_i32(rd *r) { /* i32.const N end */
  u8 op = rbyte(r);
  if (op != 0x41) die("unsupported constant expression 0x%x", op);
  u32 v = (u32)rsleb(r);
  if (rbyte(r) != 0x0b) die("unsupported constant expression");
  return v;
}
static void skip_const_expr(rd *r) { for (;;) { u8 op = rbyte(r); if (op == 0x0b) return; if (op == 0x41 || op == 0x42) rsleb(r); else if (op == 0x23 || op == 0xd2) ruleb(r); else if (op == 0x43) r->p += 4; else if (op == 0x44) r->p += 8; else if (op == 0xd0) rbyte(r); else die("unsupported constant expression 0x%x", op); } }

static void parse(const u8 *m, size_t n) {
  if (n < 8 || memcmp(m, "\0asm\1\0\0\0", 8)) die("not a WebAssembly module");
  rd r = {m + 8, m + n};
  while (r.p < r.end) {
    if (nsecs == 64) die("too many sections");
    u8 id = rbyte(&r);
    u32 size = ruleb(&r);
    secs[nsecs++] = (struct section){id, r.p, size};
    r.p += size;
  }
  int i;
  if ((i = find_sec(1)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    ntypes = ruleb(&s);
    types = xrealloc(NULL, sizeof *types * (ntypes + 4));
    for (int t = 0; t < ntypes; t++) {
      if (rbyte(&s) != 0x60) die("unsupported type form");
      struct functype *f = &types[t];
      f->np = ruleb(&s); if (f->np > 64) die("too many parameters");
      for (int k = 0; k < f->np; k++) f->p[k] = rvaltype(&s);
      f->nr = ruleb(&s); if (f->nr > 8) die("too many results");
      for (int k = 0; k < f->nr; k++) f->r[k] = rvaltype(&s);
    }
  }
  ftype = xrealloc(NULL, 4);
  gtype = xrealloc(NULL, 1);
  if ((i = find_sec(2)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    u32 cnt = ruleb(&s);
    for (u32 k = 0; k < cnt; k++) {
      u32 l = ruleb(&s); s.p += l; l = ruleb(&s); s.p += l;
      u8 kind = rbyte(&s);
      if (kind == 0) { ftype = xrealloc(ftype, 4 * (nfunc + 1)); ftype[nfunc++] = ruleb(&s); nimpf++; }
      else if (kind == 1) { rbyte(&s); skip_limits(&s); ntables++; }
      else if (kind == 2) skip_limits(&s);
      else if (kind == 3) { gtype = xrealloc(gtype, nglob + 1); gtype[nglob++] = rvaltype(&s); rbyte(&s); nimpglob++; }
      else if (kind == 4) { rbyte(&s); ruleb(&s); ntags++; }
      else die("unknown import kind %d", kind);
    }
  }
  if ((i = find_sec(3)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    u32 cnt = ruleb(&s);
    ftype = xrealloc(ftype, 4 * (nfunc + cnt));
    for (u32 k = 0; k < cnt; k++) ftype[nfunc++] = ruleb(&s);
  }
  if ((i = find_sec(4)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    u32 cnt = ruleb(&s);
    for (u32 k = 0; k < cnt; k++) {
      rbyte(&s);
      u8 f = rbyte(&s); u32 mn = ruleb(&s); if (f & 1) ruleb(&s);
      if (ntables == 0) tab0_size = mn;
      ntables++;
    }
  }
  if ((i = find_sec(13)) >= 0) { rd s = {secs[i].data, secs[i].data + secs[i].size}; ntags += ruleb(&s); }
  if ((i = find_sec(6)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    u32 cnt = ruleb(&s);
    for (u32 k = 0; k < cnt; k++) { gtype = xrealloc(gtype, nglob + 1); gtype[nglob++] = rvaltype(&s); rbyte(&s); skip_const_expr(&s); }
  }
  tab0 = xrealloc(NULL, sizeof *tab0 * (tab0_size + 1));
  for (u32 k = 0; k < tab0_size; k++) tab0[k] = -1;
  if ((i = find_sec(9)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    u32 cnt = ruleb(&s);
    for (u32 k = 0; k < cnt; k++) {
      u32 flags = ruleb(&s);
      if (flags != 0 && flags != 2) die("unsupported element segment kind %u", flags);
      u32 table = flags == 2 ? ruleb(&s) : 0;
      u32 off = const_expr_i32(&s);
      if (flags == 2 && rbyte(&s) != 0) die("unsupported element kind");
      u32 m = ruleb(&s);
      for (u32 j = 0; j < m; j++) { u32 f = ruleb(&s); if (table == 0 && off + j < tab0_size) tab0[off + j] = f; }
    }
  }
  fname = xrealloc(NULL, sizeof *fname * nfunc);
  memset(fname, 0, sizeof *fname * nfunc);
  for (int k = 0; k < nsecs; k++) {
    if (secs[k].id != 0) continue;
    rd s = {secs[k].data, secs[k].data + secs[k].size};
    u32 l = ruleb(&s);
    if (l != 4 || memcmp(s.p, "name", 4)) continue;
    s.p += 4;
    while (s.p < s.end) {
      u8 sub = rbyte(&s);
      u32 sz = ruleb(&s);
      const u8 *next = s.p + sz;
      if (sub == 1) {
        u32 cnt = ruleb(&s);
        for (u32 j = 0; j < cnt; j++) {
          u32 idx = ruleb(&s), nl = ruleb(&s);
          if (idx < (u32)nfunc) { fname[idx] = xrealloc(NULL, nl + 1); memcpy(fname[idx], s.p, nl); fname[idx][nl] = 0; }
          s.p += nl;
        }
      }
      s.p = next;
    }
  }
  bodies = xrealloc(NULL, sizeof *bodies * (nfunc - nimpf + 1));
  if ((i = find_sec(10)) >= 0) {
    rd s = {secs[i].data, secs[i].data + secs[i].size};
    u32 cnt = ruleb(&s);
    if ((int)cnt != nfunc - nimpf) die("function and code sections disagree");
    for (u32 k = 0; k < cnt; k++) { u32 sz = ruleb(&s); bodies[k] = (struct body){s.p, sz}; s.p += sz; }
  }
}
static int func_by_name(const char *n) { for (int i = 0; i < nfunc; i++) if (fname[i] && !strcmp(fname[i], n)) return i; return -1; }
static const char *fn_name(int f) { static char b[32]; if (fname[f]) return fname[f]; snprintf(b, sizeof b, "function %d", f); return b; }

/* ---- instructions ---- */
enum { K_PLAIN, K_BLOCK, K_LOOP, K_IF, K_ELSE, K_END, K_TRY, K_BRANCH, K_CALL, K_CALLI };
typedef struct {
  u32 start, end;   /* bytes in the body */
  u8 op;
  u8 kind;
  u8 dead;          /* in unreachable code */
  u8 nres;          /* K_END/K_ELSE: results the block leaves; K_CALL: results of the callee */
  u8 rtype;         /* their type (at most one) */
  int target;       /* K_CALL: function; K_CALLI: type */
  u32 depth;        /* enclosing blocks of the function at this instruction */
  int site;         /* instrumented call: its number, else -1 */
  int lo, hi;       /* K_IF: call sites in the then arm */
  u32 below, nbelow, nargs; /* instrumented call: types (in tpool) of the values under its arguments, then its arguments */
} insn;
static insn *ins;
static int nins, cap_ins;
static u8 *tpool;
static u32 ntpool, cap_tpool;
static void tpush(u8 t) { if (ntpool == cap_tpool) { cap_tpool = cap_tpool ? cap_tpool * 2 : 1024; tpool = xrealloc(tpool, cap_tpool); } tpool[ntpool++] = t; }

/* per function analysis */
static u8 *ltype; static u32 nlocals, nparams;      /* locals of the function being worked on */
static u8 *in_s;          /* function is in S */
static u8 *type_reaches;  /* call_indirect of this type can reach fork */
static int mode_all;
static int unwind_check; /* --unwind=check: a flag test after each call instead of an exception */
static u32 wrap;         /* blocks the wrapper puts around the body: 2 (try_table) or 1 (check) */
static int f_point, f_ctl, f_catch, f_unwound, f_root;

static int is_instrumented_call(int op, u32 target) {
  if (op == 0x10) return in_s[target] || (int)target == f_root;
  return mode_all || type_reaches[target];
}

/* decode the blocktype at r: returns 0x40, a value type, or 0x100 + type index */
static u32 rblocktype(rd *r) {
  u8 c = *r->p;
  if (c == 0x40 || c == I32 || c == I64 || c == F32 || c == F64 || c == V128 || c == FUNCREF || c == EXTERNREF) { r->p++; return c; }
  s64 t = rsleb(r);
  if (t < 0 || t >= ntypes) die("bad block type");
  return 0x100 + (u32)t;
}
static int bt_results(u32 bt, u8 *rt) {
  if (bt == 0x40) return 0;
  if (bt < 0x100) { *rt = (u8)bt; return 1; }
  struct functype *f = &types[bt - 0x100];
  if (f->np) die("blocks with parameters are not supported");
  if (f->nr > 1) die("blocks with several results are not supported");
  if (f->nr) *rt = f->r[0];
  return f->nr;
}

/* Decode a body into ins[], with types: the operand stack of the current block at each
 * instrumented call is recorded. Also numbers the call sites and finds each if's range. */
struct ctl { u32 height; u32 bt; int dead; int ins_if; int kind; };
static void analyze(const u8 *body, u32 len, int f, int instrument) {
  rd r = {body, body + len};
  nins = 0; ntpool = 0;
  nparams = types[ftype[f]].np;
  nlocals = nparams;
  ltype = xrealloc(ltype, nparams + 1);
  memcpy(ltype, types[ftype[f]].p, nparams);
  u32 groups = ruleb(&r);
  for (u32 g = 0; g < groups; g++) {
    u32 cnt = ruleb(&r);
    u8 t = rvaltype(&r);
    if (nlocals + cnt > 100000) die("too many locals");
    ltype = xrealloc(ltype, nlocals + cnt + 1);
    for (u32 k = 0; k < cnt; k++) ltype[nlocals++] = t;
  }
  static u8 *stk; static u32 cap_stk;
  u32 sp = 0;
  static struct ctl *cs; static u32 cap_cs;
  u32 csp = 0;
  int site = 0;
#define PUSH(t) do { if (sp == cap_stk) { cap_stk = cap_stk ? cap_stk * 2 : 256; stk = xrealloc(stk, cap_stk); } stk[sp++] = (t); } while (0)
#define POP() (sp > cs[csp - 1].height ? stk[--sp] : (cs[csp - 1].dead ? ANY : (die("operand stack underflow at byte %u", (unsigned)(i0 - body)), 0)))
#define CTL(k, b, ii) do { if (csp == cap_cs) { cap_cs = cap_cs ? cap_cs * 2 : 64; cs = xrealloc(cs, cap_cs * sizeof *cs); } cs[csp++] = (struct ctl){sp, (b), 0, (ii), (k)}; } while (0)
  CTL(K_BLOCK, types[ftype[f]].nr ? types[ftype[f]].r[0] : 0x40, -1);
  if (types[ftype[f]].nr > 1) die("functions with several results are not supported");
  while (r.p < r.end) {
    const u8 *i0 = r.p;
    u8 op = rbyte(&r);
    if (nins == cap_ins) { cap_ins = cap_ins ? cap_ins * 2 : 1024; ins = xrealloc(ins, cap_ins * sizeof *ins); }
    insn *in = &ins[nins];
    memset(in, 0, sizeof *in);
    in->op = op; in->kind = K_PLAIN; in->site = -1; in->depth = csp - 1; in->dead = cs[csp - 1].dead;
    in->start = (u32)(i0 - body);
    u8 t;
    switch (op) {
    case 0x00: cs[csp - 1].dead = 1; sp = cs[csp - 1].height; in->kind = K_BRANCH; break; /* unreachable */
    case 0x01: break;
    case 0x02: case 0x03: { u32 bt = rblocktype(&r); u8 rt; bt_results(bt, &rt); if (sp != cs[csp - 1].height && !cs[csp - 1].dead) die("value on the stack at a block start"); in->kind = op == 2 ? K_BLOCK : K_LOOP; CTL(in->kind, bt, -1); break; }
    case 0x04: { u32 bt = rblocktype(&r); u8 rt; bt_results(bt, &rt); POP(); if (sp != cs[csp - 1].height && !cs[csp - 1].dead) die("value on the stack at an if"); in->kind = K_IF; in->lo = site; CTL(K_IF, bt, nins); break; }
    case 0x05: {
      struct ctl *c = &cs[csp - 1];
      if (c->kind != K_IF) die("else without if");
      in->kind = K_ELSE; in->nres = (u8)bt_results(c->bt, &in->rtype);
      if (!c->dead && sp != c->height + in->nres) die("unexpected stack height at else");
      ins[c->ins_if].hi = site;
      sp = c->height; c->dead = 0; c->kind = K_ELSE;
      in->depth = csp - 2; /* the else belongs to the if's level */
      break;
    }
    case 0x0b: {
      struct ctl *c = &cs[csp - 1];
      in->kind = K_END; in->nres = (u8)bt_results(c->bt, &in->rtype);
      if (!c->dead && sp != c->height + in->nres) die("unexpected stack height at end (byte %u)", in->start);
      if (c->kind == K_IF) ins[c->ins_if].hi = site;
      sp = c->height;
      csp--;
      in->depth = csp ? csp - 1 : 0;
      for (int k = 0; k < in->nres; k++) PUSH(in->rtype);
      if (!csp) { in->end = (u32)(r.p - body); nins++; if (r.p != r.end) die("code after the end of the function"); goto done; }
      break;
    }
    case 0x0c: ruleb(&r); cs[csp - 1].dead = 1; sp = cs[csp - 1].height; in->kind = K_BRANCH; break;
    case 0x0d: ruleb(&r); POP(); in->kind = K_BRANCH; break;
    case 0x0e: { u32 n = ruleb(&r); for (u32 k = 0; k <= n; k++) ruleb(&r); POP(); cs[csp - 1].dead = 1; sp = cs[csp - 1].height; in->kind = K_BRANCH; break; }
    case 0x0f: cs[csp - 1].dead = 1; sp = cs[csp - 1].height; in->kind = K_BRANCH; break;
    case 0x10: case 0x11: {
      u32 x = ruleb(&r), ty = x;
      if (op == 0x11) { if (ruleb(&r) != 0) die("call_indirect on a table other than 0"); }
      else { if (x >= (u32)nfunc) die("bad call"); ty = ftype[x]; }
      struct functype *ft = &types[ty];
      if (ft->nr > 1) die("calls with several results are not supported");
      int instr = instrument && !cs[csp - 1].dead && is_instrumented_call(op, x);
      if (instr) {
        u32 nargs = ft->np + (op == 0x11), h = cs[csp - 1].height;
        if (sp < h + nargs) die("operand stack underflow at a call");
        in->below = ntpool; in->nbelow = sp - nargs - h; in->nargs = nargs;
        for (u32 k = h; k < sp; k++) { u8 ty2 = stk[k]; if (ty2 != I32 && ty2 != I64 && ty2 != F32 && ty2 != F64) die("a %s value is live across a call that can fork", ty2 == V128 ? "v128" : "reference"); tpush(ty2); }
        in->kind = op == 0x10 ? K_CALL : K_CALLI;
        in->site = site++;
      }
      in->target = (int)x;
      in->nres = (u8)ft->nr; in->rtype = ft->nr ? ft->r[0] : 0;
      for (u32 k = 0; k < (u32)ft->np + (op == 0x11); k++) POP();
      if (ft->nr) PUSH(ft->r[0]);
      break;
    }
    case 0x12: case 0x13: die("tail calls are not supported");
    case 0x06: case 0x07: case 0x18: case 0x19: case 0x09: die("legacy exception instructions are not supported");
    case 0x08: { u32 tag = ruleb(&r); (void)tag; cs[csp - 1].dead = 1; sp = cs[csp - 1].height; in->kind = K_BRANCH; break; } /* throw: our tags take no values; others only appear in sjlj code */
    case 0x0a: POP(); cs[csp - 1].dead = 1; sp = cs[csp - 1].height; in->kind = K_BRANCH; break; /* throw_ref */
    case 0x1f: { /* try_table */
      u32 bt = rblocktype(&r); u8 rt; bt_results(bt, &rt);
      u32 n = ruleb(&r);
      for (u32 k = 0; k < n; k++) { u8 kind = rbyte(&r); if (kind < 2) ruleb(&r); ruleb(&r); }
      if (sp != cs[csp - 1].height && !cs[csp - 1].dead) die("value on the stack at a try_table");
      in->kind = K_TRY; CTL(K_TRY, bt, -1);
      break;
    }
    case 0x1a: POP(); break;
    case 0x1b: { POP(); u8 a = POP(); POP(); PUSH(a); break; }
    case 0x1c: { u32 n = ruleb(&r); if (n != 1) die("bad select"); u8 a = rvaltype(&r); POP(); POP(); POP(); PUSH(a); break; }
    case 0x20: { u32 x = ruleb(&r); if (x >= nlocals) die("bad local"); PUSH(ltype[x]); break; }
    case 0x21: { u32 x = ruleb(&r); if (x >= nlocals) die("bad local"); POP(); break; }
    case 0x22: { u32 x = ruleb(&r); if (x >= nlocals) die("bad local"); POP(); PUSH(ltype[x]); break; }
    case 0x23: { u32 x = ruleb(&r); if (x >= (u32)nglob) die("bad global"); PUSH(gtype[x]); break; }
    case 0x24: ruleb(&r); POP(); break;
    case 0x25: ruleb(&r); POP(); PUSH(FUNCREF); break;
    case 0x26: ruleb(&r); POP(); POP(); break;
    case 0x3f: ruleb(&r); PUSH(I32); break;
    case 0x40: ruleb(&r); POP(); PUSH(I32); break;
    case 0x41: rsleb(&r); PUSH(I32); break;
    case 0x42: rsleb(&r); PUSH(I64); break;
    case 0x43: r.p += 4; PUSH(F32); break;
    case 0x44: r.p += 8; PUSH(F64); break;
    case 0xd0: rbyte(&r); PUSH(FUNCREF); break;
    case 0xd1: POP(); PUSH(I32); break;
    case 0xd2: ruleb(&r); PUSH(FUNCREF); break;
    case 0xfc: {
      u32 sub = ruleb(&r);
      if (sub <= 7) { POP(); PUSH(sub < 4 ? I32 : I64); }
      else if (sub == 8) { ruleb(&r); ruleb(&r); POP(); POP(); POP(); }
      else if (sub == 9 || sub == 13) ruleb(&r);
      else if (sub == 10 || sub == 12 || sub == 14) { ruleb(&r); ruleb(&r); POP(); POP(); POP(); }
      else if (sub == 11 || sub == 17) { ruleb(&r); POP(); POP(); POP(); }
      else if (sub == 15) { ruleb(&r); POP(); POP(); PUSH(I32); }
      else if (sub == 16) { ruleb(&r); PUSH(I32); }
      else die("unsupported instruction 0xfc %u", sub);
      break;
    }
    case 0xfd: die("SIMD instructions are not supported");
    case 0xfe: die("atomic instructions are not supported");
    default:
      if (op >= 0x28 && op <= 0x3e) { /* loads and stores */
        u32 align = ruleb(&r); if (align & 0x40) ruleb(&r); ruleb(&r);
        if (op <= 0x35) { POP(); t = op == 0x28 || (op >= 0x2c && op <= 0x2f) ? I32 : op == 0x29 || op >= 0x30 ? I64 : op == 0x2a ? F32 : F64; PUSH(t); }
        else { POP(); POP(); }
      } else if (op >= 0x45 && op <= 0xc4) {
        int npop; u8 res;
        if (op == 0x45 || op == 0x50) npop = 1, res = I32;
        else if (op <= 0x66) npop = 2, res = I32;
        else if (op <= 0x69) npop = 1, res = I32;
        else if (op <= 0x78) npop = 2, res = I32;
        else if (op <= 0x7b) npop = 1, res = I64;
        else if (op <= 0x8a) npop = 2, res = I64;
        else if (op <= 0x91) npop = 1, res = F32;
        else if (op <= 0x98) npop = 2, res = F32;
        else if (op <= 0x9f) npop = 1, res = F64;
        else if (op <= 0xa6) npop = 2, res = F64;
        else {
          npop = 1;
          static const u8 conv[] = { /* 0xa7 .. 0xc4 */
            I32, I32, I32, I32, I32, I64, I64, I64, I64, I64, I64, F32, F32, F32, F32, F32,
            F64, F64, F64, F64, F64, I32, I64, F32, F64, I32, I32, I64, I64, I64 };
          res = conv[op - 0xa7];
        }
        while (npop--) POP();
        PUSH(res);
      } else die("unsupported instruction 0x%x at byte %u", op, in->start);
    }
    in->end = (u32)(r.p - body);
    nins++;
  }
  die("function body without end");
done:;
#undef PUSH
#undef POP
#undef CTL
}

/* ---- code generation ---- */
static int g_state, g_fp, g_end, g_result, tag_fork, ty_void, ty_ret_i32;
static int *rw_of; /* function -> its rewinding copy, or -1 */
/* locals added to the function being rewritten */
static u32 pool_base[4], pool_n[4], l_site, l_cond, nlocals_out;
static int tix(u8 t) { return t == I32 ? 0 : t == I64 ? 1 : t == F32 ? 2 : 3; }
static u32 frame_size, *loff;

static void plan_locals(void) {
  memset(pool_n, 0, sizeof pool_n);
  for (int i = 0; i < nins; i++) {
    insn *in = &ins[i];
    if (in->site < 0) continue;
    u32 c[4] = {0};
    for (u32 k = 0; k < in->nbelow + in->nargs; k++) c[tix(tpool[in->below + k])]++;
    if (in->nres) c[tix(in->rtype)]++;
    for (int k = 0; k < 4; k++) if (c[k] > pool_n[k]) pool_n[k] = c[k];
  }
  for (int k = 0; k < 4; k++) if (pool_n[k] < 1) pool_n[k] = 1; /* one of each, for block results */
  u32 n = nlocals;
  for (int k = 0; k < 4; k++) { pool_base[k] = n; n += pool_n[k]; }
  l_site = n++;
  l_cond = n++;
  nlocals_out = n;
  /* the frame: function number, site, then the locals (8-byte ones first) */
  loff = xrealloc(loff, sizeof *loff * nlocals_out);
  u32 off = 8;
  for (int pass = 0; pass < 2; pass++)
    for (u32 l = 0; l < nlocals_out; l++) {
      u8 t = l < nlocals ? ltype[l] : l < pool_base[1] ? I32 : l < pool_base[2] ? I64 : l < pool_base[3] ? F32 : l < l_site ? F64 : I32;
      if (t != I32 && t != I64 && t != F32 && t != F64) die("a local of type 0x%x cannot be saved", t);
      int wide = t == I64 || t == F64;
      if (wide != (pass == 0)) continue;
      loff[l] = off; off += wide ? 8 : 4;
    }
  frame_size = (off + 7) & ~7u;
}
static u8 local_type(u32 l) { return l < nlocals ? ltype[l] : l < pool_base[1] ? I32 : l < pool_base[2] ? I64 : l < pool_base[3] ? F32 : l < l_site ? F64 : I32; }
static void emit_locals(buf *o) { /* runs of one type */
  u32 groups = 0;
  for (u32 l = nparams; l < nlocals_out; l++) if (l == nparams || local_type(l) != local_type(l - 1)) groups++;
  buleb(o, groups);
  for (u32 l = nparams; l < nlocals_out;) {
    u32 e = l;
    while (e < nlocals_out && local_type(e) == local_type(l)) e++;
    buleb(o, e - l); bput(o, local_type(l));
    l = e;
  }
}
static void op_u(buf *o, u8 op, u64 v) { bput(o, op); buleb(o, v); }
static void i32c(buf *o, s32 v) { bput(o, 0x41); bsleb(o, v); }
static void mem_op(buf *o, u8 op, u32 align, u32 off) { bput(o, op); buleb(o, align); buleb(o, off); }
static u8 store_op(u8 t) { return t == I32 ? 0x36 : t == I64 ? 0x37 : t == F32 ? 0x38 : 0x39; }
static u8 load_op(u8 t) { return t == I32 ? 0x28 : t == I64 ? 0x29 : t == F32 ? 0x2a : 0x2b; }
static u32 lg_align(u8 t) { return t == I64 || t == F64 ? 3 : 2; }
static void zero_of(buf *o, u8 t) {
  if (t == I32) i32c(o, 0);
  else if (t == I64) { bput(o, 0x42); bsleb(o, 0); }
  else if (t == F32) { bput(o, 0x43); for (int k = 0; k < 4; k++) bput(o, 0); }
  else { bput(o, 0x44); for (int k = 0; k < 8; k++) bput(o, 0); }
}

/* The handler, after the body's try_table caught the fork tag: save this frame, throw again. */
static void keep_unwinding(buf *o, int f) { /* throw again, or return to the caller, which checks the state */
  if (!unwind_check) { op_u(o, 0x08, tag_fork); return; }
  struct functype *ft = &types[ftype[f]];
  if (ft->nr) zero_of(o, ft->r[0]);
  bput(o, 0x0f);
}
static void emit_handler(buf *o, int f) {
  /* full: mark the overflow (fp = end + 1) and keep unwinding without saving */
  op_u(o, 0x23, g_fp); i32c(o, (s32)frame_size); bput(o, 0x6a); op_u(o, 0x23, g_end); bput(o, 0x4b); /* i32.gt_u */
  bput(o, 0x04); bput(o, 0x40);
  op_u(o, 0x23, g_end); i32c(o, 1); bput(o, 0x6a); op_u(o, 0x24, g_fp); keep_unwinding(o, f);
  bput(o, 0x0b);
  op_u(o, 0x23, g_fp); i32c(o, f); mem_op(o, 0x36, 2, 0);
  op_u(o, 0x23, g_fp); op_u(o, 0x20, l_site); mem_op(o, 0x36, 2, 4);
  for (u32 l = 0; l < nlocals_out; l++) { u8 t = local_type(l); op_u(o, 0x23, g_fp); op_u(o, 0x20, l); mem_op(o, store_op(t), lg_align(t), loff[l]); }
  op_u(o, 0x23, g_fp); i32c(o, (s32)frame_size); bput(o, 0x6a); op_u(o, 0x24, g_fp);
  keep_unwinding(o, f);
}
/* F$rw entry: if rewinding, pop this function's frame into its locals */
static void emit_restore(buf *o, int f) {
  op_u(o, 0x23, g_state); i32c(o, 2); bput(o, 0x46); bput(o, 0x04); bput(o, 0x40);
  op_u(o, 0x23, g_fp); i32c(o, (s32)frame_size); bput(o, 0x6b); op_u(o, 0x24, g_fp);
  op_u(o, 0x23, g_fp); mem_op(o, 0x28, 2, 0); i32c(o, f); bput(o, 0x47); bput(o, 0x04); bput(o, 0x40); bput(o, 0x00); bput(o, 0x0b); /* wrong frame: trap */
  for (u32 l = 0; l < nlocals_out; l++) { u8 t = local_type(l); op_u(o, 0x23, g_fp); mem_op(o, load_op(t), lg_align(t), loff[l]); op_u(o, 0x21, l); }
  op_u(o, 0x23, g_fp); mem_op(o, 0x28, 2, 4); op_u(o, 0x21, l_site);
  bput(o, 0x0b);
}
static void emit_bt(buf *o, int nres, u8 rt) { bput(o, nres ? rt : 0x40); }
/* re-emit a branch with label L -> L + inner + (2 if it targets the function) */
static void emit_branch(buf *o, const u8 *body, insn *in, u32 inner) {
  rd r = {body + in->start, body + in->end};
  u8 op = rbyte(&r);
  bput(o, op);
  if (op == 0x0c || op == 0x0d) { u32 L = ruleb(&r); buleb(o, L + inner + (L == in->depth ? wrap : 0)); }
  else if (op == 0x0e) {
    u32 n = ruleb(&r); buleb(o, n);
    for (u32 k = 0; k <= n; k++) { u32 L = ruleb(&r); buleb(o, L + inner + (L == in->depth ? wrap : 0)); }
  } else bmem(o, r.p, r.end - r.p); /* return, unreachable, throw */
}
static void emit_try(buf *o, const u8 *body, insn *in) { /* a try_table of the program: catch labels that leave the function shift by 2 */
  rd r = {body + in->start + 1, body + in->end};
  bput(o, 0x1f);
  const u8 *bt0 = r.p; rblocktype(&r); bmem(o, bt0, r.p - bt0);
  u32 n = ruleb(&r); buleb(o, n);
  for (u32 k = 0; k < n; k++) {
    u8 kind = rbyte(&r); bput(o, kind);
    if (kind < 2) buleb(o, ruleb(&r));
    u32 L = ruleb(&r); buleb(o, L + (L == in->depth ? wrap : 0));
  }
}
static void emit_raw(buf *o, const u8 *body, insn *in) { bmem(o, body + in->start, in->end - in->start); }

/* spill slots of an instrumented call: below values, then arguments, then the result */
static void site_slots(insn *in, u32 *slot, u32 *rslot) {
  u32 used[4] = {0};
  for (u32 k = 0; k < in->nbelow + in->nargs; k++) { int t = tix(tpool[in->below + k]); slot[k] = pool_base[t] + used[t]++; }
  if (in->nres) { int t = tix(in->rtype); *rslot = pool_base[t] + used[t]++; }
}

static void emit_wrapper_open(buf *o, int f) {
  struct functype *ft = &types[ftype[f]];
  bput(o, 0x02); bput(o, 0x40);                         /* block $h */
  if (unwind_check) return;                             /* calls branch to $h when the state says "unwinding" */
  bput(o, 0x1f); emit_bt(o, ft->nr, ft->nr ? ft->r[0] : 0); /* try_table (result) (catch $fork $h) */
  buleb(o, 1); bput(o, 0x00); buleb(o, tag_fork); buleb(o, 0);
}
static void emit_wrapper_close(buf *o, int f) {
  bput(o, 0x0f);  /* return: the try_table's end has just been emitted (check: the body's results) */
  bput(o, 0x0b);  /* end $h */
  emit_handler(o, f);
  bput(o, 0x0b);  /* end of function */
}

/* --unwind=check: after a call that can fork, leave for the handler if the state says so */
static void emit_check(buf *o, insn *in, u32 inner) {
  if (!unwind_check) return;
  op_u(o, 0x23, g_state); i32c(o, 1); bput(o, 0x46); bput(o, 0x04); bput(o, 0x40);
  i32c(o, in->site); op_u(o, 0x21, l_site);
  bput(o, 0x0c); buleb(o, in->depth + inner + 1);
  bput(o, 0x0b);
}
/* The function as it runs normally: the original code plus the try wrapper and the call site
 * numbers (and spills, where values sit under the arguments). */
static void gen_normal(buf *o, const u8 *body, int f) {
  emit_locals(o);
  emit_wrapper_open(o, f);
  for (int i = 0; i < nins; i++) {
    insn *in = &ins[i];
    if (in->kind == K_BRANCH) { emit_branch(o, body, in, 0); continue; }
    if (in->kind == K_TRY) { emit_try(o, body, in); continue; }
    if (unwind_check && i == nins - 1) continue; /* the function's end: emit_wrapper_close returns */
    if (in->site < 0) { emit_raw(o, body, in); continue; }
    u32 slot[80], rslot = 0;
    if (in->nbelow + in->nargs > 80) die("too many values at a call");
    if (!in->nbelow && !unwind_check) { i32c(o, in->site); op_u(o, 0x21, l_site); emit_raw(o, body, in); continue; }
    if (!in->nbelow) { emit_raw(o, body, in); emit_check(o, in, 0); continue; } /* the result stays on the stack */
    site_slots(in, slot, &rslot);
    for (int k = (int)(in->nbelow + in->nargs) - 1; k >= 0; k--) op_u(o, 0x21, slot[k]);
    if (!unwind_check) { i32c(o, in->site); op_u(o, 0x21, l_site); }
    for (u32 k = in->nbelow; k < in->nbelow + in->nargs; k++) op_u(o, 0x20, slot[k]);
    emit_raw(o, body, in);
    if (in->nres) op_u(o, 0x21, rslot);
    emit_check(o, in, 0);
    for (u32 k = 0; k < in->nbelow; k++) op_u(o, 0x20, slot[k]);
    if (in->nres) op_u(o, 0x20, rslot);
  }
  emit_wrapper_close(o, f);
}

/* The rewinding copy. Code between control instructions and calls is guarded by
 * "if (state != REWIND)"; the operand stack is empty at each guard edge (values that must
 * cross one are kept in locals: `pending`). */
static u32 pending[82], npending;
static int guard_open;
static void guard_on(buf *o) {
  if (guard_open) return;
  op_u(o, 0x23, g_state); i32c(o, 2); bput(o, 0x47); bput(o, 0x04); bput(o, 0x40);
  guard_open = 1;
  for (u32 k = 0; k < npending; k++) op_u(o, 0x20, pending[k]);
  npending = 0;
}
static void guard_off(buf *o) { if (guard_open) { bput(o, 0x0b); guard_open = 0; } }
/* before else/end: leave the block's result on the stack, outside any guard */
static void settle_results(buf *o, insn *in) {
  if (guard_open) {
    if (in->nres && !in->dead) { u32 l = pool_base[tix(in->rtype)]; op_u(o, 0x21, l); guard_off(o); op_u(o, 0x20, l); return; }
    guard_off(o);
    if (in->nres) zero_of(o, in->rtype); /* the tail was unreachable code */
    return;
  }
  if (npending) { for (u32 k = 0; k < npending; k++) op_u(o, 0x20, pending[k]); if (npending != in->nres) die("internal: %u values pending at a block end needing %u", npending, in->nres); npending = 0; return; }
  if (in->nres) zero_of(o, in->rtype);
}
static void gen_rewind(buf *o, const u8 *body, int f) {
  emit_locals(o);
  emit_restore(o, f);
  emit_wrapper_open(o, f);
  guard_open = 0; npending = 0;
  for (int i = 0; i < nins; i++) {
    insn *in = &ins[i];
    switch (in->kind) {
    case K_BLOCK: case K_LOOP: case K_TRY:
      if (npending) die("internal: values pending at a block start");
      guard_off(o);
      if (in->kind == K_TRY) emit_try(o, body, in); else emit_raw(o, body, in);
      break;
    case K_IF: {
      /* the condition is on the stack inside the guard (or pending from a call) */
      guard_on(o);
      op_u(o, 0x21, l_cond);
      guard_off(o);
      if (in->lo < in->hi) { /* rewinding: take the then arm if the saved site is in it */
        op_u(o, 0x23, g_state); i32c(o, 2); bput(o, 0x46); bput(o, 0x04); bput(o, 0x40);
        op_u(o, 0x20, l_site); i32c(o, in->lo); bput(o, 0x4f); /* ge_u */
        op_u(o, 0x20, l_site); i32c(o, in->hi); bput(o, 0x49); /* lt_u */
        bput(o, 0x71); op_u(o, 0x21, l_cond);
        bput(o, 0x0b);
      } else { /* no call site in the then arm: rewinding goes to the else arm */
        op_u(o, 0x23, g_state); i32c(o, 2); bput(o, 0x46); bput(o, 0x04); bput(o, 0x40); i32c(o, 0); op_u(o, 0x21, l_cond); bput(o, 0x0b);
      }
      op_u(o, 0x20, l_cond);
      emit_raw(o, body, in);
      break;
    }
    case K_ELSE: case K_END:
      settle_results(o, in);
      if (in->kind == K_END && i == nins - 1) { if (!unwind_check) bput(o, 0x0b); break; } /* the function's end closes the try_table */
      emit_raw(o, body, in);
      if (in->kind == K_END && in->nres) { /* keep the block's result in a local: the next guard must see it */
        u32 l = pool_base[tix(in->rtype)];
        op_u(o, 0x21, l);
        pending[npending++] = l;
      }
      break;
    case K_BRANCH:
      guard_on(o);
      emit_branch(o, body, in, 1);
      break;
    case K_CALL: case K_CALLI:
      if (in->site >= 0) {
        u32 slot[80], rslot = 0;
        if (in->nbelow + in->nargs > 80) die("too many values at a call");
        guard_on(o);
        site_slots(in, slot, &rslot);
        for (int k = (int)(in->nbelow + in->nargs) - 1; k >= 0; k--) op_u(o, 0x21, slot[k]);
        guard_off(o);
        op_u(o, 0x23, g_state); i32c(o, 2); bput(o, 0x46); bput(o, 0x04); bput(o, 0x40); /* rewinding? */
        op_u(o, 0x20, l_site); i32c(o, in->site); bput(o, 0x46); bput(o, 0x04); bput(o, 0x40); /* this site? */
        for (u32 k = in->nbelow; k < in->nbelow + in->nargs; k++) op_u(o, 0x20, slot[k]);
        if (in->kind == K_CALL) {
          int callee = rw_of[in->target];
          if (callee < 0) die("internal: no rewinding copy of %s", fn_name(in->target));
          op_u(o, 0x10, callee);
        } else { bput(o, 0x11); buleb(o, in->target); buleb(o, ntables); } /* the table of rewinding copies */
        if (in->nres) op_u(o, 0x21, rslot);
        emit_check(o, in, 2);
        bput(o, 0x0b);
        bput(o, 0x05); /* else: run normally */
        i32c(o, in->site); op_u(o, 0x21, l_site);
        for (u32 k = in->nbelow; k < in->nbelow + in->nargs; k++) op_u(o, 0x20, slot[k]);
        emit_raw(o, body, in);
        if (in->nres) op_u(o, 0x21, rslot);
        emit_check(o, in, 1);
        bput(o, 0x0b);
        npending = 0;
        for (u32 k = 0; k < in->nbelow; k++) pending[npending++] = slot[k];
        if (in->nres) pending[npending++] = rslot;
        break;
      }
      /* a call that cannot fork */ __attribute__((fallthrough));
    default:
      guard_on(o);
      emit_raw(o, body, in);
    }
  }
  emit_wrapper_close(o, f);
}

/* ---- the call graph ---- */
static void build_s(void) {
  in_s = xrealloc(NULL, nfunc); memset(in_s, 0, nfunc);
  type_reaches = xrealloc(NULL, ntypes + 4); memset(type_reaches, 0, ntypes + 4);
  /* direct callees and indirect call types of every defined function */
  int changed = 1;
  struct edges { int n; int *to; int ni; int *ity; } *E = xrealloc(NULL, sizeof *E * nfunc);
  memset(E, 0, sizeof *E * nfunc);
  for (int f = nimpf; f < nfunc; f++) {
    cur_fn = fn_name(f);
    analyze(bodies[f - nimpf].p, bodies[f - nimpf].len, f, 0);
    for (int i = 0; i < nins; i++) {
      if (ins[i].op == 0x10) { E[f].to = xrealloc(E[f].to, sizeof(int) * (E[f].n + 1)); E[f].to[E[f].n++] = ins[i].target; }
      if (ins[i].op == 0x11) { E[f].ity = xrealloc(E[f].ity, sizeof(int) * (E[f].ni + 1)); E[f].ity[E[f].ni++] = ins[i].target; }
    }
  }
  cur_fn = "";
  int special[] = {f_point, f_ctl, f_catch};
  if (mode_all) {
    for (int f = nimpf; f < nfunc; f++) {
      int calls = E[f].ni > 0;
      for (int k = 0; k < E[f].n; k++) if (E[f].to[k] >= nimpf) calls = 1;
      in_s[f] = (u8)calls;
    }
    for (int k = 0; k < 3; k++) if (special[k] >= 0) in_s[special[k]] = 0;
    for (int t = 0; t < ntypes; t++) type_reaches[t] = 1;
    return;
  }
  while (changed) {
    changed = 0;
    for (u32 i = 0; i < tab0_size; i++) if (tab0[i] >= 0 && in_s[tab0[i]] && !type_reaches[ftype[tab0[i]]]) { type_reaches[ftype[tab0[i]]] = 1; changed = 1; }
    for (int f = nimpf; f < nfunc; f++) {
      if (in_s[f] || f == f_catch || f == f_point || f == f_ctl || f == f_root) continue;
      int r = 0;
      for (int k = 0; k < E[f].n && !r; k++) r = E[f].to[k] == f_root || in_s[E[f].to[k]];
      for (int k = 0; k < E[f].ni && !r; k++) r = type_reaches[E[f].ity[k]];
      if (r) { in_s[f] = 1; changed = 1; }
    }
  }
}

/* ---- writing the module ---- */
static void sec_out(buf *o, u8 id, const buf *c) { bput(o, id); buleb(o, c->n); bbuf(o, c); }
static void sec_raw(buf *o, int i) { bput(o, secs[i].id); buleb(o, secs[i].size); bmem(o, secs[i].data, secs[i].size); }
static int add_type(int np, const u8 *p, int nr, const u8 *r) {
  for (int t = 0; t < ntypes; t++) if (type_eq(t, np, p, nr, r)) return t;
  struct functype *f = &types[ntypes];
  f->np = np; f->nr = nr; memcpy(f->p, p, np); memcpy(f->r, r, nr);
  return ntypes++;
}

int main(int argc, char **argv) {
  const char *in = NULL, *out = NULL, *root = NULL;
  int stats = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--all")) mode_all = 1;
    else if (!strncmp(argv[i], "--root=", 7)) root = argv[i] + 7;
    else if (!strcmp(argv[i], "--stats")) stats = 1;
    else if (!strcmp(argv[i], "--unwind=check")) unwind_check = 1;
    else if (!strcmp(argv[i], "--unwind=throw")) unwind_check = 0;
    else if (!in) in = argv[i];
    else if (!out) out = argv[i];
    else die("usage: wasmfork [--all | --root=NAME] [--stats] in.wasm out.wasm");
  }
  if (!in || !out) die("usage: wasmfork [--all | --root=NAME] [--stats] in.wasm out.wasm");
  FILE *fp = fopen(in, "rb");
  if (!fp) die("cannot open %s", in);
  fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
  u8 *m = xrealloc(NULL, n);
  if (fread(m, 1, n, fp) != (size_t)n) die("cannot read %s", in);
  fclose(fp);
  parse(m, n);
  f_point = func_by_name("__fork_point");
  f_ctl = func_by_name("__fork_ctl");
  f_catch = func_by_name("__fork_catch");
  f_unwound = func_by_name("__fork_unwound");
  f_root = root ? func_by_name(root) : f_point;
  if (root && f_root < 0) die("no function named %s", root);
  int have_hooks = f_point >= 0 && f_ctl >= 0 && f_catch >= 0 && f_unwound >= 0;
  if (!have_hooks && (f_point >= 0 || f_ctl >= 0 || f_catch >= 0 || f_unwound >= 0))
    die("%s has only some of __fork_point, __fork_ctl, __fork_catch, __fork_unwound (names section stripped?)", in);
  if (!have_hooks && !mode_all && !root) { /* the program does not use fork: nothing to do */
    if (stats) fprintf(stderr, "wasmfork: %s does not call fork; copied unchanged\n", in);
    fp = fopen(out, "wb"); if (!fp || fwrite(m, 1, n, fp) != (size_t)n) die("cannot write %s", out); fclose(fp);
    return 0;
  }
  if (find_sec(10) < 0 || find_sec(4) < 0) die("the module has no code or no table");
  if (ntables > 1) die("%s already has a second table: transformed already?", in);
  wrap = unwind_check ? 1 : 2;
  build_s();

  /* new types, globals, tag, functions */
  u8 i32t = I32;
  ty_void = add_type(0, NULL, 0, NULL);
  ty_ret_i32 = add_type(0, NULL, 1, &i32t);
  g_state = nglob; g_fp = nglob + 1; g_end = nglob + 2; g_result = nglob + 3;
  tag_fork = ntags;
  rw_of = xrealloc(NULL, sizeof(int) * nfunc);
  int next = nfunc, ns = 0;
  for (int f = 0; f < nfunc; f++) rw_of[f] = in_s[f] ? (ns++, next++) : -1;
  int f_point_rw = -1;
  if (f_root >= 0) { f_point_rw = next++; rw_of[f_root] = f_point_rw; }
  int f_trap = next++;
  int nnew = next - nfunc;
  u32 *new_type = xrealloc(NULL, sizeof(u32) * nnew);
  for (int f = 0; f < nfunc; f++) if (in_s[f]) new_type[rw_of[f] - nfunc] = ftype[f];
  if (f_point_rw >= 0) new_type[f_point_rw - nfunc] = ftype[f_root];
  new_type[f_trap - nfunc] = ty_void;

  /* the code */
  buf code = {0};
  u32 ndef = nfunc - nimpf;
  buleb(&code, ndef + nnew);
  buf *rwbody = xrealloc(NULL, sizeof(buf) * nfunc);
  memset(rwbody, 0, sizeof(buf) * nfunc);
  size_t old_code = 0, s_code = 0;
  for (u32 d = 0; d < ndef; d++) {
    int f = nimpf + d;
    old_code += bodies[d].len;
    buf b = {0};
    cur_fn = fn_name(f);
    if (f == f_point && have_hooks && f_root == f_point) { /* throw the fork tag (or set the state) */
      buleb(&b, 0);
      if (unwind_check) { i32c(&b, 1); op_u(&b, 0x24, g_state); i32c(&b, 0); }
      else op_u(&b, 0x08, tag_fork);
      bput(&b, 0x0b);
    } else if (f == f_ctl && have_hooks) { /* (op, val): see enum FC_* in libc/src/fork.c */
      buleb(&b, 0);
      /* 0 ready, 1 get state, 2 set state, 3 get fp, 4 set fp, 5 get result, 6 set result, 7 set end */
      int g[] = {-1, g_state, g_state, g_fp, g_fp, g_result, g_result, g_end};
      for (int k = 0; k < 8; k++) {
        op_u(&b, 0x20, 0); i32c(&b, k); bput(&b, 0x46); bput(&b, 0x04); bput(&b, 0x40);
        if (k == 0) i32c(&b, 1);
        else if (k == 1 || k == 3 || k == 5) op_u(&b, 0x23, g[k]);
        else { op_u(&b, 0x20, 1); op_u(&b, 0x24, g[k]); i32c(&b, 0); }
        bput(&b, 0x0f); bput(&b, 0x0b);
      }
      i32c(&b, 0); bput(&b, 0x0b);
    } else if (f == f_catch && have_hooks && unwind_check) {
      /* loop { r = rw ? call_indirect t1 : call_indirect t0; if (state != UNWIND) return r; unwound(); rw = 1 } */
      buleb(&b, 1); buleb(&b, 1); bput(&b, I32);
      bput(&b, 0x03); bput(&b, 0x40);
      op_u(&b, 0x20, 1); bput(&b, 0x04); bput(&b, I32);
      op_u(&b, 0x20, 0); bput(&b, 0x11); buleb(&b, ty_ret_i32); buleb(&b, ntables);
      bput(&b, 0x05);
      op_u(&b, 0x20, 0); bput(&b, 0x11); buleb(&b, ty_ret_i32); buleb(&b, 0);
      bput(&b, 0x0b);
      op_u(&b, 0x21, 2);
      op_u(&b, 0x23, g_state); i32c(&b, 1); bput(&b, 0x47); bput(&b, 0x04); bput(&b, 0x40); op_u(&b, 0x20, 2); bput(&b, 0x0f); bput(&b, 0x0b);
      op_u(&b, 0x10, f_unwound);
      i32c(&b, 1); op_u(&b, 0x21, 1);
      bput(&b, 0x0c); buleb(&b, 0);
      bput(&b, 0x0b);
      bput(&b, 0x00); bput(&b, 0x0b);
    } else if (f == f_catch && have_hooks) {
      /* loop { block { try_table (catch fork 0) { rw ? call_indirect t1 : call_indirect t0; return } } unwound(); rw = 1 } */
      buleb(&b, 0);
      bput(&b, 0x03); bput(&b, 0x40);
      bput(&b, 0x02); bput(&b, 0x40);
      bput(&b, 0x1f); bput(&b, 0x40); buleb(&b, 1); bput(&b, 0x00); buleb(&b, tag_fork); buleb(&b, 0);
      op_u(&b, 0x20, 1); bput(&b, 0x04); bput(&b, I32);
      op_u(&b, 0x20, 0); bput(&b, 0x11); buleb(&b, ty_ret_i32); buleb(&b, ntables);
      bput(&b, 0x05);
      op_u(&b, 0x20, 0); bput(&b, 0x11); buleb(&b, ty_ret_i32); buleb(&b, 0);
      bput(&b, 0x0b);
      bput(&b, 0x0f);
      bput(&b, 0x0b); /* try_table */
      bput(&b, 0x0b); /* block */
      op_u(&b, 0x10, f_unwound);
      i32c(&b, 1); op_u(&b, 0x21, 1);
      bput(&b, 0x0c); buleb(&b, 0);
      bput(&b, 0x0b); /* loop */
      bput(&b, 0x00); bput(&b, 0x0b);
    } else if (in_s[f]) {
      analyze(bodies[d].p, bodies[d].len, f, 1);
      plan_locals();
      gen_normal(&b, bodies[d].p, f);
      gen_rewind(&rwbody[f], bodies[d].p, f);
      s_code += bodies[d].len;
    } else bmem(&b, bodies[d].p, bodies[d].len);
    buleb(&code, b.n); bbuf(&code, &b);
    free(b.p);
  }
  cur_fn = "";
  size_t rw_code = 0;
  for (int f = 0; f < nfunc; f++) if (in_s[f]) { buleb(&code, rwbody[f].n); bbuf(&code, &rwbody[f]); rw_code += rwbody[f].n; }
  if (f_point_rw >= 0) { /* rewinding reached the fork point: back to normal, return the result */
    buf b = {0};
    buleb(&b, 0);
    i32c(&b, 0); op_u(&b, 0x24, g_state);
    struct functype *ft = &types[ftype[f_root]];
    if (ft->nr) op_u(&b, 0x23, g_result);
    bput(&b, 0x0b);
    if (ft->nr && ft->r[0] != I32) die("the fork point must return an i32");
    buleb(&code, b.n); bbuf(&code, &b);
  }
  { buf b = {0}; buleb(&b, 0); bput(&b, 0x00); bput(&b, 0x0b); buleb(&code, b.n); bbuf(&code, &b); }

  /* write the module, section by section */
  buf o = {0};
  bmem(&o, "\0asm\1\0\0\0", 8);
  int wrote_tag = 0;
  for (int i = 0; i < nsecs; i++) {
    struct section *s = &secs[i];
    buf c = {0};
    rd r = {s->data, s->data + s->size};
    if (!wrote_tag && (s->id == 6 || s->id == 7 || s->id == 8 || s->id == 9 || s->id == 12 || s->id == 10 || s->id == 11) && find_sec(13) < 0) {
      buf t = {0}; buleb(&t, 1); bput(&t, 0); buleb(&t, ty_void); sec_out(&o, 13, &t); free(t.p); wrote_tag = 1;
      if (find_sec(6) < 0) { buf g = {0}; buleb(&g, 4); for (int k = 0; k < 4; k++) { bput(&g, I32); bput(&g, 1); i32c(&g, 0); bput(&g, 0x0b); } sec_out(&o, 6, &g); free(g.p); }
    }
    switch (s->id) {
    case 1: { /* types: the old ones, then any new ones */
      ruleb(&r); buleb(&c, ntypes); bmem(&c, r.p, r.end - r.p);
      int old = 0; { rd q = {s->data, s->data + s->size}; old = ruleb(&q); }
      for (int t = old; t < ntypes; t++) {
        bput(&c, 0x60); buleb(&c, types[t].np); bmem(&c, types[t].p, types[t].np); buleb(&c, types[t].nr); bmem(&c, types[t].r, types[t].nr);
      }
      sec_out(&o, 1, &c); break;
    }
    case 3: { u32 cnt = ruleb(&r); buleb(&c, cnt + nnew); bmem(&c, r.p, r.end - r.p); for (int k = 0; k < nnew; k++) buleb(&c, new_type[k]); sec_out(&o, 3, &c); break; }
    case 4: { u32 cnt = ruleb(&r); buleb(&c, cnt + 1); bmem(&c, r.p, r.end - r.p); bput(&c, FUNCREF); bput(&c, 1); buleb(&c, tab0_size); buleb(&c, tab0_size); sec_out(&o, 4, &c); break; }
    case 13: { u32 cnt = ruleb(&r); buleb(&c, cnt + 1); bmem(&c, r.p, r.end - r.p); bput(&c, 0); buleb(&c, ty_void); sec_out(&o, 13, &c); wrote_tag = 1; break; }
    case 6: { u32 cnt = ruleb(&r); buleb(&c, cnt + 4); bmem(&c, r.p, r.end - r.p); for (int k = 0; k < 4; k++) { bput(&c, I32); bput(&c, 1); i32c(&c, 0); bput(&c, 0x0b); } sec_out(&o, 6, &c); break; }
    case 9: { /* elements: the old segments, then table 1 = the rewinding copies */
      u32 cnt = ruleb(&r); buleb(&c, cnt + 1); bmem(&c, r.p, r.end - r.p);
      bput(&c, 2); buleb(&c, ntables); i32c(&c, 0); bput(&c, 0x0b); bput(&c, 0); buleb(&c, tab0_size);
      for (u32 k = 0; k < tab0_size; k++) buleb(&c, tab0[k] >= 0 && rw_of[tab0[k]] >= 0 ? (u32)rw_of[tab0[k]] : (u32)f_trap);
      sec_out(&o, 9, &c); break;
    }
    case 10: sec_out(&o, 10, &code); break;
    default: sec_raw(&o, i);
    }
    free(c.p);
  }
  if (find_sec(9) < 0) die("the module has no element section");
  fp = fopen(out, "wb");
  if (!fp || fwrite(o.p, 1, o.n, fp) != o.n) die("cannot write %s", out);
  fclose(fp);
  if (stats) {
    int nf = 0; for (int f = nimpf; f < nfunc; f++) nf++;
    fprintf(stderr, "wasmfork: %s: %d of %d functions instrumented (%s); code %zu -> %zu bytes (those functions: %zu, their rewinding copies: %zu); file %ld -> %zu bytes (+%.0f%%)\n",
            in, ns, nf, mode_all ? "all" : root ? root : "can reach fork", old_code, code.n, s_code, rw_code, n, o.n, 100.0 * ((double)o.n - n) / n);
  }
  return 0;
}
