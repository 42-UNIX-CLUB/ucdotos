// SPDX-License-Identifier: GPL-2.0-or-later
/* In-memory file system. The initial contents come from a tar archive the boot loader
 * hands to the kernel (an initrd); file data stays in place inside the archive until a
 * file is written to.
 *
 * An inode is the object (type, mode, owner, times, data). A directory entry (dentry) is
 * one name for an inode in one directory, so a file may have several names (hard links).
 * Each directory keeps its entries in a hash table for lookups and in a list for
 * listings, in the order they were added. */
#include "kernel.h"

struct inode *vfs_root;
static u32 next_ino = 1;
u32 vfs_ninodes;
int vfs_enforce = VFS_ENFORCE_DEFAULT;
struct fscred fscred[NPROC]; /* fscred[0]: the kernel itself, root */
void locks_free(struct inode *i);
void locks_proc_end(void);
void store_forget(struct inode *i);
void store_ns(int op, struct inode *dir, const char *name, u32 n, struct inode *i);
void store_rename(struct inode *odir, const char *on, u32 onl, struct inode *i, struct inode *ndir, const char *nn, u32 nnl, struct inode *old, int exchange);

/* File times: the wall clock at boot plus the monotonic clock since (performance.now steps
 * by 0.1 ms in Chrome, finer under Node; Date.now only by 1 ms), and strictly increasing, so
 * two changes in a row never get the same stamp (make compares them). */
s64 vfs_now(void) {
  static double base;
  static s64 last;
  double mono = host_now();
  if (!base) base = host_epoch() - mono;
  double ms = base + mono;
  s64 sec = (s64)(ms / 1000.0);
  s64 ns = (s64)((ms - (double)sec * 1000.0) * 1e6);
  if (ns < 0) ns = 0; else if (ns > 999999999) ns = 999999999;
  s64 t = KT_MAKE(sec, ns);
  if (t <= last) { t = last + 1; if (KT_NSEC(t) > 999999999) t = KT_MAKE(KT_SEC(t) + 1, 0); }
  return last = t;
}
static struct fscred *cred(void) { return cur ? &fscred[cur->pid] : &fscred[0]; }
int vfs_any_chroot; /* set once any process calls chroot: until then the root is always vfs_root */
struct inode *vfs_cred_root(void) { if (!vfs_any_chroot) return vfs_root; struct inode *r = cred()->root; return r ? r : vfs_root; }

/* ---- credentials ---- */
void vfs_proc_start(int pid, int parent, struct inode *exe) {
  struct fscred *c = &fscred[pid];
  struct inode *oldroot = c->root, *oldexe = c->exe;
  u32 gen = c->gen;
  if (parent > 0) *c = fscred[parent];
  else { memset(c, 0, sizeof *c); c->umask = 022; } /* the first process (init) is root, uid 0 gid 0 */
  c->gen = gen + 1; /* record locks name their owner by pid and generation */
  c->sync_target = 0;
  c->lockwait = 0;
  c->exe = exe;
  if (exe) inode_get(exe);
  if (c->root) inode_get(c->root);
  if (oldroot) inode_put(oldroot);
  if (oldexe) inode_put(oldexe);
}
/* execve: same process, new program. Credentials, umask, chroot and record locks stay
 * (POSIX keeps record locks across exec, so the lock generation is not bumped, unlike
 * vfs_proc_start); only the program /proc/PID/exe names changes. */
void vfs_proc_exec(int pid, struct inode *exe) {
  struct fscred *c = &fscred[pid];
  struct inode *oldexe = c->exe;
  c->exe = exe;
  if (exe) inode_get(exe);
  if (oldexe) inode_put(oldexe);
}
/* At spawn and execve, after the credentials are in place: a set-user-ID or set-group-ID
 * program runs with its owner's (group's) id as the effective and saved id, as on Linux,
 * but only in a session entitled to administration (ucos.admin=1). Elsewhere the bits are
 * ignored, as on a file system mounted nosuid: in an entitled session su and sudo can make
 * anyone root anyway, so honouring the bits there gives no power the session lacks; in an
 * unentitled one they are the only way up, and stay shut. Then, as Linux does at every
 * exec, the saved ids become the effective ones. */
int vfs_admin;
void vfs_exec_ids(int pid, struct inode *exe) {
  struct fscred *c = &fscred[pid];
  if (exe && vfs_admin && vfs_enforce >= 2) {
    if (exe->mode & S_ISUID) c->euid = c->fsuid = exe->uid;
    if ((exe->mode & (S_ISGID | 010)) == (S_ISGID | 010)) c->egid = c->fsgid = exe->gid; /* set-group-ID without group execute means mandatory locking, not this */
  }
  c->suid = c->euid; c->sgid = c->egid;
}
void vfs_proc_end(int pid) {
  struct fscred *c = &fscred[pid];
  if (c->root) inode_put(c->root);
  if (c->exe) inode_put(c->exe);
  c->root = c->exe = NULL;
  procfs_forget(pid);
  locks_proc_end();
}
static int in_group(struct fscred *c, u32 g) {
  if (c->fsgid == g) return 1;
  for (u32 k = 0; k < c->ngroups; k++) if (c->groups[k] == g) return 1;
  return 0;
}
/* mask: 4 read, 2 write, 1 execute or search. Root passes everything except execute,
 * which needs at least one x bit (on a non-directory), as on Linux. */
int vfs_may(struct inode *i, int mask) {
  if (vfs_enforce < 2) return 0;
  struct fscred *c = cred();
  if (c->fsuid == 0) return (mask & 1) && !S_ISDIR(i->mode) && !(i->mode & 0111) ? -EACCES : 0;
  u32 m = i->mode;
  if (c->fsuid == i->uid) m >>= 6;
  else if (in_group(c, i->gid)) m >>= 3;
  return (m & mask & 7) == (u32)(mask & 7) ? 0 : -EACCES;
}
int vfs_may_exec(struct inode *i) {
  long l = store_need_data(i); /* -ERESTARTSYS while the contents load from the store */
  if (l) return l;
  if (S_ISREG(i->mode) && i->priv && (l = remote_open(i, O_RDONLY))) return l; /* a remote program: fetch it first (spawn reads its bytes) */
  if (vfs_enforce >= 2) return vfs_may(i, 1);
  if (vfs_enforce >= 1 && !(i->mode & 0111)) return -EACCES;
  return 0;
}

/* ---- inodes ---- */
static struct inode *inode_alloc(u32 mode) {
  /* a directory's dirdata lives right after its inode, in the same block (DIRDATA) */
  struct inode *i = kzalloc(sizeof *i + (S_ISDIR(mode) ? sizeof(struct dirdata) : 0));
  if (!i) return NULL;
  if (S_ISDIR(mode)) { i->dir = DIRDATA(i); i->dir->parent = i; }
  struct fscred *c = cred();
  i->ino = next_ino++;
  vfs_ninodes++;
  i->mode = mode;
  i->uid = c->fsuid; i->gid = c->fsgid;
  i->atime = i->mtime = i->ctime = vfs_now();
  return i;
}
static void inode_free(struct inode *i) {
  vfs_ninodes--;
  store_forget(i);
  if (i->owned) kfree(i->data);
  if (i->dir) kfree(i->dir->tab); /* the dirdata itself is part of the inode block */
  locks_free(i); /* also forgets the inode in the lock bookkeeping */
  kfree(i);
}
void inode_drop(struct inode *i) { inode_free(i); }
void inode_get(struct inode *i) { i->refs++; }
void procfs_release(struct inode *i);
void inode_put(struct inode *i) {
  if (--i->refs) return;
  if (!i->nlink) inode_free(i);
  else if (i->kind) procfs_release(i); /* a /proc entry of a process that is gone */
}
void vfs_touch(struct inode *i, int what) {
  s64 t = vfs_now();
  if (what & 1) i->atime = t;
  if (what & 2) i->mtime = t;
  if (what & 4) i->ctime = t;
  vfs_changed(i);
}

/* ---- directory entries ---- */
static u32 name_hash(const char *s, u32 n) { /* FNV-1a */
  u32 h = 2166136261u;
  for (u32 k = 0; k < n; k++) h = (h ^ (u8)s[k]) * 16777619u;
  return h;
}
/* Directories of up to SMALL_DIR entries have no hash table: a lookup walks the list and
 * compares lengths, then bytes, which for a few entries is cheaper than hashing the name. */
#define SMALL_DIR 16
static inline int name_eq(const char *a, const char *b, u32 n) { for (u32 k = 0; k < n; k++) if (a[k] != b[k]) return 0; return 1; }
static inline __attribute__((always_inline)) struct dentry *d_lookup(struct inode *dir, const char *name, u32 n) {
  struct dirdata *dd = DIRDATA(dir);
  if (!dd->tab) {
    for (struct dentry *e = dd->first; e; e = e->next)
      if (e->nlen == n && e->name[0] == name[0] && name_eq(e->name, name, n)) return e;
    return NULL;
  }
  u32 h = name_hash(name, n);
  for (struct dentry *e = dd->tab[h & dd->mask]; e; e = e->hnext)
    if (e->hash == h && e->nlen == n && name_eq(e->name, name, n)) return e;
  return NULL;
}
static int d_resize(struct dirdata *dd, u32 size) {
  struct dentry **t = kzalloc(size * sizeof *t);
  if (!t) return -ENOMEM;
  for (struct dentry *e = dd->first; e; e = e->next) { u32 b = e->hash & (size - 1); e->hnext = t[b]; t[b] = e; }
  kfree(dd->tab);
  dd->tab = t; dd->mask = size - 1;
  return 0;
}
/* Add a name. Does not touch link counts. */
static struct dentry *d_add(struct inode *dir, const char *name, u32 n, struct inode *i) {
  struct dirdata *dd = dir->dir;
  /* the table appears when the directory outgrows SMALL_DIR, then keeps at most one entry per bucket */
  if (dd->tab ? dd->count >= dd->mask + 1 : dd->count >= SMALL_DIR)
    if (d_resize(dd, dd->tab ? (dd->mask + 1) * 2 : 2 * SMALL_DIR) && !dd->tab && dd->count >= 4 * SMALL_DIR) return NULL;
  struct dentry *e = kmalloc(sizeof *e + n + 1);
  if (!e) return NULL;
  e->hash = name_hash(name, n); e->nlen = n;
  memcpy(e->name, name, n); e->name[n] = 0;
  e->ino = i; e->dir = dir; e->seq = dd->nextseq++; e->type = i->mode >> 12;
  if (dd->tab) { u32 b = e->hash & dd->mask; e->hnext = dd->tab[b]; dd->tab[b] = e; }
  else e->hnext = NULL;
  e->next = NULL; e->prev = dd->last;
  if (dd->last) dd->last->next = e; else dd->first = e;
  dd->last = e;
  dd->count++;
  if (!i->alias) i->alias = e;
  return e;
}
/* Another name of e's inode in the same directory, for the inode's alias. */
static struct dentry *alias_other(struct dentry *e) {
  if (e->ino->nlink < 2 || S_ISDIR(e->ino->mode)) return NULL;
  for (struct dentry *o = e->dir->dir->first; o; o = o->next) if (o != e && o->ino == e->ino) return o;
  return NULL;
}
/* Remove a name. Does not touch link counts. */
static void d_remove(struct dentry *e) {
  struct dirdata *dd = e->dir->dir;
  if (dd->tab) {
    struct dentry **pp = &dd->tab[e->hash & dd->mask];
    while (*pp != e) pp = &(*pp)->hnext;
    *pp = e->hnext;
  }
  if (e->prev) e->prev->next = e->next; else dd->first = e->next;
  if (e->next) e->next->prev = e->prev; else dd->last = e->prev;
  dd->count--; dd->gen++;
  if (e->ino->alias == e) e->ino->alias = alias_other(e);
  kfree(e);
  if (dd->tab && dd->count <= SMALL_DIR / 2) { kfree(dd->tab); dd->tab = NULL; dd->mask = 0; } /* small again: back to the list */
  else if (dd->mask >= 63 && dd->count < (dd->mask + 1) / 8) d_resize(dd, (dd->mask + 1) / 4); /* a failure only costs memory */
}
static int is_dot(const char *s, u32 n) { return (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'); }

/* ---- paths ---- */
int path_from_user(char *k, u32 u) {
  int r = str_from_user(k, u, 256); /* almost every path: the same single copy as before */
  if (r != -ENAMETOOLONG) return r;
  u64 limit = (u64)cur->mem_pages() << 16;
  for (u32 off = 256; off < VFS_PATH_MAX; off += 256) { /* longer: on in pieces of 256 */
    u64 at = (u64)u + off;
    if (at >= limit) return -EFAULT;
    u32 n = limit - at < 256 ? (u32)(limit - at) : 256;
    cur->copy_in((u32)(usize)(k + off), (u32)at, n);
    for (u32 j = 0; j < n; j++) if (!k[off + j]) return off + j;
    if (n < 256) return -EFAULT; /* ran off the end of memory */
  }
  return -ENAMETOOLONG;
}
int vfs_symlink_target(struct inode *i, char *buf, u32 cap) {
  if (i->kind) return procfs_readlink(i, buf, cap);
  if (i->size >= cap) return -ENAMETOOLONG;
  memcpy(buf, i->data, i->size);
  buf[i->size] = 0;
  return i->size;
}
/* Resolve a path. On success nd->dir is the directory that holds the last component,
 * nd->last its name and nd->ino what it names (NULL if nothing has that name yet). Symbolic
 * links in the middle are always followed; in the last component only with W_FOLLOW or a
 * trailing slash. At most 40 links per walk (ELOOP), as Linux. ".." is physical: after
 * following a link to a directory it leads to that directory's real parent. */
static __attribute__((noinline)) int walk_full(struct inode *base, const char *path, int flags, struct nameidata *nd);
/* The common case first: plain names, each one a directory except the last, no ".", "..",
 * symbolic link, /proc or trailing slash, and no permission checks. Anything else goes to
 * walk_full, which starts again from the beginning (nothing has been changed). */
int vfs_walk(struct inode *base, const char *path, int flags, struct nameidata *nd) {
  struct inode *d = *path == '/' ? vfs_cred_root() : base;
  const char *p = path;
  if (!*p || vfs_enforce >= 2) return walk_full(base, path, flags, nd);
  for (;;) {
    while (*p == '/') p++;
    const char *s = p;
    while (*p && *p != '/') p++;
    u32 n = p - s;
    if (!n) break;
    if (n > VFS_NAME_MAX || s[0] == '.' || d->kind) return walk_full(d, s, flags, nd); /* go on from here */
    struct dentry *de = d_lookup(d, s, n);
    if (!*p) { /* the last component */
      if (de && de->type == (S_IFLNK >> 12) && (flags & W_FOLLOW)) return walk_full(d, s, flags, nd);
      nd->dir = d; nd->de = de; nd->ino = de ? de->ino : NULL; nd->file = NULL; nd->last = s; nd->lastlen = n; nd->dot = 0; nd->trailing = 0;
      return 0;
    }
    if (!de || de->type != (S_IFDIR >> 12)) return walk_full(d, s, flags, nd); /* missing, a link, or not a directory */
    d = de->ino;
  }
  return walk_full(base, path, flags, nd);
}
static __attribute__((noinline)) int walk_full(struct inode *base, const char *path, int flags, struct nameidata *nd) {
  if (!*path) return -ENOENT;
  struct inode *root = vfs_cred_root();
  struct inode *d = *path == '/' ? root : base;
  const char *p = path;
  int links = 0, nb = 0;
  nd->file = NULL; nd->de = NULL;
  for (;;) {
    while (*p == '/') p++;
    if (!*p) { /* "/" itself, or a link to it */
      if (!S_ISDIR(d->mode)) return -ENOTDIR;
      nd->dir = nd->ino = d; nd->last = "."; nd->lastlen = 1; nd->dot = 1; nd->trailing = 1;
      return 0;
    }
    const char *s = p;
    while (*p && *p != '/') p++;
    u32 n = p - s;
    const char *rest = p;
    while (*rest == '/') rest++;
    if (!S_ISDIR(d->mode)) return -ENOTDIR;
    if (n > VFS_NAME_MAX) return -ENAMETOOLONG;
    if (vfs_enforce >= 2 && vfs_may(d, 1)) return -EACCES;
    struct inode *c;
    struct dentry *de = NULL;
    int dot = 0;
    if (n == 1 && s[0] == '.') { c = d; dot = 1; }
    else if (n == 2 && s[0] == '.' && s[1] == '.') { c = d == root ? d : d->dir->parent; dot = 2; }
    else if (d->kind) c = procfs_lookup(d, s, n);
    else { de = d_lookup(d, s, n); c = de ? de->ino : NULL; }
    if (!*rest) { /* the last component */
      nd->dir = d; nd->de = de; nd->ino = c; nd->last = s; nd->lastlen = n; nd->dot = dot; nd->trailing = *p == '/';
      if (!c || !S_ISLNK(c->mode) || !((flags & W_FOLLOW) || (nd->trailing && !(flags & W_NOTRAIL)))) return 0;
    } else if (!c) return -ENOENT;
    else if (!S_ISLNK(c->mode)) { d = c; p = rest; continue; }
    /* c is a symbolic link, and it must be followed */
    if (++links > 40) return -ELOOP;
    if (c->kind >= K_MAGIC) { /* /proc/N/fd/M, exe, cwd, /dev/tty: straight to the object */
      struct file *f = NULL;
      struct inode *t = procfs_follow(c, &f);
      if (!*rest) {
        if (!t && !f) return -ENOENT;
        nd->ino = t; nd->file = t ? NULL : f; nd->de = NULL; nd->dot = 0;
        if (nd->trailing && (!t || !S_ISDIR(t->mode))) return -ENOTDIR;
        return 0;
      }
      if (!t) return f ? -ENOTDIR : -ENOENT;
      d = t; p = rest;
      continue;
    }
    char *out = nd->buf[nb];
    nb ^= 1;
    int len = vfs_symlink_target(c, out, VFS_PATH_MAX);
    if (len < 0) return len;
    if (!len) return -ENOENT;
    const char *tail = *rest ? rest : (*p == '/' ? "/" : "");
    u32 tl = strlen(tail);
    if ((u32)len + 1 + tl >= VFS_PATH_MAX) return -ENAMETOOLONG;
    if (tl) { out[len] = '/'; memcpy(out + len + 1, tail, tl + 1); }
    if (out[0] == '/') d = root; /* else relative to the directory that holds the link */
    p = out;
  }
}
/* vfs_lookup that says why it failed (ENOENT, ELOOP, ENOTDIR, EACCES...): spawn and execve */
struct inode *vfs_lookup_err(struct inode *cwd, const char *path, int *err) {
  struct nameidata nd;
  int e = vfs_walk(cwd, path, W_FOLLOW, &nd);
  if (!e && !nd.ino) e = -ENOENT;
  if (!e && nd.trailing && !S_ISDIR(nd.ino->mode)) e = -ENOTDIR;
  *err = e;
  return e ? NULL : nd.ino;
}
struct inode *vfs_lookup(struct inode *cwd, const char *path) {
  struct nameidata nd;
  if (vfs_walk(cwd, path, W_FOLLOW, &nd) || !nd.ino) return NULL;
  if (nd.trailing && !S_ISDIR(nd.ino->mode)) return NULL;
  return nd.ino;
}

/* ---- namespace operations ---- */
static void dir_changed(struct inode *dir) { s64 t = vfs_now(); dir->mtime = dir->ctime = t; vfs_changed(dir); }
/* Create the last component of a resolved path. */
struct inode *vfs_mknod_at(struct nameidata *nd, u32 mode, int *err) {
  struct inode *dir = nd->dir;
  if (!dir->nlink) { *err = -ENOENT; return NULL; } /* the directory was removed */
  if (nd->dot || nd->ino || nd->file) { *err = -EEXIST; return NULL; }
  if (dir->kind) { *err = -EACCES; return NULL; }
  if (nd->trailing && !S_ISDIR(mode)) { *err = S_ISREG(mode) ? -EISDIR : -ENOENT; return NULL; }
  if ((*err = vfs_may(dir, 3))) return NULL;
  if ((*err = store_room_name(dir))) return NULL;
  struct inode *i = inode_alloc(mode);
  if (!i) { *err = -ENOMEM; return NULL; }
  if (dir->mode & S_ISGID) { /* BSD group semantics, as on Linux */
    i->gid = dir->gid;
    if (S_ISDIR(mode)) i->mode |= S_ISGID;
  }
  i->owned = 1;
  if (!d_add(dir, nd->last, nd->lastlen, i)) { inode_free(i); *err = -ENOMEM; return NULL; }
  if (S_ISDIR(mode)) { i->nlink = 2; dir->nlink++; i->dir->parent = dir; }
  else i->nlink = 1;
  dir_changed(dir);
  store_ns(1, dir, nd->last, nd->lastlen, i);
  *err = 0;
  return i;
}
struct inode *vfs_create(struct inode *cwd, const char *path, u32 mode, int *err) {
  struct nameidata nd;
  if ((*err = vfs_walk(cwd, path, 0, &nd))) return NULL;
  return vfs_mknod_at(&nd, mode, err);
}
/* Take one name away from i (whose entry was already removed). */
static void drop_link(struct inode *dir, struct inode *i) {
  if (S_ISDIR(i->mode)) {
    i->nlink = 0;
    dir->nlink--;
    i->dir->parent = i; /* ".." inside a removed directory stays put */
  } else i->nlink--;
  i->ctime = vfs_now();
  vfs_changed(i);
  if (!i->nlink && !i->refs) inode_free(i);
}
int vfs_unlink_at(struct nameidata *nd, int rmdir) {
  struct inode *dir = nd->dir, *i = nd->ino;
  if (!dir->nlink) return -ENOENT; /* "." inside a directory that is already gone */
  if (nd->dot == 1) return rmdir ? -EINVAL : -EISDIR;
  if (nd->dot == 2) return rmdir ? -ENOTEMPTY : -EISDIR;
  if (!i) return -ENOENT;
  if (!nd->de) return dir->kind ? -EACCES : -EBUSY;
  int isdir = S_ISDIR(i->mode);
  if (isdir && !rmdir) return -EISDIR;
  if (!isdir && rmdir) return -ENOTDIR;
  if (nd->trailing && !isdir) return -ENOTDIR;
  if (isdir && i->dir->count) return -ENOTEMPTY;
  if (i == vfs_cred_root() || i == vfs_root) return -EBUSY;
  int e = vfs_may(dir, 3);
  if (e) return e;
  if ((dir->mode & S_ISVTX) && vfs_enforce >= 2 && cred()->fsuid && cred()->fsuid != i->uid && cred()->fsuid != dir->uid) return -EPERM;
  store_ns(2, dir, nd->last, nd->lastlen, i);
  d_remove(nd->de);
  dir_changed(dir);
  drop_link(dir, i);
  return 0;
}
int vfs_link_at(struct inode *i, struct nameidata *nd) {
  struct inode *dir = nd->dir;
  if (S_ISDIR(i->mode)) return -EPERM;
  if (!dir->nlink) return -ENOENT;
  if (nd->dot || nd->ino || nd->file) return -EEXIST;
  if (nd->trailing) return -ENOENT;
  if (dir->kind || i->kind) return -EXDEV;
  if (!i->nlink) return -ENOENT; /* an unlinked open file cannot get a name back */
  if (i->nlink >= 65000) return -EMLINK;
  int e = vfs_may(dir, 3);
  if (!e) e = store_room_name(dir);
  if (e) return e;
  if (!d_add(dir, nd->last, nd->lastlen, i)) return -ENOMEM;
  i->nlink++;
  i->ctime = vfs_now();
  vfs_changed(i);
  dir_changed(dir);
  store_ns(3, dir, nd->last, nd->lastlen, i);
  return 0;
}
static int is_under(struct inode *d, struct inode *top) { /* d is top or below it */
  for (;; d = d->dir->parent) {
    if (d == top) return 1;
    if (d == vfs_root || d->dir->parent == d) return 0;
  }
}
int vfs_rename_nd(struct nameidata *f, struct nameidata *t, int flags) {
  struct inode *odir = f->dir, *ndir = t->dir, *i = f->ino, *old = t->ino;
  if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) return -EINVAL;
  if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE)) return -EINVAL;
  if (!odir->nlink || !ndir->nlink) return -ENOENT;
  if (f->dot || !i) return f->dot ? -EBUSY : -ENOENT;
  if (!f->de) return odir->kind ? -EACCES : -EBUSY;
  if (t->dot) return -EINVAL; /* the target would be named "." or ".." */
  if (odir->kind || ndir->kind) return -EXDEV;
  if (i == vfs_root || i == vfs_cred_root()) return -EBUSY;
  int idir = S_ISDIR(i->mode);
  if ((f->trailing || t->trailing) && !idir) return -ENOTDIR;
  if (idir && is_under(ndir, i)) return -EINVAL; /* a directory cannot move into itself */
  if (old && !t->de) return -EBUSY;
  if (flags & RENAME_EXCHANGE) {
    if (!old) return -ENOENT;
    if (S_ISDIR(old->mode) && is_under(odir, old)) return -EINVAL;
  }
  if (old == i) return 0; /* two names of one file: nothing happens, as POSIX says */
  if (old && (flags & RENAME_NOREPLACE)) return -EEXIST;
  int e = vfs_may(odir, 3);
  if (!e && ndir != odir) e = vfs_may(ndir, 3);
  /* as Linux: a sticky directory protects both the name that moves and the one it replaces;
   * a directory that changes parent needs write permission on itself (its ".." changes) */
  if (!e && vfs_enforce >= 2 && cred()->fsuid) {
    u32 me = cred()->fsuid;
    if ((odir->mode & S_ISVTX) && me != i->uid && me != odir->uid) e = -EPERM;
    else if (old && (ndir->mode & S_ISVTX) && me != old->uid && me != ndir->uid) e = -EPERM;
    else if (idir && ndir != odir) e = vfs_may(i, 2);
    else if (old && (flags & RENAME_EXCHANGE) && S_ISDIR(old->mode) && ndir != odir) e = vfs_may(old, 2);
  }
  if (e) return e;
  if (flags & RENAME_EXCHANGE) {
    int odd = S_ISDIR(old->mode);
    store_rename(odir, f->last, f->lastlen, i, ndir, t->last, t->lastlen, old, 1); /* before the names move */
    f->de->ino = old; t->de->ino = i; f->de->type = old->mode >> 12; t->de->type = i->mode >> 12;
    if (i->alias == f->de) i->alias = t->de;
    if (old->alias == t->de) old->alias = f->de;
    if (idir) i->dir->parent = ndir;
    if (odd) old->dir->parent = odir;
    if (idir != odd) { if (idir) { odir->nlink--; ndir->nlink++; } else { ndir->nlink--; odir->nlink++; } }
    s64 now = vfs_now();
    i->ctime = old->ctime = now;
    vfs_changed(i); vfs_changed(old);
    dir_changed(odir); if (ndir != odir) dir_changed(ndir);
    return 0;
  }
  if (old) {
    int odd = S_ISDIR(old->mode);
    if (odd != idir) return odd ? -EISDIR : -ENOTDIR;
    if (odd && old->dir->count) return -ENOTEMPTY;
  }
  if (t->lastlen > VFS_NAME_MAX) return -ENAMETOOLONG;
  store_rename(odir, f->last, f->lastlen, i, ndir, t->last, t->lastlen, old, 0);
  struct dentry *ode = f->de, *nde;
  if (old) { /* reuse the target's entry */
    nde = t->de;
    nde->ino = i; nde->type = i->mode >> 12;
    if (old->alias == nde) old->alias = NULL; /* its other names, if any, are not searched for */
  } else if (!(nde = d_add(ndir, t->last, t->lastlen, i))) return -ENOMEM;
  if (i->alias == ode) i->alias = nde; /* so d_remove leaves it alone */
  d_remove(ode);
  if (idir && odir != ndir) { odir->nlink--; ndir->nlink++; i->dir->parent = ndir; }
  i->ctime = vfs_now();
  vfs_changed(i);
  dir_changed(odir);
  if (ndir != odir) dir_changed(ndir);
  if (old) drop_link(ndir, old);
  return 0;
}
/* The path of a directory (or of any inode with a known name), for getcwd and /proc. */
int vfs_path(struct inode *ino, char *buf, u32 cap) {
  char tmp[VFS_PATH_MAX];
  u32 pos = sizeof tmp - 1;
  tmp[pos] = 0;
  struct inode *root = vfs_cred_root();
  while (ino != root && ino != vfs_root) {
    struct dentry *e = ino->alias;
    if (!ino->nlink || !e) return -ENOENT; /* removed */
    u32 n = e->nlen;
    if (pos < n + 1) return -ENAMETOOLONG;
    pos -= n; memcpy(tmp + pos, e->name, n);
    tmp[--pos] = '/';
    ino = e->dir;
  }
  if (tmp[pos] == 0) tmp[--pos] = '/';
  u32 len = sizeof tmp - pos;
  if (len > cap) return -ERANGE;
  memcpy(buf, tmp + pos, len);
  return len;
}
static struct inode *symlink_inode(const char *target, u32 len) {
  struct inode *i = inode_alloc(S_IFLNK | 0777);
  if (!i) return NULL;
  i->data = kmalloc(len + 1);
  if (!i->data) { inode_free(i); return NULL; }
  memcpy(i->data, target, len); i->data[len] = 0;
  i->size = len; i->owned = 1;
  return i;
}
int vfs_symlink_at(const char *target, struct nameidata *nd) {
  struct inode *dir = nd->dir;
  u32 len = strlen(target);
  if (!len) return -ENOENT;
  if (!dir->nlink) return -ENOENT;
  if (nd->dot || nd->ino || nd->file) return -EEXIST;
  if (nd->trailing) return -ENOENT;
  if (dir->kind) return -EACCES;
  int e = vfs_may(dir, 3);
  if (!e) e = store_room_name(dir);
  if (e) return e;
  struct inode *i = symlink_inode(target, len);
  if (!i) return -ENOMEM;
  if (!d_add(dir, nd->last, nd->lastlen, i)) { inode_free(i); return -ENOMEM; }
  i->nlink = 1;
  dir_changed(dir);
  store_ns(1, dir, nd->last, nd->lastlen, i);
  return 0;
}
int vfs_symlink(struct inode *base, const char *target, const char *path) {
  struct nameidata nd;
  int e = vfs_walk(base, path, 0, &nd);
  return e ? e : vfs_symlink_at(target, &nd);
}
/* An inode for /proc that lives in no directory (it is reached through procfs_lookup). */
struct inode *inode_alloc_proc(u32 mode, int kind, int dev, struct inode *parent) {
  struct inode *i = inode_alloc(mode);
  if (!i) return NULL;
  i->kind = kind; i->dev = dev; i->nlink = S_ISDIR(mode) ? 2 : 1;
  i->uid = i->gid = 0;
  if (i->dir && parent) i->dir->parent = parent;
  return i;
}
void vfs_mkdev(const char *path, int dev) { vfs_mkdev_mode(path, dev, 0666, 0, 0); }
void vfs_mkdev_mode(const char *path, int dev, u32 mode, u32 uid, u32 gid) {
  /* The kernel makes the node, also when it does so inside a process's call (a pty's
   * /dev/pts/N is made when /dev/ptmx is opened): root's rights, the real root directory. */
  struct fscred *c = cred(), saved = *c;
  c->fsuid = c->fsgid = 0; c->root = NULL;
  int err;
  struct inode *i = vfs_create(vfs_root, path, S_IFCHR | 0666, &err);
  if (i) { i->dev = dev; i->uid = uid; i->gid = gid; i->mode = S_IFCHR | (mode & 07777); }
  c->fsuid = saved.fsuid; c->fsgid = saved.fsgid; c->root = saved.root;
}

/* ---- the boot image ---- */
static struct inode *mkdir_p(const char *path, u32 n) {
  struct inode *d = vfs_root;
  u32 k = 0;
  while (k < n) {
    while (k < n && path[k] == '/') k++;
    u32 s = k;
    while (k < n && path[k] != '/') k++;
    if (k == s) break;
    if (is_dot(path + s, k - s)) { if (path[s + 1] == '.') d = d->dir->parent; continue; }
    if (k - s > VFS_NAME_MAX) return NULL;
    struct dentry *e = d_lookup(d, path + s, k - s);
    struct inode *c = e ? e->ino : NULL;
    if (!c) {
      c = inode_alloc(S_IFDIR | 0755);
      if (!c || !d_add(d, path + s, k - s, c)) { if (c) inode_free(c); return NULL; }
      c->nlink = 2; d->nlink++; c->dir->parent = d;
    }
    if (!S_ISDIR(c->mode)) return NULL;
    d = c;
  }
  return d;
}
static s64 octal64(const u8 *p, int n) {
  s64 v = 0;
  while (n > 0 && *p == ' ') p++, n--;
  for (int k = 0; k < n && p[k] >= '0' && p[k] <= '7'; k++) v = v * 8 + (p[k] - '0');
  return v;
}
static u32 octal(const u8 *p, int n) { return (u32)octal64(p, n); }
/* Split "a/b/c" into the directory (created as needed) and the base name. */
static struct inode *tar_parent(const char *name, u32 n, const char **base, u32 *blen) {
  while (n > 1 && name[n - 1] == '/') n--;
  u32 k = n;
  while (k && name[k - 1] != '/') k--;
  *base = name + k; *blen = n - k;
  return k ? mkdir_p(name, k) : vfs_root;
}
/* pax extended header: "LEN key=value\n" records; we use path and linkpath */
static void pax_get(const u8 *p, u32 len, char *path, char *link) {
  for (u32 off = 0; off < len;) {
    u32 rl = 0, k = off;
    while (k < len && p[k] >= '0' && p[k] <= '9') rl = rl * 10 + (p[k++] - '0');
    if (!rl || off + rl > len || k >= len) return;
    const u8 *kv = p + k + 1, *end = p + off + rl - 1; /* end: the newline */
    const u8 *eq = kv;
    while (eq < end && *eq != '=') eq++;
    u32 klen = eq - kv, vlen = end > eq ? (u32)(end - eq - 1) : 0;
    char *dst = (klen == 4 && !memcmp(kv, "path", 4)) ? path : (klen == 8 && !memcmp(kv, "linkpath", 8)) ? link : NULL;
    if (dst && vlen < VFS_PATH_MAX) { memcpy(dst, eq + 1, vlen); dst[vlen] = 0; }
    off += rl;
  }
}
int vfs_load_tar(u8 *tar, u32 len) {
  int files = 0;
  static char lname[VFS_PATH_MAX], llink[VFS_PATH_MAX]; /* from GNU 'L'/'K' or pax entries */
  lname[0] = llink[0] = 0;
  for (u32 off = 0; off + 512 <= len;) {
    u8 *h = tar + off;
    if (!h[0]) break;
    char name[VFS_PATH_MAX], link[VFS_PATH_MAX];
    u32 n = 0;
    if (h[345]) { while (n < 155 && h[345 + n]) { name[n] = h[345 + n]; n++; } name[n++] = '/'; }
    for (u32 k = 0; k < 100 && h[k]; k++) name[n++] = h[k];
    name[n] = 0;
    u32 ln = 0;
    for (; ln < 100 && h[157 + ln]; ln++) link[ln] = h[157 + ln];
    link[ln] = 0;
    u32 size = octal(h + 124, 12), mode = octal(h + 100, 8), uid = octal(h + 108, 8), gid = octal(h + 116, 8);
    s64 mtime = KT_MAKE(octal64(h + 136, 12), 0);
    u8 type = h[156];
    u8 *data = h + 512;
    if (size > len - off - 512) break; /* a truncated or corrupt archive: keep what came before */
    off += 512 + ((size + 511) & ~511u);
    if (off > len) off = len;
    if (type == 'L' || type == 'K') { /* GNU long name or long link target for the next entry */
      char *dst = type == 'L' ? lname : llink;
      u32 m = size < VFS_PATH_MAX - 1 ? size : VFS_PATH_MAX - 1;
      memcpy(dst, data, m); dst[m] = 0;
      continue;
    }
    if (type == 'x') { pax_get(data, size, lname, llink); continue; }
    if (type == 'g') continue;
    if (lname[0]) { strlcpy(name, lname, sizeof name); n = strlen(name); lname[0] = 0; }
    if (llink[0]) { strlcpy(link, llink, sizeof link); llink[0] = 0; }
    const char *base; u32 bl;
    struct inode *dir = tar_parent(name, n, &base, &bl);
    if (!dir) continue;
    struct inode *f;
    if (type == '5') {
      f = (bl && !is_dot(base, bl)) ? mkdir_p(name, n) : (bl == 1 || !bl) ? dir : NULL;
      if (f) { f->mode = S_IFDIR | (mode & 07777); f->uid = uid; f->gid = gid; f->mtime = mtime; }
      continue;
    }
    if (!bl || is_dot(base, bl) || bl > VFS_NAME_MAX) continue;
    struct dentry *e = d_lookup(dir, base, bl);
    if (e) { /* a later entry replaces an earlier one, as tar does; never a directory */
      if (S_ISDIR(e->ino->mode)) continue;
      struct inode *was = e->ino;
      d_remove(e);
      drop_link(dir, was);
    }
    if (type == '1') { /* hard link to an earlier entry, named by its archive path */
      struct nameidata nd;
      if (vfs_walk(vfs_root, link, 0, &nd) || !nd.ino || S_ISDIR(nd.ino->mode)) continue;
      if (d_add(dir, base, bl, nd.ino)) nd.ino->nlink++;
      continue;
    }
    if (type == '2') {
      if (!link[0]) continue;
      f = inode_alloc(S_IFLNK | 0777);
      if (!f) break;
      f->size = strlen(link);
      f->data = kmalloc(f->size + 1);
      if (!f->data) { inode_free(f); break; }
      memcpy(f->data, link, f->size + 1);
      f->owned = 1;
    } else if (type == '0' || type == 0 || type == '7') {
      f = inode_alloc(S_IFREG | (mode & 07777));
      if (!f) break;
      f->data = data; f->size = f->cap = size; f->owned = 0;
    } else if (type == '6') {
      f = inode_alloc(S_IFIFO | (mode & 07777));
      if (!f) break;
    } else continue; /* devices ('3', '4') are not created from an image */
    f->uid = uid; f->gid = gid; f->mtime = mtime;
    if (!d_add(dir, base, bl, f)) { inode_free(f); break; }
    f->nlink = 1;
    files++;
  }
  return files;
}
void vfs_init(void) {
  vfs_root = inode_alloc(S_IFDIR | 0755);
  vfs_root->nlink = 2;
  mkdir_p("dev", 3); mkdir_p("tmp", 3); mkdir_p("proc", 4); mkdir_p("run", 3); mkdir_p("dev/shm", 7);
  vfs_lookup(vfs_root, "/tmp")->mode = S_IFDIR | 01777;
  vfs_lookup(vfs_root, "/dev/shm")->mode = S_IFDIR | 01777;
  vfs_mkdev("/dev/null", DEV_NULL);
  vfs_mkdev("/dev/zero", DEV_ZERO);
  /* Linux has crw--w---- root tty; here others may write too (0622), because the test
   * scripts read what programs in the desktop write to the console (the kernel log) */
  vfs_mkdev_mode("/dev/console", DEV_CONSOLE, 0622, 0, GID_TTY);
  vfs_mkdev("/dev/urandom", DEV_RANDOM);
  vfs_mkdev("/dev/random", DEV_RANDOM);
  procfs_init();
}

/* ---- regular files ---- */
static long reg_read(struct file *f, u32 ubuf, u32 len) {
  struct inode *i = f->ino;
  if (f->off >= i->size) return 0;
  if (len > i->size - f->off) len = i->size - f->off;
  if (copy_to_user(ubuf, i->data + f->off, len)) return -EFAULT;
  f->off += len;
  /* access time, "relatime" without the 24-hour rule: only when the file changed since
   * it was last read, so a read of an unchanged file never reads the clock */
  if (i->atime < i->mtime || i->atime < i->ctime) vfs_touch(i, 1);
  return len;
}
static int reg_reserve(struct inode *i, u32 need) {
  if (i->owned && need <= i->cap) return 0;
  u64 cap = i->cap < 256 ? 256 : i->cap;
  while (cap < need) cap *= 2;
  if (cap > 0x7fffffff) cap = need; /* kmalloc refuses more than 2 GiB anyway */
  u8 *d = kmalloc(cap);
  if (!d) return -ENOSPC;
  if (i->size) memcpy(d, i->data, i->size);
  if (i->owned) kfree(i->data);
  i->data = d; i->cap = cap; i->owned = 1;
  return 0;
}
void store_dirty(struct inode *i, u32 lo, u32 hi);
static long reg_write(struct file *f, u32 ubuf, u32 len) {
  struct inode *i = f->ino;
  if (!len) return 0; /* writing nothing never extends the file */
  if (f->flags & O_APPEND) f->off = i->size;
  if (!user_ok(ubuf, len)) return -EFAULT; /* before growing the file for nothing */
  if ((u64)f->off + len > 0xffffffffu) return -ENOSPC;
  if (i->sflags & S_PERSIST_BIT) { long e = store_room(i, f->off + len); if (e) return e; } /* the browser's storage is full */
  if (reg_reserve(i, f->off + len)) return -ENOSPC;
  u32 lo = f->off;
  if (f->off > i->size) { lo = i->size; memset(i->data + i->size, 0, f->off - i->size); }
  if (copy_from_user(i->data + f->off, ubuf, len)) return -EFAULT;
  f->off += len;
  if (f->off > i->size) i->size = f->off;
  i->mtime = i->ctime = vfs_now();
  if (i->sflags) store_dirty(i, lo, f->off);
  return len;
}
static int always_ready(struct file *f, int add) { (void)f; (void)add; return POLLIN | POLLOUT; }
void flock_release(struct file *f);
static void reg_close(struct file *f) { if (f->ino->locks) flock_release(f); }
static void dir_close(struct file *f) { if (f->priv) kfree(f->priv); if (f->ino->locks) flock_release(f); }
/* A plain open file has no close function, so closing it costs nothing extra; a file that
 * takes a flock or OFD lock switches to reg_lk_fops, whose close drops those locks. */
const struct fops reg_fops = {reg_read, reg_write, always_ready, NULL, NULL};
const struct fops reg_lk_fops = {reg_read, reg_write, always_ready, reg_close, NULL};
const struct fops dir_fops = {NULL, NULL, always_ready, dir_close, NULL};
int reg_truncate(struct inode *i, u32 size) {
  u32 old = i->size;
  if (size > i->size && (i->sflags & S_PERSIST_BIT)) { long e = store_room(i, size); if (e) return e; }
  if (size > i->size) { if (reg_reserve(i, size)) return -ENOSPC; memset(i->data + i->size, 0, size - i->size); }
  i->size = size;
  i->mtime = i->ctime = vfs_now();
  if (i->sflags) store_dirty(i, size < old ? size : old, size > old ? size : old);
  else vfs_changed(i);
  return 0;
}

/* ---- directory listings: the cursor remembers where it was, unless an entry went away ---- */
struct dircursor { struct dentry *at; u32 gen, off; };
/* The first entry at listing offset off or later. Offsets: 0 ".", 1 "..", then seq + 2. */
struct dentry *vfs_dir_seek(struct file *f, u32 off) {
  struct dirdata *dd = f->ino->dir;
  struct dircursor *c = f->priv;
  if (c && c->gen == dd->gen && c->off == off && c->at) return c->at;
  struct dentry *e = dd->first;
  while (e && e->seq + 2 < off) e = e->next;
  return e;
}
void vfs_dir_remember(struct file *f, struct dentry *next, u32 off) {
  struct dircursor *c = f->priv;
  if (!c) { c = f->priv = kzalloc(sizeof *c); if (!c) return; }
  c->at = next; c->gen = f->ino->dir->gen; c->off = off;
}

/* ---- for the persistent store (store.c), which rebuilds the stored layer at boot ---- */
struct inode *vfs_inode_new(u32 mode) { return inode_alloc(mode); }
struct dentry *vfs_d_lookup(struct inode *dir, const char *name, u32 n) { return d_lookup(dir, name, n); }
struct dentry *vfs_d_add(struct inode *dir, const char *name, u32 n, struct inode *i) { return d_add(dir, name, n, i); }
void vfs_d_remove(struct dentry *e) { d_remove(e); }
