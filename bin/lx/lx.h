// SPDX-License-Identifier: GPL-2.0-or-later
/* The Linux binary engine: shared declarations.
 *
 * Module map (each line is one file; see docs/LINUX-BINARIES.md):
 *   rv.h rv_decode.c rv_disasm.c   RISC-V front end: CPU state, decoder (pure)
 *   rv_interp.c rv_ops.inc         reference interpreter (two dispatch modes, one handler source)
 *   softfp.c rv_fp.c               IEEE 754 arithmetic: soft-float reference, host fast paths
 *   rv_arch.c                      everything else the engine needs to know about riscv64
 *   mem.c                          the guest address space inside the engine's memory
 *   elf.c                          ELF64 loader, initial stack and auxiliary vector
 *   path.c                         guest root and path translation (a chroot in user space)
 *   sys.c sys_sig.c sys_proc.c     the Linux system call layer: files and memory, signals, processes
 *   sys_thread.c                   guest threads as green threads, futex
 *   host_ucos.c / host_linux.c     lowest level: UC.OS kernel calls, or the host's Linux
 *   main.c                         command line, configuration, process start
 */
#ifndef LX_H
#define LX_H
#include "lxlib.h"

/* The engine's name is a placeholder until the owners choose one. Change it here. */
#define LX_NAME "lx"

#define LX_PAGE 4096ull

/* ---------------- guest memory (mem.c) ----------------
 * The guest sees addresses [0, g_size). Guest address a lives at host address g_mem + a.
 * [0, g_lo) is never mapped (null pointer guard). The top page is never mapped either,
 * so one compare covers any access of up to 8 bytes. */
extern uint8_t *g_mem;
extern uint64_t g_lo, g_size, g_span; /* g_span = g_size - g_lo - 8 */

static inline int g_inrange(uint64_t a) { return a - g_lo <= g_span; }
/* Range check for a buffer the system call layer is about to touch. */
static inline int g_okrange(uint64_t a, uint64_t n) {
  return a >= g_lo && n <= g_size && a <= g_size - n && (n == 0 || a + n <= g_size - LX_PAGE);
}
static inline void *g2h(uint64_t a) { return g_mem + a; }

#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANON 0x20
#define LX_MAP_NORESERVE 0x4000
#define LX_MAP_FIXED_NOREPLACE 0x100000
#define LX_MAP_GROWSDOWN 0x0100
#define LX_MAP_STACK 0x20000

int mem_init(uint64_t size);
void mem_reset(void);                       /* drop every mapping (execve) */
int64_t mem_map(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off);
int64_t mem_unmap(uint64_t addr, uint64_t len);
int64_t mem_protect(uint64_t addr, uint64_t len, int prot);
int64_t mem_remap(uint64_t old, uint64_t oldlen, uint64_t newlen, int flags, uint64_t newaddr);
int64_t mem_sync(uint64_t addr, uint64_t len);
int64_t mem_brk(uint64_t want);
void mem_set_brk(uint64_t start);
int mem_is_mapped(uint64_t addr, uint64_t len);
uint64_t mem_stack_top(void);
uint64_t mem_used_bytes(void);
extern int mem_protect_enforced;            /* reserved: 0 means protections are recorded, not enforced */

/* ---------------- guest strings and copies ---------------- */
/* Copy a NUL-terminated guest string into buf; -EFAULT or -ENAMETOOLONG. */
int64_t g_strncpy(char *buf, uint64_t ga, size_t cap);

/* ---------------- configuration and process (main.c) ---------------- */
struct lx_config {
  const char *root;       /* host directory that is the guest's "/" ("/" = no translation) */
  const char *self;       /* path of the engine binary (to start more engines) */
  int trace;              /* 1 log system calls to stderr, 2 also every block */
  int stats;              /* print system call counts at exit */
  uint64_t mem_size;      /* guest window size */
};
extern struct lx_config lx_cfg;

/* ---------------- loader (elf.c) ---------------- */
struct lx_image {
  uint64_t entry;         /* where execution starts (the interpreter's entry if there is one) */
  uint64_t sp;            /* initial stack pointer */
  uint64_t phdr, phent, phnum, base, prog_entry, brk;
  int machine;
};
/* Load the program `path` (host path) with argv/envp (host strings); execfn is what the
 * guest called it. Returns 0 or -errno. */
int64_t elf_load(const char *hostpath, const char *execfn, char *const argv[], char *const envp[], struct lx_image *img);

/* ---------------- logging ---------------- */
void lx_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void lx_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
