// SPDX-License-Identifier: GPL-2.0-or-later
// The architecture-neutral half of the translator: region discovery, control flow graph,
// structuring, register caching in locals, exits, calls, budget checks, module assembly.
//
// Structuring follows the published algorithm of N. Ramsey, "Beyond Relooper: recursive
// translation of unstructured control flow to structured control flow" (ICFP 2022): walk
// the dominator tree; a loop header becomes a wasm `loop`; a node with two or more
// forward in-edges ("merge node") becomes the code right after a wasm `block` that its
// predecessors leave with `br`; every other node is placed inline at its only
// predecessor's branch. It needs a reducible graph; otherwise the region falls back to
// the dispatch form (a loop around a br_table over block numbers).
// Dominators: K. Cooper, T. Harvey, K. Kennedy, "A Simple, Fast Dominance Algorithm" (2001).
#include "xgen.h"
#include "port.h"

#define MAXI 8192
#define MAXB 1024
#define MAXDEPTH 256        // guest call depth carried on the wasm stack before calls leave instead
#define LH 4096             // leader hash size (power of two, > 2 * MAXB)
#define MAXT 16             // learned targets per indirect jump

typedef struct {
  uint64_t start;
  int first, n;              // instructions I[first .. first + n)
  int term;                  // FL_* of the last instruction, FL_NONE if it falls into the next block
  int succ[MAXT + 1], nsucc;  // successor blocks (-1: outside the region)
  uint64_t spc[MAXT + 1];    // successor addresses (branch: taken, not taken; indirect jump: learned targets)
  int rpo, idom, nfwd, loophead, child, sibling, pos;
} xblock;

static xinsn I[MAXI];
static int nI;
static xblock B[MAXB];
static int nB;
static uint64_t L[MAXB];     // leaders in discovery order; block i starts at L[i]
static int nL;
static uint64_t lh_key[LH];
static int16_t lh_val[LH];   // leader index, or -1 for an empty slot
static int ord[MAXB];        // blocks in reverse postorder
static int nord;
static int preds_start[MAXB + 1], preds[(MAXT + 1) * MAXB];

static int lh_slot(uint64_t pc) {
  uint32_t h = (uint32_t)((pc >> 1) * 2654435761u) & (LH - 1);
  while (lh_val[h] >= 0 && lh_key[h] != pc) h = (h + 1) & (LH - 1);
  return h;
}
static int leader_index(uint64_t pc) { int s = lh_slot(pc); return lh_val[s]; }
static int add_leader(uint64_t pc, int max) {
  int s = lh_slot(pc);
  if (lh_val[s] >= 0) return 1;
  if (nL >= max) return 0;
  lh_key[s] = pc;
  lh_val[s] = (int16_t)nL;
  L[nL++] = pc;
  return 1;
}

static int is_terminator(int flow, const xlat_opts *o) {
  switch (flow) {
  case FL_NONE: return 0;
  case FL_CALL: return !o->calls;
  case FL_INDCALL: return !(o->calls && o->indirect);
  default: return 1;
  }
}

// Pass 1: find the leaders reachable from the entry by direct branches and fall-through.
// Pass 2: cut blocks at leaders and control transfers.
static void discover(uint64_t entry, const xlat_opts *o) {
  memset(lh_val, 0xff, sizeof lh_val);
  nL = 0;
  int maxb = o->max_blocks < MAXB ? o->max_blocks : MAXB;
  int maxi = o->max_insns < MAXI ? o->max_insns : MAXI;
  add_leader(entry, maxb);
  int scanned = 0;
  for (int li = 0; li < nL && scanned < 4 * maxi; li++) {
    uint64_t pc = L[li];
    for (;;) {
      xinsn x;
      if (!arch_decode(pc, &x, o) || x.flow == FL_STOP) break;
      scanned++;
      if (x.flow == FL_BRANCH) { add_leader(x.target, maxb); add_leader(pc + x.d.len, maxb); break; }
      if (x.flow == FL_JUMP) { add_leader(x.target, maxb); break; }
      if (x.flow == FL_INDJUMP && o->learn) {
        uint64_t t[MAXT];
        int n = engine_site_targets(pc, t, MAXT);
        for (int i = 0; i < n; i++) add_leader(t[i], maxb);
      }
      if (is_terminator(x.flow, o)) break;
      pc += x.d.len;
      if (leader_index(pc) >= 0) break;
    }
  }
  nI = 0;
  nB = nL;
  for (int b = 0; b < nB; b++) {
    xblock *k = &B[b];
    memset(k, 0, sizeof *k);
    k->start = L[b];
    k->first = nI;
    k->succ[0] = k->succ[1] = -1;
    uint64_t pc = L[b];
    int trunc = 0;
    for (;;) {
      if (nI >= maxi) { k->term = FL_NONE; k->spc[0] = pc; k->nsucc = 0; trunc = 1; break; }
      xinsn x;
      if (!arch_decode(pc, &x, o) || x.flow == FL_STOP) { k->term = FL_STOP; k->spc[0] = pc; break; }
      I[nI++] = x;
      k->n++;
      if (is_terminator(x.flow, o)) {
        k->term = x.flow;
        if (x.flow == FL_BRANCH) { k->spc[0] = x.target; k->spc[1] = pc + x.d.len; k->nsucc = 2; }
        else if (x.flow == FL_JUMP) { k->spc[0] = x.target; k->nsucc = 1; }
        else if (x.flow == FL_INDJUMP && o->learn) {
          k->nsucc = engine_site_targets(pc, k->spc, MAXT);
          engine_site_in_region(pc);
        }
        break;
      }
      pc += x.d.len;
      if (leader_index(pc) >= 0) { k->term = FL_NONE; k->spc[0] = pc; k->nsucc = 1; break; }
    }
    if (k->term == FL_NONE && k->nsucc == 0) k->nsucc = 1;   // truncated: falls out of the region
    for (int s = 0; s < k->nsucc; s++) k->succ[s] = leader_index(k->spc[s]);
    for (int s = k->nsucc; s <= MAXT; s++) k->succ[s] = -1;
    if (trunc) k->succ[0] = -1;   // out of instruction budget: leave (a block may even be empty)
  }
}

// ---- analysis ----
static void compute_rpo(void) {
  static int stack[MAXB], it[MAXB], post[MAXB];
  static uint8_t seen[MAXB];
  memset(seen, 0, (size_t)nB);
  int sp = 0, np = 0;
  stack[sp] = 0; it[sp] = 0; sp++; seen[0] = 1;
  while (sp) {
    int b = stack[sp - 1];
    if (it[sp - 1] < B[b].nsucc) {
      int s = B[b].succ[it[sp - 1]++];
      if (s >= 0 && !seen[s]) { seen[s] = 1; stack[sp] = s; it[sp] = 0; sp++; }
    } else { post[np++] = b; sp--; }
  }
  for (int b = 0; b < nB; b++) B[b].rpo = -1;
  nord = np;
  for (int i = 0; i < np; i++) { ord[i] = post[np - 1 - i]; B[ord[i]].rpo = i; }
}

static int intersect(int a, int b) {
  while (a != b) {
    while (B[a].rpo > B[b].rpo) a = B[a].idom;
    while (B[b].rpo > B[a].rpo) b = B[b].idom;
  }
  return a;
}

static int dominates(int a, int b) {   // does a dominate b?
  for (;;) {
    if (a == b) return 1;
    if (b == ord[0]) return 0;
    b = B[b].idom;
  }
}

// Returns 0 if the graph is irreducible.
static int analyse(void) {
  compute_rpo();
  // predecessor lists (reachable blocks only)
  memset(preds_start, 0, sizeof(int) * (size_t)(nB + 1));
  for (int b = 0; b < nB; b++) if (B[b].rpo >= 0)
    for (int s = 0; s < B[b].nsucc; s++) if (B[b].succ[s] >= 0) preds_start[B[b].succ[s] + 1]++;
  for (int b = 0; b < nB; b++) preds_start[b + 1] += preds_start[b];
  static int fill[MAXB];
  memcpy(fill, preds_start, sizeof(int) * (size_t)nB);
  for (int b = 0; b < nB; b++) if (B[b].rpo >= 0)
    for (int s = 0; s < B[b].nsucc; s++) if (B[b].succ[s] >= 0) preds[fill[B[b].succ[s]]++] = b;
  // dominators
  for (int b = 0; b < nB; b++) B[b].idom = -1;
  B[ord[0]].idom = ord[0];
  for (int changed = 1; changed;) {
    changed = 0;
    for (int i = 1; i < nord; i++) {
      int b = ord[i], nd = -1;
      for (int p = preds_start[b]; p < preds_start[b + 1]; p++) {
        int q = preds[p];
        if (B[q].idom < 0) continue;
        nd = nd < 0 ? q : intersect(q, nd);
      }
      if (nd != B[b].idom) { B[b].idom = nd; changed = 1; }
    }
  }
  // back edges, loop headers, merge nodes, reducibility
  int reducible = 1;
  for (int i = 0; i < nord; i++) {
    int b = ord[i];
    for (int s = 0; s < B[b].nsucc; s++) {
      int t = B[b].succ[s];
      if (t < 0) continue;
      if (B[t].rpo <= B[b].rpo) { B[t].loophead = 1; if (!dominates(t, b)) reducible = 0; }
      else B[t].nfwd++;
    }
  }
  // dominator tree, children kept in increasing reverse postorder
  for (int b = 0; b < nB; b++) B[b].child = B[b].sibling = -1;
  for (int i = nord - 1; i >= 1; i--) {
    int b = ord[i], p = B[b].idom;
    B[b].sibling = B[p].child;
    B[p].child = b;
  }
  return reducible;
}

// ---- emission services ----
static xgen G;

uint32_t xg_import(int imp) { return (uint32_t)imp; }
uint32_t xg_helper(xgen *g, int h) { g->m->helpers_used |= 1u << h; return IMP_COUNT + (uint32_t)h; }

void xg_cpu_load64(xgen *g, uint32_t addr) { w_i32c(g->b, 0); w_mem(g->b, W_I64_LOAD, 3, addr); }
void xg_cpu_store64_begin(xgen *g) { w_i32c(g->b, 0); }
void xg_cpu_store64_end(xgen *g, uint32_t addr) { (void)g; w_mem(g->b, W_I64_STORE, 3, addr); }

void xg_get(xgen *g, int r) { if (r == 0) w_i64c(g->b, 0); else w_lget(g->b, L_X + r); }
void xg_set(xgen *g, int r) { if (r == 0) w_op(g->b, W_DROP); else w_lset(g->b, L_X + r); }
void xg_fget(xgen *g, int r) { w_lget(g->b, L_F + r); }
void xg_fset(xgen *g, int r) { w_lset(g->b, L_F + r); }

void xg_flush(xgen *g, uint64_t set) {
  for (int r = 1; r < 32; r++) if (set >> r & 1) { w_i32c(g->b, 0); w_lget(g->b, L_X + r); w_mem(g->b, W_I64_STORE, 3, g->env->a_x + 8u * (uint32_t)r); }
  for (int r = 0; r < 32; r++) if (set >> (32 + r) & 1) { w_i32c(g->b, 0); w_lget(g->b, L_F + r); w_mem(g->b, W_F64_STORE, 3, g->env->a_f + 8u * (uint32_t)r); }
}
void xg_reload(xgen *g, uint64_t set) {
  for (int r = 1; r < 32; r++) if (set >> r & 1) { w_i32c(g->b, 0); w_mem(g->b, W_I64_LOAD, 3, g->env->a_x + 8u * (uint32_t)r); w_lset(g->b, L_X + r); }
  for (int r = 0; r < 32; r++) if (set >> (32 + r) & 1) { w_i32c(g->b, 0); w_mem(g->b, W_F64_LOAD, 3, g->env->a_f + 8u * (uint32_t)r); w_lset(g->b, L_F + r); }
}
static void flush_misc(xgen *g) {
  if (g->o->budget) { w_i32c(g->b, 0); w_lget(g->b, L_BUDGET); w_mem(g->b, W_I32_STORE, 2, g->env->a_budget); }
  if (g->o->count) { w_i32c(g->b, 0); w_lget(g->b, L_IC); w_mem(g->b, W_I64_STORE, 3, g->env->a_icount); }
}
static void reload_misc(xgen *g) {
  if (g->o->budget) { w_i32c(g->b, 0); w_mem(g->b, W_I32_LOAD, 2, g->env->a_budget); w_lset(g->b, L_BUDGET); }
  if (g->o->count) { w_i32c(g->b, 0); w_mem(g->b, W_I64_LOAD, 3, g->env->a_icount); w_lset(g->b, L_IC); }
}

// The instructions of the current block that will not run when we leave at this point.
static void uncount(xgen *g, int retired) {
  if (!g->o->count) return;
  int skipped = g->blk_n - retired;
  if (skipped <= 0) return;
  w_lget(g->b, L_IC); w_i64c(g->b, skipped); w_op(g->b, W_I64_SUB); w_lset(g->b, L_IC);
}
static void br_exit(xgen *g) { w_br(g->b, (uint32_t)(g->labels - 1)); }

void xg_exit_const(xgen *g, uint64_t pc, int reason, int retired) {
  w_i32c(g->b, 0); w_i64c(g->b, (int64_t)pc); w_mem(g->b, W_I64_STORE, 3, g->env->a_pc);
  w_i32c(g->b, reason); w_lset(g->b, L_REASON);
  uncount(g, retired);
  br_exit(g);
}
void xg_exit_dyn(xgen *g, int local_pc, int reason, int retired) {
  w_i32c(g->b, 0); w_lget(g->b, (uint32_t)local_pc); w_mem(g->b, W_I64_STORE, 3, g->env->a_pc);
  w_i32c(g->b, reason); w_lset(g->b, L_REASON);
  uncount(g, retired);
  br_exit(g);
}
void xg_exit_keep_pc(xgen *g, int reason, int retired) {
  w_i32c(g->b, reason); w_lset(g->b, L_REASON);
  uncount(g, retired);
  br_exit(g);
}
void xg_fault(xgen *g, int local_addr) {
  w_i32c(g->b, 0); w_lget(g->b, (uint32_t)local_addr); w_mem(g->b, W_I64_STORE, 3, g->env->a_fault);
  xg_exit_const(g, g->cur->pc, R_FAULT, g->blk_k);
}

uint32_t xg_addr(xgen *g, int rs1, int64_t imm, int size) {
  wbuf *b = g->b;
  switch (g->o->bounds) {
  case B_UNCHECKED:
    xg_get(g, rs1);
    w_op(b, W_I32_WRAP_I64);
    if (imm >= 0 && imm < 0x10000) return g->env->gbase + (uint32_t)imm;
    w_i32c(b, (int32_t)imm); w_op(b, W_I32_ADD);
    return g->env->gbase;
  case B_MASKED:
    xg_get(g, rs1);
    w_op(b, W_I32_WRAP_I64);
    if (imm) { w_i32c(b, (int32_t)imm); w_op(b, W_I32_ADD); }
    w_i32c(b, (int32_t)(g->env->gsize - 1)); w_op(b, W_I32_AND);
    return g->env->gbase;
  default:
    xg_get(g, rs1);
    if (imm) { w_i64c(b, imm); w_op(b, W_I64_ADD); }
    w_ltee(b, L_T2);
    if (g->env->gspan) {   // the engine's window rule: a - lo <= span covers accesses of up to 8 bytes
      if (g->env->glo) { w_i64c(b, (int64_t)g->env->glo); w_op(b, W_I64_SUB); }
      w_i64c(b, (int64_t)g->env->gspan);
    } else w_i64c(b, (int64_t)(g->env->gsize - (uint64_t)size));
    w_op(b, W_I64_GT_U);
    w_if(b, WT_VOID); g->labels++;
    xg_fault(g, L_T2);
    w_op(b, W_END); g->labels--;
    w_lget(b, L_T2); w_op(b, W_I32_WRAP_I64);
    return g->env->gbase;
  }
}

// ---- control flow ----
enum { CX_EXIT, CX_LOOP, CX_BLOCK, CX_IF, CX_DISPATCH };
static struct { int kind, blk, label; } cx[3 * MAXB + 64];
static int ncx;
static void open(int op, int kind, int blk) {
  w_op(G.b, op); w_op(G.b, WT_VOID);
  cx[ncx].kind = kind; cx[ncx].blk = blk; cx[ncx].label = G.labels; ncx++;
  G.labels++;
}
static void close_(void) { w_op(G.b, W_END); ncx--; G.labels--; }
static uint32_t depth_of(int kind, int blk) {
  for (int i = ncx - 1; i >= 0; i--)
    if (cx[i].kind == kind && cx[i].blk == blk) return (uint32_t)(G.labels - 1 - cx[i].label);
  return 0xffffffffu;   // a bug: caught by the assertion in the caller
}

// Count down; at zero call the yield helper, which returns the new budget, or a negative
// number to make us leave (at resume_pc, or at the pc in local `dyn` if dyn >= 0).
static void budget_check2(uint64_t resume_pc, int dyn, int retired) {
  if (!G.o->budget) return;
  wbuf *b = G.b;
  w_lget(b, L_BUDGET); w_i32c(b, 1); w_op(b, W_I32_SUB); w_ltee(b, L_BUDGET); w_op(b, W_I32_EQZ);
  open(W_IF, CX_IF, -1);
  w_call(b, xg_import(IMP_YIELD)); w_ltee(b, L_BUDGET); w_i32c(b, 0); w_op(b, W_I32_LT_S);
  open(W_IF, CX_IF, -1);
  if (dyn >= 0) xg_exit_dyn(&G, dyn, R_JUMP, retired); else xg_exit_const(&G, resume_pc, R_JUMP, retired);
  close_();
  close_();
}
static void budget_check(uint64_t resume_pc, int retired) { budget_check2(resume_pc, -1, retired); }

// slot of the region at the pc in local `lt` -> L_TI (-1: none), through an inline cache if enabled
static void emit_lookup(int lt) {
  wbuf *b = G.b;
  if (!G.o->ic) { w_lget(b, (uint32_t)lt); w_call(b, xg_import(IMP_LOOKUP)); w_lset(b, L_TI); return; }
  uint32_t gpc = G.m->nglobals++, gslot = G.m->nglobals++;
  w_lget(b, (uint32_t)lt); w_op(b, W_GGET); wb_uleb(b, gpc); w_op(b, W_I64_EQ);
  w_if(b, WT_I32); G.labels++;
  w_op(b, W_GGET); wb_uleb(b, gslot);
  w_op(b, W_ELSE);
  w_lget(b, (uint32_t)lt); w_call(b, xg_import(IMP_LOOKUP)); w_ltee(b, L_TI); w_i32c(b, 0); w_op(b, W_I32_GE_S);
  w_if(b, WT_VOID); G.labels++;
  w_lget(b, (uint32_t)lt); w_op(b, W_GSET); wb_uleb(b, gpc);
  w_lget(b, L_TI); w_op(b, W_GSET); wb_uleb(b, gslot);
  w_op(b, W_END); G.labels--;
  w_lget(b, L_TI);
  w_op(b, W_END); G.labels--;
  w_lset(b, L_TI);
}

// Leave by a tail call into the region that runs `pc` (constant, or in local dyn).
static void store_pc_const(uint64_t pc) { w_i32c(G.b, 0); w_i64c(G.b, (int64_t)pc); w_mem(G.b, W_I64_STORE, 3, G.env->a_pc); }
static void store_pc_dyn(int l) { w_i32c(G.b, 0); w_lget(G.b, (uint32_t)l); w_mem(G.b, W_I64_STORE, 3, G.env->a_pc); }
static void chain_const(uint64_t pc, int retired) {
  if (!G.o->chain) { xg_exit_const(&G, pc, R_JUMP, retired); return; }
  uint32_t slot = engine_slot_for(pc);
  budget_check(pc, retired);
  store_pc_const(pc);           // a stub in the slot returns R_STUB: "continue at cpu->pc"
  xg_flush(&G, G.dirty);
  flush_misc(&G);
  w_lget(G.b, L_DEPTH); w_i32c(G.b, (int32_t)slot);
  w_op(G.b, W_RETURN_CALL_INDIRECT); wb_uleb(G.b, T_REGION); wb_byte(G.b, 0);
}
static void chain_dyn(int lt, int reason, int retired) {
  if (!G.o->chain) { xg_exit_dyn(&G, lt, reason, retired); return; }
  wbuf *b = G.b;
  emit_lookup(lt);
  w_lget(b, L_TI); w_i32c(b, 0); w_op(b, W_I32_LT_S);
  open(W_IF, CX_IF, -1);
  xg_exit_dyn(&G, lt, reason, retired);
  close_();
  budget_check2(0, lt, retired);
  store_pc_dyn(lt);
  xg_flush(&G, G.dirty);
  flush_misc(&G);
  w_lget(b, L_DEPTH); w_lget(b, L_TI);
  w_op(b, W_RETURN_CALL_INDIRECT); wb_uleb(b, T_REGION); wb_byte(b, 0);
}

static int module_func(xmodule *m, uint64_t pc);
static int bad_depth;

static void emit_call(const xinsn *x, int indirect) {
  wbuf *b = G.b;
  uint64_t ret = x->pc + x->d.len;
  if (indirect) { arch_emit_indirect_target(&G, x); w_lset(b, L_T0); }
  arch_emit_link(&G, x);
  // too deep: leave and let the engine run the callee from its own loop
  w_lget(b, L_DEPTH); w_i32c(b, MAXDEPTH); w_op(b, W_I32_GE_U);
  open(W_IF, CX_IF, -1);
  if (indirect) xg_exit_dyn(&G, L_T0, R_JUMP, G.blk_k + 1); else xg_exit_const(&G, x->target, R_JUMP, G.blk_k + 1);
  close_();
  // Indirect calls with learned targets (option split): one call_indirect per known target,
  // each with a constant slot, so each machine-level indirect call always goes to the same
  // place (a Chromium renderer on some processors predicts nothing else; see section 11).
  uint64_t tg[MAXT];
  int nsplit = 0;
  if (indirect && G.o->learn) {
    engine_site_in_region(x->pc);
    if (G.o->split) nsplit = engine_site_targets(x->pc, tg, 4);
  }
  if (indirect && !nsplit) {   // look the target up: slot or -1
    if (G.o->learn) { w_i32c(b, 0); w_i64c(b, (int64_t)x->pc); w_mem(b, W_I64_STORE, 3, G.env->a_site); }
    emit_lookup(L_T0);
    w_lget(b, L_TI); w_i32c(b, 0); w_op(b, W_I32_LT_S);
    open(W_IF, CX_IF, -1);
    xg_exit_dyn(&G, L_T0, R_JUMP, G.blk_k + 1);
    close_();
  }
  if (indirect) store_pc_dyn(L_T0); else store_pc_const(x->target);   // for a stub: "continue at cpu->pc"
  int f = indirect ? -1 : module_func(G.m, x->target);
  uint64_t flush = G.dirty, reload = G.cache, cu, cd;
  if (f >= 0 && G.m->leaf[f - IMP_COUNT - HLP_COUNT]) {
    flush &= G.m->fuse[f - IMP_COUNT - HLP_COUNT]; reload &= G.m->fdef[f - IMP_COUNT - HLP_COUNT];
  } else if (!indirect && f < 0 && engine_callee_summary(x->target, &cu, &cd)) { flush &= cu; reload &= cd; }
  else if (G.o->abi) reload &= 0x00000000f003fce2ull | (0xf003fcffull << 32);   // ra t0-t2 a0-a7 t3-t6; ft0-7 fa0-7 ft8-11
  xg_flush(&G, flush);
  flush_misc(&G);
  if (nsplit) {
    for (int i = 0; i < nsplit; i++) {
      w_lget(b, L_T0); w_i64c(b, (int64_t)tg[i]); w_op(b, W_I64_EQ);
      open(W_IF, CX_IF, -1);
      w_lget(b, L_DEPTH); w_i32c(b, 1); w_op(b, W_I32_ADD);
      w_i32c(b, (int32_t)engine_slot_for(tg[i]));
      w_call_indirect(b, T_REGION);
      w_lset(b, L_REASON);
      w_op(b, W_ELSE);
    }
    // a target not seen before: look it up (and learn it)
    w_i32c(b, 0); w_i64c(b, (int64_t)x->pc); w_mem(b, W_I64_STORE, 3, G.env->a_site);
    emit_lookup(L_T0);
    w_lget(b, L_TI); w_i32c(b, 0); w_op(b, W_I32_LT_S);
    open(W_IF, CX_IF, -1);
    xg_exit_dyn(&G, L_T0, R_JUMP, G.blk_k + 1);
    close_();
    w_lget(b, L_DEPTH); w_i32c(b, 1); w_op(b, W_I32_ADD);
    w_lget(b, L_TI);
    w_call_indirect(b, T_REGION);
    w_lset(b, L_REASON);
    for (int i = 0; i < nsplit; i++) close_();
  } else {
    w_lget(b, L_DEPTH); w_i32c(b, 1); w_op(b, W_I32_ADD);
    if (f >= 0) w_call(b, (uint32_t)f);
    else {
      if (indirect) w_lget(b, L_TI); else w_i32c(b, (int32_t)engine_slot_for(x->target));
      w_call_indirect(b, T_REGION);
    }
    w_lset(b, L_REASON);
  }
  xg_reload(&G, reload);
  reload_misc(&G);
  w_lget(b, L_REASON); w_i32c(b, R_RET); w_op(b, W_I32_NE);
  open(W_IF, CX_IF, -1);
  w_lget(b, L_REASON); w_i32c(b, R_STUB); w_op(b, W_I32_EQ);
  open(W_IF, CX_IF, -1);
  w_i32c(b, R_JUMP); w_lset(b, L_REASON);      // the pc is in cpu->pc already
  close_();
  uncount(&G, G.blk_k + 1);
  br_exit(&G);
  close_();
  xg_cpu_load64(&G, G.env->a_pc); w_i64c(b, (int64_t)ret); w_op(b, W_I64_NE);
  open(W_IF, CX_IF, -1);
  w_i32c(b, R_JUMP); w_lset(b, L_REASON);
  uncount(&G, G.blk_k + 1);
  br_exit(&G);
  close_();
}

static void emit_body(int bi) {
  xblock *k = &B[bi];
  G.blk_n = k->n;
  if (G.o->count && k->n) { w_lget(G.b, L_IC); w_i64c(G.b, k->n); w_op(G.b, W_I64_ADD); w_lset(G.b, L_IC); }
  for (int i = 0; i < k->n; i++) {
    const xinsn *x = &I[k->first + i];
    G.cur = x; G.blk_k = i;
    if (i == k->n - 1 && is_terminator(x->flow, G.o)) break;
    if (x->flow == FL_CALL) emit_call(x, 0);
    else if (x->flow == FL_INDCALL) emit_call(x, 1);
    else arch_emit(&G, x);
  }
  G.blk_k = k->n;
}

static int strategy_now;
static void struct_tree(int x);

static void branch_to(int src, int t, uint64_t tpc) {
  if (t < 0) { chain_const(tpc, G.blk_n); return; }
  if (strategy_now == S_DISPATCH) {
    int back = B[t].pos <= B[src].pos;
    if (!back && B[t].pos == B[src].pos + 1) return;   // falls into the next block
    if (back) budget_check(tpc, G.blk_n);
    w_i32c(G.b, B[t].pos); w_lset(G.b, L_NEXT);
    uint32_t d = depth_of(CX_DISPATCH, -1);
    if (d == 0xffffffffu) bad_depth = 1;
    w_br(G.b, d);
    return;
  }
  if (B[t].rpo <= B[src].rpo) {
    budget_check(tpc, G.blk_n);
    uint32_t d = depth_of(CX_LOOP, t);
    if (d == 0xffffffffu) bad_depth = 1;
    w_br(G.b, d);
  } else if (B[t].nfwd >= 2) {
    uint32_t d = depth_of(CX_BLOCK, t);
    if (d == 0xffffffffu) bad_depth = 1;
    w_br(G.b, d);
  } else struct_tree(t);
}

static void emit_terminator(int bi) {
  xblock *k = &B[bi];
  const xinsn *x = k->n ? &I[k->first + k->n - 1] : 0;
  if (x) G.cur = x;
  G.blk_k = k->n - 1;
  switch (k->term) {
  case FL_BRANCH:
    if (k->succ[0] == k->succ[1] && k->spc[0] == k->spc[1]) { branch_to(bi, k->succ[0], k->spc[0]); break; }
    arch_emit_cond(&G, x);
    open(W_IF, CX_IF, -1);
    branch_to(bi, k->succ[0], k->spc[0]);
    w_op(G.b, W_ELSE);
    branch_to(bi, k->succ[1], k->spc[1]);
    close_();
    break;
  case FL_NONE: case FL_JUMP:
    branch_to(bi, k->succ[0], k->spc[0]);
    break;
  case FL_CALL:      // calls are not translated as calls: link and leave
    arch_emit_link(&G, x);
    chain_const(x->target, k->n);
    break;
  case FL_INDJUMP: case FL_RET: case FL_INDCALL:
    arch_emit_indirect_target(&G, x);
    w_lset(G.b, L_T0);
    if (k->term == FL_INDJUMP && G.o->learn) {
      for (int i = 0; i < k->nsucc; i++) {   // known targets: ordinary branches inside the region
        if (k->succ[i] < 0) continue;
        w_lget(G.b, L_T0); w_i64c(G.b, (int64_t)k->spc[i]); w_op(G.b, W_I64_EQ);
        open(W_IF, CX_IF, -1);
        branch_to(bi, k->succ[i], k->spc[i]);
        close_();
      }
      w_i32c(G.b, 0); w_i64c(G.b, (int64_t)x->pc); w_mem(G.b, W_I64_STORE, 3, G.env->a_site);   // for learning
    }
    if (k->term == FL_INDCALL) arch_emit_link(&G, x);
    if (k->term == FL_RET && G.o->calls) {   // a caller in wasm checks our R_RET; at depth 0 nobody does
      w_lget(G.b, L_DEPTH); w_op(G.b, W_I32_EQZ);
      open(W_IF, CX_IF, -1);
      chain_dyn(L_T0, R_RET, k->n);
      close_();
      xg_exit_dyn(&G, L_T0, R_RET, k->n);
    } else chain_dyn(L_T0, k->term == FL_RET ? R_RET : R_JUMP, k->n);
    break;
  case FL_STOP:
    G.blk_k = k->n;
    xg_exit_const(&G, k->spc[0], R_INTERP, k->n);
    break;
  }
}

static int ys_stack[MAXB * 2], ys_sp;
static void node_within(int x, int *ys, int nys) {
  if (nys) {
    int y = ys[0];
    open(W_BLOCK, CX_BLOCK, y);
    node_within(x, ys + 1, nys - 1);
    close_();
    struct_tree(y);
    return;
  }
  emit_body(x);
  emit_terminator(x);
}
static void struct_tree(int x) {
  // merge children, largest reverse postorder number first
  int *ys = ys_stack + ys_sp, n = 0;
  for (int c = B[x].child; c >= 0; c = B[c].sibling) if (B[c].nfwd >= 2) ys[n++] = c;
  for (int i = 0; i < n / 2; i++) { int t = ys[i]; ys[i] = ys[n - 1 - i]; ys[n - 1 - i] = t; }   // children are in increasing order
  ys_sp += n;
  if (B[x].loophead) { open(W_LOOP, CX_LOOP, x); node_within(x, ys, n); close_(); }
  else node_within(x, ys, n);
  ys_sp -= n;
}

static void emit_dispatch(void) {
  wbuf *b = G.b;
  for (int i = 0; i < nord; i++) B[ord[i]].pos = i;
  w_i32c(b, 0); w_lset(b, L_NEXT);
  open(W_LOOP, CX_DISPATCH, -1);
  for (int i = nord - 1; i >= 0; i--) open(W_BLOCK, CX_BLOCK, ord[i]);
  w_lget(b, L_NEXT);
  w_op(b, W_BR_TABLE);
  wb_uleb(b, (uint32_t)nord);
  for (int i = 0; i < nord; i++) wb_uleb(b, (uint32_t)i);
  wb_uleb(b, 0);
  for (int i = 0; i < nord; i++) {
    close_();               // end of block i: its code follows
    emit_body(ord[i]);
    emit_terminator(ord[i]);
  }
  w_op(b, W_UNREACHABLE);
  close_();                 // the loop
}

// ---- modules ----
static uint64_t me_key[2 * XL_MAXFUNCS];
static int me_val[2 * XL_MAXFUNCS];
static int module_func(xmodule *m, uint64_t pc) {
  uint32_t h = (uint32_t)((pc >> 1) * 2654435761u) & (2 * XL_MAXFUNCS - 1);
  while (me_val[h] >= 0) {
    if (me_key[h] == pc) return IMP_COUNT + HLP_COUNT + me_val[h];
    h = (h + 1) & (2 * XL_MAXFUNCS - 1);
  }
  (void)m;
  return -1;
}

void xl_module_init(xmodule *m) {
  memset(m, 0, sizeof *m);
  wb_init(&m->code);
  memset(me_val, 0xff, sizeof me_val);
}
void xl_module_free(xmodule *m) { wb_free(&m->code); }

int xl_add_entry(xmodule *m, uint64_t pc, uint32_t slot) {
  if (m->nfunc >= XL_MAXFUNCS) return -1;
  uint32_t h = (uint32_t)((pc >> 1) * 2654435761u) & (2 * XL_MAXFUNCS - 1);
  while (me_val[h] >= 0) { if (me_key[h] == pc) return 0; h = (h + 1) & (2 * XL_MAXFUNCS - 1); }
  me_key[h] = pc; me_val[h] = m->nfunc;
  m->entry[m->nfunc] = pc;
  m->slot[m->nfunc] = slot;
  m->nfunc++;
  return 0;
}

// After discover + analyse: is the region a leaf (no calls, no indirect jumps, every exit a
// return), and which registers does it read and write?
static void summarize(xmodule *m, int fi) {
  uint64_t use = 0, def = 0;
  int leaf = nord > 0;
  for (int i = 0; i < nord; i++) {
    xblock *k = &B[ord[i]];
    for (int j = 0; j < k->n; j++) {
      uint64_t u, d;
      arch_regs(&I[k->first + j], &u, &d);
      use |= u; def |= d;
      int f = I[k->first + j].flow;
      if (f == FL_CALL || f == FL_INDCALL || f == FL_INDJUMP || f == FL_STOP) leaf = 0;
      if (I[k->first + j].d.op == RV_ECALL) leaf = 0;   // a system call may switch guest threads: every register changes
    }
    if (k->term == FL_STOP) leaf = 0;
    if (k->term == FL_BRANCH || k->term == FL_JUMP || k->term == FL_NONE)
      for (int s = 0; s < k->nsucc; s++) if (k->succ[s] < 0) leaf = 0;
  }
  m->leaf[fi] = (uint8_t)leaf; m->fuse[fi] = use & ~1ull; m->fdef[fi] = def & ~1ull;
}

static void translate_one(xmodule *m, int fi, const xlat_env *env, const xlat_opts *o) {
  xlat_opts oo = *o;
  if (o->strategy == S_BLOCK) oo.max_blocks = 1;
  discover(m->entry[fi], &oo);
  int reducible = analyse();
  summarize(m, fi);
  strategy_now = o->strategy == S_DISPATCH ? S_DISPATCH : S_STRUCT;
  if (strategy_now == S_STRUCT && !reducible) { strategy_now = S_DISPATCH; m->irreducible++; }
  // registers the region touches
  uint64_t use = 0, def = 0;
  for (int i = 0; i < nI; i++) { uint64_t u, d; arch_regs(&I[i], &u, &d); use |= u; def |= d; }
  if (o->calls) for (int i = 0; i < nI; i++) if (I[i].flow == FL_CALL || I[i].flow == FL_INDCALL) { use |= def; break; }
  use &= ~1ull; def &= ~1ull;
  wbuf body;
  wb_init(&body);
  memset(&G, 0, sizeof G);
  G.b = &body; G.env = env; G.o = &oo; G.m = m;
  G.cache = use | def; G.dirty = def;
  // locals
  wb_uleb(&body, 8);
  wb_uleb(&body, 31); wb_byte(&body, WT_I64);   // x1..x31
  wb_uleb(&body, 2); wb_byte(&body, WT_I32);    // budget, reason
  wb_uleb(&body, 4); wb_byte(&body, WT_I64);    // t0 t1 t2 ic
  wb_uleb(&body, 1); wb_byte(&body, WT_I32);    // next
  wb_uleb(&body, 32); wb_byte(&body, WT_F64);   // f0..f31
  wb_uleb(&body, 2); wb_byte(&body, WT_I32);
  wb_uleb(&body, 2); wb_byte(&body, WT_F64);
  wb_uleb(&body, 2); wb_byte(&body, WT_F32);
  xg_reload(&G, G.cache);
  reload_misc(&G);
  ncx = 0; ys_sp = 0; bad_depth = 0;
  G.labels = 0;
  open(W_BLOCK, CX_EXIT, -1);
  if (oo.budget && oo.calls) { G.blk_n = 0; G.cur = &I[0]; budget_check(m->entry[fi], 0); }
  if (nord == 0 || B[0].rpo != 0) { G.blk_n = 0; xg_exit_const(&G, m->entry[fi], R_INTERP, 0); }
  else if (strategy_now == S_DISPATCH) emit_dispatch();
  else struct_tree(ord[0]);
  w_op(&body, W_UNREACHABLE);
  close_();
  xg_flush(&G, G.dirty);
  flush_misc(&G);
  w_lget(&body, L_REASON);
  w_op(&body, W_END);
  wb_uleb(&m->code, body.n);
  wb_bytes(&m->code, body.p, body.n);
  wb_free(&body);
  m->ninsn_total += nI;
  m->nblocks_total += nord;
  if (bad_depth) m->fallback_dispatch = -1000000;   // marks a translator bug
}

int xl_translate_all(xmodule *m, const xlat_env *env, const xlat_opts *o) {
  if (m->nfunc > 1) for (int i = 0; i < m->nfunc; i++) {   // summaries first, for calls between them
    xlat_opts oo = *o;
    if (o->strategy == S_BLOCK) oo.max_blocks = 1;
    discover(m->entry[i], &oo);
    analyse();
    summarize(m, i);
  }
  for (int i = 0; i < m->nfunc; i++) translate_one(m, i, env, o);
  return m->fallback_dispatch < 0 ? -1 : 0;
}

static void import_func(wbuf *s, const char *name, int type) {
  wb_name(s, "e"); wb_name(s, name); wb_byte(s, 0x00); wb_uleb(s, (uint32_t)type);
}
static void functype(wbuf *s, int np, const uint8_t *p, int nr, const uint8_t *r) {
  wb_byte(s, 0x60); wb_uleb(s, (uint32_t)np); wb_bytes(s, p, (uint32_t)np); wb_uleb(s, (uint32_t)nr); wb_bytes(s, r, (uint32_t)nr);
}
static void all_types(wbuf *t) {
  static const uint8_t i32[] = { WT_I32 }, i64[] = { WT_I64 }, f64x3[] = { WT_F64, WT_F64, WT_F64 }, f64[] = { WT_F64 };
  static const uint8_t f32x3[] = { WT_F32, WT_F32, WT_F32 }, f32[] = { WT_F32 }, i64x2[] = { WT_I64, WT_I64 };
  static const uint8_t fph[] = { WT_I32, WT_I64, WT_I64, WT_I64 };
  wb_uleb(t, T_COUNT);
  functype(t, 1, i32, 1, i32);          // T_REGION
  functype(t, 0, 0, 1, i32);            // T_VOID_I32
  functype(t, 1, i64, 1, i32);          // T_I64_I32
  functype(t, 3, f64x3, 1, f64);        // T_F64x3
  functype(t, 3, f32x3, 1, f32);        // T_F32x3
  functype(t, 2, i64x2, 1, i64);        // T_I64x2_I64
  functype(t, 4, fph, 1, i64);          // T_FPHELP: (op, a, b, c) -> result
}

void xl_module_bytes(xmodule *m, wbuf *out) {
  wbuf s;
  static const uint8_t magic[] = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };
  wb_bytes(out, magic, 8);
  wb_init(&s); all_types(&s); wb_section(out, 1, &s); wb_free(&s);
  wb_init(&s);
  wb_uleb(&s, IMP_COUNT + 2);
  import_func(&s, "jit_ecall", T_VOID_I32);
  import_func(&s, "jit_yield", T_VOID_I32);
  import_func(&s, "jit_lookup", T_I64_I32);
  import_func(&s, "jit_fma64", T_F64x3);
  import_func(&s, "jit_fma32", T_F32x3);
  import_func(&s, "jit_fphelp", T_FPHELP);
  wb_name(&s, "e"); wb_name(&s, "__indirect_function_table"); wb_byte(&s, 0x01); wb_byte(&s, WT_FUNCREF); wb_byte(&s, 0x00); wb_uleb(&s, 0);
  wb_name(&s, "e"); wb_name(&s, "memory"); wb_byte(&s, 0x02); wb_byte(&s, 0x00); wb_uleb(&s, 0);
  wb_section(out, 2, &s); wb_free(&s);
  wb_init(&s);
  wb_uleb(&s, (uint32_t)(HLP_COUNT + m->nfunc));
  for (int i = 0; i < HLP_COUNT; i++) wb_uleb(&s, T_I64x2_I64);
  for (int i = 0; i < m->nfunc; i++) wb_uleb(&s, T_REGION);
  wb_section(out, 3, &s); wb_free(&s);
  if (m->nglobals) {
    wb_init(&s);
    wb_uleb(&s, m->nglobals);
    for (uint32_t i = 0; i < m->nglobals; i += 2) {
      wb_byte(&s, WT_I64); wb_byte(&s, 1); w_i64c(&s, -1); w_op(&s, W_END);   // cached target (odd: matches nothing)
      wb_byte(&s, WT_I32); wb_byte(&s, 1); w_i32c(&s, 0); w_op(&s, W_END);    // its slot
    }
    wb_section(out, 6, &s); wb_free(&s);
  }
  wb_init(&s);
  wb_uleb(&s, (uint32_t)m->nfunc);
  for (int i = 0; i < m->nfunc; i++) {
    wb_byte(&s, 0x00);                                  // active, table 0, funcref
    w_i32c(&s, (int32_t)m->slot[i]); w_op(&s, W_END);
    wb_uleb(&s, 1); wb_uleb(&s, (uint32_t)(IMP_COUNT + HLP_COUNT + i));
  }
  wb_section(out, 9, &s); wb_free(&s);
  wb_init(&s);
  wb_uleb(&s, (uint32_t)(HLP_COUNT + m->nfunc));
  arch_emit_helpers(&s, m->helpers_used);
  wb_bytes(&s, m->code.p, m->code.n);
  wb_section(out, 10, &s); wb_free(&s);
}

// The support module: grow(n) -> first new slot (filled with the stub), stub(slot) -> 0.
// Its start function grows the table by two, puts grow and stub there, and stores the
// first of the two slots at out_addr (stub is at that slot + 1).
void xl_support_module(wbuf *out, uint32_t out_addr) {
  wbuf s, f;
  static const uint8_t magic[] = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };
  static const uint8_t i32[] = { WT_I32 };
  wb_bytes(out, magic, 8);
  wb_init(&s); wb_uleb(&s, 2); functype(&s, 1, i32, 1, i32); functype(&s, 0, 0, 0, 0); wb_section(out, 1, &s); wb_free(&s);
  wb_init(&s); wb_uleb(&s, 3);
  import_func(&s, "jit_stub", 0);
  wb_name(&s, "e"); wb_name(&s, "__indirect_function_table"); wb_byte(&s, 0x01); wb_byte(&s, WT_FUNCREF); wb_byte(&s, 0x00); wb_uleb(&s, 0);
  wb_name(&s, "e"); wb_name(&s, "memory"); wb_byte(&s, 0x02); wb_byte(&s, 0x00); wb_uleb(&s, 0);
  wb_section(out, 2, &s); wb_free(&s);
  wb_init(&s); wb_uleb(&s, 3); wb_uleb(&s, 0); wb_uleb(&s, 0); wb_uleb(&s, 1); wb_section(out, 3, &s); wb_free(&s);
  wb_init(&s); wb_uleb(&s, 3); wb_section(out, 8, &s); wb_free(&s);                  // start = function 3
  wb_init(&s); wb_uleb(&s, 1); wb_byte(&s, 0x03); wb_byte(&s, 0x00);                 // declarative: 0 1 2
  wb_uleb(&s, 3); wb_uleb(&s, 0); wb_uleb(&s, 1); wb_uleb(&s, 2); wb_section(out, 9, &s); wb_free(&s);
  wb_init(&s); wb_uleb(&s, 3);
  wb_init(&f);                                         // grow(n)
  wb_uleb(&f, 0); w_op(&f, W_REF_FUNC); wb_uleb(&f, 0); w_lget(&f, 0); w_fc(&f, WFC_TABLE_GROW); wb_uleb(&f, 0); w_op(&f, W_END);
  wb_uleb(&s, f.n); wb_bytes(&s, f.p, f.n); f.n = 0;
  wb_uleb(&f, 0);                                      // stub(slot)
  w_lget(&f, 0); w_op(&f, W_REF_FUNC); wb_uleb(&f, 0); w_op(&f, W_TABLE_SET); wb_uleb(&f, 0); w_i32c(&f, 0); w_op(&f, W_END);
  wb_uleb(&s, f.n); wb_bytes(&s, f.p, f.n); f.n = 0;
  wb_uleb(&f, 1); wb_uleb(&f, 1); wb_byte(&f, WT_I32); // start: local old
  w_op(&f, W_REF_NULL); wb_byte(&f, WT_FUNCREF); w_i32c(&f, 2); w_fc(&f, WFC_TABLE_GROW); wb_uleb(&f, 0); w_lset(&f, 0);
  w_lget(&f, 0); w_op(&f, W_REF_FUNC); wb_uleb(&f, 1); w_op(&f, W_TABLE_SET); wb_uleb(&f, 0);
  w_lget(&f, 0); w_i32c(&f, 1); w_op(&f, W_I32_ADD); w_op(&f, W_REF_FUNC); wb_uleb(&f, 2); w_op(&f, W_TABLE_SET); wb_uleb(&f, 0);
  w_i32c(&f, (int32_t)out_addr); w_lget(&f, 0); w_mem(&f, W_I32_STORE, 2, 0);
  w_op(&f, W_END);
  wb_uleb(&s, f.n); wb_bytes(&s, f.p, f.n);
  wb_free(&f);
  wb_section(out, 10, &s); wb_free(&s);
}

// The blocks of the region translated last, as (start, end, reverse postorder) triples,
// for lockstep checking.
int xl_last_ranges(uint64_t *out, int cap) {
  int n = 0;
  for (int b = 0; b < nB && n < cap; b++) {
    if (B[b].rpo < 0 || B[b].n == 0) continue;
    const xinsn *last = &I[B[b].first + B[b].n - 1];
    out[3 * n] = B[b].start;
    out[3 * n + 1] = last->pc + last->d.len;
    out[3 * n + 2] = (uint64_t)B[b].rpo;
    n++;
  }
  return n;
}
