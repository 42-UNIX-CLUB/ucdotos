// SPDX-License-Identifier: GPL-2.0-or-later
/* /proc, kept small: only what the system can answer truthfully.
 *
 *   /proc/self                 symbolic link to /proc/PID of the caller
 *   /proc/PID/fd/N             "magic" links: following one reaches the open file itself,
 *   /proc/PID/exe, cwd         even an unlinked file or a pipe; reading one gives a path
 *   /proc/PID/cmdline, comm, status, mounts
 *   /proc/cpuinfo, meminfo, mounts, uptime, version, filesystems
 *
 * The inodes are made on first use and kept (at most a few thousand, all small); what
 * they show is computed at the moment they are read, from the process table. */
#include "kernel.h"
struct inode *inode_alloc_proc(u32 mode, int kind, int dev, struct inode *parent);
int vfs_symlink(struct inode *base, const char *target, const char *path);

enum { G_CPUINFO, G_MEMINFO, G_MOUNTS, G_UPTIME, G_VERSION, G_FILESYSTEMS, G_STORE, G_NROOT,
       G_CMDLINE = 0, G_COMM, G_STATUS, G_PMOUNTS, G_NPID };
static const char *root_names[G_NROOT] = {"cpuinfo", "meminfo", "mounts", "uptime", "version", "filesystems", "ucos-store"};
int store_status(char *buf, u32 cap);
long store_ctl(const char *cmd, u32 n);
static const char *pid_names[] = {"fd", "exe", "cwd", "cmdline", "comm", "status", "mounts"};
#define NPIDNAMES 7

static struct inode *proc_root, *self_link, *root_gen[G_NROOT];
static struct inode *pid_dir[NPROC], *fd_dir[NPROC], *pid_link[NPROC][2], *pid_gen[NPROC][G_NPID], *fd_link[NPROC][NFD];

static struct inode *get(struct inode **slot, u32 mode, int kind, int dev, struct inode *parent) {
  if (!*slot) *slot = inode_alloc_proc(mode, kind, dev, parent);
  return *slot;
}
static int alive(int pid) { return pid > 0 && pid < NPROC && procs[pid].state != P_FREE; }
static int parse_num(const char *s, u32 n) {
  if (!n || n > 4 || (n > 1 && s[0] == '0')) return -1;
  int v = 0;
  for (u32 k = 0; k < n; k++) { if (s[k] < '0' || s[k] > '9') return -1; v = v * 10 + (s[k] - '0'); }
  return v;
}
static void owner(struct inode *i, int pid) { if (i) { i->uid = fscred[pid].euid; i->gid = fscred[pid].egid; } }
static int eq(const char *s, u32 n, const char *lit) { return strlen(lit) == n && !memcmp(s, lit, n); }

static struct inode *pid_entry(int pid, int k) {
  struct inode *i = NULL;
  if (k == 0) i = get(&fd_dir[pid], S_IFDIR | 0500, K_FDDIR, pid, pid_dir[pid]);
  else if (k <= 2) i = get(&pid_link[pid][k - 1], S_IFLNK | 0777, k == 1 ? K_EXE : K_CWD, pid, NULL);
  else i = get(&pid_gen[pid][k - 3], S_IFREG | 0444, K_GEN, ((k - 3) << 8) | pid | 0x10000, NULL);
  owner(i, pid);
  return i;
}
struct inode *procfs_lookup(struct inode *dir, const char *s, u32 n) {
  int pid = dir->dev;
  switch (dir->kind) {
  case K_PROCROOT: {
    if (eq(s, n, "self")) return self_link;
    for (int k = 0; k < G_NROOT; k++) if (eq(s, n, root_names[k])) return get(&root_gen[k], S_IFREG | 0444, K_GEN, k, NULL);
    int p = parse_num(s, n);
    if (!alive(p)) return NULL;
    struct inode *d = get(&pid_dir[p], S_IFDIR | 0555, K_PIDDIR, p, proc_root);
    owner(d, p);
    return d;
  }
  case K_PIDDIR:
    if (!alive(pid)) return NULL;
    for (int k = 0; k < NPIDNAMES; k++) if (eq(s, n, pid_names[k])) return pid_entry(pid, k);
    return NULL;
  case K_FDDIR: {
    int fd = parse_num(s, n);
    if (!alive(pid) || fd < 0 || fd >= NFD || !procs[pid].fd[fd]) return NULL;
    struct inode *l = get(&fd_link[pid][fd], S_IFLNK | 0700, K_FDLINK, pid * NFD + fd, NULL);
    owner(l, pid);
    return l;
  }
  }
  return NULL;
}
int procfs_list(struct inode *dir, u32 idx, char *name, struct inode **ino) {
  int pid = dir->dev;
  switch (dir->kind) {
  case K_PROCROOT:
    if (idx == 0) { strlcpy(name, "self", 8); *ino = self_link; return 1; }
    if (idx <= G_NROOT) { strlcpy(name, root_names[idx - 1], 16); *ino = procfs_lookup(dir, name, strlen(name)); return 1; }
    idx -= G_NROOT + 1;
    for (int p = 1; p < NPROC; p++) if (alive(p) && !idx--) { snprintf(name, 8, "%d", p); *ino = procfs_lookup(dir, name, strlen(name)); return 1; }
    return 0;
  case K_PIDDIR:
    if (!alive(pid) || idx >= NPIDNAMES) return 0;
    strlcpy(name, pid_names[idx], 16);
    *ino = pid_entry(pid, idx);
    return 1;
  case K_FDDIR:
    if (!alive(pid)) return 0;
    for (int fd = 0; fd < NFD; fd++) if (procs[pid].fd[fd] && !idx--) { snprintf(name, 8, "%d", fd); *ino = procfs_lookup(dir, name, strlen(name)); return 1; }
    return 0;
  }
  return 0;
}
static struct file *fd_of(struct inode *link) {
  int pid = link->dev / NFD, fd = link->dev % NFD;
  return alive(pid) ? procs[pid].fd[fd] : NULL;
}
struct inode *procfs_follow(struct inode *link, struct file **file) {
  int pid = link->dev;
  switch (link->kind) {
  case K_FDLINK: {
    struct file *f = fd_of(link);
    if (!f) return NULL;
    if (!f->ino) *file = f;
    return f->ino;
  }
  case K_EXE: return alive(pid) ? fscred[pid].exe : NULL;
  case K_CWD: return alive(pid) && procs[pid].state == P_RUN ? procs[pid].cwd : NULL;
  }
  return NULL;
}
static int path_or_deleted(struct inode *i, char *buf, u32 cap) {
  int n = vfs_path(i, buf, cap);
  if (n > 0) return n - 1; /* vfs_path counts the NUL */
  if (n == -ERANGE || n == -ENAMETOOLONG) return -ENAMETOOLONG;
  return snprintf(buf, cap, i->nlink ? "inode:[%u]" : "/ (deleted inode %u)", i->ino);
}
/* The text a magic link reads as: a path, or what Linux shows for objects without one. */
int procfs_readlink(struct inode *link, char *buf, u32 cap) {
  if (link->kind == K_SELF) return cur ? snprintf(buf, cap, "%d", cur->pid) : -ENOENT;
  struct file *f = NULL;
  struct inode *i = procfs_follow(link, &f);
  if (i) return path_or_deleted(i, buf, cap);
  if (f) return snprintf(buf, cap, "pipe:[%u]", (u32)(usize)f->priv);
  return -ENOENT;
}

/* ---- generated files: the text is made when the file is opened ---- */
struct gen { u32 len, cap; char text[]; };
static long gen_read(struct file *f, u32 ubuf, u32 len) {
  struct gen *g = f->priv;
  if (f->off >= g->len) return 0;
  if (len > g->len - f->off) len = g->len - f->off;
  if (copy_to_user(ubuf, g->text + f->off, len)) return -EFAULT;
  f->off += len;
  return len;
}
static void gen_close(struct file *f) { kfree(f->priv); }
static long gen_write(struct file *f, u32 ubuf, u32 len) { /* only /proc/ucos-store takes writes: "wipe" or "sync" */
  char cmd[16];
  if (f->ino->dev != G_STORE) return -EINVAL;
  u32 n = len < sizeof cmd ? len : sizeof cmd;
  if (copy_from_user(cmd, ubuf, n)) return -EFAULT;
  long r = store_ctl(cmd, n);
  return r < 0 ? r : (long)len;
}
int procfs_writable(struct inode *i) { return i->kind == K_GEN && i->dev == G_STORE; }
static int ready(struct file *f, int add) { (void)f; (void)add; return POLLIN; }
static const struct fops gen_fops = {gen_read, gen_write, ready, gen_close, NULL};
#define ADD(...) do { if (g->len < g->cap) { int w_ = snprintf(g->text + g->len, g->cap - g->len, __VA_ARGS__); g->len += w_ < (int)(g->cap - g->len) ? w_ : (int)(g->cap - g->len) - 1; } } while (0)
void mem_figures(u64 *total, u64 *free) {
  u32 kpages = __builtin_wasm_memory_size(0), upages = 0;
  for (int k = 1; k < NPROC; k++) if (procs[k].state == P_RUN && procs[k].mem_pages) upages += procs[k].mem_pages();
  double dev = host_memory();
  u64 claimed = (u64)(kpages + upages) << 16;
  *total = dev >= 4294901760.0 ? 4294901760ull : (u64)dev;
  if (*total < claimed) *total = claimed;
  *free = *total - claimed;
}
struct file *procfs_open(struct inode *i, int flags) {
  u32 cap = 4096;
  struct gen *g = kmalloc(sizeof *g + cap);
  if (!g) return NULL;
  g->len = 0; g->cap = cap;
  int what = i->dev & 0xff, pid = (i->dev >> 8) & 0xff;
  if (i->dev & 0x10000) { /* per process; dev = (what << 8) | pid | 0x10000 */
    pid = i->dev & 0xff; what = (i->dev >> 8) & 0xff;
    struct proc *p = &procs[pid];
    if (!alive(pid)) { kfree(g); return NULL; }
    if (what == G_CMDLINE && p->args && p->args_len >= 8) {
      u32 argc = *(u32 *)p->args, off = 8;
      for (u32 a = 0; a < argc && off < p->args_len; a++) {
        u32 n = strlen((char *)p->args + off) + 1;
        if (g->len + n > g->cap) break;
        memcpy(g->text + g->len, p->args + off, n);
        g->len += n; off += n;
      }
    } else if (what == G_COMM) ADD("%s\n", p->name);
    else if (what == G_STATUS) {
      struct fscred *c = &fscred[pid];
      ADD("Name:\t%s\nState:\t%s\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n",
          p->name, p->state == P_ZOMBIE ? "Z (zombie)" : p->sleeping ? "S (sleeping)" : "R (running)",
          pid, pid, p->ppid, c->uid, c->euid, c->suid, c->fsuid, c->gid, c->egid, c->sgid, c->fsgid);
    } else if (what == G_PMOUNTS) what = 0x100 + G_MOUNTS;
    if (what < 0x100) goto done;
    what -= 0x100;
  }
  switch (what) {
  case G_CPUINFO: ADD("processor\t: 0\nvendor_id\t: WebAssembly\nmodel name\t: wasm32 virtual machine (one thread)\ncpu cores\t: 1\n\n"); break;
  case G_MEMINFO: {
    u64 total, free;
    mem_figures(&total, &free);
    ADD("MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:        %8u kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
        total >> 10, free >> 10, free >> 10, kmem_used() >> 10);
    break;
  }
  case G_MOUNTS: ADD("ucosfs / ucosfs rw,relatime 0 0\nproc /proc proc rw 0 0\n"); break;
  case G_UPTIME: { double s = host_now() / 1000.0; ADD("%.2f 0.00\n", s); break; }
  case G_VERSION: ADD("%s version %s (wasm32)\n", OS_NAME, OS_RELEASE); break;
  case G_FILESYSTEMS: ADD("\tucosfs\nnodev\tproc\n"); break;
  case G_STORE: g->len = store_status(g->text, g->cap); if (g->len >= g->cap) g->len = g->cap - 1; break;
  }
done:;
  struct file *f = file_new(&gen_fops, i, flags, g);
  if (!f) kfree(g);
  return f;
}

/* The process in slot pid is gone: free its /proc inodes unless something holds one open. */
static int drop(struct inode **slot) {
  if (!*slot) return 1;
  if ((*slot)->refs) return 0;
  inode_drop(*slot);
  *slot = NULL;
  return 1;
}
void procfs_forget(int pid) {
  int all = 1;
  for (int fd = 0; fd < NFD; fd++) all &= drop(&fd_link[pid][fd]);
  for (int k = 0; k < 2; k++) drop(&pid_link[pid][k]);
  for (int k = 0; k < G_NPID; k++) drop(&pid_gen[pid][k]);
  if (all && drop(&fd_dir[pid])) drop(&pid_dir[pid]); /* fd/ points at its parent: free it first */
}
/* The last reference to a /proc inode went: if its process is gone, free its entries now. */
void procfs_release(struct inode *i) {
  int pid;
  switch (i->kind) {
  case K_PIDDIR: case K_FDDIR: case K_EXE: case K_CWD: pid = i->dev; break;
  case K_FDLINK: pid = i->dev / NFD; break;
  case K_GEN: if (!(i->dev & 0x10000)) return; pid = i->dev & 0xff; break;
  default: return;
  }
  if (pid > 0 && pid < NPROC && procs[pid].state == P_FREE) procfs_forget(pid);
}
void procfs_init(void) {
  proc_root = vfs_lookup(vfs_root, "/proc");
  proc_root->kind = K_PROCROOT;
  proc_root->mode = S_IFDIR | 0555;
  self_link = inode_alloc_proc(S_IFLNK | 0777, K_SELF, 0, NULL);
  for (int k = 0; k < G_NROOT; k++) get(&root_gen[k], S_IFREG | (k == G_STORE ? 0644 : 0444), K_GEN, k, NULL); /* made now, so the heap does not grow later */
  /* the usual names for a process's own descriptors, as on Linux */
  vfs_symlink(vfs_root, "/proc/self/fd", "/dev/fd");
  vfs_symlink(vfs_root, "/proc/self/fd/0", "/dev/stdin");
  vfs_symlink(vfs_root, "/proc/self/fd/1", "/dev/stdout");
  vfs_symlink(vfs_root, "/proc/self/fd/2", "/dev/stderr");
  vfs_mkdev("/dev/tty", DEV_TTY);
}
