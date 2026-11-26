// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel.h"

struct proc procs[NPROC];
struct proc *cur;

/* ---- user memory access, through the process's multi-memory shim ---- */
static int user_ok(u32 addr, u32 len) {
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
  return -ENAMETOOLONG;
}

/* ---- wait queues and blocking ---- */
void wq_add(waitq *q) { *q |= 1ull << cur->pid; }
void wq_wake(waitq *q) {
  u64 m = *q;
  *q = 0;
  for (int pid = 0; m; pid++, m >>= 1) if (m & 1) host_wake(pid);
}
long block(double timeout_ms) {
  cur->block_ms = timeout_ms;
  cur->sleeping = 1;
  cur->slice_start = 0;
  return -ERESTARTSYS;
}

/* ---- files ---- */
struct file *file_new(const struct fops *ops, struct inode *ino, int flags, void *priv) {
  struct file *f = kzalloc(sizeof *f);
  if (!f) return NULL;
  f->refs = 1; f->ops = ops; f->ino = ino; f->flags = flags; f->priv = priv;
  return f;
}
void file_put(struct file *f) {
  if (--f->refs) return;
  if (f->ops->close) f->ops->close(f);
  kfree(f);
}
int fd_alloc(struct proc *p, struct file *f, int from) {
  for (int i = from; i < NFD; i++) if (!p->fd[i]) { p->fd[i] = f; p->cloexec[i] = 0; return i; }
  return -EMFILE;
}
struct file *fd_get(int fd) { return (fd >= 0 && fd < NFD) ? cur->fd[fd] : NULL; }

/* ---- lifecycle ---- */
int proc_spawn(struct proc *parent, const char *path, u8 *args, u32 args_len, const int *fdmap) {
  struct inode *exe = vfs_lookup(parent ? parent->cwd : vfs_root, path);
  if (!exe) return -ENOENT;
  if ((exe->mode & S_IFMT) != S_IFREG) return -EACCES;
  if (exe->size < 8 || memcmp(exe->data, "\0asm", 4)) return -ENOEXEC;
  struct proc *p = NULL;
  for (int i = 1; i < NPROC; i++) if (procs[i].state == P_FREE) { p = &procs[i]; p->pid = i; break; }
  if (!p) return -EAGAIN;
  int pid = p->pid;
  memset(p, 0, sizeof *p);
  p->pid = pid;
  p->state = P_RUN;
  p->ppid = parent ? parent->pid : 0;
  p->cwd = parent ? parent->cwd : vfs_root;
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
  }
  host_spawn(pid, exe->data, exe->size, !exe->owned);
  return pid;
}
static void proc_free(struct proc *p) {
  host_reap(p->pid);
  if (p->args) kfree(p->args);
  p->args = NULL;
  p->state = P_FREE;
}
void proc_exit(struct proc *p, int code) {
  if (p->state != P_RUN) return;
  for (int i = 0; i < NFD; i++) if (p->fd[i]) { file_put(p->fd[i]); p->fd[i] = NULL; }
  p->exit_code = code;
  p->state = P_ZOMBIE;
  for (int i = 1; i < NPROC; i++) { /* orphans go to init, dead orphans are reaped */
    struct proc *c = &procs[i];
    if (c->state == P_FREE || c->ppid != p->pid) continue;
    c->ppid = 1;
    if (c->state == P_ZOMBIE) proc_free(c);
  }
  struct proc *parent = (p->ppid > 0) ? &procs[p->ppid] : NULL;
  if (parent && parent->state == P_RUN) wq_wake(&parent->child_wq);
  else proc_free(p); /* nobody will wait for it */
}
long proc_wait(int pid, u32 ustatus, int options) {
  int have = 0;
  for (int i = 1; i < NPROC; i++) {
    struct proc *c = &procs[i];
    if (c->state == P_FREE || c->ppid != cur->pid) continue;
    if (pid > 0 && c->pid != pid) continue;
    have = 1;
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
  return t;
}
/* the program trapped (out-of-bounds access, unreachable, stack overflow) or failed to load */
EXPORT(proc_crashed) void proc_crashed(int pid, int sig) {
  struct proc *p = &procs[pid];
  if (p->state != P_RUN) return;
  kprintf("[kernel] pid %d (%s) killed by signal %d\n", pid, p->name, sig);
  proc_exit(p, 256 + sig);
}
