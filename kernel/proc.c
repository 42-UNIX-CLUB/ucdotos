// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel.h"

struct proc procs[NPROC];
struct proc *cur;

/* ---- user memory access, through the process's multi-memory shim ---- */
int user_ok(u32 addr, u32 len) {
  u64 end = (u64)addr + len;
  return addr != 0 && end <= ((u64)cur->mem_pages() << 16);
}
int copy_from_user(void *kdst, u32 usrc, u32 len) {
  if (!len) return 0;
  if (!user_ok(usrc, len)) return -EFAULT;
  cur->copy_in((u32)(usize)kdst, usrc, len);
  return 0;
}
int copy_to_user(u32 udst, const void *ksrc, u32 len) {
  if (!len) return 0;
  if (!user_ok(udst, len)) return -EFAULT;
  cur->copy_out(udst, (u32)(usize)ksrc, len);
  return 0;
}
int str_from_user(char *kdst, u32 usrc, u32 cap) {
  if (!usrc) return -EFAULT;
  u64 limit = (u64)cur->mem_pages() << 16;
  if (usrc >= limit) return -EFAULT;
  u32 n = cap;
  if (n > limit - usrc) n = limit - usrc;
  cur->copy_in((u32)(usize)kdst, usrc, n);
  for (u32 i = 0; i < n; i++) if (!kdst[i]) return i;
  return n < cap ? -EFAULT : -ENAMETOOLONG; /* ran off the end of memory, or too long */
}

/* ---- wait queues and blocking ---- */
void wq_add(waitq *q) { *q |= 1ull << cur->pid; }
void wq_wake(waitq *q) {
  u64 m = *q;
  *q = 0;
  for (int pid = 0; m; pid++, m >>= 1) if (m & 1) host_wake(pid);
}
long block_raw(double timeout_ms) {
  cur->block_ms = timeout_ms;
  cur->sleeping = 1;
  cur->slice_start = 0;
  return -ERESTARTSYS;
}
/* A wait is cut short by a timer (alarm) and interrupted by a signal with a handler. */
static long block_sig(double timeout_ms, int norestart) {
  long r = sig_interrupt(norestart);
  if (r) return r;
  if (cur->alarm_at) {
    double left = cur->alarm_at - host_now();
    if (left < 0) left = 0;
    if (timeout_ms < 0 || left < timeout_ms) timeout_ms = left;
  }
  return block_raw(timeout_ms);
}
long block(double timeout_ms) { return block_sig(timeout_ms, 0); }
long block_nr(double timeout_ms) { return block_sig(timeout_ms, 1); }

/* ---- files ---- */
struct file *file_new(const struct fops *ops, struct inode *ino, int flags, void *priv) {
  struct file *f = kzalloc(sizeof *f);
  if (!f) return NULL;
  f->refs = 1; f->ops = ops; f->ino = ino; f->flags = flags; f->priv = priv;
  if (ino) inode_get(ino);
  return f;
}
void file_put(struct file *f) {
  if (--f->refs) return;
  if (f->ops->close) f->ops->close(f);
  if (f->ino) inode_put(f->ino);
  kfree(f);
}
int fd_alloc(struct proc *p, struct file *f, int from) {
  /* A process killed in the middle of its own call (it typed Ctrl-C at its own terminal,
   * or hung it up) returns from that call as a zombie. It must not gain descriptors that
   * nothing would close. */
  if (p->state != P_RUN) return -EBADF;
  if (from < 0) return -EINVAL;
  for (int i = from; i < NFD; i++) if (!p->fd[i]) { p->fd[i] = f; p->cloexec[i] = 0; return i; }
  return -EMFILE;
}
struct file *fd_get(int fd) { return (fd >= 0 && fd < NFD) ? cur->fd[fd] : NULL; }

/* ---- lifecycle ---- */
/* Is this number still the id of a process group or session? Then it is not handed out
 * as a new pid, as on Linux: a new process must not land in a group it never joined. */
static int id_in_use(int id) {
  for (int i = 1; i < NPROC; i++)
    if (procs[i].state != P_FREE && (procs[i].pgid == id || procs[i].sid == id)) return 1;
  return 0;
}
int pid_alloc(void) { /* a free slot, 0 if the table is full */
  int pid = 0;
  for (int i = 1; i < NPROC; i++) if (procs[i].state == P_FREE) { if (!pid) pid = i; if (!id_in_use(i)) { pid = i; break; } }
  return pid;
}
int proc_spawn(struct proc *parent, const char *path, u8 *args, u32 args_len, const int *fdmap) {
  if (parent && parent->state != P_RUN) return -ESRCH;
  int lerr;
  struct inode *exe = vfs_lookup_err(parent ? parent->cwd : vfs_root, path, &lerr);
  if (!exe) return lerr;
  if ((exe->mode & S_IFMT) != S_IFREG) return -EACCES;
  { int e = vfs_may_exec(exe); if (e) return e; } /* file system: no execute bit, or contents still loading (kernel/vfs.c) */
  if (exe->size < 8 || memcmp(exe->data, "\0asm", 4)) return -ENOEXEC;
  int pid = pid_alloc();
  if (!pid) return -EAGAIN;
  struct proc *p = &procs[pid];
  memset(p, 0, sizeof *p);
  p->pid = pid;
  p->state = P_RUN;
  p->ppid = parent ? parent->pid : 0;
  p->cwd = parent ? parent->cwd : vfs_root;
  inode_get(p->cwd);
  vfs_proc_start(pid, parent ? parent->pid : 0, exe); /* file system: credentials, umask, chroot */
  vfs_exec_ids(pid, exe); /* set-user-ID programs, saved ids */
  p->pgid = parent ? parent->pgid : pid;
  p->sid = parent ? parent->sid : pid;
  p->args = args; p->args_len = args_len;
  p->block_ms = -1;
  const char *base = strrchr(path, '/');
  strlcpy(p->name, base ? base + 1 : path, sizeof p->name);
  if (parent) {
    for (int i = 0; i < NFD; i++) {
      int src = (fdmap && i < 3 && fdmap[i] >= 0) ? fdmap[i] : i;
      struct file *f = (src >= 0 && src < NFD) ? parent->fd[src] : NULL;
      if (!f || (parent->cloexec[src] && !(fdmap && i < 3 && fdmap[i] >= 0))) continue;
      f->refs++;
      p->fd[i] = f;
    }
    /* spawn is fork + exec: ignored signals and the mask are inherited, handlers are not */
    sig_fork_copy(p, parent);
    sig_exec_reset(p);
  }
  host_spawn(pid, exe->data, exe->size, !exe->owned);
  return pid;
}
static void proc_free(struct proc *p) {
  host_reap(p->pid);
  if (p->args) kfree(p->args);
  p->args = NULL;
  if (p->cwd) inode_put(p->cwd);
  vfs_proc_end(p->pid); /* file system: drop its chroot, exe and /proc entries */
  /* A process killed inside its own call with no parent to wait for it is freed at once
   * but runs until it next blocks: give it a directory that is never freed, not NULL. */
  p->cwd = vfs_root;
  p->state = P_FREE;
  int g = p->pgid;
  for (int i = 1; i < NPROC; i++) if (procs[i].state != P_FREE && procs[i].pgid == g) return;
  tty_pgrp_gone(g);
}
void proc_exit(struct proc *p, int code) {
  if (p->state != P_RUN) return;
  /* Mark it dead first. Closing a terminal kills other processes, and that chain must
   * never come back here for p. */
  p->exit_code = code;
  p->state = P_ZOMBIE;
  int init_alive = p->pid != 1 && procs[1].state == P_RUN;
  for (int i = 1; i < NPROC; i++) { /* orphans go to init, dead orphans are reaped */
    struct proc *c = &procs[i];
    if (c->state == P_FREE || c->ppid != p->pid) continue;
    c->ppid = init_alive ? 1 : 0;
    if (c->state == P_ZOMBIE) proc_free(c);
  }
  for (int i = 0; i < NFD; i++) if (p->fd[i]) { struct file *f = p->fd[i]; p->fd[i] = NULL; file_put(f); }
  locks_proc_end(); /* file system: its record locks go with it, and waiters wake */
  if (p->state != P_ZOMBIE) return; /* its parent died meanwhile and it was reaped */
  struct proc *parent = (p->ppid > 0) ? &procs[p->ppid] : NULL;
  /* a parent that ignores SIGCHLD (or set SA_NOCLDWAIT) does not get zombies, as on Linux */
  if (parent && parent->state == P_RUN && (parent->sa[SIGCHLD].handler == 1 || (parent->sa[SIGCHLD].flags & SA_NOCLDWAIT))) {
    wq_wake(&parent->child_wq);
    proc_free(p);
    return;
  }
  if (parent && parent->state == P_RUN) { wq_wake(&parent->child_wq); sig_child_exited(p); }
  else proc_free(p); /* nobody will wait for it */
}
long proc_wait(int pid, u32 ustatus, int options) {
  int have = 0;
  for (int i = 1; i < NPROC; i++) {
    struct proc *c = &procs[i];
    if (c->state == P_FREE || c->ppid != cur->pid) continue;
    if (pid > 0 ? c->pid != pid : pid == 0 ? c->pgid != cur->pgid : pid < -1 && c->pgid != -pid) continue;
    have = 1;
    if (c->state == P_RUN && c->wait_report && (options & (c->wait_report == 1 ? WUNTRACED : WCONTINUED))) {
      int status = c->wait_report == 1 ? (c->stop_sig << 8) | 0x7f : 0xffff;
      if (ustatus && copy_to_user(ustatus, &status, 4)) return -EFAULT;
      c->wait_report = 0;
      return c->pid;
    }
    if (c->state != P_ZOMBIE) continue;
    int status = (c->exit_code & 0xff) << 8;
    if (c->exit_code >= 256) status = c->exit_code - 256; /* killed by signal n: status = n */
    if (ustatus && copy_to_user(ustatus, &status, 4)) return -EFAULT;
    int r = c->pid;
    proc_free(c);
    return r;
  }
  if (!have) return -ECHILD;
  if (options & WNOHANG) return 0;
  wq_add(&cur->child_wq);
  return block(-1);
}

/* ---- entry points for the host ---- */
EXPORT(set_current) void set_current(int pid) { cur = &procs[pid]; }
EXPORT(proc_attach) void proc_attach(int pid, copy_fn in, copy_fn out, size_fn pages) {
  procs[pid].copy_in = in; procs[pid].copy_out = out; procs[pid].mem_pages = pages;
}
/* the host reports how long the process ran since it was last resumed */
EXPORT(proc_account) void proc_account(int pid, double ms) { procs[pid].cpu_ms += ms; }
EXPORT(proc_resumed) void proc_resumed(int pid, double now) { procs[pid].resumed_at = now; }
EXPORT(block_timeout) double block_timeout(int pid) {
  double t = procs[pid].block_ms;
  procs[pid].block_ms = -1;
  return t > 2147483647.0 ? 2147483647.0 : t; /* the host's timers cannot wait longer */
}
/* the program trapped (out-of-bounds access, unreachable, stack overflow) or failed to load */
EXPORT(proc_crashed) void proc_crashed(int pid, int sig) {
  struct proc *p = &procs[pid];
  if (p->state != P_RUN) return;
  kprintf("[kernel] pid %d (%s) killed by signal %d\n", pid, p->name, sig);
  cur = p; /* it is the one leaving: a terminal it hangs up must not spare someone else */
  proc_exit(p, 256 + sig);
}
