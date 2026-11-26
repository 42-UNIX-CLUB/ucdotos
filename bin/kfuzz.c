// SPDX-License-Identifier: GPL-2.0-or-later
/* kfuzz: a system call fuzzer for the kernel.
 *
 *   kfuzz SEED ITERATIONS [v] [p=N] [t=SECONDS] [noktest] [big]
 *
 * The first process is the supervisor. It sets up one object of every kind (files, a
 * directory, pipes with and without a reader or writer, ptys with each end closed,
 * sockets in every state, /dev/fb0, /dev/input/event0, /dev/null and friends), then
 * starts N workers (default 4) that inherit all of them. Each worker makes ITERATIONS
 * system calls with a mix of valid, boundary and garbage arguments, drawn from a
 * random generator seeded with SEED, the worker number and its generation. A worker that
 * dies (killed by another worker, crashed, exited) is replaced by the next generation
 * until its budget is spent. A worker that stops making calls for a while is killed,
 * which also exercises killing a parked process.
 *
 * At the end the supervisor kills everything that is left, removes what the workers
 * created, compares the kernel heap with the figure from before the run, then runs
 * /bin/ktest in the same boot and reports its exit status. Last line:
 *   kfuzz: PASS ...   or   kfuzz: FAIL ...
 *
 * "big" also lets file sizes between 64 MiB and 2 GiB through (the kernel then really
 * allocates that much). "v" prints every call before it is made, with its path strings,
 * and its result after, so the last line before a kernel trap or hang names the call.
 *
 * Reproducing: a worker generation's calls follow from its seed (a few use results of
 * earlier calls, such as a pty number). When a generation ends (stuck and killed, or
 * over budget) depends on timing, and with several workers so does the interleaving.
 * p=1 is the closest to a fixed sequence.
 *
 * The workers may die or get errors; that is expected. What counts as a kernel bug: a
 * trap inside the kernel, a kernel panic, a hang, the heap not returning to its size
 * from before the run, or ktest failing afterwards. The host reports a trap inside the
 * kernel like a worker crash ("killed by signal 11"); only the trap's stack trace tells
 * them apart, and tools/run.mjs --debug prints the message, not the stack.
 *
 * Seeds that found kernel bugs (on the kernel as it was before 2026-11-26's hardening,
 * with this version of kfuzz), kept as regression cases: see REGRESSION_SEEDS below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>

/* REGRESSION_SEEDS: seeds that found kernel bugs. Each hung the kernel as it was before
 * the 2026-11-26 hardening (a write or ftruncate past 2 GiB looped for ever in
 * reg_reserve); seed 6 also trapped inside it first (memset in reg_write). Seed 105 (p=4,
 * 20000 calls) once unlinked /dev/zero through unlinkat(fd, "") on the fixed kernel of
 * that afternoon; it depends on timing and is kept for that reason. */
static const struct { unsigned long long seed; long iters; int workers; } regression[] = {
  {1, 2000, 4}, {2, 2000, 4}, {3, 2000, 4}, {4, 2000, 4}, {5, 2000, 4}, {6, 2000, 4}, {105, 20000, 4},
};
#define LOGFD 63   /* the console, kept out of the workers' reach */
#define MYDIR 61   /* KDIR, and a file in it: the only directory descriptors used for */
#define MYFILE 60  /* creating and removing, and never closed or replaced by a worker */
#define KDIR "/tmp/kfuzz"
#define MAXW 16

/* ---- random numbers: xorshift64*, seeded through splitmix64 ---- */
static unsigned long long rs;
static unsigned long long mix(unsigned long long x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}
static unsigned rnd(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return (unsigned)((rs * 2685821657736338717ull) >> 32); }
static unsigned below(unsigned n) { return n ? rnd() % n : 0; }
static int chance(int pct) { return (int)below(100) < pct; }
static unsigned long long parse_u64(const char *s) { unsigned long long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0'); return v; }

static void say(const char *fmt, ...) {
  char b[512];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  __builtin_va_end(ap);
  if (n > (int)sizeof b - 1) n = sizeof b - 1;
  if (n > 0) __syscall3(SYS_write, LOGFD, b, n);
}
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static long heap_used(void) { struct sysinfo si; return __syscall1(SYS_sysinfo, &si) ? -1 : (long)si.bufferram; }

/* ---- what the supervisor hands down: fixed descriptor numbers ---- */
enum {
  F_DIR = 3,      /* KDIR, O_DIRECTORY */
  F_FILE,         /* KDIR/f, O_RDWR */
  F_APPEND,       /* KDIR/f, O_WRONLY | O_APPEND */
  F_ROFILE,       /* /etc/motd, O_RDONLY (an initrd file) */
  F_PIPE_R, F_PIPE_W,
  F_PIPE_R_NOWRITER, F_PIPE_W_NOREADER,
  F_PTM, F_PTS,
  F_PTS_HUNG,     /* slave whose master is closed */
  F_PTM_HUNG,     /* master whose slave was opened and closed */
  F_SOCK_NEW, F_SOCK_BOUND, F_SOCK_LISTEN, F_SOCK_C, F_SOCK_A, F_SOCK_HUP,
  F_FB, F_EVENT, F_NULL, F_ZERO, F_RANDOM,
  F_LAST
};
static int sup_pid, sup_ppid, verbose, big;
/* Sizes and offsets between 64 MiB and 2 GiB make the kernel really allocate that much
 * (wasm memory never shrinks). Unless "big" is given, fold them below 64 MiB. Values of
 * 2 GiB and more are kept: the kernel must refuse them without allocating. */
static long fsize(long v) { unsigned u = (unsigned)v; return !big && u > (64u << 20) && u < 0x80000000u ? (long)(u & 0x3ffffff) : v; }

/* ---- a worker ---- */
static unsigned char scratch[65536] __attribute__((aligned(16)));
static char strpool[4096];
static int nstr;
static unsigned memsize(void) { return (unsigned)__builtin_wasm_memory_size(0) << 16; }

static const char *mut_paths[] = { /* paths the workers may create, remove and rename */
  KDIR "/a", KDIR "/b", KDIR "/d", KDIR "/d/x", KDIR "/d/e", KDIR "/d/e/f", KDIR "/f", KDIR "/s.sock",
  KDIR "/l.sock", KDIR "/g", KDIR "/g/h", KDIR "/../kfuzz/a", KDIR "/a/", KDIR "/d/", KDIR "/.", KDIR "/d/..",
  "a", "b", "d", "d/x", "d/e", "g", "f", "s.sock", ".", "..", "", "d/e/../x", "g/h", "./a", "a//b",
  KDIR "/0123456789012345678901234567890123456789012345678901234567890xy",   /* 63-byte name */
  KDIR "/0123456789012345678901234567890123456789012345678901234567890xyz",  /* 64 */
  KDIR "/0123456789012345678901234567890123456789012345678901234567890xyzw", /* 65 */
  "0123456789012345678901234567890123456789012345678901234567890xyz",
};
static const char *ro_paths[] = { /* paths they may only look at */
  "/", "/dev/null", "/dev/zero", "/dev/urandom", "/dev/ptmx", "/dev/pts/0", "/dev/pts/1", "/dev/pts/2",
  "/dev/pts/15", "/dev/fb0", "/dev/input/event0", "/bin/true", "/etc/motd", "/tmp", "/proc", "/nonexistent",
  "/bin/true/x", "/dev/pts", KDIR, KDIR "/d", "//dev//null", "/dev/../dev/zero",
  /* symbolic links and /proc (2026-11-26); never /dev/fd/N or /proc/self/fd/N here: case 3
   * may open "/dev/..." paths read-write, and fd 6 is /etc/motd */
  "/proc/self", "/proc/self/exe", "/proc/self/cwd", "/proc/self/status", "/proc/cpuinfo", "/proc/meminfo",
  "/proc/self/fd", "/dev/tty", "/proc/99", KDIR "/a/b/c", KDIR "/d/e/f",
  /* not /dev/stdin: case 3 may open it read-write, and a worker's fd 0 can be a dup of the
   * read-only /etc/motd descriptor (reopening it read-write is correct, as on Linux) */
};
/* targets for new symbolic links: always inside KDIR (or /dev/null), so that nothing a worker
 * does through a link can reach the image or leave files outside KDIR */
/* not ".": a link to KDIR itself followed by ".." reaches /tmp */
static const char *link_targets[] = {"a", "d", "d/x", "../kfuzz/f", "f", KDIR "/d", KDIR "/g/h", "nonexistent", "/dev/null", "a/../d", "g//h", "s.sock", "d/e"};
/* never "..": from KDIR that is /tmp, and files made there would outlive the run */
static const char *cd_paths[] = { KDIR, KDIR "/d", KDIR "/d/e", KDIR "/g", KDIR "/a", "/nonexistent", "d", "e", "." };
static const char *spawn_paths[] = { "/bin/true", "/bin/false", "/bin/kfuzz", "/etc/motd", "/tmp", "/nonexistent", KDIR "/f", "" };

static long gen_ptr(void) { /* a pointer the kernel will write through, or read from */
  switch (below(20)) {
  case 0: return 0;
  case 1: return 1 + below(4096);             /* the far end of our stack */
  case 2: return memsize() - below(64);       /* crosses the end of memory */
  case 3: return memsize() + below(1 << 20);  /* beyond it */
  case 4: return (long)(0xffffffffu - below(16));
  case 5: return (long)0x80000000u;
  case 6: return (long)(scratch + sizeof scratch - below(64)); /* near the end of the buffer */
  default: return (long)(scratch + below(sizeof scratch - 4096));
  }
}
static long gen_len(void) {
  switch (below(16)) {
  case 0: return 0;
  case 1: return 1;
  case 2: return 4096;
  case 3: return 65536;
  case 4: return (long)(0xffffffffu - below(4));
  case 5: return 0x7fffffff;
  case 6: return (long)0x80000000u;
  case 7: return 24;
  case 8: return 1 + below(70000);
  default: return below(4096);
  }
}
static long gen_int(void) {
  switch (below(12)) {
  case 0: return 0;
  case 1: return 1;
  case 2: return -1;
  case 3: return 0x7fffffff;
  case 4: return (long)0x80000000u;
  case 5: return (long)rnd();
  case 6: return below(256);
  default: return below(8);
  }
}
static int gen_fd(void) {
  int fd;
  switch (below(16)) {
  case 0: fd = -1; break;
  case 1: fd = AT_FDCWD; break;
  case 2: fd = 64 + below(8); break;
  case 3: fd = (int)rnd(); break;
  case 4: case 5: case 6: case 7: fd = below(F_LAST); break;
  default: fd = below(64);
  }
  return fd == LOGFD || fd == MYDIR || fd == MYFILE ? 0 : fd;
}
static int gen_mut_dirfd(void) { static const int d[] = {AT_FDCWD, AT_FDCWD, AT_FDCWD, MYDIR, MYDIR, -1, 999, MYFILE}; return d[below(8)]; }
static int gen_pid(void) {
  int p;
  switch (below(8)) {
  case 0: p = -1 - (int)below(3); break;
  case 1: p = 0; break;
  case 2: p = getpid(); break;
  case 3: p = 64 + below(4); break;
  default: p = below(64);
  }
  if (p == sup_pid || p == sup_ppid || (p == 1 && sup_pid != 1)) p = getpid();
  return p;
}
static long str_ptr(const char *s) { /* copy into our pool so the kernel reads it from a normal place */
  int n = strlen(s) + 1;
  if (nstr + n > (int)sizeof strpool) nstr = 0;
  memcpy(strpool + nstr, s, n);
  long p = (long)(strpool + nstr);
  nstr += n;
  return p;
}
static long gen_mut_path(void) {
  if (chance(4)) return gen_ptr();
  if (chance(2)) { /* no NUL within PATH_MAX */
    memset(scratch, 'a', 300);
    return (long)scratch;
  }
  if (chance(2)) { /* exactly at the limit: 255 bytes plus NUL, or 256 */
    int n = 255 + below(2);
    memcpy(scratch, KDIR "/", 12);
    memset(scratch + 12, 'b', n - 12);
    for (int i = 70; i < n; i += 64) scratch[i] = '/';
    scratch[n] = 0;
    return (long)scratch;
  }
  return str_ptr(mut_paths[below(sizeof mut_paths / sizeof *mut_paths)]);
}
/* paths for chmod, chown, truncate and utimensat: names from mut_paths only, never a random
 * pointer (it can land on an earlier string such as "/etc/motd"), never "..", "." or "" (a
 * worker's working directory may still be "/") */
static long gen_attr_path(void) {
  for (;;) {
    const char *p = mut_paths[below(sizeof mut_paths / sizeof *mut_paths)];
    if (!*p || strstr(p, "..") || !strcmp(p, ".")) continue;
    return str_ptr(p);
  }
}
static long gen_ro_path(void) {
  if (chance(5)) return gen_ptr();
  return str_ptr(ro_paths[below(sizeof ro_paths / sizeof *ro_paths)]);
}
static long gen_timespec(void) {
  if (chance(10)) return gen_ptr();
  struct timespec *ts = (struct timespec *)(scratch + 60000);
  switch (below(8)) {
  case 0: ts->tv_sec = -1; ts->tv_nsec = 0; break;
  case 1: ts->tv_sec = 0; ts->tv_nsec = -5; break;
  case 2: ts->tv_sec = 0; ts->tv_nsec = 2000000000; break;
  case 3: if (chance(20)) { ts->tv_sec = 1ll << 40; ts->tv_nsec = 0; break; } /* fall through */
  default: ts->tv_sec = 0; ts->tv_nsec = below(4000000);
  }
  return (long)ts;
}
static void fill_random(void *p, unsigned n) { unsigned char *b = p; for (unsigned i = 0; i < n; i++) b[i] = rnd(); }
static long gen_sockaddr(long *len) {
  struct { unsigned short fam; char path[108]; } *a = (void *)(scratch + 61000);
  static const char *sp[] = {KDIR "/s.sock", KDIR "/l.sock", KDIR "/k.sock", KDIR "/f", KDIR, "", "s.sock"};
  memset(a, 0, sizeof *a);
  a->fam = chance(90) ? 1 : rnd();
  const char *p = sp[below(7)];
  memcpy(a->path, p, strlen(p));
  if (chance(5)) memset(a->path, 'q', sizeof a->path); /* no NUL */
  *len = chance(85) ? (long)sizeof *a : gen_len();
  return chance(5) ? gen_ptr() : (long)a;
}
static long gen_ioctl_cmd(void) {
  static const unsigned c[] = {TCGETS, TCSETS, TCSETSW, TCSETSF, TIOCGWINSZ, TIOCSWINSZ, TIOCGPGRP, TIOCSPGRP, TIOCGPTN, 0x540B, 0x540A, 0x5409, 0x540E, 0x4600, 0};
  return chance(90) ? (long)c[below(15)] : (long)rnd();
}
static long gen_ioctl_arg(long cmd) {
  if (chance(10)) return gen_ptr();
  unsigned char *p = scratch + 62000;
  if (cmd == TCSETS || cmd == TCSETSW || cmd == TCSETSF) {
    struct termios *t = (struct termios *)p;
    if (chance(50)) { __syscall3(SYS_ioctl, F_PTS, TCGETS, t); t->c_lflag ^= 1u << below(16); t->c_cc[below(NCCS)] = below(8); }
    else fill_random(t, sizeof *t);
  } else if (cmd == TIOCSPGRP) {
    int v = chance(50) ? getpid() : gen_pid();
    if (v == sup_pid) v = getpid();
    memcpy(p, &v, 4);
  } else fill_random(p, 64);
  return (long)p;
}
static long gen_pollfds(long *n) {
  struct pollfd *pf = (struct pollfd *)(scratch + 50000);
  *n = chance(90) ? below(10) : gen_len();
  for (int i = 0; i < 10; i++) { pf[i].fd = gen_fd(); pf[i].events = chance(80) ? (POLLIN | POLLOUT) : rnd(); pf[i].revents = 0; }
  return chance(5) ? gen_ptr() : (long)pf;
}
static long gen_argv(void) {
  if (chance(5)) return gen_ptr();
  long *v = (long *)(scratch + 63000);
  int n = below(5);
  static const char *words[] = {"x", "worker", "", "--", "0123456789"};
  for (int i = 0; i < n; i++) v[i] = chance(5) ? gen_ptr() : str_ptr(words[below(5)]);
  v[n] = 0;
  return (long)v;
}
static long gen_fdmap(void) {
  if (chance(30)) return 0;
  if (chance(5)) return gen_ptr();
  int *m = (int *)(scratch + 63500);
  for (int i = 0; i < 3; i++) m[i] = chance(30) ? -1 : gen_fd();
  return (long)m;
}
static int is_input(int fd) { /* reading the input device would eat the events ktest checks afterwards */
  struct stat st;
  return __syscall2(SYS_fstat, fd, &st) == 0 && (st.st_mode & S_IFMT) == S_IFCHR && st.st_rdev == 6;
}

static int path_arg(long nr, int i) { /* which arguments are path strings, for the verbose log */
  switch (nr) {
  case SYS_chdir: case SYS_spawn: return i == 0;
  case SYS_openat: case SYS_mkdirat: case SYS_unlinkat: case SYS_newfstatat: case SYS_utimensat: return i == 1;
  case SYS_renameat: return i == 1 || i == 3;
  case SYS_symlinkat: return i == 0 || i == 2;
  case SYS_linkat: case SYS_renameat2: return i == 1 || i == 3;
  case SYS_readlinkat: case SYS_fchmodat: case SYS_fchmodat2: case SYS_fchownat: case SYS_statx: case SYS_mknodat:
  case SYS_faccessat: case SYS_faccessat2: return i == 1;
  case SYS_truncate: case SYS_chroot: case SYS_statfs: return i == 0;
  }
  return 0;
}
static void show(const char *name, long nr, int n, long *a) {
  if (!verbose) return;
  char b[400];
  int len = snprintf(b, sizeof b, "[w%d] %s(", getpid(), name);
  for (int i = 0; i < n && len < (int)sizeof b; i++) {
    unsigned long v = (unsigned long)a[i];
    if (path_arg(nr, i) && v && v < memsize() - 80) len += snprintf(b + len, sizeof b - len, i ? ", \"%.60s\"" : "\"%.60s\"", (char *)v);
    else len += snprintf(b + len, sizeof b - len, i ? ", %#lx" : "%#lx", v);
  }
  if (len < (int)sizeof b) snprintf(b + len, sizeof b - len, ")");
  say("%s", b);
}
static long do_call(const char *name, long nr, int n, long a0, long a1, long a2, long a3) {
  long a[4] = {a0, a1, a2, a3};
  show(name, nr, n, a);
  long r = __syscall6(nr, a0, a1, a2, a3, 0, 0);
  if (verbose) say(" = %ld\n", r);
  return r;
}
#define C0(nr) do_call(#nr, nr, 0, 0, 0, 0, 0)
#define C1(nr, a) do_call(#nr, nr, 1, (long)(a), 0, 0, 0)
#define C2(nr, a, b) do_call(#nr, nr, 2, (long)(a), (long)(b), 0, 0)
#define C3(nr, a, b, c) do_call(#nr, nr, 3, (long)(a), (long)(b), (long)(c), 0)
#define C4(nr, a, b, c, d) do_call(#nr, nr, 4, (long)(a), (long)(b), (long)(c), (long)(d))
static long do_call5(const char *name, long nr, long a0, long a1, long a2, long a3, long a4) {
  long a[4] = {a0, a1, a2, a3};
  show(name, nr, 4, a);
  long r = __syscall6(nr, a0, a1, a2, a3, a4, 0);
  if (verbose) say(" [, %#lx] = %ld\n", (unsigned long)a4, r);
  return r;
}
#define C5(nr, a, b, c, d, e) do_call5(#nr, nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))

/* Short sequences with correct arguments, to reach states random calls rarely reach. */
static void macro(void) {
  int fd, fd2, fd3;
  long len;
  switch (below(8)) {
  case 0: { /* a socket through every state */
    fd = C3(SYS_socket, 1, 1 | (chance(70) ? O_NONBLOCK : 0), 0);
    long sa = gen_sockaddr(&len);
    C3(SYS_bind, fd, sa, len);
    C2(SYS_listen, fd, gen_int());
    fd2 = C3(SYS_socket, 1, 1, 0);
    C3(SYS_connect, fd2, sa, len);
    if (chance(70)) { fd3 = C1(SYS_accept, fd); if (chance(50)) C3(SYS_write, fd3, scratch, below(2000)); }
    C3(SYS_write, fd2, scratch, below(3000));
    if (chance(50)) C1(SYS_close, fd);
    if (chance(50)) C1(SYS_close, fd2);
    break;
  }
  case 1: { /* a pty: open both ends, type, change modes, close in either order */
    fd = C4(SYS_openat, AT_FDCWD, str_ptr("/dev/ptmx"), O_RDWR | (chance(50) ? O_NONBLOCK : 0), 0);
    int n = -1;
    C3(SYS_ioctl, fd, TIOCGPTN, &n);
    char path[32];
    snprintf(path, sizeof path, "/dev/pts/%d", n);
    fd2 = C4(SYS_openat, AT_FDCWD, str_ptr(path), O_RDWR | O_NONBLOCK, 0);
    if (chance(50)) { C0(SYS_setsid); int me = getpid(); C3(SYS_ioctl, fd2, TIOCSPGRP, &me); }
    static const char *typed[] = {"ls\r", "abc\x7f\x7f\x7f\x7f\x7f", "\x15", "\x04", "\x03", "x\x1c", "\xc3\xa9\x7f", "\r\r\r"};
    const char *t = typed[below(8)];
    C3(SYS_write, fd, str_ptr(t), strlen(t));
    C3(SYS_read, fd2, scratch, below(600));
    C3(SYS_write, fd2, scratch, below(70000));
    C3(SYS_read, fd, scratch, below(70000));
    if (chance(50)) { C1(SYS_close, fd); C1(SYS_close, fd2); } else { C1(SYS_close, fd2); C1(SYS_close, fd); }
    break;
  }
  case 2: { /* a pipe and a child that inherits it */
    int p[2] = {-1, -1};
    C2(SYS_pipe2, p, chance(50) ? O_CLOEXEC : 0);
    int map[3] = {p[0], p[1], -1};
    long argv = str_ptr("true");
    long v[2] = {argv, 0};
    memcpy(scratch + 64000, v, sizeof v);
    int pid = C4(SYS_spawn, str_ptr(chance(80) ? "/bin/true" : "/bin/false"), scratch + 64000, 0, map);
    C3(SYS_write, p[1], scratch, 100);
    C1(SYS_close, p[1]);
    C3(SYS_read, p[0], scratch, 200);
    C1(SYS_close, p[0]);
    if (pid > 0 && chance(70)) C4(SYS_wait4, pid, 0, 0, 0);
    break;
  }
  case 3: { /* file growth and boundary offsets */
    fd = C4(SYS_openat, AT_FDCWD, str_ptr(KDIR "/a"), O_RDWR | O_CREAT | (chance(30) ? O_APPEND : 0), 0644);
    static const unsigned offs[] = {0, 1, 4095, 4096, 65536, 0x7fffffff, 0x80000000u, 0xfffffff0u, 0xffffffffu, 1u << 20};
    unsigned off = offs[below(10)];
    if (chance(50)) C4(SYS_pwrite64, fd, scratch, below(5000), fsize(off));
    else { C3(SYS_lseek, fd, fsize(off), below(3)); C3(SYS_write, fd, scratch, below(5000)); }
    if (chance(30)) C2(SYS_ftruncate, fd, chance(50) ? fsize(offs[below(10)]) : (long)below(100000));
    C4(SYS_pread64, fd, scratch, below(5000), offs[below(10)]);
    if (chance(60)) C1(SYS_close, fd);
    break;
  }
  case 4: { /* directories: a cwd that is removed under us */
    C3(SYS_mkdirat, AT_FDCWD, str_ptr(KDIR "/g"), 0755);
    C1(SYS_chdir, str_ptr(KDIR "/g"));
    if (chance(50)) C3(SYS_mkdirat, AT_FDCWD, str_ptr("h"), 0755);
    C3(SYS_unlinkat, AT_FDCWD, str_ptr(KDIR "/g/h"), AT_REMOVEDIR);
    C3(SYS_unlinkat, AT_FDCWD, str_ptr(KDIR "/g"), AT_REMOVEDIR);
    C4(SYS_openat, AT_FDCWD, str_ptr("new"), O_CREAT | O_RDWR, 0644);
    C2(SYS_getcwd, scratch, 256);
    static const char *dots[] = {".", "..", "", "new"};
    C3(SYS_unlinkat, AT_FDCWD, str_ptr(dots[below(4)]), chance(50) ? AT_REMOVEDIR : 0);
    C4(SYS_renameat, AT_FDCWD, str_ptr(dots[below(4)]), AT_FDCWD, str_ptr(chance(50) ? "z" : KDIR "/z"));
    fd = C4(SYS_openat, AT_FDCWD, str_ptr("."), O_RDONLY, 0);
    C3(SYS_getdents64, fd, scratch, chance(50) ? 64 : 10);
    C1(SYS_chdir, str_ptr(KDIR));
    break;
  }
  case 5: { /* descriptor tables */
    int to = below(66);
    C3(SYS_dup3, gen_fd(), to == LOGFD || to == MYDIR || to == MYFILE ? 62 : to, chance(50) ? O_CLOEXEC : 0);
    static const long from[] = {-1, -1000, 0, 63, 64, 1000, 0x7fffffff, (long)0x80000000u};
    C3(SYS_fcntl, gen_fd(), 0 /* F_DUPFD */, from[below(8)]);
    C3(SYS_fcntl, gen_fd(), below(6), gen_int());
    break;
  }
  case 6: { /* a directory with many entries, read back with small buffers */
    char name[32];
    for (int i = 0; i < 20; i++) { snprintf(name, sizeof name, KDIR "/d/n%u", below(40)); C4(SYS_openat, AT_FDCWD, str_ptr(name), O_CREAT | O_WRONLY, 0644) >= 0 ? (void)0 : (void)0; }
    fd = C4(SYS_openat, AT_FDCWD, str_ptr(KDIR "/d"), O_RDONLY | O_DIRECTORY, 0);
    for (int i = 0; i < 4; i++) C3(SYS_getdents64, fd, scratch, below(100));
    C3(SYS_lseek, fd, below(30), 0);
    C3(SYS_getdents64, fd, scratch, 4096);
    for (int i = 0; i < 20; i++) { snprintf(name, sizeof name, KDIR "/d/n%u", below(40)); C3(SYS_unlinkat, AT_FDCWD, str_ptr(name), 0); }
    break;
  }
  case 7: { /* kill and wait */
    C2(SYS_kill, gen_pid(), chance(80) ? 9 : gen_int());
    C4(SYS_wait4, gen_pid(), chance(50) ? (long)(scratch + 100) : gen_ptr(), chance(70) ? WNOHANG : 0, 0);
    break;
  }
  }
}

static void one_call(void) {
  if (chance(15)) { macro(); return; }
  long len, n;
  int fd;
  switch (below(61)) {
  case 0: fd = gen_fd(); if (!is_input(fd)) C3(SYS_read, fd, gen_ptr(), gen_len()); break;
  case 1: C3(SYS_write, gen_fd(), gen_ptr(), gen_len()); break;
  case 2: C4(SYS_openat, gen_mut_dirfd(), gen_mut_path(), gen_int() & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC), gen_int()); break;
  case 3: { /* read-only, except devices: a worker must not rewrite /bin/true */
    long path = gen_ro_path();
    int dev = path >= (long)strpool && path < (long)strpool + (long)sizeof strpool && !strncmp((char *)path, "/dev/", 5);
    C4(SYS_openat, gen_fd(), path, (dev && chance(50) ? O_RDWR : O_RDONLY) | (gen_int() & (O_NONBLOCK | O_DIRECTORY | O_CLOEXEC)), 0);
    break;
  }
  case 4: fd = gen_fd(); if (fd > F_LAST || chance(30)) C1(SYS_close, fd); break;
  case 5: C3(SYS_lseek, gen_fd(), fsize(gen_int()), chance(90) ? below(3) : gen_int()); break;
  case 6: C2(SYS_fstat, gen_fd(), gen_ptr()); break;
  case 7: C3(SYS_newfstatat, gen_fd(), chance(50) ? gen_ro_path() : gen_mut_path(), gen_ptr()); break;
  case 8: C3(SYS_getdents64, gen_fd(), gen_ptr(), gen_len()); break;
  case 9: C1(SYS_dup, gen_fd()); break;
  case 10: { int to = chance(90) ? (int)below(66) : gen_fd(); if (to == LOGFD || to == MYDIR || to == MYFILE) to = 62; C3(SYS_dup3, gen_fd(), to, chance(50) ? O_CLOEXEC : gen_int()); break; }
  case 11: C3(SYS_fcntl, gen_fd(), below(7), gen_int()); break;
  case 12: C2(SYS_pipe2, chance(90) ? (long)(scratch + 200) : gen_ptr(), gen_int() & (O_NONBLOCK | O_CLOEXEC | 1)); break;
  case 13: C1(SYS_chdir, chance(90) ? str_ptr(cd_paths[below(sizeof cd_paths / sizeof *cd_paths)]) : gen_ptr()); break;
  case 14: C2(SYS_getcwd, gen_ptr(), gen_len()); break;
  case 15: C3(SYS_mkdirat, gen_mut_dirfd(), gen_mut_path(), gen_int()); break;
  case 16: C3(SYS_unlinkat, gen_mut_dirfd(), gen_mut_path(), chance(50) ? AT_REMOVEDIR : gen_int()); break;
  case 17: C4(SYS_renameat, gen_mut_dirfd(), gen_mut_path(), gen_mut_dirfd(), gen_mut_path()); break;
  case 18: C2(SYS_ftruncate, gen_fd(), chance(80) ? (long)below(70000) : fsize(gen_int())); break;
  case 19: { long p = gen_pollfds(&n); C3(SYS_ppoll, p, n, chance(30) ? 0 : gen_timespec()); break; }
  case 20: C1(SYS_nanosleep, gen_timespec()); break;
  case 21: C2(SYS_clock_gettime, chance(80) ? below(4) : gen_int(), gen_ptr()); break;
  case 22: C0(SYS_sched_yield); break;
  case 23: C1(SYS_uname, gen_ptr()); break;
  case 24: C0(SYS_getpid); C0(SYS_getppid); C0(SYS_getuid); break;
  case 25: C2(SYS_getrandom, gen_ptr(), chance(80) ? below(5000) : gen_len()); break;
  case 26: C4(SYS_wait4, gen_pid(), gen_ptr(), chance(80) ? WNOHANG : gen_int(), 0); break;
  case 27: C2(SYS_kill, gen_pid(), chance(30) ? 0 : chance(70) ? 9 : gen_int()); break;
  case 28: C1(SYS_sysinfo, gen_ptr()); break;
  case 29: { int pg = chance(50) ? gen_pid() : 0; C2(SYS_setpgid, gen_pid(), pg == sup_pid ? 0 : pg); break; }
  case 30: C1(SYS_getpgid, gen_pid()); break;
  case 31: C0(SYS_setsid); break;
  case 32: C3(SYS_socket, chance(90) ? 1 : gen_int(), chance(80) ? (1 | (gen_int() & (O_NONBLOCK | O_CLOEXEC))) : gen_int(), 0); break;
  case 33: { long a = gen_sockaddr(&len); C3(SYS_bind, gen_fd(), a, len); break; }
  case 34: C2(SYS_listen, gen_fd(), gen_int()); break;
  case 35: C1(SYS_accept, gen_fd()); break;
  case 36: { long a = gen_sockaddr(&len); C3(SYS_connect, gen_fd(), a, len); break; }
  case 37: { /* spawn: mostly harmless programs; sometimes another worker */
    long path = chance(95) ? str_ptr(spawn_paths[below(sizeof spawn_paths / sizeof *spawn_paths)]) : gen_ptr();
    long argv = gen_argv();
    if (chance(10)) { /* a short-lived nested worker */
      char seed[24], it[16], sp[16], spp[16];
      snprintf(seed, sizeof seed, "%u", rnd()); snprintf(it, sizeof it, "%u", 20 + below(100));
      snprintf(sp, sizeof sp, "%d", sup_pid); snprintf(spp, sizeof spp, "%d", sup_ppid);
      long *v = (long *)(scratch + 63000);
      v[0] = str_ptr("kfuzz"); v[1] = str_ptr("worker"); v[2] = str_ptr(seed); v[3] = str_ptr(it); v[4] = str_ptr(sp); v[5] = str_ptr(spp); v[6] = str_ptr("0"); v[7] = 0;
      path = str_ptr("/bin/kfuzz"); argv = (long)v;
      int *m = (int *)(scratch + 63500); m[0] = m[1] = m[2] = F_NULL;
      C4(SYS_spawn, path, argv, 0, m);
    } else C4(SYS_spawn, path, argv, chance(80) ? 0 : gen_argv(), gen_fdmap());
    break;
  }
  case 38: C2(SYS_getargs, gen_ptr(), gen_len()); break;
  case 39: C2(SYS_procinfo, chance(80) ? (long)below(70) : gen_int(), gen_ptr()); break;
  case 40: C0(SYS_intr); break;
  case 41: C4(SYS_utimensat, gen_fd(), chance(50) ? gen_mut_path() : gen_ro_path(), 0, 0); break;
  case 42: fd = gen_fd(); if (!is_input(fd)) C4(SYS_pread64, fd, gen_ptr(), gen_len(), chance(50) ? (long)below(100000) : gen_int()); break;
  case 43: C4(SYS_pwrite64, gen_fd(), gen_ptr(), gen_len(), chance(50) ? (long)below(100000) : fsize(gen_int())); break;
  case 44: { long cmd = gen_ioctl_cmd(); C3(SYS_ioctl, gen_fd(), cmd, gen_ioctl_arg(cmd)); break; }
  case 45: /* unimplemented or out-of-range numbers; and rarely exit */
    if (chance(3)) C1(SYS_exit, gen_int());
    else {
      long nr = chance(50) ? (long)below(600) : gen_int();
      /* since 2026-11-26 these numbers are real calls that change modes, owners or the root,
       * and gen_int() is often a descriptor of the image's /etc/motd: leave them to cases 46-60 */
      if (nr == SYS_fchmod || nr == SYS_fchmodat || nr == SYS_fchownat || nr == SYS_fchown || nr == SYS_fchmodat2 || nr == SYS_chroot || nr == SYS_fchdir) nr = 600;
      C4(nr, gen_int(), gen_int(), gen_int(), gen_int());
    }
    break;
  /* ---- file system calls added on 2026-11-26: links, modes, owners, locks, statx ---- */
  /* targets only from the list: a random pointer can land on an earlier string such as
   * "/etc/motd", and a chmod through the link would then change the image */
  case 46: C3(SYS_symlinkat, chance(97) ? str_ptr(link_targets[below(sizeof link_targets / sizeof *link_targets)]) : (long)(scratch + 63800), gen_mut_dirfd(), gen_mut_path()); break;
  case 47: { static const long fl[] = {0, AT_SYMLINK_FOLLOW, AT_EMPTY_PATH, 0x1400, 1}; C5(SYS_linkat, gen_mut_dirfd(), gen_mut_path(), gen_mut_dirfd(), gen_mut_path(), chance(90) ? fl[below(5)] : gen_int()); break; }
  case 48: C4(SYS_readlinkat, chance(50) ? gen_mut_dirfd() : gen_fd(), chance(50) ? gen_ro_path() : gen_mut_path(), chance(80) ? (long)scratch : gen_ptr(), chance(80) ? (long)below(300) : gen_len()); break;
  case 49: if (chance(50)) C3(SYS_fchmodat, gen_mut_dirfd(), gen_attr_path(), gen_int());
           else C4(SYS_fchmodat2, gen_mut_dirfd(), gen_attr_path(), gen_int(), chance(50) ? AT_SYMLINK_NOFOLLOW : gen_int()); break;
  case 50: C5(SYS_fchownat, gen_mut_dirfd(), gen_attr_path(), chance(50) ? -1 : gen_int(), chance(50) ? -1 : gen_int(), chance(70) ? (chance(50) ? AT_SYMLINK_NOFOLLOW : 0) : gen_int()); break;
  case 51: C5(SYS_renameat2, gen_mut_dirfd(), gen_mut_path(), gen_mut_dirfd(), gen_mut_path(), chance(80) ? (long)below(4) : gen_int()); break;
  case 52: C5(SYS_statx, chance(50) ? gen_mut_dirfd() : gen_fd(), chance(50) ? gen_ro_path() : gen_mut_path(), chance(80) ? (long)(below(2) * AT_SYMLINK_NOFOLLOW | below(2) * AT_EMPTY_PATH) : gen_int(), gen_int(), gen_ptr()); break;
  case 53: { static const long ty[] = {S_IFREG, S_IFIFO, S_IFSOCK, S_IFCHR, S_IFBLK, S_IFDIR, 0, S_IFLNK}; C4(SYS_mknodat, gen_mut_dirfd(), gen_mut_path(), ty[below(8)] | (gen_int() & 07777), gen_int()); break; }
  case 54: if (chance(50)) C3(SYS_faccessat, gen_mut_dirfd(), chance(50) ? gen_ro_path() : gen_mut_path(), chance(80) ? (long)below(8) : gen_int());
           else C4(SYS_faccessat2, gen_mut_dirfd(), chance(50) ? gen_ro_path() : gen_mut_path(), below(8), chance(80) ? (long)(below(2) * AT_EACCESS | below(2) * AT_SYMLINK_NOFOLLOW) : gen_int()); break;
  case 55: C2(SYS_flock, gen_fd(), chance(80) ? (long)(1 + below(8)) : gen_int()); break;
  case 56: { /* record locks: a valid struct flock most of the time */
    static const int cmds[] = {F_GETLK, F_SETLK, F_SETLKW, F_GETLK64, F_SETLK64, F_SETLKW64, F_OFD_GETLK, F_OFD_SETLK, F_OFD_SETLKW, F_DUPFD_CLOEXEC};
    long *fl = (long *)(scratch + 1000);
    if (chance(80)) { short *sh = (short *)fl; sh[0] = below(3); sh[1] = below(3); fl[1] = chance(50) ? (long)below(100) : gen_int(); fl[2] = chance(50) ? (long)below(100) - 20 : gen_int(); fl[3] = 0; }
    else fill_random(fl, 32);
    C3(SYS_fcntl, gen_fd(), chance(90) ? cmds[below(10)] : gen_int(), chance(90) ? (long)fl : gen_ptr());
    break;
  }
  case 57: if (chance(50)) C2(SYS_truncate, gen_attr_path(), chance(80) ? (long)below(70000) : fsize(gen_int()));
           else C4(SYS_fallocate, gen_fd(), chance(80) ? 0 : gen_int(), chance(80) ? (long)below(70000) : fsize(gen_int()), chance(80) ? (long)below(70000) : fsize(gen_int())); break;
  case 58: { /* vectored I/O: iovecs that point anywhere */
    long *iov = (long *)(scratch + 2000);
    int cnt = chance(90) ? (int)below(20) : gen_int();
    for (int k = 0; k < 20; k++) { iov[2 * k] = chance(80) ? (long)(scratch + 4096 + below(60000)) : gen_ptr(); iov[2 * k + 1] = chance(80) ? (long)below(1000) : gen_len(); }
    static const long nrs[] = {SYS_readv, SYS_writev, SYS_preadv, SYS_pwritev};
    long nr = nrs[below(4)];
    fd = gen_fd();
    if ((nr == SYS_readv || nr == SYS_preadv) && is_input(fd)) break;
    if (nr == SYS_readv) C3(SYS_readv, fd, chance(90) ? (long)iov : gen_ptr(), cnt);
    else if (nr == SYS_writev) C3(SYS_writev, fd, chance(90) ? (long)iov : gen_ptr(), cnt);
    else if (nr == SYS_preadv) C4(SYS_preadv, fd, chance(90) ? (long)iov : gen_ptr(), cnt, chance(80) ? (long)below(100000) : gen_int());
    else C4(SYS_pwritev, fd, chance(90) ? (long)iov : gen_ptr(), cnt, chance(80) ? (long)below(100000) : fsize(gen_int()));
    break;
  }
  case 59: switch (below(6)) {
    case 0: C1(SYS_fsync, gen_fd()); break;
    case 1: C1(SYS_fdatasync, gen_fd()); break;
    case 2: C1(SYS_syncfs, gen_fd()); break;
    case 3: C0(SYS_sync); break;
    case 4: C2(SYS_statfs, chance(50) ? gen_ro_path() : gen_mut_path(), gen_ptr()); break;
    default: C2(SYS_fstatfs, gen_fd(), gen_ptr()); break;
    }
    break;
  case 60: switch (below(9)) {
    case 0: C1(SYS_umask, gen_int()); break;
    case 1: C1(SYS_chroot, chance(90) ? str_ptr(cd_paths[below(sizeof cd_paths / sizeof *cd_paths)]) : gen_ptr()); break;
    case 2: C1(SYS_fchdir, chance(50) ? MYDIR : F_DIR); break; /* only KDIR: relative names must stay inside it */
    case 3: { static const long n1[] = {SYS_setuid, SYS_setgid, SYS_setfsuid, SYS_setfsgid}; C1(n1[below(4)], chance(50) ? 1000 : gen_int()); break; }
    case 4: C3(chance(50) ? SYS_setresuid : SYS_setresgid, chance(50) ? -1 : gen_int(), chance(50) ? 1000 : gen_int(), -1); break;
    case 5: C3(chance(50) ? SYS_getresuid : SYS_getresgid, gen_ptr(), gen_ptr(), gen_ptr()); break;
    case 6: C2(chance(50) ? SYS_getgroups : SYS_setgroups, chance(80) ? (long)below(20) : gen_int(), gen_ptr()); break;
    case 7: C4(SYS_utimensat, gen_mut_dirfd(), chance(90) ? gen_attr_path() : 0, chance(50) ? gen_timespec() : 0, chance(80) ? 0 : gen_int()); break;
    default: { /* only descriptors of KDIR and its files: a worker's own fds may name image files */
      static const int own[] = {MYDIR, MYFILE, MYDIR, MYFILE, -1, 999, AT_FDCWD, 64}; /* 3..5 can be dup3 targets */
      fd = own[below(8)];
      if (chance(50)) C2(SYS_fchmod, fd, gen_int()); else C3(SYS_fchown, fd, chance(50) ? -1 : gen_int(), gen_int()); break; }
    }
    break;
  }
}

static int worker(unsigned long long seed, long iters) {
  rs = mix(seed) | 1;
  int fd = open(KDIR, O_RDONLY | O_DIRECTORY);
  if (fd >= 0 && fd != MYDIR) { dup2(fd, MYDIR); close(fd); }
  fd = open(KDIR "/f", O_RDWR | O_CREAT, 0644);
  if (fd >= 0 && fd != MYFILE) { dup2(fd, MYFILE); close(fd); }
  if (chance(80)) __syscall0(SYS_setsid); /* own session: a hangup must not reach the supervisor */
  for (long i = 0; i < iters; i++) one_call();
  return 0;
}

/* ---- the supervisor ---- */
struct slot { int pid; long budget, used; unsigned gen; unsigned long long last_sys; double last_change; };
static struct slot slots[MAXW];
static int nworkers = 4;
static unsigned long long seed0;
static long respawns, stuck_kills, crashes;

static int start_worker(int w) {
  struct slot *s = &slots[w];
  char a1[24], a2[24], a3[16], a4[16], a5[4];
  int vb = verbose | big << 1;
  snprintf(a1, sizeof a1, "%llu", mix(seed0 ^ ((unsigned long long)w << 32) ^ s->gen));
  snprintf(a2, sizeof a2, "%ld", s->budget - s->used);
  snprintf(a3, sizeof a3, "%d", sup_pid);
  snprintf(a4, sizeof a4, "%d", sup_ppid);
  snprintf(a5, sizeof a5, "%d", vb);
  char *argv[] = {"kfuzz", "worker", a1, a2, a3, a4, a5, 0};
  int map[3] = {F_NULL, F_NULL, F_NULL};
  s->pid = spawn("/bin/kfuzz", argv, 0, map);
  s->last_sys = 0;
  s->last_change = now_ms();
  if (verbose) say("[kfuzz] worker %d generation %u is pid %d, seed %s, %s calls\n", w, s->gen, s->pid, a1, a2);
  return s->pid;
}
static unsigned long long syscalls_of(int pid) {
  struct procinfo pi;
  for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) if (pi.pid == pid) return pi.syscalls;
  return 0;
}
static void setup(void) {
  int fds[2];
  mkdir(KDIR, 0755);
  chdir(KDIR);
  mkdir(KDIR "/d", 0755);
#define AT(n, expr) do { int _f = (expr); if (_f >= 0 && _f != (n)) { dup2(_f, (n)); close(_f); } } while (0)
  AT(F_DIR, open(KDIR, O_RDONLY | O_DIRECTORY));
  AT(F_FILE, open(KDIR "/f", O_RDWR | O_CREAT, 0644));
  AT(F_APPEND, open(KDIR "/f", O_WRONLY | O_APPEND));
  AT(F_ROFILE, open("/etc/motd", O_RDONLY));
  pipe(fds); AT(F_PIPE_R, fds[0]); AT(F_PIPE_W, fds[1]);
  pipe(fds); AT(F_PIPE_R_NOWRITER, fds[0]); close(fds[1]);
  pipe(fds); AT(F_PIPE_W_NOREADER, fds[1]); close(fds[0]);
  char path[32];
  int ptm = open("/dev/ptmx", O_RDWR), n = -1;
  __syscall3(SYS_ioctl, ptm, TIOCGPTN, &n);
  snprintf(path, sizeof path, "/dev/pts/%d", n);
  AT(F_PTM, ptm); AT(F_PTS, open(path, O_RDWR));
  ptm = open("/dev/ptmx", O_RDWR); __syscall3(SYS_ioctl, ptm, TIOCGPTN, &n);
  snprintf(path, sizeof path, "/dev/pts/%d", n);
  AT(F_PTS_HUNG, open(path, O_RDWR)); close(ptm);
  ptm = open("/dev/ptmx", O_RDWR); __syscall3(SYS_ioctl, ptm, TIOCGPTN, &n);
  snprintf(path, sizeof path, "/dev/pts/%d", n);
  close(open(path, O_RDWR)); AT(F_PTM_HUNG, ptm);
  struct { unsigned short fam; char path[108]; } a = {1, KDIR "/l.sock"}, b = {1, KDIR "/b.sock"};
  AT(F_SOCK_NEW, __syscall3(SYS_socket, 1, 1, 0));
  int s = __syscall3(SYS_socket, 1, 1, 0); __syscall3(SYS_bind, s, &b, sizeof b); AT(F_SOCK_BOUND, s);
  s = __syscall3(SYS_socket, 1, 1 | O_NONBLOCK, 0); __syscall3(SYS_bind, s, &a, sizeof a); __syscall2(SYS_listen, s, 16); AT(F_SOCK_LISTEN, s);
  s = __syscall3(SYS_socket, 1, 1, 0); __syscall3(SYS_connect, s, &a, sizeof a); AT(F_SOCK_C, s);
  AT(F_SOCK_A, __syscall1(SYS_accept, F_SOCK_LISTEN));
  s = __syscall3(SYS_socket, 1, 1, 0); __syscall3(SYS_connect, s, &a, sizeof a); AT(F_SOCK_HUP, s);
  close(__syscall1(SYS_accept, F_SOCK_LISTEN));
  AT(F_FB, open("/dev/fb0", O_RDWR));
  AT(F_EVENT, open("/dev/input/event0", O_RDONLY | O_NONBLOCK));
  AT(F_NULL, open("/dev/null", O_RDWR));
  AT(F_ZERO, open("/dev/zero", O_RDWR));
  AT(F_RANDOM, open("/dev/urandom", O_RDONLY));
}
static void drain(void) { /* keep shared buffers moving without ever blocking ourselves */
  static const int rd[] = {F_PIPE_R, F_PTM, F_PTS, F_SOCK_C, F_SOCK_A};
  struct pollfd pf[5];
  for (int i = 0; i < 5; i++) { pf[i].fd = rd[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
  struct timespec zero = {0, 0};
  if (__syscall3(SYS_ppoll, pf, 5, &zero) <= 0) return;
  for (int i = 0; i < 5; i++) if (pf[i].revents & POLLIN) __syscall3(SYS_read, rd[i], scratch, sizeof scratch);
}
/* Remove a tree with chdir and relative names: renames can nest it deeper than PATH_MAX. */
static void remove_tree(const char *name, int depth) {
  static char names[16][32][260]; /* names may be 255 bytes since 2026-11-26 */
  if (depth < 16 && chdir(name) == 0) {
    for (int pass = 0; pass < 1000; pass++) { /* collect a batch, close, then remove it */
      DIR *d = opendir(".");
      if (!d) break;
      int n = 0;
      for (struct dirent *e; n < 32 && (e = readdir(d));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) snprintf(names[depth][n++], sizeof names[0][0], "%s", e->d_name);
      closedir(d);
      if (!n) break;
      int removed = 0;
      for (int i = 0; i < n; i++) {
        if (unlink(names[depth][i]) == 0) { removed++; continue; }
        remove_tree(names[depth][i], depth + 1);
        removed += access(names[depth][i], F_OK) != 0;
      }
      if (!removed) break;
    }
    chdir("..");
  }
  rmdir(name);
}
/* names in / and /tmp before the run, to report anything the workers left elsewhere */
static char before[2][64][260];
static int nbefore[2], nleft;
static void snapshot(int which, const char *dir, int report) {
  DIR *d = opendir(dir);
  if (!d) return;
  for (struct dirent *e; (e = readdir(d));) {
    if (!report) { if (nbefore[which] < 64) snprintf(before[which][nbefore[which]++], 72, "%s", e->d_name); continue; }
    int seen = 0;
    for (int i = 0; i < nbefore[which]; i++) seen |= !strcmp(before[which][i], e->d_name);
    if (!seen) { say("[kfuzz] left behind: %s/%s\n", which ? "/tmp" : "", e->d_name); nleft++; }
  }
  closedir(d);
}
static void kill_everyone(void) {
  for (int round = 0; round < 50; round++) {
    struct procinfo pi;
    int others = 0, st;
    for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) {
      if (pi.pid == getpid() || pi.pid == sup_ppid || (sup_pid != 1 && pi.pid == 1)) continue;
      others++;
      if (pi.state != 2) __syscall2(SYS_kill, pi.pid, 9);
    }
    while (waitpid(-1, &st, WNOHANG) > 0) {}
    if (!others) return;
    usleep(2000);
  }
}

static int supervisor(long iters, double tlimit, int run_ktest) {
  memset(slots, 0, sizeof slots);
  respawns = stuck_kills = crashes = 0;
  nbefore[0] = nbefore[1] = nleft = 0;
  int m[16]; /* the first use of each pty number creates /dev/pts/N for good: do that before measuring */
  for (int i = 0; i < 16; i++) m[i] = open("/dev/ptmx", O_RDWR);
  for (int i = 0; i < 16; i++) close(m[i]);
  snapshot(0, "/", 0);
  snapshot(1, "/tmp", 0);
  long heap0 = heap_used();
  double t0 = now_ms();
  __syscall0(SYS_setsid);
  setup();
  long heap1 = heap_used();
  say("[kfuzz] seed %llu, %ld calls per worker, %d workers, heap %ld bytes before, %ld after setup\n", seed0, iters, nworkers, heap0, heap1);
  for (int w = 0; w < nworkers; w++) { slots[w].budget = iters; start_worker(w); }
  double last_report = t0;
  for (;;) {
    int live = 0, st;
    long done = 0, total = 0;
    int pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
      for (int w = 0; w < nworkers; w++) {
        struct slot *s = &slots[w];
        if (s->pid != pid) continue;
        s->used += s->last_sys ? (long)s->last_sys : 1;
        if (WIFSIGNALED(st) && WTERMSIG(st) == 11) crashes++;
        s->pid = 0;
        if (s->used < s->budget && s->gen < 100000 && (tlimit <= 0 || now_ms() - t0 < tlimit)) { s->gen++; respawns++; start_worker(w); }
      }
    }
    double now = now_ms();
    for (int w = 0; w < nworkers; w++) {
      struct slot *s = &slots[w];
      total += s->budget;
      done += s->used < s->budget ? s->used : s->budget;
      if (s->pid <= 0) continue;
      live++;
      unsigned long long c = syscalls_of(s->pid);
      if (c != s->last_sys) { s->last_sys = c; s->last_change = now; }
      else if (now - s->last_change > 400) { __syscall2(SYS_kill, s->pid, 9); stuck_kills++; s->last_change = now; }
      if (s->budget - s->used <= (long)c) __syscall2(SYS_kill, s->pid, 9); /* over budget (counts include libc's own calls) */
    }
    if (!live) break;
    if (tlimit > 0 && now - t0 > tlimit) { for (int w = 0; w < nworkers; w++) if (slots[w].pid > 0) __syscall2(SYS_kill, slots[w].pid, 9); }
    if (now - last_report > 5000) {
      say("[kfuzz] %.0f s: %ld of %ld calls, %ld respawns, heap %ld\n", (now - t0) / 1000, done, total, respawns, heap_used());
      static long last_done = -1;
      if (done == last_done) { /* no progress for 5 s: say what every process is doing */
        struct procinfo pi;
        for (int i = 0; __syscall2(SYS_procinfo, i, &pi) == 0; i++) say("[kfuzz]   pid %d ppid %d state %d calls %llu %s\n", pi.pid, pi.ppid, pi.state, pi.syscalls, pi.name);
        for (int w = 0; w < nworkers; w++) say("[kfuzz]   slot %d: pid %d used %ld of %ld\n", w, slots[w].pid, slots[w].used, slots[w].budget);
      }
      last_done = done;
      last_report = now;
    }
    drain();
    usleep(20000);
  }
  kill_everyone();
  for (int fd = 3; fd < 64; fd++) if (fd != LOGFD) close(fd);
  chdir("/tmp");
  remove_tree("kfuzz", 0);
  chdir("/");
  long heap2 = heap_used();
  snapshot(0, "/", 1);
  snapshot(1, "/tmp", 1);
  long calls = 0;
  for (int w = 0; w < nworkers; w++) calls += slots[w].used;
  say("[kfuzz] done in %.1f s: about %ld calls, %ld respawns, %ld stuck workers killed, %ld worker crashes\n", (now_ms() - t0) / 1000, calls, respawns, stuck_kills, crashes);
  say("[kfuzz] kernel heap: %ld bytes before, %ld after (difference %ld)\n", heap0, heap2, heap2 - heap0);
  int kst = 0, kpid = -1;
  if (run_ktest) {
    char *argv[] = {"ktest", 0};
    char *envp[] = {"HOME=/home/user", "PATH=/bin", 0};
    int map[3] = {LOGFD, LOGFD, LOGFD};
    kpid = spawn("/bin/ktest", argv, envp, map);
    if (kpid < 0 || waitpid(kpid, &kst, 0) != kpid) kst = -1;
    say("[kfuzz] ktest after the run: %s %d\n", kst == -1 ? "could not run, status" : "exit status", kst == -1 ? -1 : (WIFSIGNALED(kst) ? 128 + WTERMSIG(kst) : WEXITSTATUS(kst)));
  }
  int ok = (!run_ktest || kst == 0) && heap2 == heap0 && !nleft;
  say("kfuzz: %s seed %llu, heap difference %ld, %d names left behind, ktest %s\n", ok ? "PASS" : "FAIL", seed0, heap2 - heap0, nleft, run_ktest ? (kst == 0 ? "ok" : "FAILED") : "skipped");
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc >= 7 && !strcmp(argv[1], "worker")) {
    sup_pid = atoi(argv[4]); sup_ppid = atoi(argv[5]); verbose = atoi(argv[6]) & 1; big = atoi(argv[6]) >> 1 & 1;
    return worker(parse_u64(argv[2]), atol(argv[3]));
  }
  int top = getpid() == 1 || (argc > 2 && !strcmp(argv[argc - 1], "--top"));
  if (top && argc >= 2 && !strcmp(argv[1], "regress")) { /* every kept seed in one boot, then ktest once */
    dup2(2, LOGFD);
    sup_pid = getpid();
    sup_ppid = getppid();
    int n = sizeof regression / sizeof *regression, bad = 0;
    for (int i = 0; i < n; i++) {
      seed0 = regression[i].seed;
      nworkers = regression[i].workers;
      bad |= supervisor(regression[i].iters, 0, i == n - 1);
    }
    say("kfuzz regress: %d seeds, %s\n", n, bad ? "FAIL" : "PASS");
    return bad;
  }
  if (argc < 3 || !top) {
    fprintf(stderr, "usage: kfuzz SEED ITERATIONS [v] [p=N] [t=SECONDS] [noktest] [big]   (as init, or add --top last)\n"
                    "       kfuzz regress   (the seeds that found bugs, then ktest)\n");
    return 2;
  }
  dup2(2, LOGFD);
  seed0 = parse_u64(argv[1]);
  long iters = atol(argv[2]);
  double tlimit = 0;
  int run_ktest = 1;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "v")) verbose = 1;
    else if (!strncmp(argv[i], "p=", 2)) { nworkers = atoi(argv[i] + 2); if (nworkers < 1) nworkers = 1; if (nworkers > MAXW) nworkers = MAXW; }
    else if (!strncmp(argv[i], "t=", 2)) tlimit = atof(argv[i] + 2) * 1000;
    else if (!strcmp(argv[i], "noktest")) run_ktest = 0;
    else if (!strcmp(argv[i], "big")) big = 1;
  }
  sup_pid = getpid();
  sup_ppid = getppid();
  return supervisor(iters, tlimit, run_ktest);
}
