// SPDX-License-Identifier: GPL-2.0-or-later
/* The Linux binary engine: command line and the main loop.
 *
 *   lx [options] [--] PROGRAM [ARGS...]
 *     --root=DIR    the guest's "/" (default /debian if it exists, else "/"; or $LX_ROOT)
 *     --trace       log every system call to stderr ($LX_TRACE=1)
 *     --stats       print a count of system calls at exit ($LX_STATS=1)
 *     --mem=MIB     size of the guest address space (default 1024)
 *     --argv0=NAME  argv[0] for the guest (default PROGRAM)
 *     --budget=N    instructions between two yields to the host kernel
 *     --no-fuse     disable superinstructions (for measurement)
 *     --dispatch=switch|tail  interpreter dispatch (default: tail natively, switch in wasm)
 *     --soft-fp     soft-float only, no host fast paths (for testing)
 *     --ext=zba,zbb,zbs  decode these extensions beyond RV64GC
 * PROGRAM is a guest path (resolved inside the root). */
#include <stdarg.h>
#include "sys.h"
#include "host.h"
#include "path.h"
#include "rv.h"
#include "rv_fp.h"

struct lx_config lx_cfg = {"/", LX_NAME, 0, 0, 1024ull << 20};
void hb_init(void);
extern int rv_fuse, rv_dispatch_tail;

void lx_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s: ", LX_NAME);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}
void lx_die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s: ", LX_NAME);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  hb_exit(127);
}

int64_t elf_load_image(const char *hostpath, const char *execfn, char *const argv[], char *const envp[]) {
  struct lx_image img;
  arch_cur->flush_code(0, ~0ull);
  mem_reset();
  int64_t r = elf_load(hostpath, execfn, argv, envp, &img);
  if (r < 0) return r;
  arch_cur->reset(img.entry, img.sp);
  return 0;
}

static int pk_kind(const char *hp) {
  struct lx_stat st;
  int64_t r = hb_fstatat(LX_AT_FDCWD, hp, &st, LX_AT_SYMLINK_NOFOLLOW);
  if (r < 0) return (int)r;
  return (st.mode & LX_S_IFMT) == LX_S_IFLNK;
}
static int64_t pk_readlink(const char *hp, char *buf, size_t cap) {
  int64_t r = hb4(LX_SYS_readlinkat, LX_AT_FDCWD, hbp(hp), hbp(buf), cap - 1);
  if (r >= 0) buf[r] = 0;
  return r;
}
static const struct path_ops pops = {pk_kind, pk_readlink, fd_path_get};

static int opt(const char *a, const char *name, const char **val) {
  size_t n = strlen(name);
  if (strncmp(a, name, n)) return 0;
  if (a[n] == '=') { *val = a + n + 1; return 1; }
  if (a[n] == 0) { *val = 0; return 1; }
  return 0;
}

__attribute__((noreturn)) void lx_main_loop(void);
static uint64_t stop_after;
extern struct rv_cpu rv_cpu;

/* Debugging aid: where is the guest after N instructions? */
static void dump_state(void) {
  fprintf(stderr, "%s: stopped after %llu instructions at pc 0x%llx\n", LX_NAME, (unsigned long long)rv_cpu.instret, (unsigned long long)rv_cpu.pc);
  for (int r = 0; r < 32; r++) fprintf(stderr, "x%-2d %016llx%s", r, (unsigned long long)rv_cpu.x[r], r % 4 == 3 ? "\n" : "  ");
  uint64_t pc = rv_cpu.pc;
  for (int k = 0; k < 24 && g_inrange(pc); k++) {
    uint32_t raw; memcpy(&raw, g_mem + pc, 4);
    struct rv_insn in; int n = rv_decode(raw, &in); char buf[96]; rv_disasm(&in, pc, buf, sizeof buf);
    fprintf(stderr, "  %llx: %s\n", (unsigned long long)pc, buf); pc += (uint64_t)n;
  }
}

int main(int argc, char **argv, char **envp) {
  const char *v, *argv0 = 0;
  int i = 1;
  /* Default root: /debian (where the package manager installs, a placeholder) if it
     exists, else "/". --root or LX_ROOT override. */
  {
    struct lx_stat st;
    if (hb_fstatat(LX_AT_FDCWD, "/debian", &st, 0) == 0 && (st.mode & LX_S_IFMT) == LX_S_IFDIR) lx_cfg.root = "/debian";
  }
  if (getenv("LX_ROOT")) lx_cfg.root = getenv("LX_ROOT");
  if (getenv("LX_TRACE")) lx_cfg.trace = atoi(getenv("LX_TRACE"));
  if (getenv("LX_STATS")) lx_cfg.stats = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--")) { i++; break; }
    if (opt(a, "--root", &v) && v) lx_cfg.root = v;
    else if (opt(a, "--trace", &v)) lx_cfg.trace = v ? atoi(v) : 1;
    else if (opt(a, "--stats", &v)) lx_cfg.stats = 1;
    else if (opt(a, "--mem", &v) && v) lx_cfg.mem_size = (uint64_t)atoi(v) << 20;
    else if (opt(a, "--argv0", &v) && v) argv0 = v;
    else if (opt(a, "--budget", &v) && v) lx_budget = (uint64_t)atoi(v);
    else if (opt(a, "--no-fuse", &v)) rv_fuse = 0;
    else if (opt(a, "--dispatch", &v) && v) rv_dispatch_tail = !strcmp(v, "tail");
    else if (opt(a, "--stop-after", &v) && v) stop_after = strtoull(v, 0, 10);
    else if (opt(a, "--soft-fp", &v)) fp_fast_enabled = 0;
    else if (opt(a, "--ext", &v) && v) {
      if (strstr(v, "zba")) rv_decode_ext |= RV_EXT_ZBA;
      if (strstr(v, "zbb")) rv_decode_ext |= RV_EXT_ZBB;
      if (strstr(v, "zbs")) rv_decode_ext |= RV_EXT_ZBS;
    } else { fprintf(stderr, "%s: unknown option %s\n", LX_NAME, a); return 2; }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: %s [--root=DIR] [--trace] [--stats] [--mem=MIB] [--] PROGRAM [ARGS...]\n"
                    "Runs a Linux %s program.\n", LX_NAME, arch_cur->name);
    return 2;
  }
  /* Where this engine lives, so an emulated fork can start another one. */
  static char self[4096];
#ifdef __wasm__
  snprintf(self, sizeof self, "%s", strchr(argv[0], '/') ? argv[0] : "/bin/" LX_NAME);
#else
  int64_t sl = hb4(LX_SYS_readlinkat, LX_AT_FDCWD, hbp("/proc/self/exe"), hbp(self), sizeof self - 1);
  if (sl > 0) self[sl] = 0; else snprintf(self, sizeof self, "%s", argv[0]);
#endif
  lx_cfg.self = self;
  hb_init();
  {
    extern int lx_errfd;
    int64_t d = hb3(LX_SYS_fcntl, 2, 0, 600);
    if (d < 0) d = hb3(LX_SYS_fcntl, 2, 0, 40);
    if (d >= 0) { hb3(LX_SYS_fcntl, d, 2, 1); lx_errfd = (int)d; }
  }
  if (path_init(lx_cfg.root, &pops) < 0) lx_die("bad root %s", lx_cfg.root);
  if (path_is_rooted()) {
    /* start in the root's / unless the host cwd is inside the root */
    char cwd[LX_PATH_MAX], g[LX_PATH_MAX];
    if (hb2(LX_SYS_getcwd, hbp(cwd), sizeof cwd) > 0 && path_from_host(cwd, g, sizeof g) == 0) path_set_cwd(g);
    else {
      hb1(LX_SYS_chdir, hbp(path_root()));
      path_set_cwd("/");
    }
  }
  int64_t r = mem_init(lx_cfg.mem_size);
  if (r < 0) lx_die("cannot reserve %llu MiB for the guest", (unsigned long long)(lx_cfg.mem_size >> 20));
  int gargc = argc - i;
  char **gargv = malloc((size_t)(gargc + 1) * sizeof *gargv);
  for (int k = 0; k < gargc; k++) gargv[k] = argv[i + k];
  gargv[gargc] = 0;
  if (argv0) gargv[0] = (char *)argv0;
  r = lx_exec(argv[i], gargv, envp);
  if (r < 0) {
    fprintf(stderr, "%s: %s: %s\n", LX_NAME, argv[i],
            r == -ENOENT ? "not found" : r == -ENOEXEC ? "not a program this engine can run" :
            r == -EACCES ? "permission denied" : "cannot run");
    return r == -ENOENT ? 127 : 126;
  }
  if (r == 1) return 0; /* cannot happen outside an emulated child */
  lx_main_loop();
}

void lx_main_loop(void) {
  struct lx_fault f;
  for (;;) {
    memset(&f, 0, sizeof f);
    int stop = arch_cur->run(&f);
    if (stop == LX_STOP_SYSCALL) lx_syscall();
    else if (stop == LX_STOP_BUDGET) { hb_tick(); lx_thread_yield(); }
    else lx_trap(stop, &f);
    if (lx_sig_pending || hb_pending_signal) lx_signals();
    if (stop_after && rv_cpu.instret >= stop_after) { dump_state(); hb_exit(125); }
  }
}
