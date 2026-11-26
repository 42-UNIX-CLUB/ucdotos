// SPDX-License-Identifier: GPL-2.0-or-later
// The translator as a component of an engine. See jitcore.h. This file knows the
// translator (xlat.h, rv.h of libjit) but not the engine's headers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "jitcore.h"
#include "xlat.h"
#include "engine.h"   // only for the reference interpreter used by lockstep (rvinterp.c)
#include "rvfp.h"

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
#else
#define EXPORT(name)
#endif

int jitcore_mode = JX_OFF;
int jitcore_verbose;

static xlat_opts XO;
static int threshold = 50, budget = 4096;
static uint64_t cache_max = 32u << 20;
static struct jitcore_layout LY;
static xlat_env XE;
static struct { int32_t budget; int32_t pad; uint64_t icount, fault, site; } jx;   // translator bookkeeping
static int loader_slot;           // the host's loader in our table (0: none)
static int leave_now;             // set by the yield helper: return to the engine

static struct {
  uint64_t regions, enters, fails, yields, lookups, misses, flushes, wasm_bytes, live_bytes, insns, lockstep, lockstep_bad;
  double t_gen, t_load;
} st;

// ---- what libjit's other files need from their environment ----
void *jit_malloc(size_t n) { return malloc(n); }
void *jit_realloc(void *p, size_t n) { return realloc(p, n); }
void jit_free(void *p) { free(p); }
void *jit_alloc_guest(size_t n) { (void)n; return 0; }
void jit_print(const char *s, size_t n) { if (write(2, s, n) < 0) {} }
double jit_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }
void jit_exit(int code) { exit(code); }
int jit_host_load(const void *p, size_t len) {
  if (!loader_slot) return -1;
  return ((int (*)(const void *, size_t))(uintptr_t)(unsigned)loader_slot)(p, len);
}
void port_guest_write(int fd, const void *p, size_t n) { (void)fd; (void)p; (void)n; }
void port_timeslice(void) {}
// the reference interpreter (lockstep) works on a shadow state over the same guest memory
uint8_t *rv_mem;
uint64_t rv_memsize;
static rv_cpu ref;
rv_cpu *cpu = &ref;
int engine_ecall(void) { return R_INTERP; }
void engine_fence_i(void) {}
void engine_frm_changed(void) {}

EXPORT(jit_attach) void jitcore_attach(int slot) { loader_slot = slot; }

// ---- options ----
static int eq(const char *a, int n, const char *b) { return (int)strlen(b) == n && !strncmp(a, b, (size_t)n); }
static void defaults(void);
int jitcore_options(const char *s) {
  defaults();
  while (s && *s) {
    while (*s == ' ' || *s == ',') s++;
    if (!*s) break;
    const char *k = s;
    while (*s && *s != '=' && *s != ' ' && *s != ',') s++;
    int kn = (int)(s - k);
    const char *v = "1";
    int vn = 1;
    if (*s == '=') { v = ++s; while (*s && *s != ' ' && *s != ',') s++; vn = (int)(s - v); }
    long n = atol(v);
    if (eq(k, kn, "strategy")) XO.strategy = eq(v, vn, "block") ? S_BLOCK : eq(v, vn, "dispatch") ? S_DISPATCH : S_STRUCT;
    else if (eq(k, kn, "bounds")) XO.bounds = eq(v, vn, "unchecked") ? B_UNCHECKED : eq(v, vn, "masked") ? B_MASKED : B_CHECKED;
    else if (eq(k, kn, "threshold")) threshold = (int)n;
    else if (eq(k, kn, "budget")) { budget = (int)n; XO.budget = n > 0; }
    else if (eq(k, kn, "calls")) XO.calls = (int)n;
    else if (eq(k, kn, "chain")) XO.chain = (int)n;
    else if (eq(k, kn, "learn")) XO.learn = (int)n;
    else if (eq(k, kn, "ic")) XO.ic = (int)n;
    else if (eq(k, kn, "split")) XO.split = (int)n;
    else if (eq(k, kn, "abi")) XO.abi = (int)n;
    else if (eq(k, kn, "fp")) arch_fp_enabled = (int)n;
    else if (eq(k, kn, "cache")) cache_max = (uint64_t)n << 10;
    else if (eq(k, kn, "maxblocks")) XO.max_blocks = (int)n;
    else if (eq(k, kn, "maxinsns")) XO.max_insns = (int)n;
    else if (eq(k, kn, "verbose")) jitcore_verbose = (int)n;
    else return -1;
  }
  if (!XO.calls) XO.indirect = 0;
  return 0;
}
static void defaults(void) {
  static int done;
  if (done) return;
  done = 1;
  XO.strategy = S_STRUCT; XO.calls = 1; XO.indirect = 1; XO.chain = 1; XO.ic = 1; XO.learn = 1;
  XO.bounds = B_CHECKED;   // faults where the interpreter faults; see docs/BINARY-TRANSLATION.md 3.5
  XO.budget = 1; XO.max_insns = 4096; XO.max_blocks = 512;
}
int jitcore_threshold(void) { return threshold; }

// ---- regions and table slots ----
enum { RS_COLD, RS_DONE, RS_FAILED };
typedef struct region {
  uint64_t pc;
  uint32_t slot;
  uint8_t state, retrans, leaf;
  uint64_t use, def;
  uint32_t nr;
  uint64_t *ranges;
  struct region *next;
} region;
#define RH (1u << 16)
static region *rhash[RH];
static uint32_t support_slots[2];
static uint32_t slot_next, slot_end;
static inline uint32_t rh(uint64_t pc) { return (uint32_t)((pc >> 1) * 2654435761u) >> 16; }
static region *rfind(uint64_t pc) { for (region *r = rhash[rh(pc)]; r; r = r->next) if (r->pc == pc) return r; return 0; }
static region *rget(uint64_t pc) {
  region *r = rfind(pc);
  if (r) return r;
  r = calloc(1, sizeof *r);
  r->pc = pc;
  r->next = rhash[rh(pc)];
  rhash[rh(pc)] = r;
  return r;
}
static uint32_t alloc_slot(void) {
  if (slot_next == slot_end) {
    int first = ((int (*)(int))(uintptr_t)support_slots[0])(1024);
    if (first < 0) return 0;
    slot_next = (uint32_t)first; slot_end = slot_next + 1024;
  }
  return slot_next++;
}
uint32_t engine_slot_for(uint64_t pc) { region *r = rget(pc); if (!r->slot) r->slot = alloc_slot(); return r->slot; }
int engine_readable(uint64_t pc, uint32_t *raw) { return !(pc & 1) && jitcore_host_fetch(pc, raw); }

// learned targets of indirect jumps
typedef struct site { uint64_t pc; int n, nregs; uint32_t misses; uint64_t t[16]; region *regs[4]; struct site *next; } site;
static site *shash[4096];
static region *translating;
static site *sget(uint64_t pc, int create) {
  uint32_t h = (uint32_t)((pc >> 1) * 2654435761u) >> 20;
  for (site *s = shash[h]; s; s = s->next) if (s->pc == pc) return s;
  if (!create) return 0;
  site *s = calloc(1, sizeof *s);
  s->pc = pc; s->next = shash[h]; shash[h] = s;
  return s;
}
int engine_site_targets(uint64_t pc, uint64_t *out, int cap) {
  if (!XO.learn) return 0;
  site *s = sget(pc, 0);
  if (!s) return 0;
  int n = s->n < cap ? s->n : cap;
  memcpy(out, s->t, sizeof(uint64_t) * (size_t)n);
  return n;
}
void engine_site_in_region(uint64_t pc) {
  site *s = sget(pc, 1);
  if (translating && s->nregs < 4) s->regs[s->nregs++] = translating;
  s->misses = 0;
}
int engine_callee_summary(uint64_t pc, uint64_t *use, uint64_t *def) {
  region *r = rfind(pc);
  if (!r || r->state != RS_DONE || !r->leaf) return 0;
  *use = r->use; *def = r->def;
  return 1;
}
static void drop(region *r) {
  if (r->state == RS_DONE && support_slots[1]) ((int (*)(int))(uintptr_t)support_slots[1])((int)r->slot);
  r->state = RS_COLD;
}
static void learn(uint64_t sitepc, uint64_t target) {
  site *s = sget(sitepc, 1);
  int known = 0;
  for (int i = 0; i < s->n; i++) if (s->t[i] == target) known = 1;
  if (!known && s->n < 16) s->t[s->n++] = target;
  if (++s->misses < 256) return;
  s->misses = 0;
  for (int i = 0; i < s->nregs; i++) { region *r = s->regs[i]; if (r->state == RS_DONE && r->retrans < 4) { drop(r); r->retrans++; } }
  s->nregs = 0;
}

void jitcore_flush(void) {
  for (uint32_t h = 0; h < RH; h++) for (region *r = rhash[h]; r; r = r->next) drop(r);
  for (uint32_t h = 0; h < 4096; h++) for (site *s = shash[h]; s; s = s->next) s->nregs = 0;
  st.live_bytes = 0;
  st.flushes++;
}
void jitcore_frm_changed(int frm) { if (frm != rvfp_frm) { rvfp_frm = frm; jitcore_flush(); } }

// ---- helpers called by translated code (exported by the engine program) ----
EXPORT(jit_stub) int jit_stub(int depth) { (void)depth; return R_STUB; }
EXPORT(jit_yield) int jit_yield(void) {
  st.yields++;
  if (jitcore_mode == JX_LOCKSTEP) return -1;   // lockstep compares one loop iteration at a time
  if (jitcore_host_yield()) { leave_now = 1; return -1; }
  return budget;
}
EXPORT(jit_lookup) int jit_lookup(uint64_t pc) {
  st.lookups++;
  if (jx.site) { learn(jx.site, pc); jx.site = 0; }
  region *r = rfind(pc);
  if (r && r->state == RS_DONE) return (int)r->slot;
  st.misses++;
  return -1;
}
EXPORT(jit_ecall) int jit_ecall(void) { return jitcore_host_ecall() ? R_JUMP : 0; }
EXPORT(jit_fma64) double jit_fma64(double a, double b, double c) { return rvfp_fma64(a, b, c); }
EXPORT(jit_fma32) float jit_fma32(float a, float b, float c) { return rvfp_fma32(a, b, c); }
EXPORT(jit_fphelp) uint64_t jit_fphelp(int op, uint64_t a, uint64_t b, uint64_t c) { return rvfp_help(0, op, a, b, c); }

// ---- set-up ----
static int load_support(void) {
  wbuf sup;
  wb_init(&sup);
  xl_support_module(&sup, (uint32_t)(uintptr_t)support_slots);
  int e = jit_host_load(sup.p, sup.n);
  wb_free(&sup);
  if (e) return -1;
  support_slots[1] = support_slots[0] + 1;
  slot_next = slot_end = 0;
  return 0;
}
int jitcore_init(const struct jitcore_layout *l) {
  defaults();
  LY = *l;
  memset(&XE, 0, sizeof XE);
  XE.a_x = l->x; XE.a_f = l->f; XE.a_pc = l->pc; XE.a_fcsr = l->fcsr; XE.a_res = l->res;
  XE.a_budget = (uint32_t)(uintptr_t)&jx.budget; XE.a_icount = (uint32_t)(uintptr_t)&jx.icount;
  XE.a_fault = (uint32_t)(uintptr_t)&jx.fault; XE.a_site = (uint32_t)(uintptr_t)&jx.site;
  XE.gbase = l->gbase; XE.gsize = l->gsize; XE.glo = l->glo; XE.gspan = l->gspan;
  if (XO.bounds == B_MASKED && (l->gsize & (l->gsize - 1))) XO.bounds = B_CHECKED;   // masking needs a power of two
  rv_mem = (uint8_t *)(uintptr_t)l->gbase;
  rv_memsize = l->gsize;
  jx.budget = budget;
  if (jitcore_mode == JX_LOCKSTEP) { XO.calls = 0; XO.indirect = 0; XO.chain = 0; XO.learn = 0; XO.ecall_exits = 1; XO.count = 1; XO.budget = 1; }
  if (!loader_slot || load_support()) {
    if (jitcore_verbose) jitcore_host_log("translator unavailable (no loader from the host): interpreting");
    jitcore_mode = JX_OFF;
    return -1;
  }
  return 0;
}

static wbuf modbuf;
static xmodule XM;
int jitcore_translate(uint64_t pc) {
  if (jitcore_mode == JX_OFF) return 0;
  region *r = rget(pc);
  if (r->state == RS_DONE) return 1;
  if (r->state == RS_FAILED) return 0;
  double t0 = jit_now_ms();
  if (!r->slot) r->slot = alloc_slot();
  if (!r->slot) { r->state = RS_FAILED; return 0; }
  xl_module_init(&XM);
  translating = r;
  xl_add_entry(&XM, pc, r->slot);
  int bad = xl_translate_all(&XM, &XE, &XO) < 0;
  translating = 0;
  modbuf.n = 0;
  if (!bad) xl_module_bytes(&XM, &modbuf);
  if (jitcore_mode == JX_LOCKSTEP && !bad) {
    extern int xl_last_ranges(uint64_t *out, int cap);
    static uint64_t tmp[3 * 1024];
    int n = xl_last_ranges(tmp, 1024);
    free(r->ranges);
    r->ranges = malloc(sizeof(uint64_t) * 3 * (size_t)(n ? n : 1));
    memcpy(r->ranges, tmp, sizeof(uint64_t) * 3 * (size_t)n);
    r->nr = (uint32_t)n;
  }
  if (!bad && st.live_bytes + modbuf.n > cache_max) {   // the bound on generated code: start again
    jitcore_flush();
    if (!r->slot) r->slot = alloc_slot();
  }
  double t1 = jit_now_ms();
  int e = bad ? -1 : jit_host_load(modbuf.p, modbuf.n);
  double t2 = jit_now_ms();
  st.t_gen += t1 - t0; st.t_load += t2 - t1;
  if (e) {
    st.fails++;
    r->state = RS_FAILED;
    xl_module_free(&XM);
    if (jitcore_verbose) { char m[96]; snprintf(m, sizeof m, "region %llx not translated (%d)", (unsigned long long)pc, bad ? -9 : e); jitcore_host_log(m); }
    return 0;
  }
  st.regions++; st.wasm_bytes += modbuf.n; st.live_bytes += modbuf.n; st.insns += (uint64_t)XM.ninsn_total;
  r->state = RS_DONE; r->leaf = XM.leaf[0]; r->use = XM.fuse[0]; r->def = XM.fdef[0];
  xl_module_free(&XM);
  return 1;
}
int jitcore_ready(uint64_t pc) { region *r = rfind(pc); return r && r->state == RS_DONE; }

void jitcore_after_copy(void) {
  support_slots[0] = support_slots[1] = 0;
  slot_next = slot_end = 0;
  st.live_bytes = 0;
  if (jitcore_mode == JX_OFF) return;
  if (load_support()) { jitcore_mode = JX_OFF; return; }
  for (uint32_t h = 0; h < RH; h++) for (region *r = rhash[h]; r; r = r->next) { if (r->state != RS_FAILED) r->state = RS_COLD; r->slot = 0; r->retrans = 0; }
  for (uint32_t h = 0; h < 4096; h++) for (site *s = shash[h]; s; s = s->next) s->nregs = 0;
}

// ---- lockstep: the reference interpreter (libjit/rvinterp.c) on a shadow state ----
typedef struct { uint64_t addr; uint8_t n, oldv[8], newv[8]; } slog;
static slog *logv;
static int nlog, caplog, logging;
void rv_store_hook(uint64_t addr, int n) {
  if (!logging) return;
  if (nlog == caplog) { caplog = caplog ? caplog * 2 : 4096; logv = realloc(logv, sizeof(slog) * (size_t)caplog); }
  logv[nlog].addr = addr; logv[nlog].n = (uint8_t)n;
  memcpy(logv[nlog].oldv, rv_mem + addr, (size_t)n);
  nlog++;
}
#define HX(r) (*(uint64_t *)(uintptr_t)(LY.x + 8u * (r)))
#define HF(r) (*(uint64_t *)(uintptr_t)(LY.f + 8u * (r)))
#define HPC (*(uint64_t *)(uintptr_t)LY.pc)
#define HFCSR (*(uint32_t *)(uintptr_t)LY.fcsr)
static int block_of(region *r, uint64_t pc) {
  for (uint32_t i = 0; i < r->nr; i++) if (pc >= r->ranges[3 * i] && pc < r->ranges[3 * i + 1]) return (int)r->ranges[3 * i + 2];
  return -1;
}
static int lockstep(region *r) {
  memset(&ref, 0, sizeof ref);
  for (int i = 0; i < 32; i++) { ref.x[i] = i ? HX(i) : 0; ref.f[i] = HF(i); }
  ref.pc = HPC; ref.fcsr = HFCSR; ref.res_addr = ~0ull;
  uint64_t xs[32], fs[32], pc0 = HPC; uint32_t fcsr0 = HFCSR;
  for (int i = 0; i < 32; i++) { xs[i] = HX(i); fs[i] = HF(i); }
  nlog = 0; logging = 1;
  int rwhy = -1; uint64_t ric0 = ref.icount; jx.icount = 0;
  for (int b = block_of(r, ref.pc); b >= 0;) {
    uint64_t pc = ref.pc;
    uint32_t raw = 0;
    rv_insn d;
    jitcore_host_fetch(pc, &raw);
    int known = rv_decode(raw, &d);
    if ((rwhy = rv_interp_one(&ref)) >= 0) break;
    if (known && (d.op == RV_JALR || (d.op == RV_JAL && d.rd))) break;
    int nb = block_of(r, ref.pc);
    if (nb < 0) break;
    if ((ref.pc != pc + d.len || nb != b) && nb <= b) break;
    b = nb;
  }
  logging = 0;
  for (int i = 0; i < nlog; i++) memcpy(logv[i].newv, rv_mem + logv[i].addr, logv[i].n);
  for (int i = nlog - 1; i >= 0; i--) memcpy(rv_mem + logv[i].addr, logv[i].oldv, logv[i].n);
  for (int i = 1; i < 32; i++) { HX(i) = xs[i]; HF(i) = fs[i]; }
  HPC = pc0; HFCSR = fcsr0;
  jx.budget = 1;
  int why = ((int (*)(int))(uintptr_t)r->slot)(0);
  st.lockstep++;
  int bad = 0;
  char m[160];
  for (int i = 1; i < 32 && !bad; i++) if (HX(i) != ref.x[i]) { snprintf(m, sizeof m, "lockstep: region %llx: x%d %llx, reference %llx", (unsigned long long)r->pc, i, (unsigned long long)HX(i), (unsigned long long)ref.x[i]); bad = 1; }
  for (int i = 0; i < 32 && !bad; i++) if (HF(i) != ref.f[i]) { snprintf(m, sizeof m, "lockstep: region %llx: f%d %llx, reference %llx", (unsigned long long)r->pc, i, (unsigned long long)HF(i), (unsigned long long)ref.f[i]); bad = 1; }
  if (!bad && why != R_FAULT && HPC != ref.pc) { snprintf(m, sizeof m, "lockstep: region %llx: pc %llx, reference %llx", (unsigned long long)r->pc, (unsigned long long)HPC, (unsigned long long)ref.pc); bad = 1; }
  for (int i = 0; i < nlog && !bad; i++) if (memcmp(rv_mem + logv[i].addr, logv[i].newv, logv[i].n)) { snprintf(m, sizeof m, "lockstep: region %llx: memory at %llx", (unsigned long long)r->pc, (unsigned long long)logv[i].addr); bad = 1; }
  if (bad) {
    st.lockstep_bad++;
    if (st.lockstep_bad <= 20) {
      jitcore_host_log(m);
      snprintf(m, sizeof m, "  reference ran %llu instructions (stop %d), translated %llu (reason %d); start pc %llx",
               (unsigned long long)(ref.icount - ric0), rwhy, (unsigned long long)jx.icount, why, (unsigned long long)pc0);
      jitcore_host_log(m);
    }
  }
  return why;
}

// ---- running translated code ----
int jitcore_enter(uint64_t *fault_addr) {
  leave_now = 0;
  for (;;) {
    region *r = rfind(HPC);
    if (!r || r->state != RS_DONE) return JX_INTERP;
    st.enters++;
    int why = jitcore_mode == JX_LOCKSTEP ? lockstep(r) : ((int (*)(int))(uintptr_t)r->slot)(0);
    if (jx.site) { learn(jx.site, HPC); jx.site = 0; }
    switch (why) {
    case R_JUMP: case R_RET: case R_STUB:
      if (leave_now) return JX_YIELD;
      if (jitcore_mode == JX_LOCKSTEP) return JX_INTERP;
      continue;
    case R_FAULT: *fault_addr = jx.fault; return JX_FAULT;
    default: return JX_INTERP;   // R_INTERP, R_EBREAK, R_HALT: the interpreter takes it from here
    }
  }
}

void jitcore_report(void) {
  char m[400];
  snprintf(m, sizeof m, "translator: %llu regions (%llu guest instructions, %llu bytes of wasm, %llu live), %llu failed, %llu entries, %llu yields, "
           "%llu lookups (%llu missed), %llu flushes, generation %.1f ms, host compile %.1f ms, lockstep %llu checks %llu differences",
           (unsigned long long)st.regions, (unsigned long long)st.insns, (unsigned long long)st.wasm_bytes, (unsigned long long)st.live_bytes,
           (unsigned long long)st.fails, (unsigned long long)st.enters, (unsigned long long)st.yields, (unsigned long long)st.lookups,
           (unsigned long long)st.misses, (unsigned long long)st.flushes, st.t_gen, st.t_load, (unsigned long long)st.lockstep, (unsigned long long)st.lockstep_bad);
  jitcore_host_log(m);
}
