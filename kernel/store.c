// SPDX-License-Identifier: GPL-2.0-or-later
/* Persistence, kernel half: a write-behind layer over the boot image.
 *
 * The truth stays in kernel memory. Every change to an inode in the persistent part of
 * the tree is noted (store_ns, store_rename, vfs_changed, store_dirty). The host drains
 * the noted changes in batches (store_drain) and writes each batch to browser or disk
 * storage as one atomic transaction (boot/store.mjs). fsync waits until a batch that
 * covers its changes is reported done (store_done), using the usual park-and-retry.
 *
 * What is stored is a key/value set, one layer over the image:
 *   'A' sid            anchor: this inode is the image's inode named NAME in directory
 *                      PARENT (by sid); written once, when an image inode first changes
 *   'M' sid            metadata: mode, owner, times, size, link target, flags
 *   'D' dirsid name    a directory entry: the child's sid, or 0 (a whiteout: the image's
 *                      entry of that name is gone)
 *   'B' sid block      64 KiB of file data
 * At boot the image is loaded first, then store_apply() lays the stored records over it
 * before init starts. File data is not read at boot: a file's contents are fetched from
 * the store when it is first opened (store_need_data), again by park-and-retry.
 *
 * A batch is an ordered list of operations in the host's terms: put(key, value),
 * del(key), delrange(lo, hi). The host does not interpret the records. */
#include "kernel.h"
void mem_figures(u64 *total, u64 *free);
struct inode *vfs_inode_new(u32 mode);
struct dentry *vfs_d_lookup(struct inode *dir, const char *name, u32 n);
struct dentry *vfs_d_add(struct inode *dir, const char *name, u32 n, struct inode *i);
void vfs_d_remove(struct dentry *e);

HOST(store_kick) void host_store_kick(int urgent);        /* there are changes to drain (urgent: someone waits) */
HOST(store_fetch) void host_store_fetch(u32 sid, u32 size); /* load a file's contents, then call store_fill_ptr/store_filled */
HOST(store_wipe) void host_store_wipe(void);              /* empty the store */

#define STORE_SCHEMA 2 /* 2: times are packed (seconds * 2^30 + ns); 1 stored plain nanoseconds */
#define BLK 65536u
#define BUDGET (8u << 20) /* file data per batch; a larger burst is split over batches */
enum { S_PERSIST = 1, S_IMAGE = 2, S_DATAOK = 4, S_LAZY = 8, S_FETCH = 16, S_ANCH = 32, S_NEVER = S_NEVER_BIT };
enum { MF_ATTRS = 2, MF_DATA = 4, MF_LINK = 8 };
enum { OP_PUT = 1, OP_DEL = 2, OP_DELRANGE = 3 };

struct sdirty {
  struct inode *i;
  struct sdirty *next, *prev;
  u32 lo, hi;   /* data not yet in a batch */
  u32 ssize;    /* bytes [0, ssize) of the stored blocks are already right */
  u8 meta, data;
};

static int attached, store_on, broken, whole_file, readonly_;
static const char *why = "";
static u32 next_sid = 2, change_no, durable_no, inflight_cover, batch_seq, kicked;
static struct inode **sidmap;
static u32 sidcap;
static struct sdirty *dhead, *dtail;
static u32 ndirty;
static u8 *nslog, *batch;
static u32 nslen, nscap, nsops, blen, bcap, bops;
static waitq sync_wq, fetch_wq;
static u8 *boot_blob;
static u32 boot_len;
static double quota, usage;
static u64 bytes_out, batches, fetches, fetch_bytes;

/* ---- small helpers ---- */
static void be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void le32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void fail(const char *w) {
  if (broken) return;
  broken = 1; why = w; store_on = 0;
  kprintf("[store] persistence stopped: %s\n", w);
  wq_wake(&sync_wq);
}
static int reserve(u8 **b, u32 *cap, u32 need) {
  if (need <= *cap) return 0;
  u32 c = *cap ? *cap : 4096;
  while (c < need) c *= 2;
  u8 *n = krealloc(*b, c);
  if (!n) { fail("out of kernel memory for the change log"); return -1; }
  *b = n; *cap = c;
  return 0;
}
/* append one operation: op, u16 klen, key, then u32 vlen + value (put) or u16 len + key2 (delrange) */
static void emit(u8 **b, u32 *len, u32 *cap, u32 *count, int op, const u8 *k, u32 kl, const u8 *v, u32 vl) {
  if (reserve(b, cap, *len + 1 + 2 + kl + 4 + vl)) return;
  u8 *p = *b + *len;
  p[0] = op; p[1] = kl; p[2] = kl >> 8;
  memcpy(p + 3, k, kl);
  u32 n = 3 + kl;
  if (op == OP_PUT) { le32(p + n, vl); memcpy(p + n + 4, v, vl); n += 4 + vl; }
  else if (op == OP_DELRANGE) { p[n] = vl; p[n + 1] = vl >> 8; memcpy(p + n + 2, v, vl); n += 2 + vl; }
  *len += n; (*count)++;
}
static void log_op(int op, const u8 *k, u32 kl, const u8 *v, u32 vl) { emit(&nslog, &nslen, &nscap, &nsops, op, k, kl, v, vl); }
static u32 key_s(u8 *k, char c, u32 sid) { k[0] = c; be32(k + 1, sid); return 5; }
static u32 key_d(u8 *k, u32 dir, const char *name, u32 n) { k[0] = 'D'; be32(k + 1, dir); memcpy(k + 5, name, n); return 5 + n; }
static u32 key_b(u8 *k, u32 sid, u32 blk) { k[0] = 'B'; be32(k + 1, sid); be32(k + 5, blk); return 9; }

static void kick(void) { change_no++; if (!kicked) { kicked = 1; host_store_kick(0); } }
static struct inode *getmap(u32 sid) { return sid < sidcap ? sidmap[sid] : NULL; }
static int setmap(u32 sid, struct inode *i) {
  if (sid >= sidcap) {
    u32 c = sidcap ? sidcap : 1024;
    while (c <= sid) c *= 2;
    struct inode **n = krealloc(sidmap, c * sizeof *n);
    if (!n) { fail("out of kernel memory for the inode map"); return -1; }
    memset(n + sidcap, 0, (c - sidcap) * sizeof *n);
    sidmap = n; sidcap = c;
  }
  sidmap[sid] = i;
  return 0;
}
static u32 new_sid(struct inode *i) {
  u32 s = next_sid++;
  if (setmap(s, i)) return 0;
  i->sid = s;
  return s;
}
static void sd_drop(struct inode *i) {
  struct sdirty *d = i->sd;
  if (!d) return;
  if (d->prev) d->prev->next = d->next; else dhead = d->next;
  if (d->next) d->next->prev = d->prev; else dtail = d->prev;
  ndirty--;
  kfree(d);
  i->sd = NULL;
}
static struct sdirty *sd_of(struct inode *i) {
  if (i->sd) return i->sd;
  struct sdirty *d = kzalloc(sizeof *d);
  if (!d) { fail("out of kernel memory for dirty tracking"); return NULL; }
  d->i = i; d->ssize = i->size;
  d->prev = dtail;
  if (dtail) dtail->next = d; else dhead = d;
  dtail = d;
  ndirty++;
  i->sd = d;
  return d;
}

/* An image inode gets a sid the first time something about it must be stored, with an
 * anchor naming where it is in the image: its directory (anchored first) and its name.
 * It has not moved itself (a move would have given it a sid), so its current name is its
 * image name. */
static u32 ensure_sid(struct inode *i) {
  if (i->sid) return i->sid;
  if (!(i->sflags & S_IMAGE)) return 0;
  struct dentry *e = i->alias;
  if (!e) return 0;
  u32 p = ensure_sid(e->dir);
  if (!p || !new_sid(i)) return 0;
  i->sflags = (i->sflags & ~S_IMAGE) | S_ANCH;
  u8 k[8], v[4 + VFS_NAME_MAX];
  be32(v, p);
  memcpy(v + 4, e->name, e->nlen);
  log_op(OP_PUT, k, key_s(k, 'A', i->sid), v, 4 + e->nlen);
  kick();
  return i->sid;
}
static void forget_store(struct inode *i) { /* the inode's last name went: its records go */
  if (i->sid) {
    u8 k[12], k2[12];
    log_op(OP_DEL, k, key_s(k, 'A', i->sid), NULL, 0);
    log_op(OP_DEL, k, key_s(k, 'M', i->sid), NULL, 0);
    key_b(k, i->sid, 0); key_b(k2, i->sid + 1, 0);
    log_op(OP_DELRANGE, k, 9, k2, 9);
    setmap(i->sid, NULL);
    i->sid = 0;
    kick();
  }
  sd_drop(i);
  i->sflags &= ~(S_PERSIST | S_IMAGE | S_ANCH);
}
static void unpersist_tree(struct inode *i) { /* moved out of the persistent part */
  forget_store(i);
  if (i->dir) for (struct dentry *e = i->dir->first; e; e = e->next) if (e->ino->sflags & S_PERSIST) unpersist_tree(e->ino);
}
static void name_put(struct inode *dir, const char *name, u32 n, struct inode *child) {
  u8 k[8 + VFS_NAME_MAX], v[4];
  be32(v, child ? child->sid : 0);
  log_op(OP_PUT, k, key_d(k, dir->sid, name, n), v, 4);
  kick();
}
static void name_gone(struct inode *dir, const char *name, u32 n) {
  /* an image directory needs a whiteout to hide the image's entry; a directory made after
   * the store was attached has nothing under it in the image */
  if (dir->sflags & S_ANCH || dir == vfs_root) { name_put(dir, name, n, NULL); return; }
  u8 k[8 + VFS_NAME_MAX];
  log_op(OP_DEL, k, key_d(k, dir->sid, name, n), NULL, 0);
  kick();
}
static void adopt(struct inode *i) { /* moved into the persistent part: store all of it */
  if (i->sflags & S_NEVER) return; /* a remote file: fetched again from the server, never stored */
  i->sflags = (i->sflags & ~S_IMAGE) | S_PERSIST | S_DATAOK;
  if (!i->sid && !new_sid(i)) return;
  struct sdirty *d = sd_of(i);
  if (!d) return;
  d->meta = 1;
  if (S_ISREG(i->mode) && i->size) { d->data = 1; d->lo = 0; d->hi = i->size; d->ssize = 0; }
  kick();
  if (i->dir)
    for (struct dentry *e = i->dir->first; e; e = e->next) { adopt(e->ino); name_put(i, e->name, e->nlen, e->ino); }
}
static int last_link(struct inode *i) { return S_ISDIR(i->mode) || i->nlink <= 1; }

/* ---- hooks called by vfs.c ---- */
void vfs_changed(struct inode *i) {
  if (!store_on || !(i->sflags & S_PERSIST) || !ensure_sid(i)) return;
  struct sdirty *d = sd_of(i);
  if (d && !d->meta) { d->meta = 1; kick(); }
}
void store_dirty(struct inode *i, u32 lo, u32 hi) {
  if (!store_on || !(i->sflags & S_PERSIST) || !ensure_sid(i)) return;
  struct sdirty *d = sd_of(i);
  if (!d) return;
  d->meta = 1;
  if (!(i->sflags & S_DATAOK) || whole_file) { lo = 0; hi = i->size > hi ? i->size : hi; } /* first change of an image file: copy all of it */
  if (!d->data) { d->data = 1; d->lo = lo; d->hi = hi; }
  else { if (lo < d->lo) d->lo = lo; if (hi > d->hi) d->hi = hi; }
  if (lo < d->ssize) d->ssize = lo;
  kick();
}
void store_forget(struct inode *i) { /* the inode is freed */
  sd_drop(i);
  if (i->sid && getmap(i->sid) == i) setmap(i->sid, NULL);
}
/* op 1: i was created in dir; op 2: i is about to lose this name; op 3: i got a new name */
void store_ns(int op, struct inode *dir, const char *name, u32 n, struct inode *i) {
  if (!store_on || !(dir->sflags & S_PERSIST) || !ensure_sid(dir)) return;
  if (op == 1) {
    if (i->sflags & S_NEVER) return;
    i->sflags |= S_PERSIST | S_DATAOK;
    if (!new_sid(i)) return;
    struct sdirty *d = sd_of(i);
    if (d) d->meta = 1;
    name_put(dir, name, n, i);
  } else if (op == 2) {
    name_gone(dir, name, n);
    if (last_link(i)) forget_store(i);
  } else if (op == 3) {
    if (!(i->sflags & S_PERSIST)) adopt(i);
    else if (!ensure_sid(i)) return;
    name_put(dir, name, n, i);
    vfs_changed(i);
  }
}
void store_rename(struct inode *odir, const char *on, u32 onl, struct inode *i, struct inode *ndir, const char *nn, u32 nnl,
                  struct inode *old, int exchange) {
  if (!store_on) return;
  int op = (odir->sflags & S_PERSIST) && ensure_sid(odir), np = (ndir->sflags & S_PERSIST) && ensure_sid(ndir);
  if (!op && !np) return;
  /* sids first, while every name is still where it was */
  if ((i->sflags & S_PERSIST) && !ensure_sid(i)) return;
  if (old && (old->sflags & S_PERSIST) && !ensure_sid(old)) return;
  if (exchange) {
    if (op) { if (!(old->sflags & S_PERSIST)) adopt(old); name_put(odir, on, onl, old); }
    else if (old->sflags & S_PERSIST) unpersist_tree(old);
    if (np) { if (!(i->sflags & S_PERSIST)) adopt(i); name_put(ndir, nn, nnl, i); }
    else if (i->sflags & S_PERSIST) unpersist_tree(i);
    return;
  }
  if (old && (old->sflags & S_PERSIST) && last_link(old)) forget_store(old);
  if (np) {
    if (!(i->sflags & S_PERSIST)) adopt(i);
    name_put(ndir, nn, nnl, i);
  }
  if (op) {
    name_gone(odir, on, onl);
    if (!np && last_link(i)) unpersist_tree(i);
  }
}

/* ---- draining: the host asks for a batch, writes it, and says when it is durable ---- */
static void emit_meta(struct inode *i, u32 size) {
  u8 k[8], v[48 + VFS_PATH_MAX];
  u32 n = 0, fl = MF_ATTRS;
  if (S_ISREG(i->mode) && (i->sflags & S_DATAOK)) fl |= MF_DATA;
  if (S_ISLNK(i->mode) && !(i->sflags & S_ANCH)) fl |= MF_LINK;
  v[n++] = STORE_SCHEMA; v[n++] = fl; v[n++] = 0; v[n++] = 0;
  le32(v + n, i->mode); le32(v + n + 4, i->uid); le32(v + n + 8, i->gid); le32(v + n + 12, size); n += 16;
  memcpy(v + n, &i->atime, 8); memcpy(v + n + 8, &i->mtime, 8); memcpy(v + n + 16, &i->ctime, 8); n += 24;
  if (fl & MF_LINK) { u32 l = i->size < VFS_PATH_MAX ? i->size : VFS_PATH_MAX - 1; memcpy(v + n, i->data, l); n += l; }
  emit(&batch, &blen, &bcap, &bops, OP_PUT, k, key_s(k, 'M', i->sid), v, n);
}
/* Returns the batch length; the batch is at store_batch(). Header: "UCSB", seq, op count. */
EXPORT(store_drain) u32 store_drain(u32 budget) {
  if (!budget) budget = BUDGET;
  blen = 12; bops = 0;
  if (reserve(&batch, &bcap, 12)) return 0;
  if (nslen) {
    if (reserve(&batch, &bcap, blen + nslen)) return 0;
    memcpy(batch + blen, nslog, nslen);
    blen += nslen; bops += nsops;
    nslen = nsops = 0;
  }
  u32 data = 0;
  for (struct sdirty *d = dhead, *next; d; d = next) {
    next = d->next;
    struct inode *i = d->i;
    if (d->data && S_ISREG(i->mode) && !(i->sflags & S_LAZY)) {
      u32 hi = d->hi < i->size ? d->hi : i->size, b = d->lo / BLK;
      for (; b * BLK < hi && data < budget; b++) {
        u8 k[12];
        u32 off = b * BLK, len = i->size - off < BLK ? i->size - off : BLK;
        emit(&batch, &blen, &bcap, &bops, OP_PUT, k, key_b(k, i->sid, b), i->data + off, len);
        data += len;
      }
      d->lo = b * BLK;
      if (d->lo >= hi) { d->data = 0; i->sflags |= S_DATAOK; d->ssize = i->size; }
    } else d->data = 0;
    u32 size = d->data ? (d->ssize < i->size ? d->ssize : i->size) : i->size;
    emit_meta(i, size);
    if (!d->data && S_ISREG(i->mode) && (i->sflags & S_DATAOK)) { /* blocks past the end go */
      u8 k[12], k2[12];
      key_b(k, i->sid, (size + BLK - 1) / BLK); key_b(k2, i->sid + 1, 0);
      emit(&batch, &blen, &bcap, &bops, OP_DELRANGE, k, 9, k2, 9);
    }
    if (!d->data) sd_drop(i);
  }
  memcpy(batch, "UCSB", 4); le32(batch + 4, ++batch_seq); le32(batch + 8, bops);
  inflight_cover = dhead ? 0 : change_no; /* durable up to here once written, unless data was left over */
  kicked = dhead != NULL;
  bytes_out += blen; batches++;
  return blen;
}
EXPORT(store_batch) u8 *store_batch(void) { return batch; }
EXPORT(store_pending) int store_pending(void) { return store_on && (nslen || dhead); }
EXPORT(store_done) void store_done(u32 seq, int ok) {
  if (seq != batch_seq) return;
  if (!ok) { fail("the browser refused to store a batch (storage full or unavailable)"); return; }
  if (inflight_cover && (s32)(inflight_cover - durable_no) > 0) durable_no = inflight_cover;
  wq_wake(&sync_wq);
}
EXPORT(store_usage) void store_usage(double used, double total) { usage = used; quota = total; }
long store_sync(struct file *f, int all) {
  (void)f; (void)all;
  struct fscred *c = &fscred[cur->pid];
  if (broken) { c->sync_target = 0; return -EIO; }
  if (!store_on) return 0;
  if (!c->sync_target) {
    if (!nslen && !dhead && durable_no == change_no) return 0;
    c->sync_target = change_no;
  }
  if ((s32)(durable_no - c->sync_target) >= 0) { c->sync_target = 0; return 0; }
  wq_add(&sync_wq);
  host_store_kick(1);
  return block(-1);
}

/* ---- contents on first use ---- */
long store_need_data(struct inode *i) {
  if (!(i->sflags & S_LAZY)) return 0;
  if (!(i->sflags & S_FETCH)) { i->sflags |= S_FETCH; fetches++; host_store_fetch(i->sid, i->size); }
  wq_add(&fetch_wq);
  return block(-1);
}
EXPORT(store_fill_ptr) u8 *store_fill_ptr(u32 sid, u32 size) {
  struct inode *i = getmap(sid);
  if (!i || !(i->sflags & S_LAZY) || size != i->size) return NULL;
  u8 *p = kmalloc(size ? size : 1);
  if (!p) return NULL;
  i->data = p; i->cap = size; i->owned = 1;
  return p;
}
EXPORT(store_filled) void store_filled(u32 sid, int ok) {
  struct inode *i = getmap(sid);
  if (i && (i->sflags & S_LAZY)) {
    if (!ok) { /* the store lost it: an empty file, said loudly, rather than a hang */
      kprintf("[store] contents of inode %u could not be read; the file is now empty\n", i->ino);
      if (i->owned) kfree(i->data);
      i->data = NULL; i->size = i->cap = 0; i->owned = 0;
    } else fetch_bytes += i->size;
    i->sflags &= ~(S_LAZY | S_FETCH);
  }
  wq_wake(&fetch_wq);
}

/* ---- boot: lay the stored layer over the image ---- */
EXPORT(store_schema) int store_schema(void) { return STORE_SCHEMA; }
/* flags: 1 attach (persist changes), 2 read only (load, never write), 4 whole-file dirty tracking (for measurements) */
static u32 boot_flags;
EXPORT(store_boot) void store_boot(u8 *blob, u32 len, u32 flags) { boot_blob = blob; boot_len = len; boot_flags = flags; }
struct rec { const u8 *k; const u8 *v; u32 kl, vl; };
static void free_tree(struct inode *i) {
  if (i->dir) while (i->dir->first) { struct dentry *e = i->dir->first; struct inode *c = e->ino; vfs_d_remove(e); if (S_ISDIR(c->mode)) free_tree(c); else if (!--c->nlink && !c->refs) inode_drop(c); }
  if (!i->refs) inode_drop(i); else i->nlink = 0;
}
static int in_list(struct inode **v, u32 n, struct inode *i) { for (u32 k = 0; k < n; k++) if (v[k] == i) return 1; return 0; }
/* default: everything persists except what the kernel makes afresh at every boot */
static const char *skip_default[] = {"/tmp", "/run", "/proc", "/dev"};
static struct inode *bound_ino[32];
static u8 bound_on[32];
static u32 nbound;
static void bound(const char *path, int on) {
  struct inode *i = vfs_lookup(vfs_root, path);
  if (i && S_ISDIR(i->mode) && nbound < 32) { bound_ino[nbound] = i; bound_on[nbound++] = on; }
}
static void read_config(void) { /* /etc/ucos/persist.conf: lines "skip PATH" or "persist PATH" */
  struct inode *c = vfs_lookup(vfs_root, "/etc/ucos/persist.conf");
  if (!c || !S_ISREG(c->mode) || !c->data) { for (u32 k = 0; k < 4; k++) bound(skip_default[k], 0); return; }
  char line[VFS_PATH_MAX];
  for (u32 off = 0; off < c->size;) {
    u32 n = 0;
    while (off < c->size && c->data[off] != '\n') { if (n < sizeof line - 1) line[n++] = c->data[off]; off++; }
    off++;
    line[n] = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "skip ", 5) || !strncmp(p, "persist ", 8)) {
      int on = *p == 'p';
      p += on ? 8 : 5;
      while (*p == ' ') p++;
      char *e = p + strlen(p);
      while (e > p && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
      if (*p == '/') bound(p, on);
    }
  }
}
static void mark_tree(struct inode *i, int on) {
  for (u32 k = 0; k < nbound; k++) if (bound_ino[k] == i) on = bound_on[k];
  if (on) { i->sflags |= S_PERSIST; if (!i->sid) i->sflags |= S_IMAGE; }
  else i->sflags &= ~(S_PERSIST | S_IMAGE);
  if (!i->dir || i->kind) return;
  for (struct dentry *e = i->dir->first; e; e = e->next) {
    struct inode *c = e->ino;
    if (S_ISDIR(c->mode)) { mark_tree(c, on); continue; }
    if (c->sflags & S_NEVER) continue; /* a remote file: like an image file it comes back by itself, but its bytes are never stored */
    if (on) { c->sflags |= S_PERSIST; if (!c->sid) c->sflags |= S_IMAGE; } else c->sflags &= ~(S_PERSIST | S_IMAGE);
  }
}
static void apply_meta(struct inode *i, const u8 *v, u32 vl) {
  if (vl < 44 || v[0] != STORE_SCHEMA) return;
  u32 fl = v[1];
  if (!(fl & MF_ATTRS)) return;
  u32 mode, uid, gid, size;
  memcpy(&mode, v + 4, 4); memcpy(&uid, v + 8, 4); memcpy(&gid, v + 12, 4); memcpy(&size, v + 16, 4);
  if ((mode & S_IFMT) != (i->mode & S_IFMT)) return; /* the image changed the type under it: image wins */
  i->mode = mode; i->uid = uid; i->gid = gid;
  memcpy(&i->atime, v + 20, 8); memcpy(&i->mtime, v + 28, 8); memcpy(&i->ctime, v + 36, 8);
  if (S_ISREG(mode) && (fl & MF_DATA)) {
    if (i->owned) kfree(i->data);
    i->data = NULL; i->owned = 0; i->cap = 0; i->size = size;
    i->sflags |= S_DATAOK;
    if (size) i->sflags |= S_LAZY;
  }
  if (S_ISLNK(mode) && (fl & MF_LINK)) {
    u32 l = vl - 44;
    u8 *t = kmalloc(l + 1);
    if (t) { if (i->owned) kfree(i->data); memcpy(t, v + 44, l); t[l] = 0; i->data = t; i->size = l; i->owned = 1; }
  }
}
static void garbage(char c, u32 sid) { u8 k[8]; log_op(OP_DEL, k, key_s(k, c, sid), NULL, 0); }
void store_apply(void) {
  if (!boot_blob && !boot_flags) return;
  attached = 1;
  readonly_ = !!(boot_flags & 2);
  whole_file = !!(boot_flags & 4);
  double t0 = host_now();
  /* the records: A, M and D puts (the host sends everything but the data blocks) */
  u32 nrec = 0, cap = 0, maxsid = 1;
  struct rec *r = NULL;
  for (u32 off = 0; off + 3 <= boot_len;) {
    const u8 *p = boot_blob + off;
    u32 kl = p[1] | p[2] << 8;
    if (p[0] != OP_PUT || off + 3 + kl + 4 > boot_len) break;
    u32 vl;
    memcpy(&vl, p + 3 + kl, 4);
    if (off + 7 + kl + vl > boot_len) break;
    if (nrec == cap) { cap = cap ? cap * 2 : 1024; struct rec *n = krealloc(r, cap * sizeof *r); if (!n) break; r = n; }
    r[nrec].k = p + 3; r[nrec].kl = kl; r[nrec].v = p + 7 + kl; r[nrec].vl = vl;
    if (kl >= 5 && (p[3] == 'A' || p[3] == 'M') && rd32(p + 4) > maxsid) maxsid = rd32(p + 4);
    nrec++;
    off += 7 + kl + vl;
  }
  setmap(maxsid + 1, NULL);
  const struct rec **anchor = kzalloc((maxsid + 1) * sizeof *anchor), **meta = kzalloc((maxsid + 1) * sizeof *meta);
  vfs_root->sid = 1; setmap(1, vfs_root); vfs_root->sflags |= S_ANCH;
  if (anchor && meta) {
    for (u32 k = 0; k < nrec; k++) {
      if (r[k].kl != 5) continue;
      u32 s = rd32(r[k].k + 1);
      if (r[k].k[0] == 'A') anchor[s] = &r[k]; else if (r[k].k[0] == 'M') meta[s] = &r[k];
    }
    /* 1. anchors, parents first (a parent always has the smaller sid) */
    for (u32 s = 2; s <= maxsid; s++) {
      const struct rec *a = anchor[s];
      if (!a || a->vl < 5) continue;
      struct inode *dir = getmap(rd32(a->v));
      u32 nl = a->vl - 4;
      if (!dir || !S_ISDIR(dir->mode) || nl > VFS_NAME_MAX) { garbage('A', s); continue; }
      struct dentry *e = vfs_d_lookup(dir, (const char *)a->v + 4, nl);
      struct inode *i = e ? e->ino : NULL;
      if (!i) { /* the new image no longer has it: make it again where it was */
        u32 mode = S_IFDIR | 0755;
        if (meta[s] && meta[s]->vl >= 44) memcpy(&mode, meta[s]->v + 4, 4);
        if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) mode = S_IFDIR | 0755;
        i = vfs_inode_new(mode);
        if (!i || !vfs_d_add(dir, (const char *)a->v + 4, nl, i)) continue;
        i->owned = 1;
        if (S_ISDIR(mode)) { i->nlink = 2; dir->nlink++; i->dir->parent = dir; } else i->nlink = 1;
      }
      i->sid = s; setmap(s, i); i->sflags |= S_ANCH;
    }
    /* 2. metadata; nodes without an anchor are made here */
    for (u32 s = 2; s <= maxsid; s++) {
      const struct rec *m = meta[s];
      if (!m || m->vl < 44) continue;
      struct inode *i = getmap(s);
      if (!i) {
        u32 mode;
        memcpy(&mode, m->v + 4, 4);
        if (!(i = vfs_inode_new(mode))) continue;
        i->owned = 1; i->sid = s; setmap(s, i);
        i->nlink = S_ISDIR(mode) ? 2 : 0;
      }
      apply_meta(i, m->v, m->vl);
    }
    /* 3. entries: first every name that goes or is replaced, then every name that comes */
    struct inode **gone = NULL;
    u32 ngone = 0, gcap = 0;
    for (u32 k = 0; k < nrec; k++) {
      if (r[k].k[0] != 'D' || r[k].kl < 6 || r[k].vl != 4) continue;
      struct inode *dir = getmap(rd32(r[k].k + 1)), *c = getmap(rd32(r[k].v));
      if (!dir || !S_ISDIR(dir->mode)) continue;
      struct dentry *e = vfs_d_lookup(dir, (const char *)r[k].k + 5, r[k].kl - 5);
      if (!e || e->ino == c) continue;
      struct inode *was = e->ino;
      vfs_d_remove(e);
      if (S_ISDIR(was->mode)) { dir->nlink--; was->dir->parent = was; } else was->nlink--;
      if (ngone == gcap) { gcap = gcap ? gcap * 2 : 64; struct inode **n = krealloc(gone, gcap * sizeof *n); if (!n) continue; gone = n; }
      gone[ngone++] = was;
    }
    for (u32 k = 0; k < nrec; k++) {
      if (r[k].k[0] != 'D' || r[k].kl < 6 || r[k].vl != 4) continue;
      u32 cs = rd32(r[k].v);
      struct inode *dir = getmap(rd32(r[k].k + 1)), *c = getmap(cs);
      if (!cs) continue;
      if (!dir || !S_ISDIR(dir->mode) || !c || (S_ISDIR(c->mode) && c->alias)) { log_op(OP_DEL, r[k].k, r[k].kl, NULL, 0); continue; }
      if (vfs_d_lookup(dir, (const char *)r[k].k + 5, r[k].kl - 5)) continue;
      if (!vfs_d_add(dir, (const char *)r[k].k + 5, r[k].kl - 5, c)) continue;
      if (S_ISDIR(c->mode)) { c->dir->parent = dir; dir->nlink++; } else c->nlink++;
    }
    for (u32 k = 0; k < ngone; k++) { /* names that went and did not come back elsewhere */
      struct inode *g = gone[k];
      if (g->alias || in_list(gone, k, g)) continue;
      if (g->sid) { garbage('A', g->sid); garbage('M', g->sid); setmap(g->sid, NULL); g->sid = 0; }
      if (S_ISDIR(g->mode)) free_tree(g); else if (!g->nlink && !g->refs) inode_drop(g);
    }
    kfree(gone);
    /* 4. stored nodes that no name reaches any more */
    for (u32 s = 2; s <= maxsid; s++) {
      struct inode *i = getmap(s);
      if (!i || i->alias) continue;
      garbage('A', s); garbage('M', s);
      u8 k[12], k2[12];
      key_b(k, s, 0); key_b(k2, s + 1, 0);
      log_op(OP_DELRANGE, k, 9, k2, 9);
      setmap(s, NULL); i->sid = 0;
      if (S_ISDIR(i->mode)) free_tree(i); else if (!i->nlink && !i->refs) inode_drop(i);
    }
  }
  kfree(anchor); kfree(meta); kfree(r);
  next_sid = maxsid + 1;
  read_config();
  mark_tree(vfs_root, 1);
  store_on = (boot_flags & 1) && !readonly_;
  kicked = nslen != 0;
  if (kicked) { change_no++; host_store_kick(0); }
  kprintf("[store] %u records applied in %.2f ms%s\n", nrec, host_now() - t0, readonly_ ? " (read only: changes are not kept)" : "");
}

/* ---- status and control: /proc/ucos-store ---- */
int store_status(char *buf, u32 cap) {
  u64 total, free;
  store_statfs(&total, &free);
  return snprintf(buf, cap,
                  "state: %s\nreason: %s\nquota_bytes: %.0f\nused_bytes: %.0f\npending_changes: %u\ndirty_inodes: %u\nbatches: %llu\nbatch_bytes: %llu\n"
                  "files_loaded: %llu\nbytes_loaded: %llu\n",
                  !attached ? "off (no store)" : broken ? "stopped" : readonly_ ? "read only" : store_on ? "on" : "wiped",
                  why, quota, usage, change_no - durable_no, ndirty, batches, bytes_out, fetches, fetch_bytes);
}
long store_ctl(const char *cmd, u32 n) {
  if (n >= 4 && !memcmp(cmd, "wipe", 4)) {
    if (!attached) return -ENODEV;
    host_store_wipe();
    store_on = 0; why = "wiped: the next boot starts from the image";
    kprintf("[store] wiped\n");
    return 0;
  }
  if (n >= 4 && !memcmp(cmd, "sync", 4)) return store_sync(NULL, 1);
  return -EINVAL;
}
void store_statfs(u64 *total, u64 *free) {
  mem_figures(total, free);
  if (attached && quota > 0) {
    u64 q = (u64)quota, left = usage < quota ? (u64)(quota - usage) : 0;
    if (q < *total) *total = q;
    if (left < *free) *free = left;
  }
  if (broken) *free = 0;
}
