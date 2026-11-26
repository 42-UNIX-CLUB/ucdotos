// SPDX-License-Identifier: GPL-2.0-or-later
/* In-memory filesystem. The initial contents come from a tar archive the
 * boot loader hands to the kernel (an initrd); file data stays in place
 * inside the archive until a file is written to. */
#include "kernel.h"

struct inode *vfs_root;
static u32 next_ino = 1;

static struct inode *inode_new(struct inode *parent, const char *name, u32 nlen, u32 mode) {
  struct inode *i = kzalloc(sizeof *i);
  if (!i) return NULL;
  i->ino = next_ino++;
  i->mode = mode;
  i->nlink = 1;
  i->mtime = (s64)(host_epoch() / 1000);
  if (nlen >= sizeof i->name) nlen = sizeof i->name - 1;
  memcpy(i->name, name, nlen);
  i->parent = parent ? parent : i;
  if (parent) { /* append, so directory listings keep creation order */
    struct inode **pp = &parent->child;
    while (*pp) pp = &(*pp)->next;
    *pp = i;
  }
  return i;
}
static struct inode *child_find(struct inode *dir, const char *name, u32 nlen) {
  for (struct inode *c = dir->child; c; c = c->next)
    if (strlen(c->name) == nlen && !memcmp(c->name, name, nlen)) return c;
  return NULL;
}
/* Walk a path. If last != NULL, stop at the parent and return the final component. */
static struct inode *walk(struct inode *cwd, const char *path, const char **last, u32 *lastlen) {
  struct inode *d = (*path == '/') ? vfs_root : cwd;
  for (;;) {
    while (*path == '/') path++;
    if (!*path) { if (last) { *last = "."; *lastlen = 1; } return d; }
    const char *s = path;
    while (*path && *path != '/') path++;
    u32 n = path - s;
    const char *rest = path;
    while (*rest == '/') rest++;
    if (last && !*rest) { *last = s; *lastlen = n; return d; }
    if ((d->mode & S_IFMT) != S_IFDIR) return NULL;
    if (n == 1 && s[0] == '.') continue;
    if (n == 2 && s[0] == '.' && s[1] == '.') { d = d->parent; continue; }
    d = child_find(d, s, n);
    if (!d) return NULL;
  }
}
struct inode *vfs_lookup(struct inode *cwd, const char *path) { return walk(cwd, path, NULL, NULL); }

struct inode *vfs_create(struct inode *cwd, const char *path, u32 mode, int *err) {
  const char *last; u32 n;
  struct inode *dir = walk(cwd, path, &last, &n);
  if (!dir || (dir->mode & S_IFMT) != S_IFDIR) { *err = -ENOENT; return NULL; }
  if ((n == 1 && last[0] == '.') || (n == 2 && last[0] == '.' && last[1] == '.')) { *err = -EEXIST; return NULL; }
  if (child_find(dir, last, n)) { *err = -EEXIST; return NULL; }
  struct inode *i = inode_new(dir, last, n, mode);
  if (!i) { *err = -ENOMEM; return NULL; }
  i->owned = 1;
  return i;
}
int vfs_unlink(struct inode *cwd, const char *path, int rmdir) {
  struct inode *i = vfs_lookup(cwd, path);
  if (!i) return -ENOENT;
  if (i == vfs_root) return -EPERM;
  int isdir = (i->mode & S_IFMT) == S_IFDIR;
  if (isdir && !rmdir) return -EISDIR;
  if (!isdir && rmdir) return -ENOTDIR;
  if (isdir && i->child) return -ENOTEMPTY;
  struct inode **pp = &i->parent->child;
  while (*pp != i) pp = &(*pp)->next;
  *pp = i->next;
  i->nlink = 0; /* open files keep the inode alive; it is simply leaked for now */
  return 0;
}
int vfs_rename(struct inode *cwd, const char *from, const char *to) {
  struct inode *i = vfs_lookup(cwd, from);
  if (!i) return -ENOENT;
  if (i == vfs_root) return -EPERM;
  const char *last; u32 n;
  struct inode *dir = walk(cwd, to, &last, &n);
  if (!dir || (dir->mode & S_IFMT) != S_IFDIR) return -ENOENT;
  if (n >= sizeof i->name) return -ENAMETOOLONG;
  for (struct inode *d = dir; ; d = d->parent) { /* a directory cannot move into itself */
    if (d == i) return -EINVAL;
    if (d == vfs_root) break;
  }
  struct inode *old = child_find(dir, last, n);
  if (old == i) return 0;
  if (old) {
    int odir = (old->mode & S_IFMT) == S_IFDIR, idir = (i->mode & S_IFMT) == S_IFDIR;
    if (odir != idir) return odir ? -EISDIR : -ENOTDIR;
    if (odir && old->child) return -ENOTEMPTY;
    struct inode **pp = &dir->child;
    while (*pp != old) pp = &(*pp)->next;
    *pp = old->next;
    old->nlink = 0;
  }
  struct inode **pp = &i->parent->child;
  while (*pp != i) pp = &(*pp)->next;
  *pp = i->next;
  i->next = NULL;
  memset(i->name, 0, sizeof i->name);
  memcpy(i->name, last, n);
  i->parent = dir;
  pp = &dir->child;
  while (*pp) pp = &(*pp)->next;
  *pp = i;
  return 0;
}
int vfs_path(struct inode *ino, char *buf, u32 cap) {
  char tmp[PATH_MAX];
  u32 pos = sizeof tmp - 1;
  tmp[pos] = 0;
  while (ino != vfs_root) {
    u32 n = strlen(ino->name);
    if (pos < n + 1) return -ENAMETOOLONG;
    pos -= n; memcpy(tmp + pos, ino->name, n);
    tmp[--pos] = '/';
    ino = ino->parent;
  }
  if (tmp[pos] == 0) tmp[--pos] = '/';
  u32 len = sizeof tmp - pos;
  if (len > cap) return -ERANGE;
  memcpy(buf, tmp + pos, len);
  return len;
}
void vfs_mkdev(const char *path, int dev) {
  int err;
  struct inode *i = vfs_create(vfs_root, path, S_IFCHR | 0666, &err);
  if (i) i->dev = dev;
}
static struct inode *mkdir_p(const char *path, u32 n) {
  struct inode *d = vfs_root;
  u32 i = 0;
  while (i < n) {
    while (i < n && path[i] == '/') i++;
    u32 s = i;
    while (i < n && path[i] != '/') i++;
    if (i == s) break;
    if (i - s == 1 && path[s] == '.') continue;
    struct inode *c = child_find(d, path + s, i - s);
    if (!c) c = inode_new(d, path + s, i - s, S_IFDIR | 0755);
    d = c;
  }
  return d;
}
static u32 octal(const u8 *p, int n) { u32 v = 0; for (int i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0'); return v; }

int vfs_load_tar(u8 *tar, u32 len) {
  int files = 0;
  for (u32 off = 0; off + 512 <= len;) {
    u8 *h = tar + off;
    if (!h[0]) break;
    char name[256];
    u32 n = 0;
    if (h[345]) { while (n < 155 && h[345 + n]) { name[n] = h[345 + n]; n++; } name[n++] = '/'; }
    for (u32 i = 0; i < 100 && h[i]; i++) name[n++] = h[i];
    name[n] = 0;
    u32 size = octal(h + 124, 12), mode = octal(h + 100, 8);
    u8 type = h[156];
    u8 *data = h + 512;
    off += 512 + ((size + 511) & ~511u);
    if (type == '5') { mkdir_p(name, n); continue; }
    if (type != '0' && type != 0) continue;
    const char *slash = strrchr(name, '/');
    struct inode *dir = slash ? mkdir_p(name, slash - name) : vfs_root;
    const char *base = slash ? slash + 1 : name;
    if (!*base || child_find(dir, base, strlen(base))) continue;
    struct inode *f = inode_new(dir, base, strlen(base), S_IFREG | (mode & 0777));
    f->data = data; f->size = f->cap = size; f->owned = 0;
    files++;
  }
  return files;
}
void vfs_init(void) {
  vfs_root = inode_new(NULL, "", 0, S_IFDIR | 0755);
  mkdir_p("dev", 3); mkdir_p("tmp", 3); mkdir_p("proc", 4); mkdir_p("run", 3);
  vfs_mkdev("/dev/null", DEV_NULL);
  vfs_mkdev("/dev/zero", DEV_ZERO);
  vfs_mkdev("/dev/console", DEV_CONSOLE);
  vfs_mkdev("/dev/urandom", DEV_RANDOM);
}

/* ---- regular files ---- */
static long reg_read(struct file *f, u32 ubuf, u32 len) {
  struct inode *i = f->ino;
  if (f->off >= i->size) return 0;
  if (len > i->size - f->off) len = i->size - f->off;
  if (copy_to_user(ubuf, i->data + f->off, len)) return -EFAULT;
  f->off += len;
  return len;
}
static int reg_reserve(struct inode *i, u32 need) {
  if (i->owned && need <= i->cap) return 0;
  u32 cap = i->cap < 256 ? 256 : i->cap;
  while (cap < need) cap *= 2;
  u8 *d = kmalloc(cap);
  if (!d) return -ENOSPC;
  if (i->size) memcpy(d, i->data, i->size);
  if (i->owned) kfree(i->data);
  i->data = d; i->cap = cap; i->owned = 1;
  return 0;
}
static long reg_write(struct file *f, u32 ubuf, u32 len) {
  struct inode *i = f->ino;
  if (f->flags & O_APPEND) f->off = i->size;
  if (reg_reserve(i, f->off + len)) return -ENOSPC;
  if (f->off > i->size) memset(i->data + i->size, 0, f->off - i->size);
  if (copy_from_user(i->data + f->off, ubuf, len)) return -EFAULT;
  f->off += len;
  if (f->off > i->size) i->size = f->off;
  i->mtime = (s64)(host_epoch() / 1000);
  return len;
}
static int always_ready(struct file *f, int add) { (void)f; (void)add; return POLLIN | POLLOUT; }
const struct fops reg_fops = {reg_read, reg_write, always_ready, NULL, NULL};
const struct fops dir_fops = {NULL, NULL, always_ready, NULL, NULL};
int reg_truncate(struct inode *i, u32 size) {
  if (size > i->size) { if (reg_reserve(i, size)) return -ENOSPC; memset(i->data + i->size, 0, size - i->size); }
  i->size = size;
  return 0;
}
