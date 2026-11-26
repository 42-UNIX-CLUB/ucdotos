// SPDX-License-Identifier: GPL-2.0-or-later
/* The system call table. Numbers and semantics follow Linux's generic table. */
#include "kernel.h"
long proc_wait(int pid, u32 ustatus, int options);
int reg_truncate(struct inode *i, u32 size);
struct file *dev_open(struct inode *i, int flags);
long remote_open(struct inode *i, int flags); /* remote.c: fetch a remote file on first open */
u32 remote_blocks(struct inode *i);

static struct inode *at_base(int dirfd, const char *path, long *err) {
  if (!path[0]) { *err = -ENOENT; return NULL; } /* checked before the descriptor, as on Linux */
  if (dirfd == AT_FDCWD || path[0] == '/') return cur->cwd;
  struct file *d = fd_get(dirfd);
  if (!d) { *err = -EBADF; return NULL; }
  if (!d->ino || (d->ino->mode & S_IFMT) != S_IFDIR) { *err = -ENOTDIR; return NULL; }
  return d->ino;
}
static long sys_openat(int dirfd, u32 upath, int flags, int mode) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
  struct inode *base = at_base(dirfd, path, &err);
  if (!base) return err;
  struct inode *i = vfs_lookup(base, path);
  if (!i) {
    if (!(flags & O_CREAT)) return -ENOENT;
    int e;
    i = vfs_create(base, path, S_IFREG | (mode & 0777), &e);
    if (!i) return e;
  } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
  u32 type = i->mode & S_IFMT;
  if ((flags & O_DIRECTORY) && type != S_IFDIR) return -ENOTDIR;
  if (type == S_IFDIR && (flags & O_ACCMODE) != O_RDONLY) return -EISDIR;
  struct file *f;
  if (type == S_IFCHR) {
    f = dev_open(i, flags);
    if (!f) return -ENOENT;
  } else if (type == S_IFDIR) f = file_new(&dir_fops, i, flags, NULL);
  else if (type == S_IFREG) {
    if (i->priv) { long r = remote_open(i, flags); if (r) return r; } /* a remote file not fetched yet */
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) reg_truncate(i, 0);
    f = file_new(&reg_fops, i, flags, NULL);
  } else return -EACCES;
  if (!f) return -ENOMEM;
  int fd = fd_alloc(cur, f, 0);
  if (fd < 0) { file_put(f); return fd; }
  cur->cloexec[fd] = !!(flags & O_CLOEXEC);
  return fd;
}
static long sys_close(int fd) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  cur->fd[fd] = NULL;
  file_put(f);
  return 0;
}
static long sys_read(int fd, u32 ubuf, u32 len) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
  if (!f->ops->read) return (f->ino && (f->ino->mode & S_IFMT) == S_IFDIR) ? -EISDIR : -EINVAL;
  return f->ops->read(f, ubuf, len);
}
static long sys_write(int fd, u32 ubuf, u32 len) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
  if (!f->ops->write) return -EINVAL;
  return f->ops->write(f, ubuf, len);
}
static long sys_lseek(int fd, long off, int whence) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  if (!f->ino || ((f->ino->mode & S_IFMT) == S_IFCHR && f->ino->dev != DEV_FB)) return -ESPIPE;
  s64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (s64)f->off : whence == SEEK_END ? (s64)f->ino->size : -1;
  /* the result must fit the 32-bit return value without looking like an error */
  if (base < 0 || base + off < 0 || base + off > 0x7fffffff) return -EINVAL;
  return f->off = (u32)(base + off);
}
static void fill_stat(struct stat *st, struct inode *i) {
  memset(st, 0, sizeof *st);
  if (!i) { st->st_mode = S_IFIFO | 0600; return; }
  st->st_ino = i->ino; st->st_mode = i->mode; st->st_nlink = i->nlink; st->st_size = i->size;
  st->st_uid = st->st_gid = 1000; st->st_rdev = i->dev; st->st_blksize = 4096;
  st->st_blocks = ((i->mode & S_IFMT) == S_IFREG && i->priv) ? remote_blocks(i) : (i->size + 511) / 512;
  st->st_atime = st->st_mtime = st->st_ctime = i->mtime;
}
static long sys_fstat(int fd, u32 ust) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  struct stat st;
  fill_stat(&st, f->ino);
  return copy_to_user(ust, &st, sizeof st);
}
static long sys_newfstatat(int dirfd, u32 upath, u32 ust) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
  struct inode *base = at_base(dirfd, path, &err);
  if (!base) return err;
  struct inode *i = vfs_lookup(base, path);
  if (!i) return -ENOENT;
  struct stat st;
  fill_stat(&st, i);
  return copy_to_user(ust, &st, sizeof st);
}
static long sys_getdents64(int fd, u32 ubuf, u32 len) {
  struct file *f = fd_get(fd);
  if (!f || !f->ino) return -EBADF;
  if ((f->ino->mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
  u32 done = 0, idx = 0;
  union { struct dirent64 d; u8 raw[sizeof(struct dirent64) + 72]; } e;
  for (struct inode *c = NULL;; idx++) {
    const char *name; struct inode *ino;
    if (idx == 0) name = ".", ino = f->ino;
    else if (idx == 1) name = "..", ino = f->ino->parent;
    else { c = (idx == 2) ? f->ino->child : c->next; if (!c) break; name = c->name; ino = c; }
    if (idx < f->off) continue;
    u32 nl = strlen(name), rec = (sizeof(struct dirent64) + nl + 1 + 7) & ~7u;
    if (done + rec > len) { if (!done) return -EINVAL; break; } /* as Linux: buffer too small for one entry */
    memset(&e, 0, sizeof e);
    e.d.d_ino = ino->ino; e.d.d_off = idx + 1; e.d.d_reclen = rec;
    u32 t = ino->mode & S_IFMT;
    e.d.d_type = t == S_IFDIR ? DT_DIR : t == S_IFCHR ? DT_CHR : t == S_IFSOCK ? DT_SOCK : DT_REG;
    memcpy(e.d.d_name, name, nl);
    if (copy_to_user(ubuf + done, &e, rec)) return -EFAULT;
    done += rec;
    f->off = idx + 1;
  }
  return done;
}
static long sys_dup3(int old, int new, int flags) {
  struct file *f = fd_get(old);
  if (!f || new < 0 || new >= NFD) return -EBADF;
  if (old == new || (flags & ~O_CLOEXEC)) return -EINVAL;
  if (cur->state != P_RUN) return -EBADF; /* see fd_alloc */
  /* Install first, close after: closing the old file can hang up a terminal, which can
   * kill this very process and close its descriptors, f included. */
  struct file *old_f = cur->fd[new];
  f->refs++;
  cur->fd[new] = f;
  cur->cloexec[new] = !!(flags & O_CLOEXEC);
  if (old_f) file_put(old_f);
  return new;
}
static long sys_dup(int old, int from) {
  struct file *f = fd_get(old);
  if (!f) return -EBADF;
  if (from < 0 || from >= NFD) return -EINVAL;
  int fd = fd_alloc(cur, f, from);
  if (fd >= 0) f->refs++;
  return fd;
}
static long sys_fcntl(int fd, int cmd, long arg) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  switch (cmd) {
  case 0: return sys_dup(fd, arg);                 /* F_DUPFD */
  case 1: return cur->cloexec[fd];                 /* F_GETFD */
  case 2: cur->cloexec[fd] = arg & 1; return 0;    /* F_SETFD */
  case 3: return f->flags;                         /* F_GETFL */
  case 4: f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) | (arg & (O_NONBLOCK | O_APPEND)); return 0;
  }
  return -EINVAL;
}
static long sys_pipe2(u32 ufds, int flags) {
  struct file *r, *w;
  int e = pipe_create(&r, &w);
  if (e) return e;
  r->flags |= flags & O_NONBLOCK; w->flags |= flags & O_NONBLOCK;
  int fds[2];
  fds[0] = fd_alloc(cur, r, 0);
  fds[1] = fd_alloc(cur, w, 0);
  if (fds[0] < 0 || fds[1] < 0 || copy_to_user(ufds, fds, sizeof fds)) {
    if (fds[0] >= 0) cur->fd[fds[0]] = NULL;
    if (fds[1] >= 0) cur->fd[fds[1]] = NULL;
    file_put(r); file_put(w);
    return fds[0] < 0 || fds[1] < 0 ? -EMFILE : -EFAULT;
  }
  cur->cloexec[fds[0]] = cur->cloexec[fds[1]] = !!(flags & O_CLOEXEC);
  return 0;
}
static long sys_chdir(u32 upath) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
  struct inode *i = vfs_lookup(cur->cwd, path);
  if (!i) return -ENOENT;
  if ((i->mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
  inode_get(i);
  inode_put(cur->cwd);
  cur->cwd = i;
  return 0;
}
static long sys_getcwd(u32 ubuf, u32 cap) {
  char path[PATH_MAX];
  int n = vfs_path(cur->cwd, path, sizeof path);
  if (n < 0) return n;
  if ((u32)n > cap) return -ERANGE;
  return copy_to_user(ubuf, path, n) ? -EFAULT : n;
}
static long sys_mkdirat(int dirfd, u32 upath, int mode) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
  struct inode *base = at_base(dirfd, path, &err);
  if (!base) return err;
  int e = 0;
  return vfs_create(base, path, S_IFDIR | (mode & 0777), &e) ? 0 : e;
}
static long sys_unlinkat(int dirfd, u32 upath, int flags) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
  struct inode *base = at_base(dirfd, path, &err);
  if (!base) return err;
  return vfs_unlink(base, path, !!(flags & AT_REMOVEDIR));
}
static long sys_ftruncate(int fd, long size) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  if (!f->ino || (f->ino->mode & S_IFMT) != S_IFREG || (f->flags & O_ACCMODE) == O_RDONLY || size < 0) return -EINVAL;
  return reg_truncate(f->ino, size);
}
static long sys_nanosleep(u32 ureq) {
  double now = host_now();
  if (cur->sleep_until == 0) {
    struct timespec ts;
    if (copy_from_user(&ts, ureq, sizeof ts)) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
    cur->sleep_until = now + (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
  }
  if (now >= cur->sleep_until) { cur->sleep_until = 0; return 0; }
  return block(cur->sleep_until - now);
}
static long sys_clock_gettime(int clk, u32 uts) {
  double ms = clk == CLOCK_REALTIME ? host_epoch() : host_now();
  if (clk == CLOCK_PROCESS_CPUTIME_ID) ms = cur->cpu_ms + (cur->resumed_at > 0 ? ms - cur->resumed_at : 0);
  else if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return -EINVAL;
  struct timespec ts;
  ts.tv_sec = (s64)(ms / 1000.0);
  ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
  return copy_to_user(uts, &ts, sizeof ts);
}
static long sys_uname(u32 ubuf) {
  struct utsname u;
  memset(&u, 0, sizeof u);
  strlcpy(u.sysname, OS_NAME, 65); strlcpy(u.nodename, "42lisboa", 65);
  strlcpy(u.release, OS_RELEASE, 65); strlcpy(u.version, "#1 " __DATE__ " " __TIME__, 65);
  strlcpy(u.machine, "wasm32", 65);
  return copy_to_user(ubuf, &u, sizeof u);
}
static long sys_ppoll(u32 ufds, u32 nfds, u32 utimeout) {
  struct pollfd fds[NFD];
  if (nfds > NFD) return -EINVAL;
  if (copy_from_user(fds, ufds, nfds * sizeof fds[0])) return -EFAULT;
  double now = host_now(), timeout = -1;
  if (utimeout) {
    struct timespec ts;
    if (copy_from_user(&ts, utimeout, sizeof ts)) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
    timeout = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
  }
  int expired = cur->sleep_until != 0 && now >= cur->sleep_until;
  int ready = 0;
  for (int pass = 0; pass < 2; pass++) { /* pass 0: check; pass 1: register as a waiter */
    for (u32 i = 0; i < nfds; i++) {
      struct file *f = fd_get(fds[i].fd);
      fds[i].revents = 0;
      if (fds[i].fd < 0) continue;
      int m = !f ? 0x20 /* POLLNVAL */ : f->ops->poll ? f->ops->poll(f, pass) : (POLLIN | POLLOUT);
      m &= fds[i].events | POLLERR | POLLHUP | 0x20;
      if (m) { fds[i].revents = m; ready++; }
    }
    if (ready || timeout == 0 || expired) break;
  }
  if (ready || timeout == 0 || expired) {
    cur->sleep_until = 0;
    if (copy_to_user(ufds, fds, nfds * sizeof fds[0])) return -EFAULT;
    return ready;
  }
  if (timeout > 0 && cur->sleep_until == 0) cur->sleep_until = now + timeout;
  return block(timeout > 0 ? cur->sleep_until - now : -1);
}
static long sys_spawn(u32 upath, u32 uargv, u32 uenvp, u32 ufdmap) {
  char path[PATH_MAX];
  long err = str_from_user(path, upath, sizeof path);
  if (err < 0) return err;
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
  int fdmap[3] = {-1, -1, -1};
  if (ufdmap && copy_from_user(fdmap, ufdmap, sizeof fdmap)) { kfree(args); return -EFAULT; }
  int pid = proc_spawn(cur, path, args, len, ufdmap ? fdmap : NULL);
  if (pid < 0) kfree(args);
  return pid;
}
static long sys_getargs(u32 ubuf, u32 cap) {
  if (!cur->args) return 0;
  if (cap < cur->args_len) return -ERANGE;
  return copy_to_user(ubuf, cur->args, cur->args_len) ? -EFAULT : (long)cur->args_len;
}
static long sys_procinfo(int idx, u32 uinfo) {
  for (int i = 1; i < NPROC; i++) {
    struct proc *p = &procs[i];
    if (p->state == P_FREE || idx--) continue;
    struct procinfo pi;
    memset(&pi, 0, sizeof pi);
    pi.pid = p->pid; pi.ppid = p->ppid; pi.state = (p->state == P_RUN && p->sleeping) ? 3 : p->state; pi.syscalls = p->nsys;
    pi.cpu_us = (u64)(p->cpu_ms * 1000.0);
    pi.mem_pages = (p->state == P_RUN && p->mem_pages) ? p->mem_pages() : 0;
    strlcpy(pi.name, p->name, sizeof pi.name);
    return copy_to_user(uinfo, &pi, sizeof pi);
  }
  return -ESRCH;
}
static long sys_renameat(int fd1, u32 ufrom, int fd2, u32 uto) {
  char from[PATH_MAX], to[PATH_MAX];
  long err = str_from_user(from, ufrom, sizeof from);
  if (err < 0) return err;
  if ((err = str_from_user(to, uto, sizeof to)) < 0) return err;
  struct inode *b1 = at_base(fd1, from, &err), *b2 = b1 ? at_base(fd2, to, &err) : NULL;
  if (!b2) return err;
  return vfs_rename(b1, from, b2, to);
}
static double boot_ms;
static long sys_sysinfo(u32 uinfo) {
  struct sysinfo si;
  memset(&si, 0, sizeof si);
  if (!boot_ms) boot_ms = host_now();
  si.uptime = (long)((host_now()) / 1000.0);
  si.mem_unit = 1;
  u32 kpages = __builtin_wasm_memory_size(0), upages = 0, n = 0;
  for (int i = 1; i < NPROC; i++) {
    if (procs[i].state == P_FREE) continue;
    n++;
    if (procs[i].state == P_RUN && procs[i].mem_pages) upages += procs[i].mem_pages();
  }
  si.procs = n;
  /* total = what the device reports, capped at what one wasm32 memory can address;
   * used (total - free) = everything the kernel and all processes have claimed from the browser */
  double dev = host_memory();
  u64 claimed = (u64)(kpages + upages) << 16;
  si.totalram = dev >= 4294901760.0 ? 4294901760u : (u32)dev;
  if (si.totalram < claimed) si.totalram = claimed > 4294901760u ? 4294901760u : (u32)claimed;
  si.freeram = si.totalram - (claimed < si.totalram ? (u32)claimed : si.totalram);
  si.bufferram = kmem_used();
  return copy_to_user(uinfo, &si, sizeof si);
}
static long sys_kill(int pid, int sig) {
  if (sig < 0 || sig > 64) return -EINVAL;
  if (pid <= 0 || pid >= NPROC || procs[pid].state != P_RUN) return -ESRCH;
  if (sig == 0) return 0;
  proc_exit(&procs[pid], 256 + sig);
  host_wake(pid); /* let its suspended stack unwind */
  return pid == cur->pid ? -ERESTARTSYS : 0;
}
static long sys_prw(int wr, int fd, u32 ubuf, u32 len, u32 pos) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  if (!f->ino) return -ESPIPE;
  if ((f->flags & O_ACCMODE) == (wr ? O_RDONLY : O_WRONLY)) return -EBADF;
  if (wr ? !f->ops->write : !f->ops->read) return -EINVAL;
  u32 saved = f->off;
  int app = f->flags & O_APPEND;
  f->flags &= ~O_APPEND;
  f->off = pos;
  long r = wr ? f->ops->write(f, ubuf, len) : f->ops->read(f, ubuf, len);
  f->off = saved;
  f->flags |= app;
  return r;
}
static long sys_setpgid(int pid, int pgid) {
  if (pid == 0) pid = cur->pid;
  if (pid <= 0 || pid >= NPROC || procs[pid].state != P_RUN) return -ESRCH;
  if (pid != cur->pid && procs[pid].ppid != cur->pid) return -EPERM;
  if (pgid < 0) return -EINVAL;
  procs[pid].pgid = pgid ? pgid : pid;
  return 0;
}
static long sys_getrandom(u32 ubuf, u32 len) {
  if (len > 33554431) len = 33554431; /* Linux's limit per call; the count comes back short */
  if (len && !user_ok(ubuf, len)) return -EFAULT;
  u32 tmp[64];
  for (u32 i = 0; i < len; i += sizeof tmp) {
    u32 n = len - i < sizeof tmp ? len - i : sizeof tmp;
    for (u32 j = 0; j < (n + 3) / 4; j++) tmp[j] = host_random();
    copy_to_user(ubuf + i, tmp, n);
  }
  return len;
}

/* Every call except the four hottest. Kept out of syscall_pid so that the hot path has no
 * stack frame: several of these cases need large buffers on the kernel stack. */
static __attribute__((noinline)) long syscall_slow(long nr, long a, long b, long c, long d, long e, long f) {
  switch (nr) {
  case SYS_openat: return sys_openat(a, b, c, d);
  case SYS_close: return sys_close(a);
  case SYS_lseek: return sys_lseek(a, b, c);
  case SYS_fstat: return sys_fstat(a, b);
  case SYS_newfstatat: return sys_newfstatat(a, b, c);
  case SYS_getdents64: return sys_getdents64(a, b, c);
  case SYS_dup: return sys_dup(a, 0);
  case SYS_dup3: return sys_dup3(a, b, c);
  case SYS_fcntl: return sys_fcntl(a, b, c);
  case SYS_pipe2: return sys_pipe2(a, b);
  case SYS_chdir: return sys_chdir(a);
  case SYS_getcwd: return sys_getcwd(a, b);
  case SYS_mkdirat: return sys_mkdirat(a, b, c);
  case SYS_unlinkat: return sys_unlinkat(a, b, c);
  case SYS_ftruncate: return sys_ftruncate(a, b);
  case SYS_renameat: return sys_renameat(a, b, c, d);
  case SYS_sysinfo: return sys_sysinfo(a);
  case SYS_ioctl: { struct file *fl = fd_get(a); return !fl ? -EBADF : fl->ops->ioctl ? fl->ops->ioctl(fl, b, c) : -ENOTTY; }
  case SYS_ppoll: return sys_ppoll(a, b, c);
  case SYS_nanosleep: return sys_nanosleep(a);
  case SYS_clock_gettime: return sys_clock_gettime(a, b);
  case SYS_sched_yield: return 0;
  case SYS_uname: return sys_uname(a);
  case SYS_getuid: return 1000;
  case SYS_getrandom: return sys_getrandom(a, b);
  case SYS_wait4: return proc_wait(a, b, c);
  case SYS_kill: return sys_kill(a, b);
  case SYS_exit: case SYS_exit_group: proc_exit(cur, a & 0xff); return -ERESTARTSYS;
  case SYS_intr: { int was = cur->intr; cur->intr = 0; return was; }
  case SYS_utimensat: { /* only "set the times to now" is supported */
    char path[PATH_MAX];
    long err = str_from_user(path, b, sizeof path);
    if (err < 0) return err;
    struct inode *i = vfs_lookup(cur->cwd, path);
    if (!i) return -ENOENT;
    i->mtime = (s64)(host_epoch() / 1000);
    return 0;
  }
  case SYS_pread64: return sys_prw(0, a, b, c, d);
  case SYS_pwrite64: return sys_prw(1, a, b, c, d);
  case SYS_setpgid: return sys_setpgid(a, b);
  case SYS_getpgid: return (a == 0 || (a > 0 && a < NPROC && procs[a].state == P_RUN)) ? procs[a ? a : cur->pid].pgid : -ESRCH;
  case SYS_setsid: cur->sid = cur->pgid = cur->pid; return cur->pid;
  case SYS_socket: return sys_socket(a, b, c);
  case SYS_bind: return sys_bind(a, b, c);
  case SYS_listen: return sys_listen(a, b);
  case SYS_accept: return sys_accept(a, b, c);
  case SYS_connect: return sys_connect(a, b, c);
  case SYS_getsockname: return sys_getsockname(a, b, c);
  case SYS_getpeername: return sys_getpeername(a, b, c);
  case SYS_sendto: return sys_sendto(a, b, c, d, e, f);
  case SYS_recvfrom: return sys_recvfrom(a, b, c, d, e, f);
  case SYS_setsockopt: return sys_setsockopt(a, b, c, d, e);
  case SYS_getsockopt: return sys_getsockopt(a, b, c, d, e);
  case SYS_shutdown: return sys_shutdown(a, b);
  case SYS_netstat: return sys_netstat(a, b);
  case SYS_ifinfo: return sys_ifinfo(a, b);
  case SYS_ifctl: return sys_ifctl(a, b, c);
  case SYS_spawn: return sys_spawn(a, b, c, d);
  case SYS_getargs: return sys_getargs(a, b);
  case SYS_procinfo: return sys_procinfo(a, b);
  }
  return -ENOSYS;
}

/* Time slicing. Nothing can interrupt running WebAssembly, so the kernel takes the CPU back
 * at system call boundaries: every 256th call it looks at the clock, and a process that has
 * run for more than SLICE_MS is parked for one turn of the event loop before the call is
 * carried out. Input, the screen and other processes get to run. The call itself has not
 * started yet, so the process simply retries it when it resumes. Both helpers are out of
 * line so that the clock read and its floating point stay off the hot path. */
static __attribute__((noinline)) int slice_over(void) {
  double now = host_now();
  if (cur->slice_start == 0) { cur->slice_start = now; return 0; }
  return now - cur->slice_start > SLICE_MS;
}
static __attribute__((noinline)) long preempt(void) {
  cur->nsys--; /* the call will be made again */
  long r = block(0);
  cur->sleeping = 0; /* preempted, not sleeping */
  return r;
}

/* Entry point. Every process calls this through its own tiny wasm trampoline, which
 * supplies the caller's pid (the program cannot forge it). The kernel therefore knows
 * who is calling without depending on the order in which the host resumes stacks.
 * The system call counter doubles as the time slice counter. */
EXPORT(syscall_pid) long syscall_pid(int pid, long nr, long a, long b, long c, long d, long e, long f) {
  struct proc *p = &procs[pid]; /* a local, so the cases below need not reload cur */
  cur = p;
  p->sleeping = 0;
  if (__builtin_expect((++p->nsys & 255) == 0, 0) && slice_over()) return preempt();
  switch (nr) {
  case SYS_getppid: return p->ppid;
  case SYS_getpid: return p->pid;
  case SYS_read: return sys_read(a, b, c);
  case SYS_write: return sys_write(a, b, c);
  }
  return syscall_slow(nr, a, b, c, d, e, f);
}
