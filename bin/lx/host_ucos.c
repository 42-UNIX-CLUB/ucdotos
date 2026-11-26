// SPDX-License-Identifier: GPL-2.0-or-later
/* Host back end for UC.OS: the engine is an ordinary UC.OS program and talks to the
 * kernel with raw system calls. UC.OS uses Linux's generic numbers, the same table as a
 * riscv64 guest, so most calls go straight through; structures are UC.OS's (wasm32). */
#ifdef __wasm__
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "host.h"

volatile int hb_pending_signal;

int64_t hb_raw(int nr, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
  if (nr == 46 /* ftruncate */ && (b < 0 || b > 0x7fffffff)) return b < 0 ? -EINVAL : -27;
  return __syscall6(nr, (long)a, (long)b, (long)c, (long)d, (long)e, (long)f);
}

int64_t hb_fstatat(int dirfd, const char *path, struct lx_stat *st, int flags) {
  /* AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH have Linux's values in UC.OS too: pass them on
   * (the path module finds symbolic links with NOFOLLOW and resolves them in the guest root) */
  struct stat s;
  memset(&s, 0, sizeof s);
  long r = path ? __syscall4(SYS_newfstatat, dirfd, path, &s, flags) : __syscall2(SYS_fstat, dirfd, &s);
  if (r < 0) return r;
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
  long r = __syscall2(SYS_clock_gettime, clk, &t);
  if (r < 0) return r;
  *sec = t.tv_sec; *nsec = t.tv_nsec;
  return 0;
}
int64_t hb_nanosleep(int64_t sec, int64_t nsec) {
  struct timespec t;
  t.tv_sec = sec; t.tv_nsec = (long)nsec;
  return __syscall1(SYS_nanosleep, &t);
}
int64_t hb_ppoll(void *fds, int n, int64_t ns) {
  struct timespec t, *tp = 0;
  if (ns >= 0) { t.tv_sec = ns / 1000000000; t.tv_nsec = (long)(ns % 1000000000); tp = &t; }
  return __syscall3(SYS_ppoll, fds, n, tp);
}
int64_t hb_utimensat(int dirfd, const char *path, const int64_t *ts, int flags) {
  /* ts: atime sec, nsec, mtime sec, nsec (UTIME_NOW / UTIME_OMIT have Linux's values), or NULL */
  struct timespec t[2];
  if (!ts) return __syscall4(SYS_utimensat, dirfd, path, 0, flags);
  t[0].tv_sec = ts[0]; t[0].tv_nsec = (long)ts[1]; t[1].tv_sec = ts[2]; t[1].tv_nsec = (long)ts[3];
  return __syscall4(SYS_utimensat, dirfd, path, t, flags);
}
int64_t hb_sysinfo(struct lx_sysinfo *si) {
  struct sysinfo s;
  long r = __syscall1(SYS_sysinfo, &s);
  if (r < 0) return r;
  si->uptime = s.uptime;
  si->totalram = (uint64_t)s.totalram * (s.mem_unit ? s.mem_unit : 1);
  si->freeram = (uint64_t)s.freeram * (s.mem_unit ? s.mem_unit : 1);
  si->bufferram = (uint64_t)s.bufferram * (s.mem_unit ? s.mem_unit : 1);
  si->procs = s.procs;
  return 0;
}
int64_t hb_wait4(int pid, int *status, int options) { return __syscall4(SYS_wait4, pid, status, options & 1, 0); }
int64_t hb_lseek(int fd, int64_t off, int whence) {
  if (off > 0x7fffffff || off < -0x7fffffffLL) return -EINVAL;
  return __syscall3(SYS_lseek, fd, (long)off, whence);
}

int64_t hb_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3]) {
  return __syscall4(SYS_spawn, path, argv, envp, fdmap);
}
/* The kernel's clone primitive (SYS_pclone 520; numbered 504 in the fork stream before integration).
 * Build with -DLX_HAVE_PCLONE once it is: the child starts at __fork_child (sys_proc.c). */
int64_t hb_fork(void) {
#ifdef LX_HAVE_PCLONE
  return __syscall1(520 /* SYS_pclone */, 0);
#else
  return -ENOSYS;
#endif
}
int64_t hb_execve(const char *path, char *const argv[], char *const envp[]) {
  (void)path; (void)argv; (void)envp;
  return -ENOSYS;
}
void hb_exit(int code) { _exit(code); }
void hb_die(int sig) {
  /* kill() terminates the target with status = sig: exactly "killed by signal sig" */
  __syscall2(SYS_kill, __syscall0(SYS_getpid), sig);
  _exit(128 + sig);
}

/* The guest window: grown at the end of linear memory, once. memory.grow hands back
 * zeroed pages; the browser commits them only when touched. */
uint8_t *hb_window(uint64_t size) {
  if (size > 0xc0000000ull) return 0;
  unsigned pages = (unsigned)(size >> 16);
  long old = __builtin_wasm_memory_grow(0, pages);
  if (old < 0) return 0;
  return (uint8_t *)(size_t)((unsigned long)old << 16);
}
int hb_release(uint8_t *p, uint64_t len) { (void)p; (void)len; return 0; }

/* getppid is the cheapest kernel call (about 12 ns warm). Any call lets the kernel's time
 * slice check run (every 256th call) and a killed process die. */
void hb_tick(void) { __syscall0(SYS_getppid); }

int hb_is_native_program(const unsigned char *head, int n) {
  return n >= 4 && head[0] == 0 && head[1] == 'a' && head[2] == 's' && head[3] == 'm';
}
void hb_init(void) {}
#endif
