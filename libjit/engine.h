// SPDX-License-Identifier: GPL-2.0-or-later
// The translator's prototype engine: guest memory, the driver loop that runs translated
// regions or interprets, the region cache, table slots, and the helpers translated code
// calls. In the finished system this role belongs to the core engine (ELF loader, Linux
// system calls, address space); see docs/BINARY-TRANSLATION.md section 7.
#ifndef JIT_ENGINE_H
#define JIT_ENGINE_H
#include "port.h"
#include "rv.h"
#include "xlat.h"

extern uint8_t *rv_mem;          // guest address 0
extern uint64_t rv_memsize;      // guest memory size (power of two)
extern rv_cpu *cpu;

enum { M_INTERP = 0, M_JIT = 1, M_AOT = 2, M_PRE = 3 };
typedef struct {
  int mode;                      // M_*
  xlat_opts x;                   // translator options
  int threshold;                 // block entries before a region is translated
  int budget;                    // back edges and calls between time-slice checks
  int yield_exit;                // the budget helper makes translated code leave (re-entry picks up optimised code)
  int lockstep;                  // run each region under the interpreter too and compare
  int verbose;
  int stats;
  uint64_t memsize;
  int repeat;                    // run the program this many times in one engine (for warm figures)
  int copytest;                  // test: act as if the memory were copied to a fresh instance every N region runs
  uint64_t cache_max;            // bytes of generated wasm kept before every translation is dropped
} engine_opts;
extern engine_opts E;

typedef struct {
  uint64_t regions, region_calls, interp_blocks, interp_insns, translate_fail, yields, lockstep_checks;
  uint64_t module_bytes, guest_insns_translated, lookups, lookup_miss, cache_flushes, live_bytes;
  double t_translate, t_load;    // milliseconds: our code generation, and the host's compile + instantiate
  double t_first_load, t_max_load;
} engine_stats;
extern engine_stats S;

void engine_defaults(void);
int engine_parse_opts(const char *s);        // "mode=jit strategy=struct calls=1 ..."
int engine_init(void);
int engine_load_elf(const uint8_t *elf, size_t len);
int engine_run(void);                         // returns the guest's exit code
void engine_report(void);
void engine_reset_guest(void);                // drop guest state for a repeated run (keeps translations)

// services for the interpreter
int engine_ecall(void);                       // 0, R_HALT, or R_INTERP (not handled)
void engine_fence_i(void);
void engine_frm_changed(void);
void engine_after_copy(void);               // memory copied into a fresh instance (fork, migrate): re-create code lazily              // the guest wrote frm: translations assumed the old one
void rv_store_hook(uint64_t addr, int n);
int rv_exec(rv_cpu *c, const rv_insn *d);
int rv_interp_one(rv_cpu *c);
int rv_interp_block(rv_cpu *c, int max);
int rv_interp_pre(rv_cpu *c, int max);
void rv_predecode_flush(void);

// what the port gives the guest (fd 1 and 2 output, time, a time-slice point)
void port_guest_write(int fd, const void *p, size_t n);
void port_timeslice(void);
#endif
