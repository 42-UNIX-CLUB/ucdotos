// SPDX-License-Identifier: GPL-2.0-or-later
/* Process cloning (the primitive under fork) and execve. The kernel keeps the process
 * table and the descriptors; the host makes the WebAssembly side (boot/host.mjs:
 * clone, exec). Contract: ARCHITECTURE.md section 5. */
#include "kernel.h"

/* SYS_pclone(flags = 0). The new process gets the caller's descriptor table (the same open
 * file descriptions, close-on-exec flags kept), cwd, process group, session, name,
 * argument blob, signal dispositions and mask; no pending signals, no timer. The host,
 * inside this call, makes a new instance of the caller's module whose memory is a copy of
 * the caller's memory at this moment (same size) and whose exported mutable globals have
 * the caller's values, and later calls its export __fork_child. All of it happens within
 * this one call: nothing else runs in between, so a parent cannot die halfway. */
long sys_pclone(int flags) {
  struct proc *par = cur;
  if (flags) return -EINVAL;
  if (par->state != P_RUN) return -ESRCH;
  int pid = pid_alloc();
  if (!pid) return -EAGAIN;
  u8 *args = NULL;
  if (par->args) {
    if (!(args = kmalloc(par->args_len ? par->args_len : 1))) return -ENOMEM;
    memcpy(args, par->args, par->args_len);
  }
  struct proc *p = &procs[pid];
  memset(p, 0, sizeof *p);
  p->pid = pid;
  p->state = P_RUN;
  p->ppid = par->pid;
  p->pgid = par->pgid;
  p->sid = par->sid;
  p->cwd = par->cwd;
  inode_get(p->cwd);
  p->args = args; p->args_len = par->args_len;
  p->block_ms = -1;
  memcpy(p->name, par->name, sizeof p->name);
  for (int i = 0; i < NFD; i++) {
    if (!par->fd[i]) continue;
    par->fd[i]->refs++;
    p->fd[i] = par->fd[i];
    p->cloexec[i] = par->cloexec[i];
  }
  sig_fork_copy(p, par);
  int r = host_clone(par->pid, pid);
  if (r < 0) { /* undo; the child never ran and nobody saw its pid */
    for (int i = 0; i < NFD; i++) if (p->fd[i]) { struct file *f = p->fd[i]; p->fd[i] = NULL; file_put(f); }
    inode_put(p->cwd);
    p->cwd = vfs_root;
    if (p->args) kfree(p->args);
    p->args = NULL;
    p->state = P_FREE;
    return r;
  }
  return pid;
}

/* the argument blob for _start: u32 argc, u32 envc, then the strings (as sys_spawn makes it) */
static long args_from_user(u32 uargv, u32 uenvp, u8 **out, u32 *outlen) {
  u32 cap = 65536, len = 8;
  u8 *args = kmalloc(cap);
  if (!args) return -ENOMEM;
  u32 counts[2] = {0, 0};
  for (int which = 0; which < 2; which++) {
    u32 uvec = which ? uenvp : uargv;
    for (u32 i = 0; uvec; i++) {
      u32 up;
      if (copy_from_user(&up, uvec + i * 4, 4)) { kfree(args); return -EFAULT; }
      if (!up) break;
      int n = str_from_user((char *)args + len, up, cap - len);
      if (n < 0) { kfree(args); return n == -ENAMETOOLONG ? -7 /* E2BIG */ : n; }
      len += n + 1;
      counts[which]++;
    }
  }
  memcpy(args, counts, 8);
  *out = args; *outlen = len;
  return 0;
}

/* execve: same pid, a new program. Descriptors without close-on-exec stay, handlers go
 * back to the default, and the host drops the old instance: this call never returns to
 * it (it returns -ERESTARTSYS, and the old stack is thrown away when it asks to wait). */
long sys_execve(u32 upath, u32 uargv, u32 uenvp) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
  struct inode *exe = vfs_lookup(cur->cwd, path);
  if (!exe) return -ENOENT;
  if ((exe->mode & S_IFMT) != S_IFREG) return -EACCES;
  if (exe->size < 8 || memcmp(exe->data, "\0asm", 4)) return -ENOEXEC;
  u8 *args;
  u32 len;
  if ((err = args_from_user(uargv, uenvp, &args, &len)) < 0) return err;
  /* point of no return */
  inode_get(exe); /* closing descriptors below must not free the program under us */
  for (int i = 0; i < NFD; i++) {
    if (!cur->fd[i] || !cur->cloexec[i]) continue;
    struct file *f = cur->fd[i];
    cur->fd[i] = NULL;
    cur->cloexec[i] = 0;
    file_put(f); /* can hang up a terminal and kill us */
  }
  if (cur->state != P_RUN) { kfree(args); inode_put(exe); return -ERESTARTSYS; }
  if (cur->args) kfree(cur->args);
  cur->args = args; cur->args_len = len;
  const char *base = strrchr(path, '/');
  strlcpy(cur->name, base ? base + 1 : path, sizeof cur->name);
  sig_exec_reset(cur);
  cur->in_sigsuspend = 0;
  cur->sleep_until = 0;
  cur->block_ms = -1;
  host_exec(cur->pid, exe->data, exe->size, !exe->owned);
  inode_put(exe);
  return -ERESTARTSYS;
}

/* SYS_pmigrate(ranges, n, flags = 0): the same process continues in a new instance of its
 * module whose memory has the same size but holds only the listed ranges of the old one, at
 * the same addresses (nothing moves, so every pointer stays valid); the rest is zero and,
 * never written, costs the browser no memory. The exported mutable globals are carried over.
 * Nothing in the kernel changes: same pid, descriptors, parent, children, signals; no SIGCHLD.
 * All or nothing: on failure the call returns -errno and the old instance goes on. On success
 * it never returns: the old stack is dropped (as for execve) and the new instance starts at
 * its export __migrate_resume. */
long sys_pmigrate(u32 uranges, u32 n, int flags) {
  if (flags) return -EINVAL;
  if (n > 65536 || (uranges & 3)) return -EINVAL;
  if (n && !user_ok(uranges, n * 8)) return -EFAULT;
  int r = host_migrate(cur->pid, uranges, n);
  return r < 0 ? r : -ERESTARTSYS;
}
