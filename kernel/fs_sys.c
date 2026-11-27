// SPDX-License-Identifier: GPL-2.0-or-later
/* The file system calls: paths, links, modes, owners, times, locks, statx, statfs,
 * vectored I/O, credentials. Numbers and semantics follow Linux's generic table; what is
 * left out is listed in docs/VFS-NOTES.md. syscall_slow() hands every call it does not
 * know to fs_syscall(). */
#include "kernel.h"
struct file *dev_open(struct inode *i, int flags);
long store_sync(struct file *f, int all); /* store.c: 0 when durable, or -ERESTARTSYS */
void store_statfs(u64 *total, u64 *free);
extern u32 vfs_ninodes;

static struct fscred *cred(void) { return &fscred[cur->pid]; }
static int privileged(void) { return vfs_enforce < 2 || cred()->euid == 0; }
static int owner_or_root(struct inode *i) { return vfs_enforce < 2 || cred()->fsuid == 0 || cred()->fsuid == i->uid; }

/* dirfd + path, as every *at call takes them. An empty path names nothing (ENOENT), unless
 * AT_EMPTY_PATH says it means the descriptor itself. */
static long at_walk(int dirfd, const char *path, int wflags, int atflags, struct nameidata *nd) {
  if (!path[0]) {
    if (!(atflags & AT_EMPTY_PATH)) return -ENOENT;
    nd->dir = NULL; nd->de = NULL; nd->dot = 0; nd->trailing = 0; nd->file = NULL;
    if (dirfd == AT_FDCWD) { nd->ino = cur->cwd; return 0; }
    struct file *f = fd_get(dirfd);
    if (!f) return -EBADF;
    nd->ino = f->ino;
    if (!f->ino) nd->file = f;
    return 0;
  }
  struct inode *base = cur->cwd;
  if (dirfd != AT_FDCWD && path[0] != '/') {
    struct file *f = fd_get(dirfd);
    if (!f) return -EBADF;
    if (!f->ino || !S_ISDIR(f->ino->mode)) return -ENOTDIR;
    base = f->ino;
  }
  return vfs_walk(base, path, wflags, nd);
}
/* resolve and require that something is there */
static long at_find(int dirfd, u32 upath, int follow, int atflags, char *path, struct nameidata *nd) {
  long e = path_from_user(path, upath);
  if (e < 0) return e;
  if ((e = at_walk(dirfd, path, follow ? W_FOLLOW : 0, atflags, nd))) return e;
  if (!nd->ino && !nd->file) return -ENOENT;
  if (nd->ino && nd->trailing && !S_ISDIR(nd->ino->mode)) return -ENOTDIR;
  return 0;
}

/* ---- open ---- */
static const struct fops path_fops = {NULL, NULL, NULL, NULL, NULL}; /* O_PATH: a name, no I/O */
static struct inode *controlling_tty(void) {
  for (int k = 0; k < 3; k++) {
    struct file *f = cur->fd[k];
    if (f && f->ino && S_ISCHR(f->ino->mode) && (f->ino->dev >= DEV_PTS0 || f->ino->dev == DEV_CONSOLE)) return f->ino;
  }
  return NULL;
}
static long install(struct file *f, int flags) {
  int fd = fd_alloc(cur, f, 0);
  if (fd < 0) { file_put(f); return fd; }
  cur->cloexec[fd] = !!(flags & O_CLOEXEC);
  return fd;
}
long store_need_data(struct inode *i); /* store.c: 0, or -ERESTARTSYS while the contents load */
int procfs_writable(struct inode *i);
/* Everything open does after the walk; the common case (an existing regular file or a
 * directory, opened for reading or writing without O_CREAT, O_TRUNC, O_PATH) is handled by
 * fs_openat itself, so this stays out of the hot path. */
static __attribute__((noinline)) long open_nd(struct nameidata *nd, int flags, int mode) {
  long err;
  int excl = (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
  if (nd->file) { /* /proc/self/fd/N of a pipe or a socket: that same open file */
    if (flags & O_DIRECTORY) return -ENOTDIR;
    nd->file->refs++;
    return install(nd->file, flags);
  }
  struct inode *i = nd->ino;
  int created = 0, acc = flags & O_ACCMODE;
  if (!i) {
    if (!(flags & O_CREAT)) return -ENOENT;
    if (flags & O_DIRECTORY) return -EINVAL;
    int e;
    i = vfs_mknod_at(nd, S_IFREG | (mode & 07777 & ~cred()->umask), &e);
    if (!i) return e;
    created = 1;
  } else {
    if (excl) return -EEXIST;
    if (nd->trailing && !S_ISDIR(i->mode)) return -ENOTDIR;
    if (S_ISLNK(i->mode) && !(flags & O_PATH)) return -ELOOP; /* O_NOFOLLOW met a link */
    if ((flags & O_CREAT) && S_ISDIR(i->mode)) return -EISDIR;
  }
  u32 type = i->mode & S_IFMT;
  if ((flags & O_DIRECTORY) && type != S_IFDIR) return -ENOTDIR;
  struct file *f;
  if (flags & O_PATH) f = file_new(&path_fops, i, flags, NULL);
  else {
    if (type == S_IFDIR && acc != O_RDONLY) return -EISDIR;
    if (!created && vfs_enforce >= 2) {
      int e = vfs_may(i, (acc == O_RDONLY ? 4 : acc == O_WRONLY ? 2 : 6) | ((flags & O_TRUNC) ? 2 : 0));
      if (e) return e;
    }
    if (type == S_IFCHR) {
      if (i->dev == DEV_TTY && !(i = controlling_tty())) return -ENXIO;
      f = dev_open(i, flags);
      if (!f) return -ENOENT;
    } else if (type == S_IFDIR) f = file_new(&dir_fops, i, flags, NULL);
    else if (type == S_IFREG) {
      if (i->kind == K_GEN) {
        if (acc != O_RDONLY && !procfs_writable(i)) return -EACCES;
        if (!(f = procfs_open(i, flags))) return -ENOENT;
      } else {
        if (i->priv && (err = remote_open(i, flags))) return err; /* a remote file not fetched yet (remote.c) */
        if ((i->sflags & S_LAZY_BIT) && (err = store_need_data(i))) return err; /* contents still in the store: wait for them */
        if ((flags & O_TRUNC) && acc != O_RDONLY && (err = reg_truncate(i, 0))) return err;
        f = file_new(&reg_fops, i, flags, NULL);
      }
    } else return -ENXIO; /* FIFOs and sockets cannot be opened (named FIFOs: see VFS-NOTES.md) */
  }
  if (!f) return -ENOMEM;
  return install(f, flags);
}

long fs_openat(int dirfd, u32 upath, int flags, int mode) {
  char path[VFS_PATH_MAX] __attribute__((aligned(64)));
  long err = path_from_user(path, upath);
  if (err < 0) return err;
  if ((flags & O_TMPFILE) == O_TMPFILE) return -EOPNOTSUPP; /* callers fall back to a named file */
  if (flags & O_PATH) flags &= O_PATH | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY;
  int excl = (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
  struct nameidata nd;
  if ((err = at_walk(dirfd, path, (flags & O_NOFOLLOW) || excl ? 0 : W_FOLLOW, 0, &nd))) return err;
  struct inode *i = nd.ino;
  /* the common case: an existing file or directory, plain flags (with enforcement, the same
   * permission test open_nd makes) */
  if (i && !nd.trailing && !i->kind && !(i->sflags & S_LAZY_BIT) && !i->priv &&
      !(flags & (O_CREAT | O_TRUNC | O_PATH | O_DIRECTORY))) {
    struct file *f;
    int acc = flags & O_ACCMODE;
    if (!S_ISREG(i->mode) && !(S_ISDIR(i->mode) && acc == O_RDONLY)) return open_nd(&nd, flags, mode);
    if (vfs_enforce >= 2 && (err = vfs_may(i, acc == O_RDONLY ? 4 : acc == O_WRONLY ? 2 : 6))) return err;
    f = file_new(S_ISREG(i->mode) ? &reg_fops : &dir_fops, i, flags, NULL);
    return f ? install(f, flags) : -ENOMEM;
  }
  return open_nd(&nd, flags, mode);
}

/* ---- stat ---- */
static u32 dir_size(struct inode *i) { return i->dir ? 40 + 20 * i->dir->count : 0; } /* tmpfs's figure */
static void put_ts(struct timespec *t, s64 kt) { t->tv_sec = KT_SEC(kt); t->tv_nsec = KT_NSEC(kt); }
static u64 blocks(struct inode *i) {
  if (S_ISREG(i->mode) && i->priv) return remote_blocks(i); /* a remote file: what has arrived so far */
  return S_ISREG(i->mode) ? ((u64)i->size + 511) / 512 : S_ISDIR(i->mode) ? 8 : 0;
}
static void fill_stat(struct stat *st, struct inode *i) {
  memset(st, 0, sizeof *st);
  if (!i) { st->st_mode = S_IFIFO | 0600; st->st_blksize = 4096; return; } /* a pipe or a socket */
  st->st_dev = i->kind ? 2 : 1;
  st->st_ino = i->ino; st->st_mode = i->mode; st->st_nlink = i->nlink; st->st_uid = i->uid; st->st_gid = i->gid;
  st->st_rdev = S_ISCHR(i->mode) ? (u64)i->dev : 0;
  st->st_size = S_ISDIR(i->mode) ? dir_size(i) : i->size;
  st->st_blksize = 4096;
  st->st_blocks = blocks(i);
  put_ts(&st->st_atim, i->atime); put_ts(&st->st_mtim, i->mtime); put_ts(&st->st_ctim, i->ctime);
}
long fs_fstat(int fd, u32 ust) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  struct stat st;
  fill_stat(&st, f->ino);
  return copy_to_user(ust, &st, sizeof st);
}
long fs_newfstatat(int dirfd, u32 upath, u32 ust, int flags) {
  char path[VFS_PATH_MAX] __attribute__((aligned(64)));
  struct nameidata nd;
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) return -EINVAL;
  long e = at_find(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), flags, path, &nd);
  if (e) return e;
  struct stat st;
  fill_stat(&st, nd.ino);
  return copy_to_user(ust, &st, sizeof st);
}
static void stx_time(struct statx_timestamp *t, s64 kt) { t->tv_sec = KT_SEC(kt); t->tv_nsec = KT_NSEC(kt); }
static long fs_statx(int dirfd, u32 upath, int flags, u32 mask, u32 ubuf) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  (void)mask; /* everything is always at hand; stx_mask says what was filled */
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE)) return -EINVAL;
  if ((flags & AT_STATX_SYNC_TYPE) == AT_STATX_SYNC_TYPE) return -EINVAL;
  long e = at_find(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), flags, path, &nd);
  if (e) return e;
  struct statx sx;
  memset(&sx, 0, sizeof sx);
  struct inode *i = nd.ino;
  sx.stx_mask = STATX_BASIC_STATS | STATX_MNT_ID;
  sx.stx_blksize = 4096;
  if (!i) { sx.stx_mode = S_IFIFO | 0600; sx.stx_nlink = 1; }
  else {
    sx.stx_nlink = i->nlink; sx.stx_uid = i->uid; sx.stx_gid = i->gid; sx.stx_mode = i->mode;
    sx.stx_ino = i->ino; sx.stx_size = S_ISDIR(i->mode) ? dir_size(i) : i->size; sx.stx_blocks = blocks(i);
    stx_time(&sx.stx_atime, i->atime); stx_time(&sx.stx_mtime, i->mtime); stx_time(&sx.stx_ctime, i->ctime);
    if (S_ISCHR(i->mode)) sx.stx_rdev_minor = i->dev;
    sx.stx_dev_minor = i->kind ? 2 : 1;
    sx.stx_mnt_id = sx.stx_dev_minor;
  }
  return copy_to_user(ubuf, &sx, sizeof sx);
}

/* ---- directories ---- */
static u8 dtype(struct inode *i) {
  switch (i->mode & S_IFMT) {
  case S_IFDIR: return DT_DIR; case S_IFREG: return DT_REG; case S_IFLNK: return DT_LNK;
  case S_IFCHR: return DT_CHR; case S_IFBLK: return DT_BLK; case S_IFIFO: return DT_FIFO; case S_IFSOCK: return DT_SOCK;
  }
  return DT_UNKNOWN;
}
static long fs_getdents64(int fd, u32 ubuf, u32 len) {
  struct file *f = fd_get(fd);
  if (!f || !f->ino || (f->flags & O_PATH)) return -EBADF;
  struct inode *d = f->ino;
  if (!S_ISDIR(d->mode)) return -ENOTDIR;
  u32 done = 0, off = f->off;
  union { struct dirent64 d; u8 raw[sizeof(struct dirent64) + VFS_NAME_MAX + 8]; } e;
  char nbuf[16];
  struct dentry *c = NULL;
  int seeked = 0;
  for (;;) {
    const char *name; u32 next, nl, ino; u8 type;
    if (off == 0) { name = "."; ino = d->ino; type = DT_DIR; next = 1; }
    else if (off == 1) { name = ".."; ino = d->dir->parent->ino; type = DT_DIR; next = 2; }
    else if (d->kind) {
      if (!procfs_list(d, off - 2, nbuf, &ino, &type)) break;
      name = nbuf; next = off + 1;
    } else {
      if (!seeked) { c = vfs_dir_seek(f, off); seeked = 1; }
      if (!c) break;
      name = c->name; ino = c->ino->ino; type = dtype(c->ino); next = c->seq + 3;
    }
    nl = strlen(name);
    u32 rec = (sizeof(struct dirent64) + nl + 1 + 7) & ~7u;
    if (done + rec > len) { if (!done) return -EINVAL; break; } /* as Linux: buffer too small for one entry */
    memset(&e, 0, rec);
    e.d.d_ino = ino; e.d.d_off = next; e.d.d_reclen = rec; e.d.d_type = type;
    memcpy(e.d.d_name, name, nl);
    if (copy_to_user(ubuf + done, &e, rec)) return -EFAULT;
    done += rec;
    off = next;
    if (c && off > 2 && !d->kind && name == c->name) c = c->next;
  }
  f->off = off;
  if (!d->kind && seeked) vfs_dir_remember(f, c, off);
  return done;
}
static long set_cwd(struct inode *i) {
  if (!S_ISDIR(i->mode)) return -ENOTDIR;
  int e = vfs_may(i, 1);
  if (e) return e;
  inode_get(i);
  inode_put(cur->cwd);
  cur->cwd = i;
  return 0;
}
static long fs_chdir(u32 upath) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  long e = at_find(AT_FDCWD, upath, 1, 0, path, &nd);
  if (e) return e;
  return nd.ino ? set_cwd(nd.ino) : -ENOTDIR;
}
static long fs_fchdir(int fd) {
  struct file *f = fd_get(fd);
  if (!f) return -EBADF;
  return f->ino ? set_cwd(f->ino) : -ENOTDIR;
}
static long fs_chroot(u32 upath) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  if (!privileged()) return -EPERM;
  long e = at_find(AT_FDCWD, upath, 1, 0, path, &nd);
  if (e) return e;
  if (!nd.ino || !S_ISDIR(nd.ino->mode)) return -ENOTDIR;
  struct fscred *c = cred();
  extern int vfs_any_chroot;
  vfs_any_chroot = 1;
  inode_get(nd.ino);
  if (c->root) inode_put(c->root);
  c->root = nd.ino == vfs_root ? (inode_put(nd.ino), (struct inode *)NULL) : nd.ino;
  return 0;
}
static long fs_getcwd(u32 ubuf, u32 cap) {
  char path[VFS_PATH_MAX];
  int n = vfs_path(cur->cwd, path, sizeof path);
  if (n < 0) return n;
  if ((u32)n > cap) return -ERANGE;
  return copy_to_user(ubuf, path, n) ? -EFAULT : n;
}

/* ---- names ---- */
static long create_at(int dirfd, u32 upath, u32 mode) {
  char path[VFS_PATH_MAX];
  long err = path_from_user(path, upath);
  if (err < 0) return err;
  struct nameidata nd;
  if ((err = at_walk(dirfd, path, 0, 0, &nd))) return err;
  int e;
  return vfs_mknod_at(&nd, mode, &e) ? 0 : e;
}
static long fs_mknodat(int dirfd, u32 upath, u32 mode, u32 dev) {
  (void)dev;
  u32 type = mode & S_IFMT;
  if (type == S_IFCHR || type == S_IFBLK) return -EPERM; /* device nodes come from the kernel only */
  if (type == S_IFDIR) return -EPERM;
  if (type == 0) type = S_IFREG;
  if (type != S_IFREG && type != S_IFIFO && type != S_IFSOCK) return -EINVAL;
  return create_at(dirfd, upath, type | (mode & 07777 & ~cred()->umask));
}
static long fs_unlinkat(int dirfd, u32 upath, int flags) {
  if (flags & ~AT_REMOVEDIR) return -EINVAL;
  char path[VFS_PATH_MAX];
  long err = path_from_user(path, upath);
  if (err < 0) return err;
  struct nameidata nd;
  if ((err = at_walk(dirfd, path, W_NOTRAIL, 0, &nd))) return err;
  return vfs_unlink_at(&nd, !!(flags & AT_REMOVEDIR));
}
static long fs_renameat2(int fd1, u32 ufrom, int fd2, u32 uto, int flags) {
  char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
  long err = path_from_user(from, ufrom);
  if (err < 0) return err;
  if ((err = path_from_user(to, uto)) < 0) return err;
  struct nameidata a, b;
  if ((err = at_walk(fd1, from, W_NOTRAIL, 0, &a)) || (err = at_walk(fd2, to, W_NOTRAIL, 0, &b))) return err;
  return vfs_rename_nd(&a, &b, flags);
}
static long fs_linkat(int fd1, u32 uold, int fd2, u32 unew, int flags) {
  if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) return -EINVAL;
  char oldp[VFS_PATH_MAX], newp[VFS_PATH_MAX];
  struct nameidata a, b;
  long err = at_find(fd1, uold, !!(flags & AT_SYMLINK_FOLLOW), flags, oldp, &a);
  if (err) return err;
  if (!a.ino) return -EXDEV; /* a pipe or socket reached through /proc */
  if ((err = path_from_user(newp, unew)) < 0) return err;
  if ((err = at_walk(fd2, newp, 0, 0, &b))) return err;
  return vfs_link_at(a.ino, &b);
}
static long fs_symlinkat(u32 utarget, int dirfd, u32 upath) {
  char target[VFS_PATH_MAX], path[VFS_PATH_MAX];
  long err = path_from_user(target, utarget);
  if (err < 0) return err;
  if ((err = path_from_user(path, upath)) < 0) return err;
  struct nameidata nd;
  if ((err = at_walk(dirfd, path, 0, 0, &nd))) return err;
  return vfs_symlink_at(target, &nd);
}
static long fs_readlinkat(int dirfd, u32 upath, u32 ubuf, int size) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  if (size <= 0) return -EINVAL;
  long e = at_find(dirfd, upath, 0, AT_EMPTY_PATH, path, &nd);
  if (e) return e;
  if (!nd.ino || !S_ISLNK(nd.ino->mode)) return -EINVAL;
  char *t = nd.buf[0];
  int n = vfs_symlink_target(nd.ino, t, VFS_PATH_MAX);
  if (n < 0) return n;
  if (n > size) n = size;
  return copy_to_user(ubuf, t, n) ? -EFAULT : n;
}

/* ---- sizes ---- */
static long fs_ftruncate(int fd, long size) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_PATH)) return -EBADF;
  if (!f->ino || !S_ISREG(f->ino->mode) || f->ino->kind || (f->flags & O_ACCMODE) == O_RDONLY || size < 0) return -EINVAL;
  return reg_truncate(f->ino, size);
}
static long fs_truncate(u32 upath, long size) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  long e = at_find(AT_FDCWD, upath, 1, 0, path, &nd);
  if (e) return e;
  struct inode *i = nd.ino;
  if (!i) return -EINVAL;
  if (S_ISDIR(i->mode)) return -EISDIR;
  if (!S_ISREG(i->mode) || i->kind || size < 0) return -EINVAL;
  if ((e = vfs_may(i, 2))) return e;
  if ((e = store_need_data(i))) return e;
  if (S_ISREG(i->mode) && i->priv && (e = remote_open(i, O_RDONLY))) return e; /* fetch a remote file before cutting it */
  return reg_truncate(i, size);
}
static long fs_fallocate(int fd, int mode, long off, long len) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_ACCMODE) == O_RDONLY || (f->flags & O_PATH)) return -EBADF;
  if (off < 0 || len <= 0) return -EINVAL;
  if (!f->ino || S_ISDIR(f->ino->mode)) return -EISDIR;
  if (!S_ISREG(f->ino->mode)) return -ENODEV;
  if (mode) return -EOPNOTSUPP; /* only plain allocation: no holes to punch, files are never sparse */
  u64 end = (u64)(u32)off + (u32)len;
  if (end > 0x7fffffff) return -EFBIG;
  return end > f->ino->size ? reg_truncate(f->ino, (u32)end) : 0;
}

/* ---- modes, owners, times ---- */
static long do_chmod(struct inode *i, u32 mode) {
  if (!owner_or_root(i)) return -EPERM;
  if (i->kind) return -EPERM;
  mode &= 07777;
  if (vfs_enforce >= 2 && cred()->fsuid && !S_ISDIR(i->mode)) { /* as Linux: set-group-ID only for a group the caller is in */
    struct fscred *c = cred();
    int member = c->fsgid == i->gid;
    for (u32 k = 0; k < c->ngroups && !member; k++) member = c->groups[k] == i->gid;
    if (!member) mode &= ~S_ISGID;
  }
  i->mode = (i->mode & S_IFMT) | mode;
  vfs_touch(i, 4);
  return 0;
}
static long fs_fchmod(int fd, u32 mode) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_PATH)) return -EBADF;
  if (!f->ino) return -EINVAL;
  return do_chmod(f->ino, mode);
}
static long fs_fchmodat(int dirfd, u32 upath, u32 mode, int flags) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
  long e = at_find(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), flags, path, &nd);
  if (e) return e;
  if (!nd.ino) return -EINVAL;
  if (S_ISLNK(nd.ino->mode)) return -EOPNOTSUPP; /* a link's own mode cannot change, as on Linux */
  return do_chmod(nd.ino, mode);
}
static long do_chown(struct inode *i, u32 uid, u32 gid) {
  if (i->kind) return -EPERM;
  if (vfs_enforce >= 2 && cred()->fsuid) {
    /* as Linux: only root gives a file away; the owner may change its group to one of
     * the groups the owner is in (the effective group or a supplementary one) */
    struct fscred *c = cred();
    if (uid != (u32)-1 && (uid != i->uid || c->fsuid != i->uid)) return -EPERM;
    if (gid != (u32)-1) {
      if (c->fsuid != i->uid) return -EPERM;
      int member = gid == c->fsgid;
      for (u32 k = 0; k < c->ngroups && !member; k++) member = c->groups[k] == gid;
      if (!member && gid != i->gid) return -EPERM;
    }
  }
  if (uid != (u32)-1) i->uid = uid;
  if (gid != (u32)-1) i->gid = gid;
  /* as Linux: a change of owner clears set-user-ID, and set-group-ID on a group-executable file */
  if ((uid != (u32)-1 || gid != (u32)-1) && !S_ISDIR(i->mode) && !S_ISLNK(i->mode)) {
    i->mode &= ~S_ISUID;
    if (i->mode & 010) i->mode &= ~S_ISGID;
  }
  vfs_touch(i, 4);
  return 0;
}
static long fs_fchown(int fd, u32 uid, u32 gid) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_PATH)) return -EBADF;
  if (!f->ino) return -EINVAL;
  return do_chown(f->ino, uid, gid);
}
static long fs_fchownat(int dirfd, u32 upath, u32 uid, u32 gid, int flags) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
  long e = at_find(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), flags, path, &nd);
  if (e) return e;
  return nd.ino ? do_chown(nd.ino, uid, gid) : -EINVAL;
}
static long fs_utimensat(int dirfd, u32 upath, u32 utimes, int flags) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  struct inode *i;
  long e;
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
  if (!upath) { /* futimens: the descriptor itself */
    if (dirfd == AT_FDCWD) return -EFAULT;
    struct file *f = fd_get(dirfd);
    if (!f) return -EBADF;
    i = f->ino;
  } else {
    if ((e = at_find(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), flags, path, &nd))) return e;
    i = nd.ino;
  }
  if (!i) return -EINVAL;
  struct timespec ts[2];
  s64 now = vfs_now(), t[2] = {now, now};
  int set[2] = {1, 1}, explicit_ = 0;
  if (utimes) {
    if (copy_from_user(ts, utimes, sizeof ts)) return -EFAULT;
    for (int k = 0; k < 2; k++) {
      if (ts[k].tv_nsec == UTIME_OMIT) set[k] = 0;
      else if (ts[k].tv_nsec == UTIME_NOW) ;
      else if (ts[k].tv_nsec < 0 || ts[k].tv_nsec > 999999999) return -EINVAL;
      else { t[k] = KT_MAKE(ts[k].tv_sec, ts[k].tv_nsec); explicit_ = 1; }
    }
  }
  if (!set[0] && !set[1]) return 0;
  if (vfs_enforce >= 2 && !owner_or_root(i) && (explicit_ || vfs_may(i, 2))) return explicit_ ? -EPERM : -EACCES;
  if (set[0]) i->atime = t[0];
  if (set[1]) i->mtime = t[1];
  i->ctime = now;
  vfs_changed(i);
  return 0;
}
/* access(2): the answer follows the enforcement switch, so it says what open and spawn will do */
static long fs_faccessat(int dirfd, u32 upath, int mode, int flags) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  if (mode & ~7) return -EINVAL;
  if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) return -EINVAL;
  long e = at_find(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), flags, path, &nd);
  if (e || !mode || !nd.ino) return e;
  struct inode *i = nd.ino;
  if ((mode & 1) && !S_ISDIR(i->mode) && vfs_enforce >= 1 && !(i->mode & 0111)) return -EACCES;
  if (vfs_enforce < 2) return 0;
  struct fscred *c = cred();
  u32 saved = c->fsuid, savedg = c->fsgid;
  if (!(flags & AT_EACCESS)) { c->fsuid = c->uid; c->fsgid = c->gid; }
  e = vfs_may(i, mode);
  c->fsuid = saved; c->fsgid = savedg;
  return e;
}

/* ---- credentials: one user (uid 1000) unless a program changes them ---- */
static long put3(u32 a, u32 b, u32 c, u32 ua, u32 ub, u32 uc) {
  if (copy_to_user(ua, &a, 4) || copy_to_user(ub, &b, 4) || copy_to_user(uc, &c, 4)) return -EFAULT;
  return 0;
}
static long setres(int gid, u32 r, u32 e, u32 s) {
  struct fscred *c = cred();
  u32 *R = gid ? &c->gid : &c->uid, *E = gid ? &c->egid : &c->euid, *S = gid ? &c->sgid : &c->suid, *F = gid ? &c->fsgid : &c->fsuid;
  if (!privileged()) {
    u32 v[3] = {r, e, s};
    for (int k = 0; k < 3; k++) if (v[k] != (u32)-1 && v[k] != *R && v[k] != *E && v[k] != *S) return -EPERM;
  }
  if (r != (u32)-1) *R = r;
  if (e != (u32)-1) *E = e;
  if (s != (u32)-1) *S = s;
  *F = *E;
  return 0;
}
static long setre(int gid, u32 r, u32 e) {
  struct fscred *c = cred();
  u32 oldr = gid ? c->gid : c->uid;
  long x = setres(gid, r, e, (u32)-1);
  if (x) return x;
  if (r != (u32)-1 || (e != (u32)-1 && e != oldr)) { if (gid) c->sgid = c->egid; else c->suid = c->euid; }
  return 0;
}
static long set1(int gid, u32 v) {
  if (privileged()) return setres(gid, v, v, v);
  return setres(gid, (u32)-1, v, (u32)-1);
}
static long fs_setgroups(u32 n, u32 ulist) {
  if (!privileged()) return -EPERM;
  if (n > 16) return -EINVAL;
  u32 g[16];
  if (n && copy_from_user(g, ulist, n * 4)) return -EFAULT;
  memcpy(cred()->groups, g, n * 4);
  cred()->ngroups = n;
  return 0;
}
static long fs_getgroups(int n, u32 ulist) {
  struct fscred *c = cred();
  if (n < 0) return -EINVAL;
  if (!n) return c->ngroups;
  if ((u32)n < c->ngroups) return -EINVAL;
  return copy_to_user(ulist, c->groups, c->ngroups * 4) ? -EFAULT : (long)c->ngroups;
}

/* ---- locks: flock(2) whole-file locks and fcntl(2) record locks, advisory ---- */
struct lock {
  struct lock *next;
  u8 type;        /* F_RDLCK or F_WRLCK */
  u8 flock;       /* a flock(2) lock: owned by the open file, conflicts only with other flock locks */
  struct file *ofile; /* flock and open-file-description locks: the owner */
  int pid; u32 gen;   /* POSIX record locks: the owning process */
  u64 start, end;     /* [start, end) */
};
static waitq lock_wq;
/* Inodes that carry locks, so a process that ends can have its record locks removed at once. */
static struct inode **locked;
static u32 nlocked, caplocked;
static void track(struct inode *i) {
  for (u32 k = 0; k < nlocked; k++) if (locked[k] == i) return;
  if (nlocked == caplocked) {
    u32 cap = caplocked ? caplocked * 2 : 16;
    struct inode **n = krealloc(locked, cap * sizeof *n);
    if (!n) return; /* only means a dead process's locks are pruned later, lazily */
    locked = n; caplocked = cap;
  }
  locked[nlocked++] = i;
}
static void untrack(struct inode *i) {
  for (u32 k = 0; k < nlocked; k++) if (locked[k] == i) { locked[k] = locked[--nlocked]; break; }
  if (!nlocked && locked) { kfree(locked); locked = NULL; caplocked = 0; }
}
static int lock_valid(struct lock *l) { return l->ofile || (procs[l->pid].state == P_RUN && fscred[l->pid].gen == l->gen); }
static int same_owner(struct lock *l, struct lock *m) { return l->flock == m->flock && (l->ofile ? l->ofile == m->ofile : (!m->ofile && l->pid == m->pid && l->gen == m->gen)); }
void locks_free(struct inode *i) { while (i->locks) { struct lock *l = i->locks; i->locks = l->next; kfree(l); } if (nlocked) untrack(i); }
static int prune(struct inode *i) { /* drop record locks of processes that are gone */
  int changed = 0;
  for (struct lock **pp = &i->locks; *pp;) {
    if (!lock_valid(*pp)) { struct lock *l = *pp; *pp = l->next; kfree(l); changed = 1; }
    else pp = &(*pp)->next;
  }
  if (!i->locks) untrack(i);
  return changed;
}
void locks_proc_end(void) { /* a process slot was freed: its record locks go now, not at the next lock call */
  int changed = 0;
  for (u32 k = nlocked; k-- > 0;) changed |= prune(locked[k]);
  if (changed) wq_wake(&lock_wq);
}
static struct lock *conflict(struct inode *i, struct lock *want) {
  for (struct lock *l = i->locks; l; l = l->next)
    if (l->flock == want->flock && !same_owner(l, want) && l->start < want->end && want->start < l->end &&
        (l->type == F_WRLCK || want->type == F_WRLCK)) return l;
  return NULL;
}
/* Remove the owner's locks in [start, end), splitting those that stick out. */
static int unlock_range(struct inode *i, struct lock *who) {
  int changed = 0;
  for (struct lock **pp = &i->locks; *pp;) {
    struct lock *l = *pp;
    if (!same_owner(l, who) || l->end <= who->start || who->end <= l->start) { pp = &l->next; continue; }
    changed = 1;
    if (l->start < who->start && l->end > who->end) { /* split in two */
      struct lock *r = kmalloc(sizeof *r);
      if (r) { *r = *l; r->start = who->end; r->next = l->next; l->next = r; }
      l->end = who->start;
      pp = &l->next;
    } else if (l->start < who->start) { l->end = who->start; pp = &l->next; }
    else if (l->end > who->end) { l->start = who->end; pp = &l->next; }
    else { *pp = l->next; kfree(l); }
  }
  return changed;
}
/* Would cur waiting for the owner of l close a circle of processes waiting for each other's
 * record locks? Linux answers EDEADLK then; flock and OFD waits are not checked, as on Linux. */
static int deadlock(struct lock *l) {
  if (l->ofile) return 0;
  int p = l->pid;
  for (int steps = 0; p > 0 && steps < NPROC; steps++) {
    if (p == cur->pid) return 1;
    if (procs[p].state != P_RUN) return 0;
    /* still in that wait only if it made no system call since: a wait that a signal
     * interrupted (EINTR) returns without coming back here to clear lockwait */
    if (procs[p].nsys != fscred[p].lockwait_nsys) return 0;
    p = fscred[p].lockwait;
  }
  return 0;
}
extern const struct fops reg_lk_fops;
static int lockable(struct file *f) { return f->ino && !f->ino->kind && (f->ops == &reg_fops || f->ops == &reg_lk_fops || f->ops == &dir_fops); }
static long set_lock(struct inode *i, struct lock *want, int wait) {
  if (want->ofile && want->type != F_UNLCK && want->ofile->ops == &reg_fops) want->ofile->ops = &reg_lk_fops; /* its close must drop the lock */
  prune(i);
  cred()->lockwait = 0;
  if (want->type != F_UNLCK) {
    struct lock *l = conflict(i, want);
    if (l) {
      if (!wait) return -EAGAIN;
      if (deadlock(l)) return -EDEADLK;
      if (!l->ofile) { cred()->lockwait = l->pid; cred()->lockwait_nsys = cur->nsys; }
      wq_add(&lock_wq);
      return block(-1); /* every unlock, close and process end wakes the queue */
    }
  }
  struct lock *n = NULL;
  if (want->type != F_UNLCK && !(n = kmalloc(sizeof *n))) return -ENOLCK;
  if (unlock_range(i, want)) wq_wake(&lock_wq);
  if (n) { *n = *want; n->next = i->locks; i->locks = n; track(i); }
  else if (!i->locks) untrack(i);
  return 0;
}
static long fs_flock(int fd, int op) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_PATH)) return -EBADF;
  if (!f->ino) return -EINVAL;
  int how = op & ~LOCK_NB;
  /* only files whose close drops their locks: regular files and directories of the tree, not
   * devices or /proc's generated files (a lock there would outlive its owner) */
  if (!lockable(f)) return how == LOCK_SH || how == LOCK_EX || how == LOCK_UN ? 0 : -EINVAL;
  struct lock w = {0};
  w.flock = 1; w.ofile = f; w.start = 0; w.end = ~0ull;
  if (how == LOCK_UN) w.type = F_UNLCK;
  else if (how == LOCK_SH) w.type = F_RDLCK;
  else if (how == LOCK_EX) w.type = F_WRLCK;
  else return -EINVAL;
  return set_lock(f->ino, &w, !(op & LOCK_NB));
}
void flock_release(struct file *f) { /* the open file is going away: its flock and OFD locks go too */
  struct inode *i = f->ino;
  int changed = 0;
  for (struct lock **pp = &i->locks; *pp;) {
    if ((*pp)->ofile == f) { struct lock *l = *pp; *pp = l->next; kfree(l); changed = 1; }
    else pp = &(*pp)->next;
  }
  if (!i->locks) untrack(i);
  if (changed) wq_wake(&lock_wq);
}
void fs_close_fd(struct file *f) { /* POSIX: any close by the process drops its record locks on the file */
  struct inode *i = f->ino;
  if (!i || !i->locks) return;
  struct lock w = {0};
  w.pid = cur->pid; w.gen = fscred[cur->pid].gen; w.start = 0; w.end = ~0ull;
  if (unlock_range(i, &w)) wq_wake(&lock_wq);
  if (!i->locks) untrack(i);
}
long fs_fcntl_lock(struct file *f, int cmd, u32 uarg) {
  int big = cmd >= F_GETLK64 && cmd <= F_SETLKW64, ofd = cmd >= F_OFD_GETLK;
  struct flock64 fl;
  if (big || ofd) { if (copy_from_user(&fl, uarg, sizeof fl)) return -EFAULT; }
  else {
    struct flock s;
    if (copy_from_user(&s, uarg, sizeof s)) return -EFAULT;
    fl.l_type = s.l_type; fl.l_whence = s.l_whence; fl.l_start = s.l_start; fl.l_len = s.l_len; fl.l_pid = s.l_pid;
  }
  if (!f->ino || (f->flags & O_PATH)) return -EBADF;
  if (!lockable(f)) return -EINVAL; /* as Linux for pipes; devices and /proc files are refused too */
  int get = cmd == F_GETLK || cmd == F_GETLK64 || cmd == F_OFD_GETLK;
  int wait = cmd == F_SETLKW || cmd == F_SETLKW64 || cmd == F_OFD_SETLKW;
  if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK && fl.l_type != F_UNLCK) return -EINVAL;
  if (ofd && fl.l_pid) return -EINVAL;
  if (!get && fl.l_type == F_RDLCK && (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
  if (!get && fl.l_type == F_WRLCK && (f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
  s64 base = fl.l_whence == SEEK_SET ? 0 : fl.l_whence == SEEK_CUR ? (s64)f->off : fl.l_whence == SEEK_END ? (s64)f->ino->size : -1;
  if (base < 0) return -EINVAL;
  s64 start = base + fl.l_start, end;
  if (fl.l_len > 0) end = start + fl.l_len;
  else if (fl.l_len == 0) end = -1; /* to the end of the file, however far it grows */
  else { end = start; start += fl.l_len; }
  if (start < 0) return -EINVAL;
  struct lock w = {0};
  w.type = fl.l_type; w.start = start; w.end = end < 0 ? ~0ull : (u64)end;
  if (ofd) w.ofile = f;
  else { w.pid = cur->pid; w.gen = fscred[cur->pid].gen; }
  if (!get) return set_lock(f->ino, &w, wait);
  prune(f->ino);
  struct lock *l = w.type == F_UNLCK ? NULL : conflict(f->ino, &w);
  if (!l) fl.l_type = F_UNLCK;
  else {
    fl.l_type = l->type; fl.l_whence = SEEK_SET; fl.l_start = l->start;
    fl.l_len = l->end == ~0ull ? 0 : (s64)(l->end - l->start);
    fl.l_pid = l->ofile ? -1 : l->pid;
  }
  if (big || ofd) return copy_to_user(uarg, &fl, sizeof fl);
  struct flock s = {fl.l_type, fl.l_whence, (long)fl.l_start, (long)fl.l_len, fl.l_pid};
  if (fl.l_start > 0x7fffffff || fl.l_len > 0x7fffffff) return -EOVERFLOW;
  return copy_to_user(uarg, &s, sizeof s);
}

/* ---- statfs: what the file system can still hold ---- */
static long put_statfs(struct inode *i, u32 ubuf) {
  struct statfs s;
  memset(&s, 0, sizeof s);
  u64 total, free;
  store_statfs(&total, &free);
  s.f_type = i && i->kind ? PROC_SUPER_MAGIC : UCOSFS_MAGIC;
  s.f_bsize = s.f_frsize = 4096;
  s.f_blocks = total / 4096; s.f_bfree = s.f_bavail = free / 4096;
  s.f_files = vfs_ninodes + free / 256; s.f_ffree = free / 256; /* no inode table: each costs memory */
  s.f_fsid[0] = i && i->kind ? 2 : 1;
  s.f_namelen = VFS_NAME_MAX;
  s.f_flags = 0x400; /* ST_RELATIME */
  return copy_to_user(ubuf, &s, sizeof s);
}
static long fs_statfs(u32 upath, u32 ubuf) {
  char path[VFS_PATH_MAX];
  struct nameidata nd;
  long e = at_find(AT_FDCWD, upath, 1, 0, path, &nd);
  return e ? e : put_statfs(nd.ino, ubuf);
}
static long fs_fstatfs(int fd, u32 ubuf) {
  struct file *f = fd_get(fd);
  return f ? put_statfs(f->ino, ubuf) : -EBADF;
}

/* ---- vectored I/O ---- */
static long rw_vec(int wr, int fd, u32 uiov, int cnt, int positioned, u32 pos) {
  struct file *f = fd_get(fd);
  if (!f || (f->flags & O_PATH)) return -EBADF;
  if ((f->flags & O_ACCMODE) == (wr ? O_RDONLY : O_WRONLY)) return -EBADF;
  long (*op)(struct file *, u32, u32) = wr ? f->ops->write : f->ops->read;
  if (!op) return (!wr && f->ino && S_ISDIR(f->ino->mode)) ? -EISDIR : -EINVAL;
  if (cnt < 0 || cnt > 1024) return -EINVAL;
  if (positioned && !f->ino) return -ESPIPE;
  u32 saved = f->off;
  int app = f->flags & O_APPEND;
  if (positioned) { f->off = pos; f->flags &= ~O_APPEND; }
  long done = 0;
  struct iovec v[16];
  u64 total = 0;
  for (int k = 0; k < cnt; k++) { /* check every length first, as Linux does */
    if (k % 16 == 0 && copy_from_user(v, uiov + k * sizeof *v, (cnt - k < 16 ? cnt - k : 16) * sizeof *v)) { done = -EFAULT; goto out; }
    total += v[k % 16].iov_len;
  }
  if (total > 0x7fffffff) { done = -EINVAL; goto out; }
  for (int k = 0; k < cnt; k++) {
    if (k % 16 == 0) copy_from_user(v, uiov + k * sizeof *v, (cnt - k < 16 ? cnt - k : 16) * sizeof *v);
    u32 len = v[k % 16].iov_len;
    if (!len) continue;
    long r = op(f, (u32)(usize)v[k % 16].iov_base, len);
    if (r < 0) {
      if (done && r == -ERESTARTSYS) { cur->sleeping = 0; cur->block_ms = -1; } /* report what was done instead of waiting */
      if (!done) done = r;
      break;
    }
    done += r;
    if ((u32)r < len) break;
  }
out:
  if (positioned) { f->off = saved; f->flags |= app; }
  return done;
}

/* ---- the dispatcher ---- */
long fs_syscall(long nr, long a, long b, long c, long d, long e) {
  switch (nr) {
  case SYS_openat: return fs_openat(a, b, c, d);
  case SYS_fstat: return fs_fstat(a, b);
  case SYS_newfstatat: return fs_newfstatat(a, b, c, d);
  case SYS_statx: return fs_statx(a, b, c, d, e);
  case SYS_getdents64: return fs_getdents64(a, b, c);
  case SYS_chdir: return fs_chdir(a);
  case SYS_fchdir: return fs_fchdir(a);
  case SYS_chroot: return fs_chroot(a);
  case SYS_getcwd: return fs_getcwd(a, b);
  case SYS_mkdirat: return create_at(a, b, S_IFDIR | (c & 01777 & ~cred()->umask));
  case SYS_mknodat: return fs_mknodat(a, b, c, d);
  case SYS_unlinkat: return fs_unlinkat(a, b, c);
  case SYS_renameat: return fs_renameat2(a, b, c, d, 0);
  case SYS_renameat2: return fs_renameat2(a, b, c, d, e);
  case SYS_linkat: return fs_linkat(a, b, c, d, e);
  case SYS_symlinkat: return fs_symlinkat(a, b, c);
  case SYS_readlinkat: return fs_readlinkat(a, b, c, d);
  case SYS_ftruncate: return fs_ftruncate(a, b);
  case SYS_truncate: return fs_truncate(a, b);
  case SYS_fallocate: return fs_fallocate(a, b, c, d);
  case SYS_fchmod: return fs_fchmod(a, b);
  case SYS_fchmodat: return fs_fchmodat(a, b, c, 0);
  case SYS_fchmodat2: return fs_fchmodat(a, b, c, d);
  case SYS_fchown: return fs_fchown(a, b, c);
  case SYS_fchownat: return fs_fchownat(a, b, c, d, e);
  case SYS_utimensat: return fs_utimensat(a, b, c, d);
  case SYS_faccessat: return fs_faccessat(a, b, c, 0);
  case SYS_faccessat2: return fs_faccessat(a, b, c, d);
  case SYS_umask: { u32 old = cred()->umask; cred()->umask = a & 0777; return old; }
  case SYS_getuid: return cred()->uid;
  case SYS_geteuid: return cred()->euid;
  case SYS_getgid: return cred()->gid;
  case SYS_getegid: return cred()->egid;
  case SYS_setuid: return set1(0, a);
  case SYS_setgid: return set1(1, a);
  case SYS_setreuid: return setre(0, a, b);
  case SYS_setregid: return setre(1, a, b);
  case SYS_setresuid: return setres(0, a, b, c);
  case SYS_setresgid: return setres(1, a, b, c);
  case SYS_getresuid: return put3(cred()->uid, cred()->euid, cred()->suid, a, b, c);
  case SYS_getresgid: return put3(cred()->gid, cred()->egid, cred()->sgid, a, b, c);
  case SYS_setfsuid: { u32 old = cred()->fsuid; if (privileged() || (u32)a == cred()->uid || (u32)a == cred()->euid || (u32)a == cred()->suid) cred()->fsuid = a; return old; }
  case SYS_setfsgid: { u32 old = cred()->fsgid; if (privileged() || (u32)a == cred()->gid || (u32)a == cred()->egid || (u32)a == cred()->sgid) cred()->fsgid = a; return old; }
  case SYS_getgroups: return fs_getgroups(a, b);
  case SYS_setgroups: return fs_setgroups(a, b);
  case SYS_flock: return fs_flock(a, b);
  case SYS_fsync: case SYS_fdatasync: case SYS_syncfs: {
    struct file *f = fd_get(a);
    if (!f) return -EBADF;
    return store_sync(f, nr == SYS_syncfs);
  }
  case SYS_sync: return store_sync(NULL, 1);
  case SYS_statfs: return fs_statfs(a, b);
  case SYS_fstatfs: return fs_fstatfs(a, b);
  case SYS_readv: return rw_vec(0, a, b, c, 0, 0);
  case SYS_writev: return rw_vec(1, a, b, c, 0, 0);
  case SYS_preadv: return rw_vec(0, a, b, c, 1, d);
  case SYS_pwritev: return rw_vec(1, a, b, c, 1, d);
  }
  return -ENOSYS; /* sendfile, copy_file_range and the rest: callers fall back to read and write */
}
