// SPDX-License-Identifier: GPL-2.0-or-later
/* The Linux system call layer, part 3: execve, clone (fork, vfork), wait.
 *
 * execve is done inside the engine: drop the guest image, load the new one, keep the
 * descriptors. Nothing is needed from the host for that.
 *
 * fork: where the host can fork (natively), the engine forks with it: the child engine
 * has a copy of everything, the guest included. Where it cannot (UC.OS today), the
 * engine emulates the two patterns real programs use, fork + exec and fork + _exit:
 *   1. at clone, save what the child could change: the registers, every writable guest
 *      page, the memory map, the signal state, the working directory and the descriptor
 *      table (each open descriptor is duplicated to a spare number);
 *   2. run the guest as the child, in the same process, until it calls execve or exits;
 *   3. execve: start the target as a new process (another engine, or a UC.OS program)
 *      with the descriptors the child set up; exit: remember the status;
 *   4. put the parent back exactly as it was and return the new pid (or a made-up pid
 *      for a child that only exited) from clone.
 * Limits of the emulation (docs/LINUX-BINARIES.md): parent and child do not run at the
 * same time, so a child that waits for its parent (reads a pipe the parent fills) hangs.
 * The real fix is the kernel's clone primitive; the contract we need is in the doc. */
#include "sys.h"
#include "host.h"
#include "path.h"

char lx_exe_path[4096];
int64_t elf_load_image(const char *hostpath, const char *execfn, char *const argv[], char *const envp[]);
int elf_probe(const unsigned char *head, int n);

/* ---------------- execve ---------------- */
static void free_vec(char **v) { if (!v) return; for (char **p = v; *p; p++) free(*p); free(v); }

static char **copy_guest_vec(uint64_t ga, int64_t *err) {
  size_t n = 0, cap = 16;
  char **v = malloc(cap * sizeof *v);
  if (!v) { *err = -ENOMEM; return 0; }
  if (ga) for (;; n++) {
    if (!g_okrange(ga + 8 * n, 8)) { *err = -EFAULT; v[n] = 0; free_vec(v); return 0; }
    uint64_t p;
    memcpy(&p, g2h(ga + 8 * n), 8);
    if (!p) break;
    if (n + 2 > cap) { cap *= 2; char **nv = realloc(v, cap * sizeof *v); if (!nv) { v[n] = 0; free_vec(v); *err = -ENOMEM; return 0; } v = nv; }
    char buf[131072];
    int64_t l = g_strncpy(buf, p, sizeof buf);
    if (l < 0) { v[n] = 0; free_vec(v); *err = l == -ENAMETOOLONG ? -E2BIG : l; return 0; }
    v[n] = strdup(buf);
  }
  v[n] = 0;
  return v;
}

static int64_t run_native(const char *hostpath, char *const argv[], char *const envp[], int *noret);
int64_t lx_exec_native_child(const char *hostpath, char *const argv[], char *const envp[]);
int64_t lx_exec_engine_child(const char *gpath, char *const argv[], char *const envp[]);
static int in_child(void);
static void child_done(int64_t pid, int status);
int sys_getcwd_host(char *buf, size_t n);

/* Resolve a guest program and decide how to run it. Shared by execve and main. */
int64_t lx_exec(const char *gpath0, char *const argv0[], char *const envp[]) {
  char gpath[LX_PATH_MAX], hp[LX_PATH_MAX], abs[LX_PATH_MAX];
  char **argv = 0;
  int argc0 = 0;
  while (argv0[argc0]) argc0++;
  snprintf(gpath, sizeof gpath, "%s", gpath0);
  for (int depth = 0; depth < 5; depth++) {
    char *const *av = argv ? argv : argv0;
    int64_t r = path_to_host(LX_AT_FDCWD, gpath, 1, hp, sizeof hp);
    if (r < 0) { free_vec(argv); return r; }
    int64_t fd = hb4(LX_SYS_openat, LX_AT_FDCWD, hbp(hp), LX_O_CLOEXEC, 0);
    if (fd < 0) { free_vec(argv); return fd; }
    struct lx_stat st;
    if (hb_fstatat((int)fd, 0, &st, 0) == 0 && (st.mode & LX_S_IFMT) == LX_S_IFDIR) { hb1(LX_SYS_close, fd); free_vec(argv); return -EACCES; }
    unsigned char head[256];
    int64_t n = hb4(LX_SYS_pread64, fd, hbp(head), sizeof head - 1, 0);
    hb1(LX_SYS_close, fd);
    if (n < 0) { free_vec(argv); return n; }
    head[n > 0 ? n : 0] = 0;
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
      /* #!interpreter [one argument] */
      char *s = (char *)head + 2, *e;
      while (*s == ' ' || *s == '\t') s++;
      e = s;
      while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
      char interp[256], arg[256] = "";
      snprintf(interp, sizeof interp, "%.*s", (int)(e - s), s);
      s = e;
      while (*s == ' ' || *s == '\t') s++;
      e = s;
      while (*e && *e != '\n') e++;
      while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
      snprintf(arg, sizeof arg, "%.*s", (int)(e - s), s);
      if (!interp[0]) { free_vec(argv); return -ENOEXEC; }
      int ac = 0;
      while (av[ac]) ac++;
      char **nv = malloc((size_t)(ac + 4) * sizeof *nv);
      int k = 0;
      nv[k++] = strdup(interp);
      if (arg[0]) nv[k++] = strdup(arg);
      nv[k++] = strdup(gpath);
      for (int i = 1; i < ac; i++) nv[k++] = strdup(av[i]);
      nv[k] = 0;
      free_vec(argv);
      argv = nv;
      snprintf(gpath, sizeof gpath, "%s", interp);
      continue;
    }
    if (elf_probe(head, (int)n)) {
      if (path_resolve(LX_AT_FDCWD, gpath, 1, abs, sizeof abs) < 0) snprintf(abs, sizeof abs, "%s", gpath);
      if (!path_is_rooted() && abs[0] != '/') {
        char cwd[LX_PATH_MAX];
        if (hb2(LX_SYS_getcwd, hbp(cwd), sizeof cwd) > 0) { char t[LX_PATH_MAX]; snprintf(t, sizeof t, "%s/%s", cwd, gpath); snprintf(abs, sizeof abs, "%s", t); }
      }
      if (in_child()) {
        /* emulated fork: the exec'd program becomes a real process */
        r = lx_exec_engine_child(gpath, av, envp);
        free_vec(argv);
        return r;
      }
      /* point of no return */
      r = elf_load_image(hp, gpath0, av, envp);
      free_vec(argv);
      if (r < 0) {
        fprintf(stderr, "%s: cannot load %s (error %lld)\n", LX_NAME, gpath, (long long)-r);
        hb_die(LX_SIGKILL);
      }
      snprintf(lx_exe_path, sizeof lx_exe_path, "%s", abs);
      fd_close_cloexec();
      sig_exec_reset();
      lx_threads_reset();
      return 0;
    }
    if (hb_is_native_program(head, (int)n)) {
      int noret = 0;
      r = run_native(hp, av, envp, &noret);
      free_vec(argv);
      return r;
    }
    free_vec(argv);
    return -ENOEXEC;
  }
  free_vec(argv);
  return -ELOOP;
}

/* A native program (UC.OS wasm, or a host binary natively). execve semantics: replace the
 * process where the host can; on UC.OS start it, wait, and leave with its status. */
static int64_t run_native(const char *hostpath, char *const argv[], char *const envp[], int *noret) {
  (void)noret;
  if (in_child()) return lx_exec_native_child(hostpath, argv, envp);
  int64_t r = hb_execve(hostpath, argv, envp);
  if (r != -ENOSYS) return r;
  int fdmap[3] = {-1, -1, -1};
  int64_t pid = hb_spawn(hostpath, argv, envp, fdmap);
  if (pid < 0) return pid;
  int status = 0;
  while (hb_wait4((int)pid, &status, 0) == -EINTR) {}
  mem_reset();
  if ((status & 0x7f) == 0) hb_exit((status >> 8) & 0xff);
  hb_die(status & 0x7f);
}

int64_t sys_execve(uint64_t a[6], int *noret) {
  char g[LX_PATH_MAX];
  int64_t r = g_strncpy(g, a[0], sizeof g);
  if (r < 0) return r;
  int64_t err = 0;
  char **argv = copy_guest_vec(a[1], &err);
  if (!argv) return err;
  char **envp = copy_guest_vec(a[2], &err);
  if (!envp) { free_vec(argv); return err; }
  if (!argv[0]) { /* Linux passes an empty argv as is; programs expect argv[0] */
    free(argv); argv = malloc(2 * sizeof *argv); argv[0] = strdup(g); argv[1] = 0;
  }
  r = lx_exec(g, argv, envp);
  free_vec(argv);
  free_vec(envp);
  if (r == 0) *noret = 1;
  if (r == 1) { *noret = 1; r = 0; } /* emulated child finished: parent restored */
  return r;
}

/* ---------------- fork emulation (UC.OS) ---------------- */
struct fake_child { int64_t pid; int status; struct fake_child *next; };
static struct fake_child *fakes;
static int64_t next_fake = 100000;

struct snap {
  uint8_t cpu[1024];
  uint64_t clone_flags, child_sp, child_tls, ctid;
  void *memsnap;
  uint64_t sigmask;
  void *sigsnap;
  char cwd[LX_PATH_MAX], gcwd[LX_PATH_MAX];
  int saved_fd[LX_NFD];        /* spare descriptor holding parent's fd i, or -1 */
  int nfd;
  struct snap *outer;
};
static struct snap *child_snap; /* non-null while running an emulated child */
static int in_child(void) { return child_snap != 0; }
int lx_in_emulated_child(void) { return child_snap != 0; }

void *mem_snapshot(void);
void mem_restore(void *s);
void mem_snapshot_free(void *s);
void *sig_snapshot(void);
void sig_restore(void *s);

static int fork_mode_emulate(void) {
  static int m = -1;
  if (m < 0) { const char *e = getenv("LX_FORK"); m = e && !strcmp(e, "emulate"); }
  return m;
}

static int64_t emulated_fork(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid) {
  struct snap *s = calloc(1, sizeof *s);
  if (!s) return -ENOMEM;
  if (arch_cur->state_size > sizeof s->cpu) { free(s); return -ENOMEM; }
  /* parent's registers as they will be after clone returns (result filled in later) */
  arch_cur->save(s->cpu);
  s->clone_flags = flags;
  s->memsnap = (flags & LX_CLONE_VM) ? 0 : mem_snapshot(); /* vfork shares memory: nothing to save */
  if (!(flags & LX_CLONE_VM) && !s->memsnap) { free(s); return -ENOMEM; }
  s->sigmask = sig_get_mask();
  s->sigsnap = sig_snapshot();
  snprintf(s->gcwd, sizeof s->gcwd, "%s", path_cwd());
  if (sys_getcwd_host(s->cwd, sizeof s->cwd) < 0) s->cwd[0] = 0;
  /* Save the descriptor table: list the open descriptors first, then duplicate each one,
     close-on-exec, to a high number the child is unlikely to use (UC.OS has 64 slots). */
  int open_fds[LX_NFD], nopen = 0;
  for (int fd = 0; fd < LX_NFD; fd++) { s->saved_fd[fd] = -1; if (hb2(LX_SYS_fcntl, fd, 1) >= 0) open_fds[nopen++] = fd; }
  int base = 40;
  if (nopen) { int64_t probe = hb3(LX_SYS_fcntl, open_fds[0], 0, 600); if (probe >= 0) { hb1(LX_SYS_close, probe); base = 600; } }
  for (int k = 0; k < nopen; k++) {
    int fd = open_fds[k];
    int64_t d = hb3(LX_SYS_fcntl, fd, 0 /* F_DUPFD */, base);
    if (d >= 0) hb3(LX_SYS_fcntl, d, 2, 1);
    if (d < 0) {
      for (int j = 0; j < LX_NFD; j++) if (s->saved_fd[j] >= 0) hb1(LX_SYS_close, s->saved_fd[j]);
      if (s->memsnap) mem_snapshot_free(s->memsnap);
      free(s->sigsnap);
      free(s);
      return -EAGAIN;
    }
    s->saved_fd[fd] = (int)d;
    s->nfd++;
  }
  /* parent's view of the CLOEXEC flags must also come back: remember them in the high bit */
  for (int fd = 0; fd < LX_NFD; fd++)
    if (s->saved_fd[fd] >= 0 && (hb2(LX_SYS_fcntl, fd, 1) & 1)) s->saved_fd[fd] |= 0x40000000;
  s->outer = child_snap;
  child_snap = s;
  /* now become the child */
  if (newsp) arch_cur->set_sp(newsp);
  if (flags & LX_CLONE_SETTLS) arch_cur->set_tp(tls);
  if ((flags & LX_CLONE_CHILD_SETTID) && g_okrange(ctid, 4)) { uint32_t t = (uint32_t)next_fake; memcpy(g2h(ctid), &t, 4); }
  (void)ptid;
  return 0;
}

int sys_getcwd_host(char *buf, size_t n) {
  int64_t r = hb2(LX_SYS_getcwd, hbp(buf), n);
  return r < 0 ? (int)r : 0;
}

/* Back to the parent: restore everything and make clone return `pid`. */
static void restore_parent(int64_t pid) {
  struct snap *s = child_snap;
  child_snap = s->outer;
  /* descriptors: close what the child left, put the parent's back */
  for (int fd = 0; fd < LX_NFD; fd++) {
    int isspare = 0;
    for (int k = 0; k < LX_NFD; k++) if (s->saved_fd[k] >= 0 && (s->saved_fd[k] & 0x3fffffff) == fd) { isspare = 1; break; }
    if (!isspare && s->saved_fd[fd] < 0) { if (hb1(LX_SYS_close, fd) == 0) fd_path_set(fd, 0); }
  }
  for (int fd = 0; fd < LX_NFD; fd++) {
    if (s->saved_fd[fd] < 0) continue;
    int spare = s->saved_fd[fd] & 0x3fffffff, cloexec = (s->saved_fd[fd] & 0x40000000) != 0;
    hb3(LX_SYS_dup3, spare, fd, cloexec ? LX_O_CLOEXEC : 0);
  }
  for (int fd = 0; fd < LX_NFD; fd++) if (s->saved_fd[fd] >= 0) hb1(LX_SYS_close, s->saved_fd[fd] & 0x3fffffff);
  if (s->cwd[0]) hb1(LX_SYS_chdir, hbp(s->cwd));
  path_set_cwd(s->gcwd);
  if (s->sigsnap) { sig_restore(s->sigsnap); free(s->sigsnap); }
  if (s->memsnap) { mem_restore(s->memsnap); mem_snapshot_free(s->memsnap); }
  arch_cur->flush_code(0, ~0ull);
  arch_cur->load(s->cpu);
  sig_set_mask(s->sigmask);
  arch_cur->sys_ret(pid);
  arch_cur->sys_skip();
  free(s);
}

static void child_done(int64_t pid, int status) {
  if (pid < 0) {
    struct fake_child *f = malloc(sizeof *f);
    f->pid = next_fake++; f->status = status; f->next = fakes; fakes = f;
    pid = f->pid;
  }
  restore_parent(pid);
}

/* The emulated child calls execve on a guest program: start a new engine for it. */
int64_t lx_exec_engine_child(const char *gpath, char *const argv[], char *const envp[]) {
  int ac = 0;
  while (argv[ac]) ac++;
  char **v = malloc((size_t)(ac + 8) * sizeof *v);
  int k = 0;
  char rootopt[LX_PATH_MAX + 8], a0opt[4200];
  v[k++] = (char *)lx_cfg.self;
  snprintf(rootopt, sizeof rootopt, "--root=%s", path_root());
  v[k++] = rootopt;
  snprintf(a0opt, sizeof a0opt, "--argv0=%s", argv[0] ? argv[0] : gpath);
  v[k++] = a0opt;
  v[k++] = "--";
  v[k++] = (char *)gpath;
  for (int i = 1; i < ac; i++) v[k++] = argv[i];
  v[k] = 0;
  int fdmap[3] = {-1, -1, -1};
  int64_t pid = hb_spawn(lx_cfg.self, v, envp, fdmap);
  free(v);
  if (pid < 0) return pid;
  child_done(pid, 0);
  return 1;
}
int64_t lx_exec_native_child(const char *hostpath, char *const argv[], char *const envp[]) {
  int fdmap[3] = {-1, -1, -1};
  int64_t pid = hb_spawn(hostpath, argv, envp, fdmap);
  if (pid < 0) return pid;
  child_done(pid, 0);
  return 1;
}
/* exit in an emulated child: record the status and resume the parent. Returns 0 if this
 * process is not an emulated child (the caller then really exits). */
int lx_child_exit(int code, int sig) {
  if (!child_snap) return 0;
  child_done(-1, sig ? sig : (code & 0xff) << 8);
  return 1;
}

/* ---------------- clone / wait ---------------- */
/* The child's half of a clone made by the host (natively fork(); on UC.OS the kernel's
 * clone primitive, which starts the child at the export __fork_child instead of
 * returning from the call). The request is kept in memory so both paths can find it. */
static struct { uint64_t flags, newsp, tls, ctid; int active; } fork_req;
static void clone_child_setup(void) {
  lx_threads_reset();
  if (fork_req.newsp) arch_cur->set_sp(fork_req.newsp);
  if (fork_req.flags & LX_CLONE_SETTLS) arch_cur->set_tp(fork_req.tls);
  if ((fork_req.flags & LX_CLONE_CHILD_SETTID) && g_okrange(fork_req.ctid, 4)) { uint32_t t = (uint32_t)hb0(LX_SYS_getpid); memcpy(g2h(fork_req.ctid), &t, 4); }
  if (fork_req.flags & LX_CLONE_CHILD_CLEARTID) lx_clear_tid = fork_req.ctid;
  fork_req.active = 0;
}

#if defined(__wasm__) && defined(LX_HAVE_PCLONE)
/* UC.OS clone primitive (SYS_pclone, 520): the child is a copy of this instance's memory
 * and exported globals, entered here on a fresh stack. The engine only forks from its
 * system call handler, so the child finishes the guest's clone with result 0 and goes
 * back to the main loop. Must not return. */
__attribute__((noreturn)) void lx_main_loop(void);
void lx_jit_after_copy(void);
__attribute__((export_name("__fork_child"), noreturn)) void lx_fork_child(void) {
  if (!fork_req.active) hb_die(LX_SIGKILL); /* not a fork we asked for */
  lx_jit_after_copy(); /* the copy has a new function table and none of the parent's generated code */
  clone_child_setup();
  arch_cur->sys_ret(0);
  arch_cur->sys_skip();
  lx_main_loop();
}
#endif

static int64_t do_clone(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid) {
  if ((flags & LX_CLONE_THREAD) || ((flags & LX_CLONE_VM) && !(flags & LX_CLONE_VFORK)))
    return lx_thread_create(flags, newsp, ptid, tls, ctid); /* green threads (sys_thread.c) */
  if (!fork_mode_emulate()) {
    fork_req.flags = flags; fork_req.newsp = newsp; fork_req.tls = tls; fork_req.ctid = ctid; fork_req.active = 1;
    int64_t pid = hb_fork();
    if (pid != -ENOSYS) {
      if (pid < 0) { fork_req.active = 0; return pid; }
      if (pid == 0) { clone_child_setup(); return 0; }
      fork_req.active = 0;
      if ((flags & LX_CLONE_PARENT_SETTID) && g_okrange(ptid, 4)) { uint32_t t = (uint32_t)pid; memcpy(g2h(ptid), &t, 4); }
      /* vfork: the child has its own copy, so the parent need not wait for its exec */
      return pid;
    }
    fork_req.active = 0;
  }
  return emulated_fork(flags, newsp, ptid, tls, ctid);
}

int64_t sys_clone(uint64_t a[6]) {
  /* riscv64 selects CLONE_BACKWARDS: (flags, newsp, parent_tid, tls, child_tid) */
  return do_clone(a[0], a[1], a[2], a[3], a[4]);
}
int64_t sys_clone3(uint64_t a[6]) {
  if (a[1] < 64 || !g_okrange(a[0], 64)) return -EINVAL;
  uint64_t c[8];
  memcpy(c, g2h(a[0]), 64);
  uint64_t sp = c[5] ? c[5] + c[6] : 0;
  return do_clone(c[0] | (c[4] & 0xff), sp, c[3], c[7], c[2]);
}

int64_t sys_wait4(uint64_t a[6]) {
  int64_t pid = (int64_t)(int32_t)a[0];
  int opts = (int)a[2];
  for (struct fake_child **pp = &fakes; *pp; pp = &(*pp)->next) {
    struct fake_child *f = *pp;
    if (pid == -1 || pid == f->pid || pid == 0) {
      if (a[1] && g_okrange(a[1], 4)) memcpy(g2h(a[1]), &f->status, 4);
      int64_t r = f->pid;
      *pp = f->next;
      free(f);
      if (a[3] && g_okrange(a[3], sizeof(struct g_rusage))) memset(g2h(a[3]), 0, sizeof(struct g_rusage));
      return r;
    }
  }
  int status = 0;
  int64_t r = hb_wait4((int)pid, &status, opts & 0x7);
  if (r > 0 && a[1]) { if (!g_okrange(a[1], 4)) return -EFAULT; memcpy(g2h(a[1]), &status, 4); }
  if (r > 0 && a[3] && g_okrange(a[3], sizeof(struct g_rusage))) memset(g2h(a[3]), 0, sizeof(struct g_rusage));
  return r;
}

/* waitid(idtype, id, siginfo*, options, rusage*) on top of wait4. */
int64_t sys_waitid(uint64_t a[6]) {
  int idtype = (int)a[0];
  int64_t id = (int64_t)(int32_t)a[1];
  int opts = (int)a[3];
  int64_t pid = idtype == 0 ? -1 : idtype == 1 ? id : idtype == 2 ? -id : -1; /* P_ALL P_PID P_PGID */
  uint64_t w[6] = {(uint64_t)pid, 0, (uint64_t)(opts & 1), 0, 0, 0};
  int status = 0;
  int64_t r;
  for (struct fake_child *f = fakes; f; f = f->next) (void)f;
  if (fakes) {
    uint64_t tmp = 0;
    (void)tmp;
  }
  /* reuse wait4 with a host-side status */
  r = hb_wait4((int)pid, &status, opts & 1);
  if (r == -ECHILD && fakes) {
    struct fake_child *f = fakes; fakes = f->next; r = f->pid; status = f->status; free(f);
  }
  (void)w;
  if (r < 0) return r;
  if (a[2] && g_okrange(a[2], 128)) {
    uint8_t si[128];
    memset(si, 0, sizeof si);
    if (r > 0) {
      int32_t v;
      v = LX_SIGCHLD; memcpy(si + 0, &v, 4);
      int exited = (status & 0x7f) == 0;
      v = exited ? 1 /* CLD_EXITED */ : 2 /* CLD_KILLED */; memcpy(si + 8, &v, 4);
      v = (int32_t)r; memcpy(si + 16, &v, 4);
      v = (int32_t)lx_uid(); memcpy(si + 20, &v, 4);
      v = exited ? (status >> 8) & 0xff : status & 0x7f; memcpy(si + 24, &v, 4);
    }
    memcpy(g2h(a[2]), si, 128);
  }
  return 0;
}
