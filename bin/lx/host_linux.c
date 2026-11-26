// SPDX-License-Identifier: GPL-2.0-or-later
/* Host back end for native builds: the development machine's Linux (x86-64). With it
 * the engine is a user-mode emulator on the host, which is how most tests run.
 * Not compiled into the UC.OS build. */
#ifndef __wasm__
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "host.h"

volatile int hb_pending_signal;

/* generic (asm-generic) number -> x86-64 number, for the calls hb_raw passes through */
static int xnr(int nr) {
  switch (nr) {
  case 17: return SYS_getcwd;   case 23: return SYS_dup;      case 24: return SYS_dup3;
  case 25: return SYS_fcntl;    case 29: return SYS_ioctl;    case 32: return SYS_flock;
  case 33: return SYS_mknodat;  case 34: return SYS_mkdirat;  case 35: return SYS_unlinkat;
  case 36: return SYS_symlinkat; case 37: return SYS_linkat;  case 38: return SYS_renameat;
  case 46: return SYS_ftruncate; case 48: return SYS_faccessat; case 49: return SYS_chdir;
  case 50: return SYS_fchdir;   case 52: return SYS_fchmod;   case 53: return SYS_fchmodat;
  case 54: return SYS_fchownat; case 55: return SYS_fchown;   case 56: return SYS_openat;
  case 57: return SYS_close;    case 59: return SYS_pipe2;    case 61: return SYS_getdents64;
  case 62: return SYS_lseek;    case 63: return SYS_read;     case 64: return SYS_write;
  case 67: return SYS_pread64;  case 68: return SYS_pwrite64; case 78: return SYS_readlinkat;
  case 82: return SYS_fsync;    case 83: return SYS_fdatasync; case 124: return SYS_sched_yield;
  case 129: return SYS_kill;    case 154: return SYS_setpgid; case 155: return SYS_getpgid;
  case 156: return SYS_getsid;  case 157: return SYS_setsid;  case 160: return SYS_uname;
  case 172: return SYS_getpid;  case 173: return SYS_getppid; case 174: return SYS_getuid;
  case 198: return SYS_socket;  case 200: return SYS_bind;    case 201: return SYS_listen;
  case 202: return SYS_accept;  case 203: return SYS_connect; case 278: return SYS_getrandom;
  }
  return -1;
}

int64_t hb_raw(int nr, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
  int x = xnr(nr);
  if (x < 0) return -ENOSYS;
  long r = syscall(x, a, b, c, d, e, f);
  return r < 0 ? -errno : r;
}

int64_t hb_fstatat(int dirfd, const char *path, struct lx_stat *st, int flags) {
  struct stat s;
  int r = path ? fstatat(dirfd, path, &s, flags & AT_SYMLINK_NOFOLLOW) : fstat(dirfd, &s);
  if (r < 0) return -errno;
  st->dev = s.st_dev; st->ino = s.st_ino; st->rdev = s.st_rdev; st->nlink = s.st_nlink;
  st->mode = s.st_mode; st->uid = s.st_uid; st->gid = s.st_gid;
  st->size = s.st_size; st->blksize = s.st_blksize; st->blocks = s.st_blocks;
  st->atime = s.st_atim.tv_sec; st->atime_ns = s.st_atim.tv_nsec;
  st->mtime = s.st_mtim.tv_sec; st->mtime_ns = s.st_mtim.tv_nsec;
  st->ctime = s.st_ctim.tv_sec; st->ctime_ns = s.st_ctim.tv_nsec;
  return 0;
}

int64_t hb_clock_gettime(int clk, int64_t *sec, int64_t *nsec) {
  struct timespec t;
  if (clock_gettime(clk, &t) < 0) return -errno;
  *sec = t.tv_sec; *nsec = t.tv_nsec;
  return 0;
}
int64_t hb_nanosleep(int64_t sec, int64_t nsec) {
  struct timespec t = {sec, nsec};
  return nanosleep(&t, 0) < 0 ? -errno : 0;
}
int64_t hb_ppoll(void *fds, int n, int64_t ns) {
  struct timespec t, *tp = 0;
  if (ns >= 0) { t.tv_sec = ns / 1000000000; t.tv_nsec = ns % 1000000000; tp = &t; }
  int r = ppoll(fds, (nfds_t)n, tp, 0);
  return r < 0 ? -errno : r;
}
int64_t hb_utimensat(int dirfd, const char *path, const int64_t *ts, int flags) {
  struct timespec t[2];
  if (ts) { t[0].tv_sec = ts[0]; t[0].tv_nsec = ts[1]; t[1].tv_sec = ts[2]; t[1].tv_nsec = ts[3]; }
  long r = syscall(SYS_utimensat, dirfd, path, ts ? t : 0, flags);
  return r < 0 ? -errno : 0;
}
int64_t hb_sysinfo(struct lx_sysinfo *si) {
  struct sysinfo s;
  if (sysinfo(&s) < 0) return -errno;
  si->uptime = s.uptime;
  si->totalram = (uint64_t)s.totalram * s.mem_unit;
  si->freeram = (uint64_t)s.freeram * s.mem_unit;
  si->bufferram = (uint64_t)s.bufferram * s.mem_unit;
  si->procs = s.procs;
  return 0;
}
int64_t hb_wait4(int pid, int *status, int options) {
  int r = wait4(pid, status, options, 0);
  return r < 0 ? -errno : r;
}
int64_t hb_lseek(int fd, int64_t off, int whence) {
  off_t r = lseek(fd, off, whence);
  return r < 0 ? -errno : r;
}

int64_t hb_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3]) {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  for (int i = 0; i < 3; i++) if (fdmap[i] >= 0) posix_spawn_file_actions_adddup2(&fa, fdmap[i], i);
  pid_t pid;
  int r = posix_spawn(&pid, path, &fa, 0, argv, envp);
  posix_spawn_file_actions_destroy(&fa);
  return r ? -r : pid;
}
int64_t hb_fork(void) {
  pid_t p = fork();
  return p < 0 ? -errno : p;
}
int64_t hb_execve(const char *path, char *const argv[], char *const envp[]) {
  execve(path, argv, envp);
  return -errno;
}
void hb_exit(int code) { _exit(code); }
void hb_die(int sig) {
  struct rlimit rl = {0, 0};
  setrlimit(RLIMIT_CORE, &rl);
  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0); /* the guest died, not the engine: no host core dump */
  signal(sig, SIG_DFL);
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, sig);
  sigprocmask(SIG_UNBLOCK, &s, 0);
  raise(sig);
  _exit(128 + sig);
}

uint8_t *hb_window(uint64_t size) {
  void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}
int hb_release(uint8_t *p, uint64_t len) {
  return madvise(p, len, MADV_DONTNEED) == 0;
}
void hb_tick(void) {}

int hb_is_native_program(const unsigned char *head, int n) {
  return n >= 20 && !memcmp(head, "\177ELF", 4) && (head[18] | head[19] << 8) == 62; /* x86-64 */
}

static void on_signal(int sig) { if (sig < 32) hb_pending_signal = (int)((unsigned)hb_pending_signal | (1u << sig)); }

/* Called once at start: forward the host's asynchronous signals to the guest. No
 * SA_RESTART, so a blocking host call returns EINTR and the guest's own SA_RESTART
 * decides (sys.c). The engine handles EPIPE itself, so SIGPIPE is ignored here. */
void hb_init(void) {
  static const int fwd[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGALRM, SIGCHLD, SIGWINCH, SIGCONT};
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  for (unsigned i = 0; i < sizeof fwd / sizeof fwd[0]; i++) sigaction(fwd[i], &sa, 0);
  signal(SIGPIPE, SIG_IGN);
}
#endif
