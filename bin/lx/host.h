// SPDX-License-Identifier: GPL-2.0-or-later
/* The lowest level of the engine: what it asks of the system it runs on. Two
 * implementations: host_ucos.c (UC.OS kernel calls) and host_linux.c (the host's Linux,
 * which makes a native build a user-mode emulator on the development machine).
 *
 * hb_raw takes Linux generic system call numbers (asm-generic, the riscv64 table, which
 * is also UC.OS's table) and host-side arguments: pointers are host pointers. It is used
 * only for calls whose structures have the same layout on every side (or none). Calls
 * whose layouts differ go through the typed functions below, which use the engine's own
 * neutral structures. Results are a value or -errno (Linux numbers). */
#ifndef LX_HOST_H
#define LX_HOST_H
#include <stdint.h>

int64_t hb_raw(int nr, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f);
#define hb0(n) hb_raw(n, 0, 0, 0, 0, 0, 0)
#define hb1(n, a) hb_raw(n, (int64_t)(a), 0, 0, 0, 0, 0)
#define hb2(n, a, b) hb_raw(n, (int64_t)(a), (int64_t)(b), 0, 0, 0, 0)
#define hb3(n, a, b, c) hb_raw(n, (int64_t)(a), (int64_t)(b), (int64_t)(c), 0, 0, 0)
#define hb4(n, a, b, c, d) hb_raw(n, (int64_t)(a), (int64_t)(b), (int64_t)(c), (int64_t)(d), 0, 0)
#define hbp(p) ((int64_t)(intptr_t)(p))

struct lx_stat {
  uint64_t dev, ino, rdev, nlink;
  uint32_t mode, uid, gid;
  int64_t size, blksize, blocks;
  int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
};
/* path == 0: fstat(dirfd). flags: AT_SYMLINK_NOFOLLOW (0x100), AT_EMPTY_PATH (0x1000). */
int64_t hb_fstatat(int dirfd, const char *path, struct lx_stat *st, int flags);
int64_t hb_clock_gettime(int clk, int64_t *sec, int64_t *nsec);
int64_t hb_nanosleep(int64_t sec, int64_t nsec);
/* timeout_ns < 0: wait forever. struct pollfd has the same layout everywhere. */
int64_t hb_ppoll(void *fds, int n, int64_t timeout_ns);
/* ts: 4 values (atime sec, nsec, mtime sec, nsec) or 0 for now. UTIME_NOW/UTIME_OMIT pass through. */
int64_t hb_utimensat(int dirfd, const char *path, const int64_t *ts, int flags);
struct lx_sysinfo { int64_t uptime; uint64_t totalram, freeram, bufferram; uint32_t procs; };
int64_t hb_sysinfo(struct lx_sysinfo *si);
int64_t hb_wait4(int pid, int *status, int options);
int64_t hb_lseek(int fd, int64_t off, int whence);

/* Process creation. hb_spawn: start `path` (a host program) with argv/envp; fdmap[3]
 * names the descriptors that become the child's 0, 1, 2 (-1 = inherit). hb_fork: the
 * host's fork, or -ENOSYS where there is none (UC.OS today). */
int64_t hb_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3]);
int64_t hb_fork(void);
/* Replace the engine process itself (native only; -ENOSYS on UC.OS). */
int64_t hb_execve(const char *path, char *const argv[], char *const envp[]);
__attribute__((noreturn)) void hb_exit(int code);
/* End this process as killed by signal sig (exit status = sig, as wait() reports it). */
__attribute__((noreturn)) void hb_die(int sig);

/* The guest window: size bytes, zero filled, as one contiguous block. */
uint8_t *hb_window(uint64_t size);
/* Tell the host that [p, p+len) is free again and may read back as zero (or not). Returns
 * 1 if the range now reads as zero, 0 if the caller must clear it before reuse. */
int hb_release(uint8_t *p, uint64_t len);

/* A call cheap enough to make every few milliseconds, so the UC.OS kernel can take the
 * CPU back (time slice) and kill the process (Ctrl-C). No-op natively. */
void hb_tick(void);
/* One-time set-up (natively: forward host signals). */
void hb_init(void);
/* 1 if the host wants the guest interrupted (natively: a host signal arrived). */
extern volatile int hb_pending_signal;

/* Is this a host-native program the engine cannot run (UC.OS wasm, host ELF)? */
int hb_is_native_program(const unsigned char *head, int n);

#endif
