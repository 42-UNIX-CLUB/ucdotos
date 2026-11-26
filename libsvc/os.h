// SPDX-License-Identifier: GPL-2.0-or-later
/* The few things the service manager and its tools need that differ between UC.OS and
 * a POSIX host: sockets (the UC.OS libc has no socket wrappers; programs use the raw
 * calls, as libui does), symlinks (not in the kernel on main: detected at run time by
 * -ENOSYS, so no weak symbols are needed), clocks and starting a process. */
#ifndef SVC_OS_H
#define SVC_OS_H
#include <stdint.h>
int svc_listen_unix(const char *path, int nonblock);    /* fd or -errno; unlinks a stale path first */
int svc_connect_unix(const char *path);                 /* fd or -errno */
int svc_accept(int lfd, int nonblock);                  /* fd or -errno; close-on-exec */
int svc_set_cloexec(int fd, int on);
int svc_set_nonblock(int fd, int on);
int svc_symlink(const char *target, const char *path);  /* 0 or -errno (-ENOSYS: no symlinks) */
int svc_readlink_raw(const char *path, char *buf, int len); /* length, or -errno */
int svc_is_symlink(const char *path);
int64_t svc_now_ms(void);                               /* monotonic */
int64_t svc_epoch_ms(void);                             /* wall clock, UTC */
/* Start a program. fdmap[i] (i = 0..2) is the caller's descriptor that becomes the
 * child's i. extra/nextra: descriptors that must appear in the child as 3, 4, ...
 * (LISTEN_FDS); on UC.OS the caller must have parked placeholders on those numbers
 * with svc_reserve_fds first. pgid: 0 = a new process group led by the child, > 0 =
 * join that group, < 0 = stay in the caller's. */
int svc_spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3],
              const int *extra, int nextra, int pgid);
void svc_reserve_fds(int n); /* park /dev/null (close-on-exec) on descriptors 3 .. 3+n-1 */
/* Signals delivered to handlers (the fork work), or only termination (main)? */
int svc_have_signals(void);
/* Every process: pid, ppid, process group (-1 unknown), memory pages, name. */
struct svc_pinfo { int pid, ppid, pgid, state; uint32_t pages; char name[32]; };
int svc_procs(struct svc_pinfo *out, int max);
#endif
