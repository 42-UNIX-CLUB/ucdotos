// SPDX-License-Identifier: GPL-2.0-or-later
// The prototype engine around the translator. See engine.h.
#include "engine.h"
#include "rvfp.h"

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(#name)))
#else
#define EXPORT(name)
#endif

uint8_t *rv_mem;
uint64_t rv_memsize;
static rv_cpu cpu_state __attribute__((aligned(64)));
rv_cpu *cpu = &cpu_state;
engine_opts E;
engine_stats S;

static const uint8_t *elf_img;
static size_t elf_len;
static uint64_t brk_start, brk_cur;
static uint32_t out_hash = 2166136261u;
static uint64_t out_bytes;

// ---- options ----
void engine_defaults(void) {
  memset(&E, 0, sizeof E);
  E.mode = M_JIT;
  E.x.strategy = S_STRUCT;
  E.x.calls = 1;
  E.x.bounds = B_MASKED;
  E.x.budget = 1;
  E.x.max_insns = 4096;
  E.x.max_blocks = 512;
  E.x.indirect = 1;
  E.x.chain = 1;
  E.x.ic = 1;
  E.x.learn = 1;
  E.threshold = 50;
  E.budget = 4096;
  E.memsize = 64u << 20;
  E.repeat = 1;
  E.cache_max = 64u << 20;
}

static int streq(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return b[n] == 0; }
static long num(const char *s, int n) { long v = 0; int neg = 0, i = 0; if (n && s[0] == '-') { neg = 1; i = 1; } for (; i < n; i++) v = v * 10 + (s[i] - '0'); return neg ? -v : v; }

int engine_parse_opts(const char *s) {
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char *k = s;
    while (*s && *s != '=' && *s != ' ') s++;
    int kn = (int)(s - k);
    const char *v = "1";
    int vn = 1;
    if (*s == '=') { v = ++s; while (*s && *s != ' ') s++; vn = (int)(s - v); }
#define KEY(name) (kn == (int)sizeof(name) - 1 && streq(k, name, kn))
#define VAL(name) streq(v, name, vn)
    if (KEY("mode")) E.mode = VAL("interp") ? M_INTERP : VAL("pre") ? M_PRE : VAL("aot") ? M_AOT : M_JIT;
    else if (KEY("strategy")) E.x.strategy = VAL("block") ? S_BLOCK : VAL("dispatch") ? S_DISPATCH : S_STRUCT;
    else if (KEY("bounds")) E.x.bounds = VAL("unchecked") ? B_UNCHECKED : VAL("checked") ? B_CHECKED : B_MASKED;
    else if (KEY("calls")) E.x.calls = (int)num(v, vn);
    else if (KEY("indirect")) E.x.indirect = (int)num(v, vn);
    else if (KEY("chain")) E.x.chain = (int)num(v, vn);
    else if (KEY("ic")) E.x.ic = (int)num(v, vn);
    else if (KEY("learn")) E.x.learn = (int)num(v, vn);
    else if (KEY("abi")) E.x.abi = (int)num(v, vn);
    else if (KEY("budget")) { E.budget = (int)num(v, vn); E.x.budget = E.budget > 0; }
    else if (KEY("threshold")) E.threshold = (int)num(v, vn);
    else if (KEY("yield_exit")) E.yield_exit = (int)num(v, vn);
    else if (KEY("lockstep")) E.lockstep = (int)num(v, vn);
    else if (KEY("verbose")) E.verbose = (int)num(v, vn);
    else if (KEY("stats")) E.stats = (int)num(v, vn);
    else if (KEY("repeat")) E.repeat = (int)num(v, vn);
    else if (KEY("fp")) arch_fp_enabled = (int)num(v, vn);
    else if (KEY("fastfma")) { extern int rvfp_fast_fma; rvfp_fast_fma = (int)num(v, vn); }
    else if (KEY("maxinsns")) E.x.max_insns = (int)num(v, vn);
    else if (KEY("maxblocks")) E.x.max_blocks = (int)num(v, vn);
    else if (KEY("copytest")) E.copytest = (int)num(v, vn);
    else if (KEY("cache")) E.cache_max = (uint64_t)num(v, vn) << 10;   // KiB
    else if (KEY("mem")) E.memsize = (uint64_t)num(v, vn) << 20;
    else { jit_printf("unknown option %.*s\n", kn, k); return -1; }
  }
  if (E.lockstep) { E.x.calls = 0; E.x.budget = 1; E.yield_exit = 1; E.x.ecall_exits = 1; E.x.count = 1; E.x.indirect = 0; E.x.chain = 0; E.x.learn = 0; }
  if (!E.x.calls) E.x.indirect = 0;
  return 0;
}

// ---- regions and table slots ----
enum { RS_COLD = 0, RS_DONE = 1, RS_FAILED = 2 };
typedef struct region {
  uint64_t pc;
  uint32_t slot, count;
  uint8_t state, retrans, leaf;
  uint64_t use, def;
  uint32_t nr;
  uint64_t *ranges;          // lockstep: [start, end) pairs of the region's blocks
  struct region *next;
} region;
#define RH (1u << 16)
static region *rhash[RH];
static uint32_t support_slots[2];   // written by the support module: grow, stub
static uint32_t slot_next, slot_end;

static inline uint32_t rh(uint64_t pc) { return (uint32_t)((pc >> 1) * 2654435761u) >> 16; }
static region *rfind(uint64_t pc) {
  for (region *r = rhash[rh(pc)]; r; r = r->next) if (r->pc == pc) return r;
  return 0;
}
static region *rget(uint64_t pc) {
  region *r = rfind(pc);
  if (r) return r;
  r = jit_malloc(sizeof *r);
  memset(r, 0, sizeof *r);
  r->pc = pc;
  uint32_t h = rh(pc);
  r->next = rhash[h];
  rhash[h] = r;
  return r;
}

static uint32_t alloc_slot(void) {
  if (slot_next == slot_end) {
    int n = 1024;
    int first = jit_call(support_slots[0], n);
    if (first < 0) return 0;
    slot_next = (uint32_t)first;
    slot_end = slot_next + (uint32_t)n;
  }
  return slot_next++;
}

uint32_t engine_slot_for(uint64_t pc) {
  region *r = rget(pc);
  if (!r->slot) r->slot = alloc_slot();
  return r->slot;
}

int engine_readable(uint64_t pc, uint32_t *raw) {
  if (pc & 1 || pc > rv_memsize - 4) return 0;
  memcpy(raw, rv_mem + pc, 4);
  return 1;
}

// ---- learned targets of indirect jumps ----
typedef struct site { uint64_t pc; int n, nregs; uint32_t misses; uint64_t t[16]; region *regs[4]; struct site *next; } site;
static site *shash[4096];
static region *translating;
static site *sget(uint64_t pc, int create) {
  uint32_t h = (uint32_t)((pc >> 1) * 2654435761u) >> 20;
  for (site *s = shash[h]; s; s = s->next) if (s->pc == pc) return s;
  if (!create) return 0;
  site *s = jit_malloc(sizeof *s);
  memset(s, 0, sizeof *s);
  s->pc = pc; s->next = shash[h]; shash[h] = s;
  return s;
}
int engine_site_targets(uint64_t pc, uint64_t *out, int cap) {
  if (!E.x.learn) return 0;
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
// The indirect jump at sitepc went to target, which its region did not know. After enough
// such misses, retranslate the regions that contain the site with the targets known now.
static void learn(uint64_t sitepc, uint64_t target) {
  site *s = sget(sitepc, 1);
  int known = 0;
  for (int i = 0; i < s->n; i++) if (s->t[i] == target) known = 1;
  if (!known && s->n < 16) s->t[s->n++] = target;
  if (++s->misses < 256) return;
  s->misses = 0;
  for (int i = 0; i < s->nregs; i++) {
    region *r = s->regs[i];
    if (r->state != RS_DONE || r->retrans >= 4) continue;
    jit_call(support_slots[1], (int)r->slot);
    r->state = RS_COLD;
    r->count = (uint32_t)E.threshold - 1;
    r->retrans++;
  }
  s->nregs = 0;
}

int engine_callee_summary(uint64_t pc, uint64_t *use, uint64_t *def) {
  region *r = rfind(pc);
  if (!r || r->state != RS_DONE || !r->leaf) return 0;
  *use = r->use; *def = r->def;
  return 1;
}

// ---- helpers called by translated code ----
EXPORT(jit_stub) int jit_stub(int depth) { (void)depth; return R_STUB; }

static int yield_now(void) {
  S.yields++;
  port_timeslice();
  return E.yield_exit ? -1 : E.budget;
}
EXPORT(jit_yield) int jit_yield(void) { return yield_now(); }

EXPORT(jit_lookup) int jit_lookup(uint64_t pc) {
  S.lookups++;
  if (cpu->site) { learn(cpu->site, pc); cpu->site = 0; }
  region *r = rfind(pc);
  if (r && r->state == RS_DONE) return (int)r->slot;
  S.lookup_miss++;
  if (E.mode != M_INTERP) {     // count it as a region entry, so hot indirect targets get translated
    if (!r) r = rget(pc);
    r->count++;
  }
  return -1;
}

EXPORT(jit_ecall) int jit_ecall(void) { return engine_ecall(); }
EXPORT(jit_fma64) double jit_fma64(double a, double b, double c) { return rvfp_fma64(a, b, c); }
EXPORT(jit_fma32) float jit_fma32(float a, float b, float c) { return rvfp_fma32(a, b, c); }
EXPORT(jit_fphelp) uint64_t jit_fphelp(int op, uint64_t a, uint64_t b, uint64_t c) { return rvfp_help(cpu, op, a, b, c); }

// ---- guest system calls (the minimum the test programs use) ----
enum { G_ENOSYS = 38, G_EBADF = 9, G_ENOMEM = 12 };
int engine_ecall(void) {
  uint64_t *x = cpu->x, nr = x[17], a0 = x[10], a1 = x[11], a2 = x[12];
  int64_t r;
  switch (nr) {
  case 64:   // write
    if (a0 != 1 && a0 != 2) { r = -G_EBADF; break; }
    if (a1 > rv_memsize || a2 > rv_memsize - a1) { r = -14; break; }
    for (uint64_t i = 0; i < a2; i++) out_hash = (out_hash ^ rv_mem[a1 + i]) * 16777619u;
    out_bytes += a2;
    port_guest_write((int)a0, rv_mem + a1, (size_t)a2);
    r = (int64_t)a2;
    break;
  case 93: case 94:   // exit, exit_group
    cpu->exit_code = a0;
    cpu->halted = 1;
    return R_HALT;
  case 214:  // brk
    if (a0 >= brk_start && a0 < rv_memsize / 2) brk_cur = a0;
    r = (int64_t)brk_cur;
    break;
  case 57: r = 0; break;                    // close
  case 63: r = 0; break;                    // read: end of file
  case 113: {                               // clock_gettime(clk, ts)
    double ms = jit_now_ms();
    if (a1 > rv_memsize - 16) { r = -14; break; }
    uint64_t sec = (uint64_t)(ms / 1000), nsec = (uint64_t)((ms - (double)sec * 1000) * 1e6);
    memcpy(rv_mem + a1, &sec, 8); memcpy(rv_mem + a1 + 8, &nsec, 8);
    r = 0;
    break;
  }
  case 259: engine_fence_i(); r = 0; break; // riscv_flush_icache
  default: r = -G_ENOSYS;
  }
  x[10] = (uint64_t)r;
  return 0;
}

// fence.i: the guest says it changed code. Throw every translation away (the prototype's
// policy; a finer one would drop only regions that overlap written pages).
void engine_fence_i(void) {
  rv_predecode_flush();
  if (!support_slots[0]) return;
  for (uint32_t h = 0; h < RH; h++)
    for (region *r = rhash[h]; r; r = r->next)
      if (r->state == RS_DONE) { jit_call(support_slots[1], (int)r->slot); r->state = RS_COLD; r->count = 0; }
}

void engine_frm_changed(void) {
  extern int rvfp_frm;
  rvfp_frm = (int)(cpu->fcsr >> 5 & 7);
  engine_fence_i();
}

// After the engine's memory was copied into a fresh instance (guest fork through the
// kernel's clone call, or a migration to a smaller memory): the instance has a new, small
// function table, so every slot number in memory is meaningless and no generated module
// exists. Everything else (guest state, region records, learned targets, counters) is in
// memory and still true. Make the support module again and mark every translated region
// cold, one entry short of the threshold: hot code is translated again the next time it
// runs, cold code never. Call it first thing in __fork_child / __migrate_resume.
void engine_after_copy(void) {
  support_slots[0] = support_slots[1] = 0;
  slot_next = slot_end = 0;
  S.live_bytes = 0;
  if (E.mode == M_JIT || E.mode == M_AOT) {
    wbuf sup;
    wb_init(&sup);
    xl_support_module(&sup, (uint32_t)(uintptr_t)support_slots);
    if (jit_host_load(sup.p, sup.n)) E.mode = M_PRE;   // no generated code here: interpret
    support_slots[1] = support_slots[0] + 1;
    wb_free(&sup);
  }
  for (uint32_t h = 0; h < RH; h++)
    for (region *r = rhash[h]; r; r = r->next) {
      if (r->state == RS_DONE) r->count = (uint32_t)E.threshold - 1;
      r->state = RS_COLD; r->slot = 0; r->retrans = 0;
    }
  for (uint32_t h = 0; h < 4096; h++) for (site *s = shash[h]; s; s = s->next) s->nregs = 0;
  if (E.mode == M_AOT) E.mode = M_JIT;   // the whole-file module is gone; regions come back one by one
}

// ---- lockstep store log ----
typedef struct { uint64_t addr; uint8_t n, oldv[8], newv[8]; } slog;
static slog *logv;
static int nlog, caplog, logging;
void rv_store_hook(uint64_t addr, int n) {
  if (!logging) return;
  if (nlog == caplog) { caplog = caplog ? caplog * 2 : 4096; logv = jit_realloc(logv, sizeof(slog) * (size_t)caplog); }
  logv[nlog].addr = addr; logv[nlog].n = (uint8_t)n;
  memcpy(logv[nlog].oldv, rv_mem + addr, (size_t)n);
  nlog++;
}

// ---- translation ----
static xlat_env XE;
static wbuf modbuf;
static xmodule XM;

static int translate(region *r) {
  double t0 = jit_now_ms();
  if (!r->slot) r->slot = alloc_slot();
  if (!r->slot) { r->state = RS_FAILED; return -1; }
  xl_module_init(&XM);
  translating = r;
  xl_add_entry(&XM, r->pc, r->slot);
  if (xl_translate_all(&XM, &XE, &E.x) < 0) { jit_printf("translator: label bug at %llx\n", (unsigned long long)r->pc); r->state = RS_FAILED; xl_module_free(&XM); return -1; }
  modbuf.n = 0;
  xl_module_bytes(&XM, &modbuf);
  if (E.lockstep) {
    extern int xl_last_ranges(uint64_t *out, int cap);
    static uint64_t tmp[3 * 1024];
    int n = xl_last_ranges(tmp, 1024);
    r->ranges = jit_malloc(sizeof(uint64_t) * 3 * (size_t)(n ? n : 1));
    memcpy(r->ranges, tmp, sizeof(uint64_t) * 3 * (size_t)n);
    r->nr = (uint32_t)n;
  }
  if (S.live_bytes + modbuf.n > E.cache_max) {
    // Bound on generated code: drop every translation (slots get the stub back, so the
    // browser can free the old instances) and start again. Simple and safe; a finer
    // policy would drop the least used regions.
    engine_fence_i();
    S.cache_flushes++;
    S.live_bytes = 0;
  }
  double t1 = jit_now_ms();
  int e = jit_host_load(modbuf.p, modbuf.n);
  double t2 = jit_now_ms();
  S.t_translate += t1 - t0;
  S.t_load += t2 - t1;
  if (!S.regions) S.t_first_load = t2 - t1;
  if (t2 - t1 > S.t_max_load) S.t_max_load = t2 - t1;
  S.module_bytes += modbuf.n;
  S.guest_insns_translated += (uint64_t)XM.ninsn_total;
  if (e) {
    xl_module_free(&XM);
    S.translate_fail++;
    if (E.verbose) jit_printf("load failed (%d) for region %llx\n", e, (unsigned long long)r->pc);
    r->state = RS_FAILED;
    return -1;
  }
  S.regions++;
  S.live_bytes += modbuf.n;
  r->state = RS_DONE;
  r->leaf = XM.leaf[0]; r->use = XM.fuse[0]; r->def = XM.fdef[0];
  xl_module_free(&XM);
  if (E.verbose > 1) jit_printf("region %llx: %u bytes\n", (unsigned long long)r->pc, modbuf.n);
  return 0;
}

// AOT: one module with a region for every function symbol (STT_FUNC) of the ELF file.
static int translate_aot(void) {
  const uint8_t *e = elf_img;
  uint64_t shoff; uint16_t shentsize, shnum;
  memcpy(&shoff, e + 0x28, 8); memcpy(&shentsize, e + 0x3a, 2); memcpy(&shnum, e + 0x3c, 2);
  double t0 = jit_now_ms();
  xl_module_init(&XM);
  translating = 0;
  int nf = 0;
  for (int i = 0; i < shnum; i++) {
    const uint8_t *sh = e + shoff + (uint64_t)i * shentsize;
    uint32_t type; uint64_t off, size, entsize; uint32_t link;
    memcpy(&type, sh + 4, 4); memcpy(&off, sh + 0x18, 8); memcpy(&size, sh + 0x20, 8); memcpy(&link, sh + 0x28, 4); memcpy(&entsize, sh + 0x38, 8);
    if (type != 2 || !entsize) continue;    // SHT_SYMTAB
    for (uint64_t k = 0; k < size / entsize; k++) {
      const uint8_t *sym = e + off + k * entsize;
      uint8_t info = sym[4];
      uint64_t value; memcpy(&value, sym + 8, 8);
      if ((info & 15) != 2 || !value) continue;   // STT_FUNC
      region *r = rget(value);
      if (!r->slot) r->slot = alloc_slot();
      if (xl_add_entry(&XM, value, r->slot) == 0) nf++;
    }
  }
  if (xl_translate_all(&XM, &XE, &E.x) < 0) { jit_printf("translator: label bug (aot)\n"); return -1; }
  modbuf.n = 0;
  xl_module_bytes(&XM, &modbuf);
  double t1 = jit_now_ms();
  int err = jit_host_load(modbuf.p, modbuf.n);
  double t2 = jit_now_ms();
  S.t_translate += t1 - t0; S.t_load += t2 - t1; S.t_first_load = t2 - t1;
  S.module_bytes += modbuf.n;
  S.guest_insns_translated += (uint64_t)XM.ninsn_total;
  if (err) { jit_printf("aot module failed to load (%d)\n", err); return -1; }
  for (int i = 0; i < XM.nfunc; i++) { region *r = rget(XM.entry[i]); r->state = RS_DONE; }
  S.regions += (uint64_t)XM.nfunc;
  if (E.verbose) jit_printf("aot: %d functions, %d guest instructions, %u bytes of wasm\n", nf, XM.ninsn_total, modbuf.n);
  xl_module_free(&XM);
  return 0;
}

// ---- lockstep: run a region under the interpreter, undo, run it translated, compare ----
static int block_of(region *r, uint64_t pc) {   // reverse postorder number of the block holding pc, -1 outside
  for (uint32_t i = 0; i < r->nr; i++) if (pc >= r->ranges[3 * i] && pc < r->ranges[3 * i + 1]) return (int)r->ranges[3 * i + 2];
  return -1;
}
static rv_cpu before, after_i;
static int lockstep_region(region *r, int *why_out) {
  before = *cpu;
  nlog = 0; logging = 1;
  int why = R_JUMP;
  uint64_t start_ic = cpu->icount;
  // Translated code in lockstep mode leaves at every back edge (budget 1, yield_exit), at
  // calls and indirect jumps, and where a branch leaves the region. Stop at the same points.
  for (int b = block_of(r, cpu->pc); b >= 0;) {
    uint64_t pc = cpu->pc;
    uint32_t raw = 0;
    rv_insn d;
    memcpy(&raw, rv_mem + pc, 4);
    int known = rv_decode(raw, &d);
    int e = rv_interp_one(cpu);
    if (e >= 0) { why = e; break; }
    if (known && (d.op == RV_JALR || (d.op == RV_JAL && d.rd))) break;
    int nb = block_of(r, cpu->pc);
    if (nb < 0) break;
    if ((cpu->pc != pc + d.len || nb != b) && nb <= b) break;   // a back edge (taken, or falling into a loop header)
    b = nb;
  }
  logging = 0;
  after_i = *cpu;
  for (int i = 0; i < nlog; i++) memcpy(logv[i].newv, rv_mem + logv[i].addr, logv[i].n);
  for (int i = nlog - 1; i >= 0; i--) memcpy(rv_mem + logv[i].addr, logv[i].oldv, logv[i].n);
  *cpu = before;
  cpu->budget = 1;
  int jwhy = jit_call(r->slot, 0);
  S.lockstep_checks++;
  int bad = 0;
  for (int i = 0; i < 32; i++) if (cpu->x[i] != after_i.x[i]) { jit_printf("lockstep: region %llx: x%d = %llx, interpreter %llx\n", (unsigned long long)r->pc, i, (unsigned long long)cpu->x[i], (unsigned long long)after_i.x[i]); bad = 1; }
  for (int i = 0; i < 32; i++) if (cpu->f[i] != after_i.f[i]) { jit_printf("lockstep: region %llx: f%d = %llx, interpreter %llx\n", (unsigned long long)r->pc, i, (unsigned long long)cpu->f[i], (unsigned long long)after_i.f[i]); bad = 1; }
  if (cpu->pc != after_i.pc) { jit_printf("lockstep: region %llx: pc %llx, interpreter %llx\n", (unsigned long long)r->pc, (unsigned long long)cpu->pc, (unsigned long long)after_i.pc); bad = 1; }
  if (cpu->icount != after_i.icount) { jit_printf("lockstep: region %llx: %llu instructions, interpreter %llu\n", (unsigned long long)r->pc, (unsigned long long)(cpu->icount - start_ic), (unsigned long long)(after_i.icount - start_ic)); bad = 1; }
  if (cpu->fcsr != after_i.fcsr) { jit_printf("lockstep: region %llx: fcsr %x, interpreter %x\n", (unsigned long long)r->pc, cpu->fcsr, after_i.fcsr); bad = 1; }
  for (int i = 0; i < nlog; i++) if (memcmp(rv_mem + logv[i].addr, logv[i].newv, logv[i].n)) { jit_printf("lockstep: region %llx: memory at %llx differs\n", (unsigned long long)r->pc, (unsigned long long)logv[i].addr); bad = 1; break; }
  if (jwhy == R_FAULT && why != R_FAULT) { jit_printf("lockstep: region %llx: translated code faulted, interpreter did not\n", (unsigned long long)r->pc); bad = 1; }
  *why_out = jwhy == R_INTERP ? R_INTERP : why == R_JUMP ? jwhy : why;
  return bad;
}

// ---- the driver loop ----
static int finished, exit_status;

static int handle(int why) {
  switch (why) {
  case R_JUMP: case R_RET: case R_STUB: return 0;   // R_STUB: cpu->pc holds where to go on
  case R_INTERP: {
    int e = rv_interp_one(cpu);
    if (e < 0) return 0;
    if (e == R_INTERP) { jit_printf("illegal instruction at %llx\n", (unsigned long long)cpu->pc); finished = 1; exit_status = 132; return 1; }
    return handle(e);
  }
  case R_HALT: finished = 1; exit_status = (int)cpu->exit_code; return 1;
  case R_FAULT: jit_printf("segmentation fault at pc %llx, address %llx\n", (unsigned long long)cpu->pc, (unsigned long long)cpu->fault_addr); finished = 1; exit_status = 139; return 1;
  case R_EBREAK: jit_printf("ebreak at %llx\n", (unsigned long long)cpu->pc); finished = 1; exit_status = 133; return 1;
  default: jit_printf("unexpected exit reason %d at %llx\n", why, (unsigned long long)cpu->pc); finished = 1; exit_status = 134; return 1;
  }
}

// Kept out of line and bounded, so that the browser's optimising compiler gets to replace
// it (no on-stack replacement for wasm: a loop entered once keeps its first code).
__attribute__((noinline)) static void run_some(int n) {
  for (int i = 0; i < n && !finished; i++) {
    uint64_t pc = cpu->pc;
    region *r = rfind(pc);
    if (E.copytest && S.region_calls % (uint64_t)E.copytest == (uint64_t)E.copytest - 1) { S.region_calls++; engine_after_copy(); continue; }
    if (r && r->state == RS_DONE) {
      S.region_calls++;
      int why;
      if (E.lockstep) {
        if (lockstep_region(r, &why)) { finished = 1; exit_status = 250; return; }
      } else why = jit_call(r->slot, 0);
      if (cpu->site) { learn(cpu->site, cpu->pc); cpu->site = 0; }
      if (why != R_JUMP && why != R_RET && why != R_STUB) handle(why);
      continue;
    }
    if (E.mode != M_INTERP) {
      if (!r) r = rget(pc);
      if (r->state == RS_COLD && ++r->count >= (uint32_t)E.threshold) { translate(r); continue; }
    }
    S.interp_blocks++;
    int why = rv_interp_block(cpu, 256);
    if (why != R_JUMP) handle(why);
    if (--cpu->budget <= 0) { int b = yield_now(); cpu->budget = b > 0 ? b : E.budget; }
  }
}

__attribute__((noinline)) static void interp_some(int n) {
  for (int i = 0; i < n; i++) {
    int e = rv_interp_one(cpu);
    if (e >= 0) { if (handle(e)) return; }
  }
}

__attribute__((noinline)) static void pre_some(int n) {
  int e = rv_interp_pre(cpu, n);
  if (e >= 0) handle(e);
}

int engine_run(void) {
  finished = 0;
  cpu->budget = E.budget;
  while (!finished) {
    if (E.mode == M_INTERP) { interp_some(100000); port_timeslice(); }
    else if (E.mode == M_PRE) { pre_some(20000); port_timeslice(); }
    else run_some(10000);
  }
  return exit_status;
}

// ---- set-up ----
int engine_init(void) {
  rv_memsize = E.memsize;
  rv_mem = jit_alloc_guest((size_t)rv_memsize + 65536);   // 64 KiB of slack past the end for masked accesses
  if (!rv_mem) { jit_printf("cannot allocate %llu MiB of guest memory\n", (unsigned long long)(rv_memsize >> 20)); return -1; }
  uint32_t c = (uint32_t)(uintptr_t)cpu;
  XE.a_x = c + CPU_OFF_X(0); XE.a_f = c + CPU_OFF_F(0); XE.a_pc = c + CPU_OFF_PC; XE.a_fcsr = c + CPU_OFF_FCSR;
  XE.a_res = c + CPU_OFF_RES; XE.a_budget = c + CPU_OFF_BUDGET; XE.a_icount = c + CPU_OFF_ICOUNT;
  XE.a_fault = c + CPU_OFF_FAULT; XE.a_site = c + CPU_OFF_SITE;
  XE.gbase = (uint32_t)(uintptr_t)rv_mem;
  XE.gsize = rv_memsize;
  wb_init(&modbuf);
  if (E.mode != M_INTERP && E.mode != M_PRE) {
    wbuf sup;
    wb_init(&sup);
    xl_support_module(&sup, (uint32_t)(uintptr_t)support_slots);
    int e = jit_host_load(sup.p, sup.n);
    wb_free(&sup);
    support_slots[1] = support_slots[0] + 1;   // the module stores the first of its two slots; stub() is the second
    if (e) { jit_printf("the host cannot load generated code (%d); interpreting only\n", e); E.mode = M_INTERP; }
  }
  return 0;
}

static int load_segments(void) {
  const uint8_t *e = elf_img;
  uint64_t phoff; uint16_t phentsize, phnum;
  memcpy(&phoff, e + 0x20, 8); memcpy(&phentsize, e + 0x36, 2); memcpy(&phnum, e + 0x38, 2);
  uint64_t top = 0;
  for (int i = 0; i < phnum; i++) {
    const uint8_t *ph = e + phoff + (uint64_t)i * phentsize;
    uint32_t type; uint64_t off, vaddr, filesz, memsz;
    memcpy(&type, ph, 4); memcpy(&off, ph + 8, 8); memcpy(&vaddr, ph + 0x10, 8); memcpy(&filesz, ph + 0x20, 8); memcpy(&memsz, ph + 0x28, 8);
    if (type != 1) continue;
    if (vaddr + memsz > rv_memsize / 2 || off + filesz > elf_len) { jit_printf("segment at %llx does not fit\n", (unsigned long long)vaddr); return -1; }
    memcpy(rv_mem + vaddr, e + off, (size_t)filesz);
    memset(rv_mem + vaddr + filesz, 0, (size_t)(memsz - filesz));
    if (vaddr + memsz > top) top = vaddr + memsz;
  }
  brk_start = brk_cur = (top + 4095) & ~4095ull;
  return 0;
}

static void reset_cpu(void) {
  uint64_t entry; memcpy(&entry, elf_img + 0x18, 8);
  memset(cpu, 0, sizeof *cpu);
  cpu->pc = entry;
  cpu->res_addr = ~0ull;
  { extern int rvfp_frm; rvfp_frm = 0; }
  // a Linux-like initial stack: argc = 1, argv = { "prog", 0 }, envp = { 0 }, auxv = { AT_NULL }
  uint64_t sp = rv_memsize - 4096;
  memcpy(rv_mem + sp + 256, "prog", 5);
  uint64_t words[6] = { 1, sp + 256, 0, 0, 0, 0 };
  memcpy(rv_mem + sp, words, sizeof words);
  cpu->x[2] = sp;
}

int engine_load_elf(const uint8_t *elf, size_t len) {
  if (len < 64 || memcmp(elf, "\177ELF", 4) || elf[4] != 2 || elf[18] != 243) { jit_printf("not a 64-bit RISC-V ELF file\n"); return -1; }
  elf_img = elf; elf_len = len;
  if (load_segments()) return -1;
  reset_cpu();
  if (E.mode == M_AOT && translate_aot()) return -1;
  return 0;
}

void engine_reset_guest(void) {
  load_segments();
  reset_cpu();
  out_hash = 2166136261u;
  out_bytes = 0;
}

static uint64_t fnv64(const uint8_t *p, uint64_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

void engine_report(void) {
  uint64_t rh_ = fnv64((const uint8_t *)cpu->x, 8 * 32);
  // the stack page and everything the program wrote below its break: final memory state
  uint64_t mh = fnv64(rv_mem, brk_cur) ^ fnv64(rv_mem + rv_memsize - 65536, 65536);
  jit_printf("@@STATE exit=%d out=%08x outbytes=%llu regs=%016llx mem=%016llx icount=%llu\n", exit_status, out_hash,
             (unsigned long long)out_bytes, (unsigned long long)rh_, (unsigned long long)mh, (unsigned long long)cpu->icount);
  if (E.stats)
    jit_printf("@@STATS regions=%llu calls=%llu interp_blocks=%llu fails=%llu yields=%llu lookups=%llu miss=%llu wasm_bytes=%llu guest_insns=%llu t_translate=%.3f t_load=%.3f t_first_load=%.3f t_max_load=%.3f lockstep=%llu cache_flushes=%llu\n",
               (unsigned long long)S.regions, (unsigned long long)S.region_calls, (unsigned long long)S.interp_blocks,
               (unsigned long long)S.translate_fail, (unsigned long long)S.yields, (unsigned long long)S.lookups, (unsigned long long)S.lookup_miss,
               (unsigned long long)S.module_bytes, (unsigned long long)S.guest_insns_translated, S.t_translate, S.t_load, S.t_first_load, S.t_max_load,
               (unsigned long long)S.lockstep_checks, (unsigned long long)S.cache_flushes);
}
